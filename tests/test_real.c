/*
 * test_real.c — the runtime on geistlib with a real model (#4).
 *
 *   GEIST_TEST_MODEL=…/smollm2-360m-instruct-q8_0.gguf build/test_real
 *   GEIST_TEST_MODEL_LARGE=…/Qwen3.8-27B-Q4_0.gguf   (optional: window choice)
 *   GEIST_TEST_MODEL_RECURRENT=…/Qwen3.5-0.8B-Q8_0.gguf (optional: rewind by re-prefill)
 *
 * Greedy decoding throughout, so answers are reproducible. Checked:
 *   - the window: the trained one where it fits, else what fits into memory
 *     (against geist_model_plan); an explicit context is kept;
 *   - a follow-up turn processes only its new tokens (close + turn + prompt);
 *   - rewind: to a send boundary (truncate) gives the same answer again as
 *     the first time; to a point inside a send (re-prefill) the same context
 *     as a fresh chat; rewind(0) starts over;
 *   - overflow: REFUSE leaves the chat unchanged, DROP_OLDEST keeps the
 *     system message and answers;
 *   - cancellation from another thread, during generation and during a long
 *     prefill, within a bounded time, the chat usable afterwards;
 *   - two chats on one model in parallel answer as one alone;
 *   - stop strings, a model outliving its handle.
 */
#include "geistr.h"
#include "geistr_engine.h"

#include <geist.h>
#include <geist_util.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

static int failures;
#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, what);                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec * 1e3 + (double) t.tv_nsec / 1e6;
}

static const geistr_message SYSTEM = {"system", "You are a terse assistant. Answer in a few words."};

static geistr_chat *chat(geistr_model *m, uint32_t max_tokens, geistr_overflow overflow) {
    geistr_chat_opts o = GEISTR_CHAT_OPTS_INIT;
    o.max_tokens       = max_tokens;
    o.overflow         = overflow;
    geistr_chat *c     = nullptr;
    CHECK(geistr_chat_open(m, &o, &c) == GEISTR_OK, "chat open");
    return c;
}

/* send + drain; the answer into out (truncated to cap), the status back. */
static geistr_status ask(geistr_chat *c, size_t n, const geistr_message msgs[], char *out, size_t cap) {
    out[0]          = 0;
    geistr_status s = geistr_chat_send(c, n, msgs);
    geistr_piece  p = {.size = sizeof p};
    while (s == GEISTR_OK && (s = geistr_chat_next(c, &p)) == GEISTR_OK && p.part != GEISTR_PART_END)
        strncat(out, p.text, cap - strlen(out) - 1);
    return s;
}

static geistr_stats stats(geistr_chat *c) {
    geistr_stats st = {.size = sizeof st};
    CHECK(geistr_chat_stats(c, &st) == GEISTR_OK, "stats");
    return st;
}

static uint64_t physical_memory(void) {
#if defined(__APPLE__)
    uint64_t bytes = 0;
    size_t   len   = sizeof bytes;
    return sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) == 0 ? bytes : 0;
#else
    return (uint64_t) sysconf(_SC_PHYS_PAGES) * (uint64_t) sysconf(_SC_PAGESIZE);
#endif
}

/* The chosen window: trained where it fits, else within 3/4 of memory. */
static void window(const char *path, const char *name) {
    geistr_model *m = nullptr;
    char          error[256];
    geistr_status s = geistr_model_open(path, nullptr, &m, error, sizeof error);
    CHECK(s == GEISTR_OK, error);
    if (s != GEISTR_OK)
        return;
    geistr_model_info info = {.size = sizeof info};
    CHECK(geistr_model_info_get(m, &info) == GEISTR_OK, "info");
    /* What the runtime planned with: the same call on the same backend kind. */
    struct geist_backend *be = nullptr;
    const char           *bn = !strcmp(info.backend, "cpu") ? "cpu_neon" : info.backend;
    if (geist_backend_create(bn, nullptr, nullptr, &be) != GEIST_OK)
        geist_backend_create("cpu_x86", nullptr, nullptr, &be);
    if (be == nullptr)
        geist_backend_create("cpu_scalar", nullptr, nullptr, &be);
    struct geist_session_opts so   = {.top_p = 1.0f};
    struct geist_model_plan   plan = {};
    CHECK(be && geist_model_plan(path, be, &so, &plan) == GEIST_OK, "plan");
    const uint64_t trained = plan.context_length ? plan.context_length : 4096;
    const uint64_t need    = plan.weight_bytes + (uint64_t) info.context *
                                                      (plan.kv_bytes_per_token + plan.model_bytes_per_token);
    char what[256];
    snprintf(what, sizeof what, "%s: window %u of %llu trained, %.1f GB of %.1f GB budget (%s)", name,
             info.context, (unsigned long long) trained, (double) need / 1e9,
             (double) (physical_memory() / 4 * 3) / 1e9, info.backend);
    printf("  %s\n", what);
    CHECK(info.context <= trained && info.context % 256 == 0 &&
              (info.context == trained || need <= physical_memory() / 4 * 3),
          what);
    geist_backend_destroy(be);
    geistr_model_close(m);
}

/* A recurrent model (Qwen3.5 DeltaNet): geistlib cannot truncate it, so a
 * rewind re-prefills the kept messages; the context must match. */
static void recurrent(const char *path) {
    geistr_model *m = nullptr;
    char          error[256], a[512], b[512];
    CHECK(geistr_model_open(path, nullptr, &m, error, sizeof error) == GEISTR_OK, error);
    if (!m)
        return;
    geistr_chat   *c    = chat(m, 24, GEISTR_OVERFLOW_REFUSE);
    geistr_message t1[] = {SYSTEM, {"user", "What is the capital of France?"}};
    geistr_message t2   = {"user", "And of Italy?"};
    CHECK(ask(c, 2, t1, a, sizeof a) == GEISTR_OK && ask(c, 1, &t2, b, sizeof b) == GEISTR_OK, "recurrent: two turns");
    const uint32_t before = stats(c).context_tokens - stats(c).output_tokens;
    CHECK(geistr_chat_rewind(c, 3) == GEISTR_OK && geistr_chat_length(c) == 3, "recurrent: rewind re-prefills");
    CHECK(ask(c, 1, &t2, b, sizeof b) == GEISTR_OK && b[0], "recurrent: answers after the rewind");
    CHECK(stats(c).context_tokens - stats(c).output_tokens == before, "recurrent: the same context as before");
    printf("  recurrent model: rewind by re-prefill, context %u tokens before the answer\n", before);
    geistr_chat_close(c);
    geistr_model_close(m);
}

struct cancel_after {
    geistr_chat *c;
    unsigned     ms;
    atomic_int  *pieces; /* if set: cancel once this many pieces arrived, not after ms */
    double       at;     /* when the cancel was called */
};
static void *cancel_later(void *arg) {
    struct cancel_after *a  = arg;
    struct timespec      ts = {.tv_sec = a->ms / 1000, .tv_nsec = (long) (a->ms % 1000) * 1000000};
    if (a->pieces)
        for (int i = 0; i < 10000 && atomic_load(a->pieces) < 3; i++)
            nanosleep(&(struct timespec) {.tv_nsec = 1000000}, nullptr);
    else
        nanosleep(&ts, nullptr);
    a->at = now_ms();
    geistr_chat_cancel(a->c);
    return nullptr;
}

struct parallel {
    geistr_model *m;
    char          answer[512];
};
static void *parallel_ask(void *arg) {
    struct parallel *p = arg;
    geistr_chat     *c = chat(p->m, 24, GEISTR_OVERFLOW_REFUSE);
    geistr_message   q[] = {SYSTEM, {"user", "Name three colours."}};
    if (c)
        ask(c, 2, q, p->answer, sizeof p->answer);
    geistr_chat_close(c);
    return nullptr;
}

int main(void) {
    const char *path = getenv("GEIST_TEST_MODEL");
    if (!path || access(path, R_OK) != 0) {
        printf("test_real: SKIPPED (set GEIST_TEST_MODEL; make fetch-model)\n");
        return 0;
    }
    char          error[256], a1[1024], a2[1024], again[1024];
    geistr_model *m = nullptr;
    CHECK(geistr_model_open(path, nullptr, &m, error, sizeof error) == GEISTR_OK, error);
    if (!m)
        return 1;
    geistr_model_info info = {.size = sizeof info};
    CHECK(geistr_model_info_get(m, &info) == GEISTR_OK && !strcmp(info.chat_format, "chatml"), "SmolLM2 speaks ChatML");
    /* info's strings are borrowed until the model is released: keep copies. */
    char arch[64], backend[64];
    snprintf(arch, sizeof arch, "%s", info.arch);
    snprintf(backend, sizeof backend, "%s", info.backend);

    /* ---- a conversation: only new tokens per turn ---- */
    geistr_chat   *c     = chat(m, 40, GEISTR_OVERFLOW_REFUSE);
    geistr_message t1[]  = {SYSTEM, {"user", "What is the capital of France?"}};
    geistr_message t2    = {"user", "And of Italy?"};
    CHECK(ask(c, 2, t1, a1, sizeof a1) == GEISTR_OK && a1[0], "first answer");
    const geistr_stats s1 = stats(c);
    CHECK(s1.input_tokens > 10 && s1.output_tokens > 0 && s1.first_answer_ms >= 0, "first turn stats");
    CHECK(ask(c, 1, &t2, a2, sizeof a2) == GEISTR_OK && a2[0], "second answer");
    const geistr_stats s2 = stats(c);
    printf("  turn 1: %u input tokens; turn 2: %u input tokens (context %u)\n", s1.input_tokens, s2.input_tokens,
           s2.context_tokens);
    CHECK(s2.input_tokens < 20 && s2.input_tokens < s1.input_tokens, "a follow-up processes only its new tokens");
    CHECK(s2.context_tokens > s1.context_tokens + s2.input_tokens, "the context holds both turns and answers");
    CHECK(geistr_chat_length(c) == 5, "system, user, answer, user, answer");

    /* ---- rewind to a send boundary: the second turn again ---- */
    CHECK(geistr_chat_rewind(c, 3) == GEISTR_OK && geistr_chat_length(c) == 3, "rewind before the second turn");
    CHECK(ask(c, 1, &t2, again, sizeof again) == GEISTR_OK && !strcmp(again, a2),
          "the same question after a rewind gives the same answer");
    /* The first answer's turn close stays in the kept context: one token
     * fewer to process, the same context afterwards. */
    CHECK(stats(c).input_tokens <= s2.input_tokens && stats(c).context_tokens == s2.context_tokens,
          "and ends with the same context");
    /* ---- rewind(0): start over ---- */
    CHECK(geistr_chat_rewind(c, 0) == GEISTR_OK && geistr_chat_length(c) == 0, "rewind(0)");
    CHECK(ask(c, 2, t1, again, sizeof again) == GEISTR_OK && !strcmp(again, a1), "starting over answers as before");
    /* ---- rewind inside a send (re-prefill): the same context as a fresh chat ---- */
    geistr_message other = {"user", "What is the capital of Spain?"};
    CHECK(geistr_chat_rewind(c, 1) == GEISTR_OK && geistr_chat_length(c) == 1, "rewind to the system message");
    CHECK(ask(c, 1, &other, again, sizeof again) == GEISTR_OK && again[0], "answer after a re-prefill");
    const uint32_t ctx_rewound = stats(c).context_tokens - stats(c).output_tokens;
    geistr_chat   *fresh       = chat(m, 40, GEISTR_OVERFLOW_REFUSE);
    geistr_message both[]      = {SYSTEM, other};
    CHECK(ask(fresh, 2, both, again, sizeof again) == GEISTR_OK, "fresh chat");
    CHECK(stats(fresh).context_tokens - stats(fresh).output_tokens == ctx_rewound,
          "a re-prefilled conversation holds the same tokens as a fresh one");
    geistr_chat_close(fresh);
    geistr_chat_close(c);

    /* ---- stop strings ---- */
    {
        const char *const stop[] = {"\n"};
        geistr_chat_opts  o      = GEISTR_CHAT_OPTS_INIT;
        o.max_tokens             = 60;
        o.stop                   = stop;
        o.n_stop                 = 1;
        geistr_chat   *sc        = nullptr;
        geistr_message q[]       = {SYSTEM, {"user", "List three fruits, one per line."}};
        CHECK(geistr_chat_open(m, &o, &sc) == GEISTR_OK && ask(sc, 2, q, again, sizeof again) == GEISTR_OK &&
                  !strchr(again, '\n'),
              "a stop string ends the answer before it");
        geistr_chat_close(sc);
    }

    /* ---- cancel during generation ---- */
    {
        geistr_chat   *cc  = chat(m, 2000, GEISTR_OVERFLOW_REFUSE);
        geistr_message q[] = {SYSTEM, {"user", "Write a long story about a lighthouse keeper, at least 1000 words."}};
        CHECK(geistr_chat_send(cc, 2, q) == GEISTR_OK, "long answer");
        /* Cancel from another thread once the answer streams: no timing guess
         * about how long the model would talk on this machine. */
        atomic_int          pieces = 0;
        struct cancel_after ca     = {cc, 0, &pieces, 0};
        pthread_t           th;
        pthread_create(&th, nullptr, cancel_later, &ca);
        geistr_piece  p = {.size = sizeof p};
        geistr_status s;
        while ((s = geistr_chat_next(cc, &p)) == GEISTR_OK && p.part != GEISTR_PART_END)
            atomic_fetch_add(&pieces, 1);
        const double done = now_ms();
        pthread_join(th, nullptr);
        printf("  cancel during generation: stopped %.0f ms after the cancel, %d pieces\n", done - ca.at,
               atomic_load(&pieces));
        CHECK(s == GEISTR_CANCELLED && done - ca.at < 1000, "cancelled promptly while generating");
        CHECK(stats(cc).finish == GEISTR_FINISH_CANCELLED, "finish CANCELLED");
        geistr_message next = {"user", "Say hi."};
        CHECK(ask(cc, 1, &next, again, sizeof again) == GEISTR_OK && again[0], "the chat continues after a cancel");

        /* ---- cancel during a long prefill ---- */
        size_t big = 14000; /* about 4k tokens: fits the window, takes a while to prefill */
        char  *text = malloc(big + 1);
        for (size_t i = 0; i < big; i++)
            text[i] = "lorem ipsum dolor sit amet "[i % 27];
        text[big]              = 0;
        geistr_chat   *pc      = chat(m, 8, GEISTR_OVERFLOW_REFUSE);
        geistr_message long_q  = {"user", text};
        struct cancel_after cb = {pc, 30, nullptr, 0};
        pthread_create(&th, nullptr, cancel_later, &cb);
        const double t_send = now_ms();
        s               = geistr_chat_send(pc, 1, &long_q);
        pthread_join(th, nullptr);
        const double took2 = now_ms() - t_send;
        printf("  long prefill (%zu chars): %s after %.0f ms\n", big, geistr_status_text(s), took2);
        CHECK(s == GEISTR_CANCELLED && took2 < 1500, "a long prefill is cancelled promptly");
        if (s == GEISTR_CANCELLED)
            CHECK(geistr_chat_length(pc) == 0, "a cancelled send leaves the conversation unchanged");
        CHECK(ask(pc, 1, &next, again, sizeof again) == GEISTR_OK && again[0], "usable after a cancelled prefill");
        free(text);
        geistr_chat_close(pc);
        geistr_chat_close(cc);
    }

    /* ---- overflow ---- */
    {
        geistr_model     *small = nullptr;
        geistr_model_opts mo    = GEISTR_MODEL_OPTS_INIT;
        mo.context              = 512;
        CHECK(geistr_model_open(path, &mo, &small, error, sizeof error) == GEISTR_OK, error);
        geistr_model_info si = {.size = sizeof si};
        CHECK(small && geistr_model_info_get(small, &si) == GEISTR_OK && si.context == 512, "explicit context kept");
        char pad[1400];
        for (size_t i = 0; i < sizeof pad - 1; i++)
            pad[i] = "tell me more about it "[i % 22];
        pad[sizeof pad - 1] = 0;
        geistr_message p1[] = {SYSTEM, {"user", pad}};
        geistr_message p2   = {"user", pad};
        geistr_chat   *rc   = chat(small, 16, GEISTR_OVERFLOW_REFUSE);
        CHECK(ask(rc, 2, p1, again, sizeof again) == GEISTR_OK, "a long first turn fits");
        CHECK(ask(rc, 1, &p2, again, sizeof again) == GEISTR_CONTEXT && geistr_chat_length(rc) == 3,
              "REFUSE: the second does not, the chat is unchanged");
        geistr_chat *dc = chat(small, 16, GEISTR_OVERFLOW_DROP_OLDEST);
        CHECK(ask(dc, 2, p1, again, sizeof again) == GEISTR_OK, "first turn");
        CHECK(ask(dc, 1, &p2, again, sizeof again) == GEISTR_OK && stats(dc).dropped_messages > 0 &&
                  stats(dc).context_tokens <= 512,
              "DROP_OLDEST: old turns go, the answer comes");
        geistr_chat_close(rc);
        geistr_chat_close(dc);
        geistr_model_close(small);
    }

    /* ---- two chats in parallel answer as one alone ---- */
    {
        struct parallel one = {m, ""}, pair[2] = {{m, ""}, {m, ""}};
        parallel_ask(&one);
        pthread_t th[2];
        for (int i = 0; i < 2; i++)
            pthread_create(&th[i], nullptr, parallel_ask, &pair[i]);
        for (int i = 0; i < 2; i++)
            pthread_join(th[i], nullptr);
        CHECK(one.answer[0] && !strcmp(pair[0].answer, one.answer) && !strcmp(pair[1].answer, one.answer),
              "parallel chats on one model answer as one alone");
    }

    /* ---- the chat outlives the caller's model handle ---- */
    geistr_chat *last = chat(m, 8, GEISTR_OVERFLOW_REFUSE);
    geistr_model_close(m);
    geistr_message hi = {"user", "Hello"};
    CHECK(ask(last, 1, &hi, again, sizeof again) == GEISTR_OK, "a chat keeps its model alive");
    geistr_chat_close(last);

    /* ---- a model the caller loaded itself (geistd): wrapped, borrowed ---- */
    {
        struct geist_backend *be = nullptr;
        struct geist_model   *gm = nullptr;
        CHECK(geist_backend_create("cpu_neon", nullptr, nullptr, &be) == GEIST_OK ||
                      geist_backend_create("cpu_x86", nullptr, nullptr, &be) == GEIST_OK ||
                      geist_backend_create("cpu_scalar", nullptr, nullptr, &be) == GEIST_OK,
              "engine backend");
        CHECK(be && geist_model_load(path, be, &gm) == GEIST_OK, "engine model");
        geistr_model     *w  = nullptr;
        geistr_model_opts wo = GEISTR_MODEL_OPTS_INIT;
        wo.context           = 512;
        CHECK(geistr_model_wrap(gm, be, &wo, &w, error, sizeof error) == GEISTR_OK, error);
        geistr_model_info wi = {.size = sizeof wi};
        CHECK(w && geistr_model_info_get(w, &wi) == GEISTR_OK && wi.context == 512 &&
                      !strcmp(wi.chat_format, info.chat_format),
              "wrapped: the caller's window, the same chat format");
        geistr_chat   *wc   = w ? chat(w, 16, GEISTR_OVERFLOW_REFUSE) : nullptr;
        geistr_message q2[] = {SYSTEM, {"user", "What is the capital of France?"}};
        CHECK(wc && ask(wc, 2, q2, again, sizeof again) == GEISTR_OK && strstr(again, "Paris"),
              "a chat on a wrapped model");
        geistr_model_close(w);
        geistr_chat_close(wc); /* releases the wrapper, never the engine model */
        /* the engine model is still the caller's: usable, then destroyed once */
        struct geist_session     *raw = nullptr;
        struct geist_session_opts ro  = {.max_seq_len = 16};
        CHECK(geist_session_create(gm, be, &ro, &raw) == GEIST_OK, "the caller's model outlives the wrapper");
        geist_session_destroy(raw);
        geist_model_destroy(gm);
        geist_backend_destroy(be);
        CHECK(geistr_model_wrap(nullptr, be, nullptr, &w, error, sizeof error) == GEISTR_INVALID && !w,
              "wrap needs a model");
    }

    /* ---- window choice: this model, and a large one if given ---- */
    window(path, "test model");
    const char *rec = getenv("GEIST_TEST_MODEL_RECURRENT");
    if (rec && access(rec, R_OK) == 0)
        recurrent(rec);
    const char *large = getenv("GEIST_TEST_MODEL_LARGE");
    if (large && access(large, R_OK) == 0)
        window(large, "large model");

    if (failures) {
        fprintf(stderr, "test_real: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_real: window, only-new-tokens, rewind (truncate and re-prefill), stop strings, cancel "
           "(generation, prefill), overflow, parallel chats, lifetime, wrapped engine model passed (%s, %s, context %u)\n",
           arch, backend, info.context);
    return 0;
}

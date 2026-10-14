/* Prefix-reuse controls and timings for the #165 evidence plan
 * (docs/DECISION_EVIDENCE_PLAN_165.json). White-box like bench_decision.c:
 * the runtime wrapper, the direct engine and fresh engine sessions share one
 * loaded model.
 *
 *   bench_decision_context prepare   MODEL PROFILE cpu|gpu              prompts, IDs and cuts; no inference
 *   bench_decision_context cell      MODEL PROFILE cpu|gpu T Q N P0     controls A/B/C, 5 warm-up + N pairs
 *   bench_decision_context overhead  MODEL PROFILE cpu|gpu              context batch (wrapper) vs direct prefix
 *   bench_decision_context lifecycle MODEL PROFILE cpu|gpu CYCLES       create-use-destroy, resources
 *
 * A: geistr_decision_score per question (independent full prompt, DENSE).
 * B: a fresh engine session: prefill(cached prefix), prefill(suffix), logits.
 * C: geistr_decision_context (snapshot restore + suffix).
 * One JSON object per line on stdout; times in milliseconds. A failed call is
 * a record with "ok":false, never an exit: failures are evidence. */
#include "../src/runtime.c"
#include "decision_context_source.h"
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

enum { WARMUP = 5, THREADS = 6, MODEL_CONTEXT = 1280, CAP_MARGIN = 192, NQ = 16, LIFE_T = 1024, REQUESTS = 100 };
static const size_t SNAPSHOT_BUDGET = (size_t)1 << 30;

#define REQUIRE(x)                                                                                           \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (false)

static uint64_t rss_bytes(void) {
#ifdef __APPLE__
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) == KERN_SUCCESS
               ? info.resident_size
               : 0;
#else
    unsigned long pages = 0, resident = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f && fscanf(f, "%lu %lu", &pages, &resident) != 2)
        resident = 0;
    if (f)
        fclose(f);
    return (uint64_t)resident * 4096u;
#endif
}

static void print_floats(const char *key, size_t n, const float *x) {
    printf(",\"%s\":[", key);
    for (size_t i = 0; i < n; ++i)
        printf("%s%.9g", i ? "," : "", (double)x[i]);
    printf("]");
}

static void print_ids(const char *key, size_t n, const int32_t *x) {
    printf(",\"%s\":[", key);
    for (size_t i = 0; i < n; ++i)
        printf("%s%d", i ? "," : "", x[i]);
    printf("]");
}

static void print_string(const char *key, const char *s) {
    printf(",\"%s\":\"", key);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (*p == '"' || *p == '\\')
            printf("\\%c", *p);
        else if (*p < 0x20)
            printf("\\u%04x", *p);
        else
            putchar(*p);
    putchar('"');
}

static geistr_model *open_model(const char *path, const char *profile, bool gpu, double *ms) {
    const bool gemma = !strcmp(profile, "gemma4");
    REQUIRE(gemma || !strcmp(profile, "bonsai2"));
    geistr_decision_policy p = {.enabled = true,
                                .mode = GEISTR_DECISION_DENSE,
                                .profile = gemma ? GEISTR_DECISION_GEMMA4_E2B_Q4 : GEISTR_DECISION_BONSAI2_27B_PQ2};
    memcpy(p.sha256, geistr_decision_profile_sha256(p.profile), sizeof p.sha256);
    geistr_model_opts mo = GEISTR_MODEL_OPTS_INIT;
    mo.decision = &p;
    mo.processor = gpu ? GEISTR_PROCESSOR_GPU : GEISTR_PROCESSOR_CPU;
    mo.context = MODEL_CONTEXT;
    mo.threads = THREADS;
    geistr_model *m = nullptr;
    char message[256];
    const double start = now_ms();
    if (geistr_model_open(path, &mo, &m, message, sizeof message) != GEISTR_OK) {
        fprintf(stderr, "open: %s\n", message);
        exit(1);
    }
    *ms = now_ms() - start;
    return m;
}

static void header(const char *task, const char *profile, geistr_model *m, double open_ms) {
    geistr_decision_capability cap = {.size = sizeof cap};
    REQUIRE(geistr_decision_capability_get(m, &cap) == GEISTR_OK && cap.available && cap.dense);
    REQUIRE(geistr_decision_context_available());
    double l[3] = {};
    getloadavg(l, 3);
    printf("{\"task\":\"%s\",\"profile\":\"%s\",\"backend\":\"%s\",\"engine\":\"%s\",\"engine_version\":\"%s\","
           "\"model_sha256\":\"%s\",\"threads\":%d,\"model_context\":%d,\"model_open_ms\":%.3f,\"rss_bytes\":%llu,"
           "\"load1\":%.2f}\n",
           task, profile, cap.backend, GEISTR_ENGINE_REVISION, cap.engine_version, m->decision.sha256, THREADS,
           MODEL_CONTEXT, open_ms, (unsigned long long)rss_bytes(), l[0]);
}

/* ---- the workload: context cut by word, questions ------------------------ */

static geistr_status src_tokenize(size_t *written, size_t cap, const char *text, int32_t *ids, void *p) {
    const enum geist_status es = geist_session_tokenize(p, text, cap, ids, written);
    return es == GEIST_E_TOO_MANY_TOKENS ? GEISTR_CONTEXT : from_engine(es);
}

/* The longest whole-word prefix of the source with at most t content tokens. */
static char *cut_context(geistr_model *m, size_t t, size_t *tokens) {
    struct geist_session *s = nullptr;
    const struct geist_session_opts to = {.max_seq_len = 1, .top_p = 1.0f};
    engine_setup_lock(m);
    REQUIRE(geist_session_create(m->m, m->be, &to, &s) == GEIST_OK);
    engine_setup_unlock(m);
    static int32_t ids[8192];
    const size_t n = sizeof context_source - 1;
    char *text = malloc(n + 1);
    REQUIRE(text);
    size_t best = 0, best_tokens = 0;
    for (size_t end = 1; end <= n; ++end) {
        if (end < n && context_source[end] != ' ')
            continue;
        memcpy(text, context_source, end);
        text[end] = 0;
        size_t k = 0;
        REQUIRE(src_tokenize(&k, sizeof ids / sizeof *ids, text, ids, s) == GEISTR_OK);
        if (k > t)
            break;
        best = end, best_tokens = k;
    }
    REQUIRE(best > 0);
    memcpy(text, context_source, best);
    text[best] = 0;
    *tokens = best_tokens;
    engine_setup_lock(m);
    geist_session_destroy(s);
    engine_setup_unlock(m);
    return text;
}

struct workload {
    char *context;
    size_t context_tokens, cap;
    geistr_decision_option options[NQ][4];
    geistr_decision_question q[NQ];
};

static void workload_init(struct workload *w, geistr_model *m, size_t t) {
    w->context = cut_context(m, t, &w->context_tokens);
    w->cap = t + CAP_MARGIN;
    for (size_t i = 0; i < NQ; ++i) {
        const struct source_question *s = &source_questions[i];
        for (size_t k = 0; k < 4; ++k)
            w->options[i][k] = (geistr_decision_option){1, strlen(s->options[k]), source_option_ids[k], s->options[k]};
        w->q[i] = (geistr_decision_question){strlen(s->id), strlen(s->question), 4, s->id, s->question, w->options[i]};
    }
}

static geistr_decision_batch batch_of(const struct workload *w, size_t first, size_t n) {
    return (geistr_decision_batch){sizeof(geistr_decision_batch), GEISTR_OPERATION_DECISION, n, w->q + first};
}

static geistr_decision_request request_of(const struct workload *w, size_t i) {
    return (geistr_decision_request){.size = sizeof(geistr_decision_request),
                                     .operation = GEISTR_OPERATION_DECISION,
                                     .question_len = w->q[i].question_len,
                                     .context_len = strlen(w->context),
                                     .n_options = 4,
                                     .question = w->q[i].question,
                                     .context = w->context,
                                     .options = w->options[i]};
}

static geistr_decision_context *context_open(geistr_model *m, const struct workload *w, double *ms,
                                             geistr_status *status, char *message, size_t cap) {
    geistr_decision_context_opts o = GEISTR_DECISION_CONTEXT_OPTS_INIT;
    o.mode = GEISTR_DECISION_DENSE;
    o.max_prompt_tokens = w->cap;
    o.max_snapshot_bytes = SNAPSHOT_BUDGET;
    geistr_decision_context *c = nullptr;
    const double start = now_ms();
    *status = geistr_decision_context_open(m, strlen(w->context), cap, w->context, &o, &c, message);
    *ms = now_ms() - start;
    return c;
}

static void print_info(const geistr_decision_context *c) {
    geistr_decision_context_info info = {.size = sizeof info};
    REQUIRE(geistr_decision_context_info_get(c, &info) == GEISTR_OK);
    printf(",\"info\":{\"cached_tokens\":%zu,\"snapshot_bytes\":%zu,\"max_snapshot_bytes\":%zu,\"prefill_chunk\":%zu,"
           "\"max_prompt_tokens\":%zu,\"setup_model_calls\":%zu,\"setup_ms\":%.6f,\"backend\":\"%s\",\"kv_cache\":\"%s\","
           "\"activation\":%s%s%s,\"model_sha256\":\"%s\",\"template_sha256\":\"%s\",\"context_sha256\":\"%s\"}",
           info.cached_tokens, info.snapshot_bytes, info.max_snapshot_bytes, info.prefill_chunk,
           info.max_prompt_tokens, info.setup_model_calls, info.setup_ms, info.backend, info.kv_cache,
           info.activation ? "\"" : "", info.activation ? info.activation : "null", info.activation ? "\"" : "",
           info.model_sha256, info.template_sha256, info.context_sha256);
}

/* ---- prepare: the exact prompts, IDs and cuts, without inference --------- */

static int prepare(const char *path, const char *profile, bool gpu) {
    double open_ms;
    geistr_model *m = open_model(path, profile, gpu, &open_ms);
    header("prepare", profile, m, open_ms);
    static const size_t sizes[] = {64, 256, 1024};
    for (size_t s = 0; s < 3; ++s) {
        struct workload w;
        workload_init(&w, m, sizes[s]);
        /* The context's own geometry: same opts as geistr_decision_context_open. */
        const struct geist_decision_opts so = {.mode = GEIST_DECISION_DENSE,
                                               .max_prompt_tokens = w.cap,
                                               .max_candidates = GEISTR_DECISION_MAX_OPTIONS,
                                               .kv_mode = GEIST_KV_FP32};
        struct geist_decision *d = nullptr;
        struct geist_session *tok = nullptr;
        const struct geist_session_opts to = {.max_seq_len = 1, .top_p = 1.0f};
        engine_setup_lock(m);
        REQUIRE(geist_decision_create(m->m, m->be, &so, &d) == GEIST_OK);
        REQUIRE(geist_session_create(m->m, m->be, &to, &tok) == GEIST_OK);
        engine_setup_unlock(m);
        size_t chunk = 0;
        REQUIRE(geist_decision_prefill_chunk(&chunk, d) == GEIST_OK && chunk);
        struct decision_batch b;
        REQUIRE(decision_batch_init(&b, m->decision.profile, w.cap, strlen(w.context), w.context) == GEISTR_OK);
        const struct decision_batch_engine e = {.context = tok, .tokenize = src_tokenize};
        REQUIRE(decision_batch_cut(&b, chunk, &e) == GEISTR_OK);
        size_t n_prefix = 0;
        REQUIRE(src_tokenize(&n_prefix, b.token_capacity, b.text, b.scratch, tok) == GEISTR_OK);
        printf("{\"task\":\"prepare\",\"t\":%zu,\"context_tokens\":%zu,\"cap\":%zu,\"prefill_chunk\":%zu,"
               "\"prefix_tokens\":%zu,\"cached_tokens\":%zu,\"context_sha256\":\"%s\"",
               sizes[s], w.context_tokens, w.cap, chunk, n_prefix, b.n_cached, b.context_sha256);
        print_string("context", w.context);
        print_ids("cached_ids", b.n_cached, b.cached);
        puts("}");
        for (size_t i = 0; i < NQ; ++i) {
            const geistr_decision_request r = request_of(&w, i);
            size_t written = 0, n_prompt = 0, n_suffix = 0;
            int32_t cand[GEISTR_DECISION_MAX_OPTIONS];
            REQUIRE(decision_render(&written, DECISION_PROMPT_CAP, m->decision.profile, &r, b.text) == GEISTR_OK);
            printf("{\"task\":\"prepare\",\"t\":%zu,\"question\":\"%s\"", sizes[s], source_questions[i].id);
            print_string("prompt", b.text);
            REQUIRE(decision_map(&n_prompt, b.token_capacity, 4, DECISION_PROMPT_CAP, src_tokenize, b.text, b.prompts,
                                 b.scratch, cand, tok) == GEISTR_OK);
            REQUIRE(decision_suffix(&n_suffix, b.n_cached, b.cached, n_prompt, b.prompts) == GEISTR_OK);
            printf(",\"prompt_tokens\":%zu,\"suffix_tokens\":%zu,\"fits\":%s", n_prompt, n_suffix,
                   n_prompt <= w.cap ? "true" : "false");
            print_ids("ids", n_prompt, b.prompts);
            print_ids("candidate_ids", 4, cand);
            puts("}");
        }
        decision_batch_free(&b);
        engine_setup_lock(m);
        geist_decision_destroy(d);
        geist_session_destroy(tok);
        engine_setup_unlock(m);
        free(w.context);
    }
    geistr_model_close(m);
    return 0;
}

/* ---- controls -------------------------------------------------------------- */

/* B: a fresh session prefills the cached prefix, then the suffix. */
static bool fresh_split(geistr_model *m, size_t cap, size_t chunk, size_t n_cached, const int32_t *prompt,
                        size_t n_prompt, const int32_t *cand, float *out, double *ms) {
    const struct geist_session_opts so = {.max_seq_len = cap, .kv_mode = GEIST_KV_FP32, .m_max = chunk, .top_p = 1.0f};
    struct geist_session *s = nullptr;
    const double start = now_ms();
    engine_setup_lock(m);
    bool ok = geist_session_create(m->m, m->be, &so, &s) == GEIST_OK;
    engine_setup_unlock(m);
    engine_lock(m);
    if (ok && n_cached)
        ok = geist_session_prefill_tokens(s, n_cached, prompt) == GEIST_OK;
    if (ok)
        ok = geist_session_prefill_tokens(s, n_prompt - n_cached, prompt + n_cached) == GEIST_OK;
    size_t nl = 0;
    const float *logits = ok ? geist_session_peek_logits(&nl, s) : nullptr;
    ok = logits != nullptr;
    for (size_t k = 0; ok && k < 4; ++k) {
        ok = cand[k] >= 0 && (size_t)cand[k] < nl;
        if (ok)
            out[k] = logits[cand[k]];
    }
    engine_unlock(m);
    engine_setup_lock(m);
    geist_session_destroy(s);
    engine_setup_unlock(m);
    *ms = now_ms() - start;
    return ok;
}

static size_t argmax(const float *x) {
    size_t best = 0;
    for (size_t k = 1; k < 4; ++k)
        if (x[k] > x[best])
            best = k;
    return best;
}

/* Logits of the reference batch (C), per question. */
static float ref_c[NQ][4];

/* A, B and C on every question of the batch, once per process (untimed). */
static void controls(geistr_model *m, geistr_decision *a, const struct workload *w, size_t q) {
    char message[256];
    geistr_status s;
    double open_ms;
    geistr_decision_context *c = context_open(m, w, &open_ms, &s, message, sizeof message);
    printf("{\"control\":\"open\",\"ok\":%s,\"status\":%d,\"open_ms\":%.6f", s == GEISTR_OK ? "true" : "false", (int)s,
           open_ms);
    if (s != GEISTR_OK) {
        print_string("error", message);
        puts("}");
        return;
    }
    print_info(c);
    puts("}");
    const geistr_decision_batch batch = batch_of(w, 0, q);
    geistr_decision_batch_result r = {.size = sizeof r};
    s = geistr_decision_context_score_batch(c, &batch, &r);
    if (s != GEISTR_OK) {
        printf("{\"control\":\"C\",\"ok\":false,\"status\":%d", (int)s);
        print_string("error", geistr_decision_context_error(c));
        puts("}");
        geistr_decision_context_close(c);
        return;
    }
    for (size_t i = 0; i < q; ++i) {
        const geistr_decision_answer *ans = &r.answers[i];
        memcpy(ref_c[i], ans->logits, sizeof ref_c[i]);
        const int32_t *prompt = c->batch.prompts + i * c->batch.token_capacity;
        const size_t n_prompt = c->batch.n_prompt[i];
        float b[4] = {}, alog[4] = {};
        double b_ms = 0;
        const bool b_ok = fresh_split(m, w->cap, c->info.prefill_chunk, c->batch.n_cached, prompt, n_prompt,
                                      c->batch.candidates[i], b, &b_ms);
        const geistr_decision_request req = request_of(w, i);
        geistr_decision_result out = {.size = sizeof out};
        const geistr_status as = geistr_decision_score(a, &req, &out);
        geistr_decision_plan plan = {.size = sizeof plan};
        bool same_ids = false;
        if (as == GEISTR_OK) {
            memcpy(alog, out.logits, sizeof alog);
            same_ids = geistr_decision_plan_get(a, &plan) == GEISTR_OK && plan.n_prompt == n_prompt &&
                       !memcmp(plan.prompt_ids, prompt, n_prompt * sizeof *prompt) &&
                       !memcmp(plan.candidate_ids, c->batch.candidates[i], 4 * sizeof(int32_t));
        }
        double max_ac = 0;
        for (size_t k = 0; k < 4; ++k)
            max_ac = fmax(max_ac, fabs((double)alog[k] - (double)ans->logits[k]));
        printf("{\"control\":\"ABC\",\"question\":\"%s\",\"prompt_tokens\":%zu,\"uncached_tokens\":%zu,"
               "\"b_ok\":%s,\"a_ok\":%s,\"a_status\":%d,\"a_same_ids\":%s,\"c_vs_b_bit_identical\":%s,"
               "\"c_vs_a_bit_identical\":%s,\"c_vs_a_max_abs\":%.9g,\"best_c\":%zu,\"best_b\":%zu,\"best_a\":%zu,"
               "\"b_ms\":%.6f",
               ans->question_id, ans->prompt_tokens, ans->uncached_tokens, b_ok ? "true" : "false",
               as == GEISTR_OK ? "true" : "false", (int)as, same_ids ? "true" : "false",
               b_ok && !memcmp(b, ans->logits, sizeof b) ? "true" : "false",
               as == GEISTR_OK && !memcmp(alog, ans->logits, sizeof alog) ? "true" : "false", max_ac, ans->best_index,
               argmax(b), as == GEISTR_OK ? out.best_index : SIZE_MAX, b_ms);
        print_floats("c", 4, ans->logits);
        print_floats("b", 4, b);
        print_floats("a", 4, alog);
        char sha[65];
        REQUIRE(geistr_sha256_memory(n_prompt * sizeof *prompt, prompt, sha) == GEISTR_OK);
        printf(",\"ids_sha256\":\"%s\"", sha);
        print_ids("candidate_ids", 4, c->batch.candidates[i]);
        puts("}");
    }
    geistr_decision_context_close(c);
}

/* ---- cell: reuse (C) vs independent (A), AB/BA pairs ---------------------- */

static void print_batch(const char *name, geistr_status s, const geistr_decision_batch_result *r, double wall,
                        const geistr_decision_context *c) {
    printf(",\"%s\":{\"ok\":%s,\"status\":%d,\"wall_ms\":%.6f,\"preparation_ms\":%.6f,\"scoring_ms\":%.6f,"
           "\"batch_ms\":%.6f,\"model_calls\":%zu,\"n_answers\":%zu,\"same_as_ref\":",
           name, s == GEISTR_OK ? "true" : "false", (int)s, wall, r->preparation_ms, r->scoring_ms, r->batch_ms,
           r->model_calls, r->n_answers);
    bool same = s == GEISTR_OK;
    for (size_t i = 0; same && i < r->n_answers; ++i)
        same = !memcmp(r->answers[i].logits, ref_c[i], sizeof ref_c[i]);
    printf("%s", same ? "true" : "false");
    if (s != GEISTR_OK)
        print_string("error", c ? geistr_decision_context_error(c) : "");
    printf(",\"logits\":[");
    for (size_t i = 0; i < r->n_answers; ++i) {
        printf("%s[", i ? "," : "");
        for (size_t k = 0; k < 4; ++k)
            printf("%s%.9g", k ? "," : "", (double)r->answers[i].logits[k]);
        printf("]");
    }
    printf("]}");
}

static void reuse_side(geistr_model *m, const struct workload *w, size_t q) {
    char message[256];
    geistr_status s;
    double open_ms;
    const double start = now_ms();
    geistr_decision_context *c = context_open(m, w, &open_ms, &s, message, sizeof message);
    printf(",\"reuse\":{\"open\":{\"ok\":%s,\"status\":%d,\"wall_ms\":%.6f", s == GEISTR_OK ? "true" : "false", (int)s,
           open_ms);
    if (s != GEISTR_OK) {
        print_string("error", message);
        printf("},\"first_package_ms\":%.6f,\"failed\":true}", now_ms() - start);
        return;
    }
    geistr_decision_context_info info = {.size = sizeof info};
    REQUIRE(geistr_decision_context_info_get(c, &info) == GEISTR_OK);
    printf(",\"setup_ms\":%.6f,\"cached_tokens\":%zu,\"snapshot_bytes\":%zu}", info.setup_ms, info.cached_tokens,
           info.snapshot_bytes);
    const geistr_decision_batch batch = batch_of(w, 0, q);
    geistr_decision_batch_result r1 = {.size = sizeof r1}, r2 = {.size = sizeof r2};
    double t = now_ms();
    const geistr_status s1 = geistr_decision_context_score_batch(c, &batch, &r1);
    const double first = now_ms() - t;
    const double first_package = now_ms() - start;
    print_batch("first", s1, &r1, first, c);
    t = now_ms();
    const geistr_status s2 = geistr_decision_context_score_batch(c, &batch, &r2);
    const double repeat = now_ms() - t;
    print_batch("repeat", s2, &r2, repeat, c);
    t = now_ms();
    geistr_decision_context_close(c);
    const double close_ms = now_ms() - t;
    printf(",\"close_ms\":%.6f,\"first_package_ms\":%.6f,\"repeat_package_ms\":%.6f,\"failed\":%s}", close_ms,
           first_package, repeat, s1 == GEISTR_OK && s2 == GEISTR_OK ? "false" : "true");
}

static void independent_side(geistr_decision *a, const struct workload *w, size_t q) {
    double total = 0, prep = 0, scoring = 0;
    bool failed = false;
    printf(",\"independent\":{\"questions\":[");
    for (size_t i = 0; i < q; ++i) {
        const geistr_decision_request req = request_of(w, i);
        geistr_decision_result out = {.size = sizeof out};
        const double t = now_ms();
        const geistr_status s = geistr_decision_score(a, &req, &out);
        const double ms = now_ms() - t;
        total += ms, prep += out.preparation_ms, scoring += out.scoring_ms;
        failed |= s != GEISTR_OK;
        printf("%s{\"ok\":%s,\"status\":%d,\"wall_ms\":%.6f,\"preparation_ms\":%.6f,\"scoring_ms\":%.6f,"
               "\"model_calls\":%zu",
               i ? "," : "", s == GEISTR_OK ? "true" : "false", (int)s, ms, out.preparation_ms, out.scoring_ms,
               out.model_calls);
        if (s == GEISTR_OK)
            print_floats("logits", 4, out.logits);
        else
            print_string("error", geistr_decision_error(a));
        printf("}");
    }
    printf("],\"package_ms\":%.6f,\"preparation_ms\":%.6f,\"scoring_ms\":%.6f,\"failed\":%s}", total, prep, scoring,
           failed ? "true" : "false");
}

static int cell(const char *path, const char *profile, bool gpu, size_t t, size_t q, int n, int p0) {
    REQUIRE(q >= 1 && q <= NQ && n >= 0 && p0 >= 0);
    double open_ms;
    geistr_model *m = open_model(path, profile, gpu, &open_ms);
    header("cell", profile, m, open_ms);
    struct workload w;
    workload_init(&w, m, t);
    printf("{\"cell\":{\"t\":%zu,\"q\":%zu,\"context_tokens\":%zu,\"cap\":%zu,\"warmup\":%d,\"pairs\":%d,"
           "\"first_pair\":%d,\"snapshot_budget\":%zu}}\n",
           t, q, w.context_tokens, w.cap, WARMUP, n, p0, SNAPSHOT_BUDGET);
    geistr_decision_opts opts = GEISTR_DECISION_OPTS_INIT;
    opts.mode = GEISTR_DECISION_DENSE;
    opts.max_prompt_tokens = w.cap;
    geistr_decision *a = nullptr;
    char message[256];
    REQUIRE(geistr_decision_open(m, sizeof message, &opts, &a, message) == GEISTR_OK);
    controls(m, a, &w, q);
    for (int i = 0; i < WARMUP + n; ++i) {
        const bool warm = i < WARMUP;
        const int pair = warm ? i : p0 + i - WARMUP;
        const bool reuse_first = pair % 2 == 0;
        double l[3] = {};
        getloadavg(l, 3);
        printf("{\"pair\":%d,\"warmup\":%s,\"order\":\"%s\",\"load1\":%.2f", pair, warm ? "true" : "false",
               reuse_first ? "RI" : "IR", l[0]);
        if (reuse_first)
            reuse_side(m, &w, q), independent_side(a, &w, q);
        else
            independent_side(a, &w, q), reuse_side(m, &w, q);
        printf(",\"rss_bytes\":%llu}\n", (unsigned long long)rss_bytes());
        fflush(stdout);
    }
    geistr_decision_close(a);
    free(w.context);
    geistr_model_close(m);
    return 0;
}

/* ---- overhead: wrapper batch vs direct engine prefix call ----------------- */

static int overhead(const char *path, const char *profile, bool gpu) {
    enum { T = 256, PAIRS = 30 };
    double open_ms;
    geistr_model *m = open_model(path, profile, gpu, &open_ms);
    header("overhead", profile, m, open_ms);
    struct workload w;
    workload_init(&w, m, T);
    char message[256];
    geistr_status s;
    double ms;
    geistr_decision_context *c = context_open(m, &w, &ms, &s, message, sizeof message);
    if (s != GEISTR_OK) {
        printf("{\"setup\":{\"ok\":false,\"status\":%d", (int)s);
        print_string("error", message);
        puts("}}");
        return 0;
    }
    /* Every question's IDs from one complete batch (preflight is preparation). */
    const geistr_decision_batch all = batch_of(&w, 0, NQ);
    geistr_decision_batch_result r = {.size = sizeof r};
    REQUIRE(geistr_decision_context_score_batch(c, &all, &r) == GEISTR_OK);
    static int32_t ids[NQ][2048];
    size_t n_prompt[NQ];
    int32_t cand[NQ][4];
    const size_t n_cached = c->batch.n_cached;
    for (size_t i = 0; i < NQ; ++i) {
        n_prompt[i] = c->batch.n_prompt[i];
        REQUIRE(n_prompt[i] <= 2048);
        memcpy(ids[i], c->batch.prompts + i * c->batch.token_capacity, n_prompt[i] * sizeof(int32_t));
        memcpy(cand[i], c->batch.candidates[i], sizeof cand[i]);
        memcpy(ref_c[i], r.answers[i].logits, sizeof ref_c[i]);
    }
    const struct geist_decision_opts so = {.mode = GEIST_DECISION_DENSE,
                                           .max_prompt_tokens = w.cap,
                                           .max_candidates = GEISTR_DECISION_MAX_OPTIONS,
                                           .kv_mode = GEIST_KV_FP32};
    struct geist_decision *d = nullptr;
    struct geist_decision_prefix *p = nullptr;
    double t = now_ms();
    engine_setup_lock(m);
    REQUIRE(geist_decision_create(m->m, m->be, &so, &d) == GEIST_OK);
    engine_setup_unlock(m);
    engine_lock(m);
    REQUIRE(geist_decision_prefix_prepare(d, SNAPSHOT_BUDGET, n_cached, ids[0], &p) == GEIST_OK);
    engine_unlock(m);
    const double direct_open = now_ms() - t;
    geistr_decision_context_info info = {.size = sizeof info};
    REQUIRE(geistr_decision_context_info_get(c, &info) == GEISTR_OK);
    printf("{\"setup\":{\"ok\":true,\"t\":%d,\"context_tokens\":%zu,\"cap\":%zu,\"cached_tokens\":%zu,"
           "\"wrapper_open_ms\":%.3f,\"direct_open_ms\":%.3f}}\n",
           T, w.context_tokens, w.cap, n_cached, ms, direct_open);
    float logits[4];
    double probs[4];
    for (int i = 0; i < WARMUP + PAIRS; ++i) {
        const size_t k = (size_t)i % NQ;
        const bool ab = i % 2 == 0;
        double a_ms = 0, b_ms = 0, prep = 0, batch_ms = 0;
        bool a_ok = false, b_ok = false, a_same = false, b_same = false;
        for (int side = 0; side < 2; ++side) {
            if ((side == 0) == ab) { /* A: the runtime context batch, one question */
                const geistr_decision_batch one = batch_of(&w, k, 1);
                geistr_decision_batch_result o = {.size = sizeof o};
                a_ok = geistr_decision_context_score_batch(c, &one, &o) == GEISTR_OK;
                a_ms = o.scoring_ms, prep = o.preparation_ms, batch_ms = o.batch_ms;
                a_same = a_ok && !memcmp(o.answers[0].logits, ref_c[k], sizeof ref_c[k]);
            } else { /* B: the direct engine, bounded as the runtime's scoring phase */
                struct geist_decision_result dr = {};
                const double start = now_ms();
                engine_lock(m);
                const enum geist_status es = geist_decision_score_from_prefix(
                    d, p, n_prompt[k] - n_cached, 4, ids[k] + n_cached, cand[k], &dr);
                for (size_t j = 0; es == GEIST_OK && j < 4; ++j)
                    logits[j] = dr.logits[j], probs[j] = dr.probabilities[j];
                engine_unlock(m);
                b_ms = now_ms() - start;
                b_ok = es == GEIST_OK;
                b_same = b_ok && !memcmp(logits, ref_c[k], sizeof logits);
            }
        }
        printf("{\"pair\":%d,\"warmup\":%s,\"question\":\"%s\",\"order\":\"%s\",\"a_ok\":%s,\"b_ok\":%s,"
               "\"a_bit_identical\":%s,\"b_bit_identical\":%s,\"a_scoring_ms\":%.6f,\"b_scoring_ms\":%.6f,"
               "\"a_preparation_ms\":%.6f,\"a_batch_ms\":%.6f,\"b_probability0\":%.9g}\n",
               i, i < WARMUP ? "true" : "false", source_questions[k].id, ab ? "AB" : "BA", a_ok ? "true" : "false",
               b_ok ? "true" : "false", a_same ? "true" : "false", b_same ? "true" : "false", a_ms, b_ms, prep,
               batch_ms, probs[0]);
        fflush(stdout);
    }
    engine_setup_lock(m);
    geist_decision_prefix_destroy(p);
    geist_decision_destroy(d);
    engine_setup_unlock(m);
    geistr_decision_context_close(c);
    free(w.context);
    geistr_model_close(m);
    return 0;
}

/* ---- lifecycle --------------------------------------------------------------- */

static void resources(const char *phase, int cycle, int index, geistr_model *m) {
    geistr_decision_resources r = {.size = sizeof r};
    const geistr_status s = m ? geistr_decision_resources_get(m, &r) : GEISTR_INVALID;
    printf("{\"cycle\":%d,\"index\":%d,\"phase\":\"%s\",\"rss_bytes\":%llu,\"resources_ok\":%s,"
           "\"live_allocations\":%zu,\"live_bytes\":%zu,\"provider_known\":%s,\"provider_allocated_bytes\":%llu",
           cycle, index, phase, (unsigned long long)rss_bytes(), s == GEISTR_OK ? "true" : "false",
           r.live_allocations, r.live_bytes, r.provider_known ? "true" : "false",
           (unsigned long long)r.provider_allocated_bytes);
}

static bool chat_turn(geistr_chat *chat) {
    const geistr_message input = {"user", "Reply OK."};
    if (geistr_chat_send(chat, 1, &input) != GEISTR_OK)
        return false;
    geistr_piece piece = {.size = sizeof piece};
    geistr_status s;
    while ((s = geistr_chat_next(chat, &piece)) == GEISTR_OK && piece.part != GEISTR_PART_END)
        ;
    return s == GEISTR_OK && geistr_chat_rewind(chat, 0) == GEISTR_OK;
}

/* One measured batch: question index % 16 alone, against the warm reference. */
static void measured(geistr_decision_context *c, const struct workload *w, int cycle, int index, geistr_model *m) {
    const size_t k = (size_t)index % NQ;
    const geistr_decision_batch one = batch_of(w, k, 1);
    geistr_decision_batch_result r = {.size = sizeof r};
    const geistr_status s = geistr_decision_context_score_batch(c, &one, &r);
    resources("batch", cycle, index, m);
    printf(",\"ok\":%s,\"status\":%d,\"question\":\"%s\",\"batch_ms\":%.6f,\"model_calls\":%zu,\"same_as_warm\":%s}\n",
           s == GEISTR_OK ? "true" : "false", (int)s, source_questions[k].id, r.batch_ms, r.model_calls,
           s == GEISTR_OK && !memcmp(r.answers[0].logits, ref_c[k], sizeof ref_c[k]) ? "true" : "false");
}

static int lifecycle(const char *path, const char *profile, bool gpu, int cycles) {
    for (int cycle = 0; cycle < cycles; ++cycle) {
        resources("process_before_model", cycle, -1, nullptr);
        puts("}");
        double open_ms;
        geistr_model *m = open_model(path, profile, gpu, &open_ms);
        header("lifecycle", profile, m, open_ms);
        resources("model_loaded", cycle, -1, m);
        puts("}");
        struct workload w;
        workload_init(&w, m, LIFE_T);
        geistr_chat_opts co = GEISTR_CHAT_OPTS_INIT;
        co.max_tokens = 1;
        geistr_chat *chat = nullptr;
        REQUIRE(geistr_chat_open(m, &co, &chat) == GEISTR_OK);
        char message[256];
        geistr_status s;
        double ms;
        resources("before_context", cycle, -1, m);
        puts("}");
        geistr_decision_context *c = context_open(m, &w, &ms, &s, message, sizeof message);
        resources("context_open", cycle, -1, m);
        printf(",\"ok\":%s,\"status\":%d,\"open_ms\":%.6f", s == GEISTR_OK ? "true" : "false", (int)s, ms);
        if (s != GEISTR_OK) {
            print_string("error", message);
            puts("}");
            geistr_chat_close(chat);
            geistr_model_close(m);
            return 0;
        }
        print_info(c);
        puts("}");
        /* Warm at the largest geometry: every question in one batch. */
        const geistr_decision_batch warm = batch_of(&w, 0, NQ);
        geistr_decision_batch_result r = {.size = sizeof r};
        s = geistr_decision_context_score_batch(c, &warm, &r);
        for (size_t i = 0; s == GEISTR_OK && i < NQ; ++i)
            memcpy(ref_c[i], r.answers[i].logits, sizeof ref_c[i]);
        resources("warm", cycle, -1, m);
        printf(",\"ok\":%s,\"status\":%d,\"batch_ms\":%.6f,\"model_calls\":%zu}\n", s == GEISTR_OK ? "true" : "false",
               (int)s, r.batch_ms, r.model_calls);
        /* A second context on the same weights: no second copy of them. */
        const uint64_t before_second = rss_bytes();
        geistr_decision_context *c2 = context_open(m, &w, &ms, &s, message, sizeof message);
        geistr_decision_batch_result r2 = {.size = sizeof r2};
        const geistr_decision_batch first = batch_of(&w, 0, 1);
        const bool second_ok = s == GEISTR_OK && geistr_decision_context_score_batch(c2, &first, &r2) == GEISTR_OK &&
                               !memcmp(r2.answers[0].logits, ref_c[0], sizeof ref_c[0]);
        resources("second_context", cycle, -1, m);
        printf(",\"ok\":%s,\"rss_delta_bytes\":%lld}\n", second_ok ? "true" : "false",
               (long long)rss_bytes() - (long long)before_second);
        geistr_decision_context_close(c2);
        for (int index = 0; index < REQUESTS; ++index) {
            if (index % 10 == 0) {
                /* chat, an invalid batch (duplicate IDs), a cancelled batch */
                const bool chatted = chat_turn(chat);
                geistr_decision_question dup[2] = {w.q[0], w.q[0]};
                const geistr_decision_batch bad = {sizeof bad, GEISTR_OPERATION_DECISION, 2, dup};
                geistr_decision_batch_result x = {.size = sizeof x};
                const geistr_status invalid = geistr_decision_context_score_batch(c, &bad, &x);
                const size_t invalid_answers = x.n_answers, invalid_index = x.failed_index;
                REQUIRE(geistr_decision_context_cancel(c) == GEISTR_OK);
                const geistr_status cancelled = geistr_decision_context_score_batch(c, &first, &x);
                resources("mix", cycle, index, m);
                printf(",\"chat_ok\":%s,\"invalid_status\":%d,\"invalid_answers\":%zu,\"invalid_index\":%zu,"
                       "\"cancel_status\":%d,\"cancel_answers\":%zu}\n",
                       chatted ? "true" : "false", (int)invalid, invalid_answers, invalid_index, (int)cancelled,
                       x.n_answers);
            }
            if (index == REQUESTS / 2) { /* reset: the context is destroyed and created again */
                geistr_decision_context_close(c);
                resources("context_reset_closed", cycle, index, m);
                puts("}");
                c = context_open(m, &w, &ms, &s, message, sizeof message);
                resources("context_reset_open", cycle, index, m);
                printf(",\"ok\":%s,\"status\":%d,\"open_ms\":%.6f}\n", s == GEISTR_OK ? "true" : "false", (int)s,
                       ms);
                if (s != GEISTR_OK)
                    break;
            }
            measured(c, &w, cycle, index, m);
            fflush(stdout);
        }
        geistr_decision_context_close(c);
        resources("context_closed", cycle, -1, m);
        puts("}");
        geistr_chat_close(chat);
        free(w.context);
        resources("before_model_close", cycle, -1, m);
        puts("}");
        geistr_model_close(m);
        resources("model_closed", cycle, -1, nullptr);
        puts("}");
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 5)
        return 2;
    const bool gpu = !strcmp(argv[4], "gpu");
    REQUIRE(gpu || !strcmp(argv[4], "cpu"));
    if (!strcmp(argv[1], "prepare"))
        return prepare(argv[2], argv[3], gpu);
    if (!strcmp(argv[1], "overhead"))
        return overhead(argv[2], argv[3], gpu);
    if (!strcmp(argv[1], "lifecycle") && argc == 6)
        return lifecycle(argv[2], argv[3], gpu, atoi(argv[5]));
    if (!strcmp(argv[1], "cell") && argc == 9)
        return cell(argv[2], argv[3], gpu, (size_t)atoi(argv[5]), (size_t)atoi(argv[6]), atoi(argv[7]),
                    atoi(argv[8]));
    return 2;
}

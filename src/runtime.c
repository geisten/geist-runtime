/*
 * runtime.c — include/geistr.h on geistlib (#4).
 *
 * Model: plan the GGUF (geist_model_plan), choose the window, load once.
 * Chat: one geist_session holding the conversation. A send renders and
 * prefills only its new messages (template.c), after the previous answer's
 * turn close; next decodes token by token through the text stages
 * (stream.c). Every message records where it starts in the session, so a
 * rewind truncates there (geist_session_truncate); where the engine cannot
 * (recurrent layers) or the position is not known, the kept messages are
 * rendered and prefilled again.
 */
#include "geistr.h"
#include "common.h"
#include "geistr_engine.h"
#include "stream.h"
#include "template.h"
#include "window.h"

#include <geist.h>
#include <geist_util.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

/* Prefill runs in chunks so a cancel lands between them. The chunk follows
 * the measured speed, aiming at PREFILL_SLICE_MS per engine call: a fast GPU
 * gets large chunks (no measurable cost), a slow CPU at a long context small
 * ones (cancel stays prompt). geistlib has no cancel inside a prefill yet
 * (geistlib#628); with one the chunks can go. */
#define PREFILL_SLICE_MS 200.0
#define PREFILL_FIRST 32u /* small: the first measurement comes quickly on any device */
#define PREFILL_MIN 32u
#define PREFILL_MAX 4096u
#define MAX_STOPS 8u
#define NO_POSITION SIZE_MAX

const char *geistr_version(void) {
    return "0.1.1";
}

static void put_error(char *error, size_t cap, const char *fmt, ...) {
    if (error == nullptr || cap == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error, cap, fmt, ap);
    va_end(ap);
}

static geistr_status from_engine(enum geist_status s) {
    switch (s) {
    case GEIST_OK:
        return GEISTR_OK;
    case GEIST_E_OOM:
        return GEISTR_NO_MEMORY;
    case GEIST_E_IO:
    case GEIST_E_NOT_FOUND:
        return GEISTR_IO;
    case GEIST_E_FORMAT:
    case GEIST_E_UNSUPPORTED:
        return GEISTR_FORMAT;
    case GEIST_E_INVALID_ARG:
        return GEISTR_INVALID;
    default:
        return GEISTR_BACKEND;
    }
}

/* Physical memory; 0 if unknown. */
static uint64_t physical_memory(void) {
#if defined(__APPLE__)
    uint64_t bytes = 0;
    size_t   len   = sizeof bytes;
    return sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) == 0 ? bytes : 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    return pages > 0 && page > 0 ? (uint64_t) pages * (uint64_t) page : 0;
#endif
}

/* ------------------------------------------------------------------------ */
/* Model                                                                     */
/* ------------------------------------------------------------------------ */

struct geistr_model {
    atomic_int            refs;
    struct geist_backend *be;
    struct geist_model   *m;
    enum tpl_family       family;
    int32_t               stops[MAX_STOPS];
    size_t                n_stops;
    geist_token_t         bos;
    bool                  add_bos;
    uint32_t              context;
    /* GPU backends run one engine call at a time on a model (geistlib#576);
     * CPU sessions run in parallel. */
    bool            serialize;
    pthread_mutex_t engine;
    bool            borrowed; /* geistr_model_wrap: the caller owns m and be */
    char            error[256];
};

static void engine_lock(geistr_model *m) {
    if (m->serialize)
        pthread_mutex_lock(&m->engine);
}
static void engine_unlock(geistr_model *m) {
    if (m->serialize)
        pthread_mutex_unlock(&m->engine);
}

static struct geist_backend *backend_create(bool gpu, uint32_t threads) {
    static const char *const cpu[] = {"cpu_neon", "cpu_x86", "cpu_scalar"};
    static const char *const gpus[] = {"metal", "vulkan"};
    const char *const       *names  = gpu ? gpus : cpu;
    const size_t             n      = gpu ? 2 : 3;
    struct geist_backend_opts o     = {.max_threads = (int) threads};
    for (size_t i = 0; i < n; i++) {
        struct geist_backend *be = nullptr;
        if (geist_backend_create(names[i], &o, nullptr, &be) == GEIST_OK && be != nullptr)
            return be;
    }
    return nullptr;
}

static int32_t token_lookup(void *m, const char *text) {
    const geist_token_t t = geist_model_token_by_text(m, text);
    return t == GEIST_TOKEN_NONE ? -1 : t;
}

/* The window (D8, window.h) with three quarters of physical memory as the
 * budget. */
static geistr_status choose_window(const struct geist_model_plan *plan,
                                   uint32_t                       wanted,
                                   uint32_t                      *out,
                                   char                          *error,
                                   size_t                         cap) {
    const uint64_t per = (uint64_t) plan->kv_bytes_per_token + plan->model_bytes_per_token;
    *out = window_choose(plan->context_length, wanted, plan->weight_bytes, per, physical_memory() / 4 * 3);
    if (*out == 0) {
        put_error(error, cap, "the model does not fit into memory with a %u-token window", WINDOW_MIN);
        return GEISTR_NO_MEMORY;
    }
    return GEISTR_OK;
}

/* The model's chat format, stops and engine rules, from the loaded engine. */
static void model_describe(geistr_model *m, struct geist_model *gm, struct geist_backend *be, uint32_t window,
                           enum tpl_family forced) {
    atomic_init(&m->refs, 1);
    m->be      = be;
    m->m       = gm;
    m->context = window;
    m->family  = forced != TPL_UNKNOWN
                         ? forced
                         : tpl_family_detect(geist_model_metadata_str(gm, "tokenizer.chat_template", nullptr),
                                             geist_model_arch(gm));
    m->n_stops   = tpl_stop_ids(token_lookup, gm, geist_model_eos_token(gm), MAX_STOPS, m->stops);
    m->bos       = geist_model_bos_token(gm);
    m->add_bos   = geist_model_add_bos(gm);
    const char *name = geist_backend_name(be);
    m->serialize = name && (!strcmp(name, "metal") || !strcmp(name, "vulkan"));
    pthread_mutex_init(&m->engine, nullptr);
}

/* The caller's options over the defaults, and the chat format they force. */
static bool model_opts(const geistr_model_opts *opts, geistr_model_opts *o, enum tpl_family *forced, char *error,
                       size_t cap) {
    if (!opts_copy(o, opts, sizeof *o)) {
        put_error(error, cap, "model options: unknown size");
        return false;
    }
    if (o->chat_format && (*forced = tpl_family_from_name(o->chat_format)) == TPL_UNKNOWN) {
        put_error(error, cap, "unknown chat_format override \"%s\"", o->chat_format);
        return false;
    }
    return true;
}

static geistr_status model_open(const char              *path,
                                const void              *data,
                                size_t                   len,
                                const geistr_model_opts *opts,
                                geistr_model           **out,
                                char                    *error,
                                size_t                   cap) {
    geistr_model_opts o = GEISTR_MODEL_OPTS_INIT;
    enum tpl_family forced = TPL_UNKNOWN;
    if (!model_opts(opts, &o, &forced, error, cap))
        return GEISTR_INVALID;
    if (path != nullptr) {
        FILE *f = fopen(path, "rb");
        if (f == nullptr) {
            put_error(error, cap, "cannot open %s", path);
            return GEISTR_IO;
        }
        fclose(f);
    }
    /* Plan on the CPU first: the weight size decides AUTO (geist-serve's
     * rule: GPU for models of 1 GiB and more), then on the chosen backend,
     * whose KV mode decides the bytes per position. */
    const bool              want_gpu = o.processor == GEISTR_PROCESSOR_GPU;
    struct geist_backend   *be       = backend_create(want_gpu, o.threads);
    struct geist_model_plan plan     = {};
    struct geist_session_opts so     = {.top_p = 1.0f};
    if (be == nullptr) {
        put_error(error, cap, want_gpu ? "no GPU backend (Metal or Vulkan) is available" : "no CPU backend");
        return GEISTR_BACKEND;
    }
    enum geist_status s = path ? geist_model_plan(path, be, &so, &plan)
                               : geist_model_plan_from_memory(data, len, be, &so, &plan);
    if (s == GEIST_OK && o.processor == GEISTR_PROCESSOR_AUTO && plan.weight_bytes >= (1ull << 30)) {
        struct geist_backend *gpu = backend_create(true, o.threads);
        if (gpu != nullptr) {
            geist_backend_destroy(be);
            be = gpu;
            s  = path ? geist_model_plan(path, be, &so, &plan)
                      : geist_model_plan_from_memory(data, len, be, &so, &plan);
        }
    }
    if (s != GEIST_OK) {
        put_error(error, cap, "%s", geist_last_create_error());
        geist_backend_destroy(be);
        return from_engine(s);
    }
    uint32_t      window = 0;
    geistr_status gs     = choose_window(&plan, o.context, &window, error, cap);
    if (gs != GEISTR_OK) {
        geist_backend_destroy(be);
        return gs;
    }
    so.max_seq_len         = window;
    struct geist_model *gm = nullptr;
    s = path ? geist_model_load_with_opts(path, be, &so, &gm)
             : geist_model_load_from_memory_with_opts(data, len, be, &so, &gm);
    if (s != GEIST_OK) {
        put_error(error, cap, "%s", geist_last_create_error());
        geist_backend_destroy(be);
        return from_engine(s);
    }
    geistr_model *m = calloc(1, sizeof *m);
    if (m == nullptr) {
        geist_model_destroy(gm);
        geist_backend_destroy(be);
        return GEISTR_NO_MEMORY;
    }
    model_describe(m, gm, be, window, forced);
    *out = m;
    return GEISTR_OK;
}

geistr_status geistr_model_open(const char              *path,
                                const geistr_model_opts *opts,
                                geistr_model           **out,
                                char                    *error,
                                size_t                   cap) {
    if (out)
        *out = nullptr;
    if (!path || !*path || !out) {
        put_error(error, cap, "path and out are required");
        return GEISTR_INVALID;
    }
    return model_open(path, nullptr, 0, opts, out, error, cap);
}

geistr_status geistr_model_open_memory(const void              *data,
                                       size_t                   len,
                                       const geistr_model_opts *opts,
                                       geistr_model           **out,
                                       char                    *error,
                                       size_t                   cap) {
    if (out)
        *out = nullptr;
    if (!data || !len || !out) {
        put_error(error, cap, "data, len and out are required");
        return GEISTR_INVALID;
    }
    return model_open(nullptr, data, len, opts, out, error, cap);
}

geistr_status geistr_model_wrap(struct geist_model     *gm,
                                struct geist_backend   *be,
                                const geistr_model_opts *opts,
                                geistr_model           **out,
                                char                    *error,
                                size_t                   cap) {
    if (out)
        *out = nullptr;
    geistr_model_opts o = GEISTR_MODEL_OPTS_INIT;
    if (!gm || !be || !out) {
        put_error(error, cap, "model, backend and out are required");
        return GEISTR_INVALID;
    }
    enum tpl_family forced = TPL_UNKNOWN;
    if (!model_opts(opts, &o, &forced, error, cap))
        return GEISTR_INVALID;
    /* The caller chose the memory when it loaded the model: the window is
     * the trained one, or opts.context if smaller. */
    uint64_t trained = geist_model_context_length(gm);
    uint32_t window  = trained ? (uint32_t) (trained < UINT32_MAX ? trained : UINT32_MAX) : 4096;
    if (o.context && o.context < window)
        window = o.context;
    geistr_model *m = calloc(1, sizeof *m);
    if (m == nullptr)
        return GEISTR_NO_MEMORY;
    model_describe(m, gm, be, window, forced);
    m->borrowed = true;
    *out        = m;
    return GEISTR_OK;
}

static void model_release(geistr_model *m) {
    if (m && atomic_fetch_sub(&m->refs, 1) == 1) {
        if (!m->borrowed) {
            geist_model_destroy(m->m);
            geist_backend_destroy(m->be);
        }
        pthread_mutex_destroy(&m->engine);
        free(m);
    }
}

void geistr_model_close(geistr_model *m) {
    model_release(m);
}

geistr_status geistr_model_info_get(const geistr_model *m, geistr_model_info *info) {
    if (!m || !info)
        return GEISTR_INVALID;
    const char       *backend = geist_backend_name(m->be);
    geistr_model_info full    = {
               .size        = sizeof full,
               .arch        = geist_model_arch(m->m),
               .chat_format = tpl_family_name(m->family),
               .backend     = !strncmp(backend, "cpu", 3) ? "cpu" : backend,
               .context     = m->context,
    };
    size_t size = info->size;
    if (size < sizeof size || size > sizeof full)
        return GEISTR_INVALID;
    memcpy(info, &full, size);
    info->size = size;
    return GEISTR_OK;
}

const char *geistr_model_error(const geistr_model *m) {
    return m ? m->error : "no model";
}

/* ------------------------------------------------------------------------ */
/* Chat                                                                      */
/* ------------------------------------------------------------------------ */

struct turn {
    char  *role, *content;
    size_t cut; /* truncating here leaves exactly the messages before this one,
                   all closed; NO_POSITION where that is not known */
};

struct geistr_chat {
    geistr_model         *model;
    geistr_chat_opts      opts;
    char                **stops; /* copies of opts.stop */
    struct geist_session *s;
    atomic_bool           cancel;
    struct tpl_state      tpl;
    struct turn          *turns;
    size_t                n_turns, cap_turns;
    /* the answer of the last send */
    bool        answering, open, cancel_reported;
    size_t      answer_turn;   /* index of its assistant turn */
    const char *end_marker;    /* the model's own end-of-turn text, if it ended so */
    char       *raw;           /* the generated text, markers included (the history) */
    int32_t    *recent;        /* the answer's tokens, for str_repeats */
    size_t      n_recent, cap_recent;
    size_t      raw_len, raw_cap;
    uint32_t    limit;
    /* text stages and the pieces they produced */
    struct str_utf8   utf8;
    struct str_output output;
    struct str_stops  stop;
    struct str_pieces pieces;
    geistr_stats      stats;
    double            started;
    char              error[256];
};

static geistr_status fail(geistr_chat *c, geistr_status s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, sizeof c->error, fmt, ap);
    va_end(ap);
    return s;
}

/* The token t joins the answer: does the answer loop now? (str_repeats) */
static bool looping(geistr_chat *c, int32_t t) {
    if (c->n_recent == c->cap_recent) {
        size_t   cap   = c->cap_recent ? c->cap_recent * 2 : 256;
        int32_t *grown = realloc(c->recent, cap * sizeof *grown);
        if (!grown)
            return false; /* no memory to watch with: the answer goes on */
        c->recent = grown, c->cap_recent = cap;
    }
    c->recent[c->n_recent++] = t;
    return str_repeats(c->recent, c->n_recent);
}

/* A seed for sampling: /dev/urandom, else the clock and the process. */
static uint64_t fresh_seed(void) {
    uint64_t seed = 0;
    FILE    *f    = fopen("/dev/urandom", "rb");
    if (!f || fread(&seed, sizeof seed, 1, f) != 1) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        seed = (uint64_t) ts.tv_nsec ^ ((uint64_t) ts.tv_sec << 20) ^ (uint64_t) getpid();
    }
    if (f)
        fclose(f);
    return seed ? seed : 1; /* 0 would mean the fixed seed */
}

geistr_status geistr_chat_open(geistr_model *m, const geistr_chat_opts *opts, geistr_chat **out) {
    if (out)
        *out = nullptr;
    if (!m || !out)
        return GEISTR_INVALID;
    geistr_chat_opts o = GEISTR_CHAT_OPTS_INIT;
    if (!opts_copy(&o, opts, sizeof o) || (o.n_stop && !o.stop)) {
        snprintf(m->error, sizeof m->error, "chat options: unknown size or missing stop list");
        return GEISTR_INVALID;
    }
    if (m->family == TPL_UNKNOWN) {
        snprintf(m->error, sizeof m->error, "this model's chat format is not supported");
        return GEISTR_FORMAT;
    }
    geistr_chat  *c     = calloc(1, sizeof *c);
    char        **stops = nullptr;
    geistr_status s     = c ? stops_copy(&o, &stops) : GEISTR_NO_MEMORY;
    if (s != GEISTR_OK) {
        free(c);
        snprintf(m->error, sizeof m->error, "chat: out of memory or an empty stop string");
        return s;
    }
    /* geistlib uses one fixed seed when given 0: every process would sample the same "random" answer. */
    struct geist_session_opts so = {.max_seq_len = m->context,
                                    .temperature = o.temperature,
                                    .top_p       = o.top_p,
                                    .random_seed = o.temperature > 0 ? fresh_seed() : 0};
    engine_lock(m);
    enum geist_status es = geist_session_create(m->m, m->be, &so, &c->s);
    engine_unlock(m);
    if (es != GEIST_OK) {
        stops_free(stops, o.n_stop);
        free(c);
        snprintf(m->error, sizeof m->error, "session: %s", geist_backend_errmsg(m->be));
        return from_engine(es);
    }
    atomic_fetch_add(&m->refs, 1);
    c->model     = m;
    c->opts      = o;
    c->opts.stop = nullptr;
    c->stops     = stops;
    atomic_init(&c->cancel, false);
    tpl_init(&c->tpl, m->family);
    c->stats = (geistr_stats) {.size = sizeof c->stats, .prefill_ms = -1, .first_answer_ms = -1};
    *out     = c;
    return GEISTR_OK;
}


static void turns_drop(geistr_chat *c, size_t keep) {
    for (size_t i = keep; i < c->n_turns; i++) {
        free(c->turns[i].role);
        free(c->turns[i].content);
    }
    c->n_turns = keep;
}

void geistr_chat_close(geistr_chat *c) {
    if (!c)
        return;
    geistr_model *m = c->model;
    engine_lock(m);
    geist_session_destroy(c->s);
    engine_unlock(m);
    turns_drop(c, 0);
    free(c->turns);
    tpl_free(&c->tpl);
    stops_free(c->stops, c->opts.n_stop);
    str_pieces_free(&c->pieces);
    str_stops_free(&c->stop);
    free(c->raw);
    free(c->recent);
    free(c);
    model_release(m);
}

size_t geistr_chat_length(const geistr_chat *c) {
    return c ? c->n_turns : 0;
}

/* ---- tokens -------------------------------------------------------------- */

struct ids {
    geist_token_t *v;
    size_t         n, cap;
};

static bool ids_push(struct ids *t, size_t n, const geist_token_t *v) {
    if (t->n + n > t->cap) {
        size_t         cap  = (t->n + n) * 2 + 64;
        geist_token_t *grow = realloc(t->v, cap * sizeof *grow);
        if (!grow)
            return false;
        t->v   = grow;
        t->cap = cap;
    }
    memcpy(t->v + t->n, v, n * sizeof *v);
    t->n += n;
    return true;
}

/* Tokenize text onto t (content tokens; the chat adds BOS itself). */
static geistr_status tokenize(geistr_chat *c, const char *text, struct ids *t) {
    if (!*text)
        return GEISTR_OK;
    size_t cap = strlen(text) + 16; /* a token is at least one byte */
    if (t->n + cap > t->cap) {
        geist_token_t *grow = realloc(t->v, (t->n + cap) * sizeof *grow);
        if (!grow)
            return GEISTR_NO_MEMORY;
        t->v   = grow;
        t->cap = t->n + cap;
    }
    size_t n = 0;
    engine_lock(c->model);
    enum geist_status s = geist_session_tokenize(c->s, text, cap, t->v + t->n, &n);
    engine_unlock(c->model);
    if (s != GEIST_OK)
        return fail(c, from_engine(s), "tokenize: %s", geist_session_errmsg(c->s));
    t->n += n;
    return GEISTR_OK;
}

/* Prefill in chunks sized by time; cancel is checked between them. */
static geistr_status prefill(geistr_chat *c, size_t n, const geist_token_t *v) {
    size_t chunk = PREFILL_FIRST;
    for (size_t at = 0; at < n;) {
        if (atomic_load(&c->cancel))
            return fail(c, GEISTR_CANCELLED, "cancelled");
        const size_t k  = n - at < chunk ? n - at : chunk;
        const double t0 = now_ms();
        engine_lock(c->model);
        enum geist_status s = geist_session_prefill_tokens(c->s, k, v + at);
        engine_unlock(c->model);
        if (s != GEIST_OK)
            return fail(c, from_engine(s), "prefill: %s", geist_session_errmsg(c->s));
        at += k;
        const double took = now_ms() - t0;
        if (took > 0) { /* towards the slice, growing at most fourfold per step */
            double next = (double) k * PREFILL_SLICE_MS / took;
            if (next > 4.0 * (double) k)
                next = 4.0 * (double) k;
            chunk = next < PREFILL_MIN ? PREFILL_MIN : next > PREFILL_MAX ? PREFILL_MAX : (size_t) next;
        }
    }
    return GEISTR_OK;
}

static size_t session_length(geistr_chat *c) {
    engine_lock(c->model);
    const size_t n = geist_session_length(c->s);
    engine_unlock(c->model);
    return n;
}

/* ---- rebuilding ------------------------------------------------------------ */

static bool is_role(const char *role, const char *want) {
    return role && !strcmp(role, want);
}

/* Render turns[first..n) (with a leading system turn 0 when first > 0 and
 * keep_system) plus extra new messages into a fresh template state: the
 * whole conversation, as a first send would. With answer_follows the
 * generation prompt stays at the end. */
static char *render_all(geistr_chat     *c,
                        struct tpl_state *st,
                        bool             keep_system,
                        size_t           first,
                        size_t           n_extra,
                        const geistr_message extra[],
                        bool             answer_follows) {
    const size_t    total = (keep_system ? 1 : 0) + (c->n_turns - first) + n_extra;
    geistr_message *msgs  = calloc(total ? total : 1, sizeof *msgs);
    if (!msgs)
        return nullptr;
    size_t k = 0;
    if (keep_system)
        msgs[k++] = (geistr_message) {c->turns[0].role, c->turns[0].content};
    for (size_t i = first; i < c->n_turns; i++)
        msgs[k++] = (geistr_message) {c->turns[i].role, c->turns[i].content};
    for (size_t i = 0; i < n_extra; i++)
        msgs[k++] = extra[i];
    tpl_init(st, c->model->family);
    char *text = tpl_render_send(st, k, msgs);
    free(msgs);
    if (text && !answer_follows) { /* drop the generation prompt */
        const size_t g = strlen(tpl_generation_prompt(c->model->family)), n = strlen(text);
        text[n >= g ? n - g : 0] = 0;
    }
    return text;
}

/* Empty the session and prefill text from the start (BOS as configured). */
static geistr_status refill(geistr_chat *c, const char *text, size_t *out_tokens) {
    struct ids t = {};
    if (c->model->add_bos && c->model->bos >= 0 && !ids_push(&t, 1, &c->model->bos))
        return GEISTR_NO_MEMORY;
    geistr_status s = tokenize(c, text, &t);
    if (s == GEISTR_OK && t.n + 1 > c->model->context)
        s = fail(c, GEISTR_CONTEXT, "the conversation does not fit the context window");
    if (s == GEISTR_OK) {
        engine_lock(c->model);
        enum geist_status es = geist_session_truncate(c->s, 0);
        engine_unlock(c->model);
        s = es == GEIST_OK ? prefill(c, t.n, t.v) : fail(c, from_engine(es), "reset failed");
    }
    if (out_tokens)
        *out_tokens = t.n;
    free(t.v);
    return s;
}

/* ---- the answer ------------------------------------------------------------ */

static bool raw_append(geistr_chat *c, const char *text) {
    const size_t n = strlen(text);
    if (c->raw_len + n + 1 > c->raw_cap) {
        size_t cap  = (c->raw_len + n + 1) * 2;
        char  *grow = realloc(c->raw, cap);
        if (!grow)
            return false;
        c->raw     = grow;
        c->raw_cap = cap;
    }
    memcpy(c->raw + c->raw_len, text, n + 1);
    c->raw_len += n;
    return true;
}

/* The answer becomes the content of its assistant turn (open or not). */
static bool answer_commit(geistr_chat *c) {
    if (!c->answering && !c->open)
        return true;
    char *content = strdup(c->raw ? c->raw : "");
    if (!content)
        return false;
    free(c->turns[c->answer_turn].content);
    c->turns[c->answer_turn].content = content;
    return true;
}

static void answer_reset(geistr_chat *c) {
    c->answering = c->open = c->cancel_reported = false;
    c->end_marker = nullptr;
    c->raw_len    = 0;
    if (c->raw)
        c->raw[0] = 0;
    str_pieces_clear(&c->pieces);
}

/* ---- send ------------------------------------------------------------------ */

static bool turns_reserve(geistr_chat *c, size_t more) {
    if (c->n_turns + more <= c->cap_turns)
        return true;
    size_t       cap  = (c->n_turns + more) * 2 + 4;
    struct turn *grow = realloc(c->turns, cap * sizeof *grow);
    if (!grow)
        return false;
    c->turns     = grow;
    c->cap_turns = cap;
    return true;
}

static bool turn_push(geistr_chat *c, const char *role, const char *content, size_t cut) {
    char *r = strdup(role ? role : "user"), *t = strdup(content);
    if (!r || !t) {
        free(r);
        free(t);
        return false;
    }
    c->turns[c->n_turns++] = (struct turn) {r, t, cut};
    return true;
}

/* Would the conversation fit with the oldest `go` turns gone: held ones
 * first (a system turn stays), then the oldest new messages (a leading
 * system message stays, the last one always)? kept is scratch of count. */
static geistr_status drop_fits(geistr_chat *c, bool system, bool new_system, const geistr_message *messages,
                               size_t count, size_t go, geistr_message *kept, bool *fits) {
    const size_t held  = c->n_turns - (system ? 1 : 0);
    const size_t first = (system ? 1 : 0) + (go < held ? go : held), skip = go > held ? go - held : 0;
    size_t       k     = 0;
    if (new_system)
        kept[k++] = messages[0];
    for (size_t i = (new_system ? 1 : 0) + skip; i < count; i++)
        kept[k++] = messages[i];
    struct tpl_state fresh = {};
    char            *all   = render_all(c, &fresh, system, first, k, kept, true);
    tpl_free(&fresh);
    if (!all)
        return GEISTR_NO_MEMORY;
    struct ids t = {};
    if (c->model->add_bos && c->model->bos >= 0)
        ids_push(&t, 1, &c->model->bos);
    geistr_status s = tokenize(c, all, &t);
    *fits           = s == GEISTR_OK && t.n + 1 <= c->model->context;
    free(all), free(t.v);
    return s;
}

/* Stage outputs land here as pieces, in order. */
static bool queue_answer(void *c, const char *text) {
    return str_pieces_push(&((geistr_chat *) c)->pieces, GEISTR_PART_ANSWER, text);
}
static bool queue_thinking(void *c, const char *text) {
    return str_pieces_push(&((geistr_chat *) c)->pieces, GEISTR_PART_THINKING, text);
}
static bool to_stops(void *c, const char *text) {
    return str_stops_feed(&((geistr_chat *) c)->stop, text, queue_answer, c);
}

geistr_status geistr_chat_send(geistr_chat *c, size_t count, const geistr_message messages[]) {
    if (!c)
        return GEISTR_INVALID;
    atomic_store(&c->cancel, false);
    if (!count || !messages)
        return fail(c, GEISTR_INVALID, "send needs at least one message");
    for (size_t i = 0; i < count; i++)
        if (!messages[i].content)
            return fail(c, GEISTR_INVALID, "a message has no content");
    if (is_role(messages[count - 1].role, "assistant"))
        return fail(c, GEISTR_INVALID, "the last message must not be from the assistant");
    if (!answer_commit(c) || !turns_reserve(c, count + 1))
        return fail(c, GEISTR_NO_MEMORY, "out of memory");

    const double start = now_ms();
    const size_t len0  = session_length(c);
    /* The previous answer's turn close, the new turns, the generation
     * prompt: tokenized apart, so their boundaries are known. */
    struct tpl_state st = {.family = c->tpl.family, .started = c->tpl.started};
    if (c->tpl.folded && !(st.folded = strdup(c->tpl.folded)))
        return fail(c, GEISTR_NO_MEMORY, "out of memory");
    char *text = tpl_render_send(&st, count, messages);
    if (!text) {
        tpl_free(&st);
        return fail(c, GEISTR_NO_MEMORY, "out of memory");
    }
    const char  *gen   = tpl_generation_prompt(c->model->family);
    const size_t t_len = strlen(text), g_len = strlen(gen);
    text[t_len - g_len] = 0; /* the turns; gen follows */
    struct ids close = {}, turns = {}, prompt = {};
    geistr_status s = GEISTR_OK;
    if (len0 == 0 && c->model->add_bos && c->model->bos >= 0 && !ids_push(&turns, 1, &c->model->bos))
        s = GEISTR_NO_MEMORY;
    if (s == GEISTR_OK && c->open)
        s = tokenize(c, tpl_answer_close(c->model->family, c->end_marker), &close);
    if (s == GEISTR_OK)
        s = tokenize(c, text, &turns);
    if (s == GEISTR_OK)
        s = tokenize(c, gen, &prompt);
    const size_t adding = close.n + turns.n + prompt.n;
    size_t       input  = adding;
    uint32_t     dropped = 0, dropped_new = 0; /* held turns, and new messages, that went */
    bool         rebuilt = false;
    const geistr_message *msgs = messages; /* what is sent: fewer when the oldest new ones went */
    size_t                n_msgs = count;
    geistr_message       *kept   = nullptr;

    if (s == GEISTR_OK && len0 + adding + 1 > c->model->context) {
        if (c->opts.overflow != GEISTR_OVERFLOW_DROP_OLDEST) {
            s = fail(c, GEISTR_CONTEXT, "the conversation does not fit the context window");
        } else {
            /* Drop the oldest turns (a leading system turn stays) until the
             * whole conversation, rendered again, fits; then prefill it.
             * Held turns go first; when that is not enough (a whole
             * conversation sent at once: a resumed chat, another model), the
             * oldest new messages too (a leading system message among them
             * stays, and the last one always). Fitting is monotonic in what
             * goes, so a binary search finds the least: a few tokenizations,
             * not one per message. */
            const bool   system     = c->n_turns > 0 && is_role(c->turns[0].role, "system");
            const bool   new_system = !system && is_role(messages[0].role, "system");
            const size_t held       = c->n_turns - (system ? 1 : 0);  /* turns that may go */
            const size_t fixed      = (new_system ? 1 : 0) + 1; /* a leading system message, the last one */
            const size_t sendable   = count > fixed ? count - fixed : 0;  /* new messages that may go */
            kept                    = calloc(count, sizeof *kept);
            size_t lo = 0, hi = held + sendable;                     /* how many go */
            bool   fit_hi = false;
            s             = kept ? GEISTR_OK : GEISTR_NO_MEMORY;
            if (s == GEISTR_OK)
                s = drop_fits(c, system, new_system, messages, count, hi, kept, &fit_hi);
            if (s == GEISTR_OK && !fit_hi)
                s = fail(c, GEISTR_CONTEXT, "the conversation does not fit the context window");
            while (s == GEISTR_OK && lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                bool   fit = false;
                s          = drop_fits(c, system, new_system, messages, count, mid, kept, &fit);
                if (fit)
                    hi = mid;
                else
                    lo = mid + 1;
            }
            size_t first = (system ? 1 : 0) + (hi < held ? hi : held);
            if (s == GEISTR_OK) {
                dropped     = (uint32_t) (first - (system ? 1 : 0));
                dropped_new = (uint32_t) (hi > held ? hi - held : 0);
                if (dropped_new) {
                    size_t k = 0;
                    if (new_system)
                        kept[k++] = messages[0];
                    for (size_t i = (new_system ? 1 : 0) + dropped_new; i < count; i++)
                        kept[k++] = messages[i];
                    msgs = kept, n_msgs = k;
                }
            }
            if (s == GEISTR_OK) {
                /* Commit the drop, then refill the kept conversation with
                 * the new turns; positions inside it are not known. */
                const size_t keep_from = (system ? 1 : 0) + dropped;
                for (size_t i = system ? 1 : 0; i < keep_from; i++) {
                    free(c->turns[i].role);
                    free(c->turns[i].content);
                }
                memmove(c->turns + (system ? 1 : 0),
                        c->turns + keep_from,
                        (c->n_turns - keep_from) * sizeof *c->turns);
                c->n_turns -= dropped;
                for (size_t i = 0; i < c->n_turns; i++)
                    c->turns[i].cut = NO_POSITION;
                struct tpl_state fresh = {};
                char *all = render_all(c, &fresh, false, 0, n_msgs, msgs, true);
                if (!all) {
                    tpl_free(&fresh);
                    s = GEISTR_NO_MEMORY;
                } else {
                    s = refill(c, all, &input);
                    free(all);
                    tpl_free(&st);
                    st = fresh;
                }
                rebuilt = true;
                if (s != GEISTR_OK)
                    s = fail(c, s, "refill after dropping old turns failed");
            }
        }
    }
    if (s == GEISTR_OK && !rebuilt) {
        s = prefill(c, close.n, close.v);
        if (s == GEISTR_OK)
            s = prefill(c, turns.n, turns.v);
        if (s == GEISTR_OK)
            s = prefill(c, prompt.n, prompt.v);
        if (s != GEISTR_OK) {
            /* The new messages are not part of the conversation. */
            engine_lock(c->model);
            const enum geist_status ts = geist_session_truncate(c->s, len0);
            engine_unlock(c->model);
            if (ts != GEIST_OK) {
                struct tpl_state fresh = {};
                char *all = render_all(c, &fresh, false, 0, 0, nullptr, false);
                if (all)
                    (void) refill(c, all, nullptr);
                free(all);
                tpl_free(&fresh);
                c->open = false;
            }
        }
    }
    const size_t answer_at = len0 + adding; /* where the answer starts, if not rebuilt */
    free(close.v);
    free(turns.v);
    free(prompt.v);
    free(text);
    if (s != GEISTR_OK) {
        tpl_free(&st);
        free(kept);
        return s;
    }

    /* Commit: the previous answer is closed, the new turns and the new
     * answer's turn are part of the conversation. */
    const size_t base = len0 + close.n;
    for (size_t i = 0; i < n_msgs; i++)
        if (!turn_push(c, msgs[i].role, msgs[i].content, !rebuilt && i == 0 ? base : NO_POSITION)) {
            free(kept);
            return fail(c, GEISTR_NO_MEMORY, "out of memory");
        }
    free(kept);
    c->answer_turn = c->n_turns;
    if (!turn_push(c, "assistant", "", rebuilt ? NO_POSITION : answer_at - prompt.n))
        return fail(c, GEISTR_NO_MEMORY, "out of memory");
    tpl_free(&c->tpl);
    c->tpl = st;
    answer_reset(c);
    c->answering = c->open = true;

    const size_t   used = session_length(c);
    const uint32_t room = used + 1 < c->model->context ? (uint32_t) (c->model->context - used - 1) : 0;
    c->limit            = c->opts.max_tokens && c->opts.max_tokens < room ? c->opts.max_tokens : room;
    c->utf8             = (struct str_utf8) {};
    str_output_init(&c->output, c->opts.reasoning == GEISTR_REASONING_THINK_TAGS);
    str_output_thinking(&c->output, c->opts.thinking ? queue_thinking : nullptr);
    str_stops_free(&c->stop);
    str_stops_init(&c->stop, c->opts.n_stop, (const char *const *) c->stops);
    c->started  = start;
    c->error[0] = 0;
    c->n_recent = 0;
    c->stats    = (geistr_stats) {
           .size             = sizeof c->stats,
           .input_tokens     = (uint32_t) input,
           .context_tokens   = (uint32_t) used,
           .dropped_messages = dropped + dropped_new,
           .prefill_ms       = now_ms() - start,
           .first_answer_ms  = -1,
    };
    return GEISTR_OK;
}

/* ---- next ------------------------------------------------------------------ */

static geistr_status end(geistr_chat *c, geistr_piece *piece, geistr_finish finish) {
    if (c->answering) {
        c->answering            = false;
        c->stats.finish         = finish;
        c->stats.total_ms       = now_ms() - c->started;
        c->stats.generation_ms  = c->stats.total_ms - c->stats.prefill_ms;
        c->stats.context_tokens = (uint32_t) session_length(c);
    }
    piece->part = GEISTR_PART_END;
    piece->text = "";
    piece->len  = 0;
    return GEISTR_OK;
}

/* The answer is over: release what the stages hold back. */
static bool finish_stages(geistr_chat *c) {
    str_output_finish(&c->output);
    return str_stops_finish(&c->stop, queue_answer, c);
}

static bool is_stop_token(const geistr_chat *c, geist_token_t t) {
    for (size_t i = 0; i < c->model->n_stops; i++)
        if (c->model->stops[i] == t)
            return true;
    return false;
}

geistr_status geistr_chat_next(geistr_chat *c, geistr_piece *piece) {
    if (!c || !piece || piece->size < sizeof(geistr_piece))
        return GEISTR_INVALID;
    if (!c->open && !c->answering && c->stats.finish == GEISTR_FINISH_NONE)
        return fail(c, GEISTR_INVALID, "send first");
    for (;;) {
        if (!c->answering && c->stats.finish == GEISTR_FINISH_CANCELLED && !c->cancel_reported) {
            c->cancel_reported = true;
            return GEISTR_CANCELLED;
        }
        if (c->answering && atomic_load(&c->cancel)) {
            str_pieces_clear(&c->pieces);
            end(c, piece, GEISTR_FINISH_CANCELLED);
            snprintf(c->error, sizeof c->error, "cancelled");
            continue;
        }
        struct str_piece q;
        if (str_pieces_take(&c->pieces, &q)) {
            if (q.part == GEISTR_PART_ANSWER && c->stats.first_answer_ms < 0)
                c->stats.first_answer_ms = now_ms() - c->started;
            piece->part = (geistr_part) q.part;
            piece->text = q.text;
            piece->len  = q.len;
            return GEISTR_OK;
        }
        if (!c->answering)
            return end(c, piece, c->stats.finish);
        geistr_finish why = c->stop.stopped ? GEISTR_FINISH_STOP : GEISTR_FINISH_NONE;
        if (!why && (c->stats.output_tokens >= c->limit || session_length(c) + 1 >= c->model->context))
            why = c->opts.max_tokens && c->stats.output_tokens >= c->opts.max_tokens ? GEISTR_FINISH_LENGTH
                                                                                     : GEISTR_FINISH_CONTEXT;
        const char *p = nullptr;
        if (!why) {
            geist_token_t t = -1;
            engine_lock(c->model);
            enum geist_status es = geist_session_decode_step(c->s, &t);
            p                    = es == GEIST_OK ? geist_session_token_to_str(c->s, t) : nullptr;
            engine_unlock(c->model);
            if (es != GEIST_OK) {
                end(c, piece, GEISTR_FINISH_ERROR);
                return fail(c, GEISTR_BACKEND, "decode: %s", geist_session_errmsg(c->s));
            }
            c->stats.output_tokens++;
            const bool stop = is_stop_token(c, t);
            if (stop) /* the model ended its turn; its marker is in the context now */
                c->end_marker = p;
            why = stop ? GEISTR_FINISH_STOP : looping(c, t) ? GEISTR_FINISH_REPETITION : GEISTR_FINISH_NONE;
        }
        if (why) {
            if (!finish_stages(c))
                return fail(c, GEISTR_NO_MEMORY, "out of memory");
            end(c, piece, why);
            continue;
        }
        if (p == nullptr)
            continue;
        char text[256];
        if (!raw_append(c, p))
            return fail(c, GEISTR_NO_MEMORY, "out of memory");
        if (!str_utf8_feed(&c->utf8, p, text, sizeof text)) {
            end(c, piece, GEISTR_FINISH_ERROR);
            return fail(c, c->utf8.failed ? GEISTR_BACKEND : GEISTR_NO_MEMORY,
                        c->utf8.failed ? "the model produced invalid UTF-8" : "a token piece is too long");
        }
        if (!str_output_feed(&c->output, text, to_stops, c))
            return fail(c, GEISTR_NO_MEMORY, "out of memory");
    }
}

geistr_status geistr_chat_cancel(geistr_chat *c) {
    if (!c)
        return GEISTR_INVALID;
    atomic_store(&c->cancel, true);
    return GEISTR_OK;
}

/* ---- rewind ---------------------------------------------------------------- */

geistr_status geistr_chat_limit(geistr_chat *c, uint32_t max_tokens) {
    if (!c || c->answering)
        return GEISTR_INVALID;
    c->opts.max_tokens = max_tokens;
    return GEISTR_OK;
}

geistr_status geistr_chat_rewind(geistr_chat *c, size_t keep) {
    if (!c || keep > c->n_turns)
        return GEISTR_INVALID;
    if (!answer_commit(c))
        return fail(c, GEISTR_NO_MEMORY, "out of memory");
    if (keep == c->n_turns) { /* nothing dropped; an unfinished answer ends */
        if (c->answering) {
            geistr_piece unused = {.size = sizeof unused};
            finish_stages(c);
            end(c, &unused, GEISTR_FINISH_CANCELLED);
            c->cancel_reported = true;
        }
        return GEISTR_OK;
    }
    const size_t cut = keep == 0 ? 0 : c->turns[keep].cut;
    bool         ok  = false;
    if (cut != NO_POSITION) {
        engine_lock(c->model);
        ok = geist_session_truncate(c->s, cut) == GEIST_OK;
        engine_unlock(c->model);
    }
    turns_drop(c, keep);
    answer_reset(c);
    c->stats.finish = GEISTR_FINISH_NONE;
    /* The template state of the kept messages (Gemma 3's pending system
     * fold, whether a system message still leads). */
    struct tpl_state fresh = {};
    char            *all   = render_all(c, &fresh, false, 0, 0, nullptr, false);
    if (!all) {
        tpl_free(&fresh);
        return fail(c, GEISTR_NO_MEMORY, "out of memory");
    }
    geistr_status s = GEISTR_OK;
    if (!ok) { /* not truncatable here: the kept messages again, from the start */
        s = refill(c, all, nullptr);
        for (size_t i = 0; i < c->n_turns; i++)
            c->turns[i].cut = NO_POSITION;
    }
    free(all);
    tpl_free(&c->tpl);
    c->tpl = fresh;
    return s;
}

/* ---- stats ----------------------------------------------------------------- */

geistr_status geistr_chat_stats(const geistr_chat *c, geistr_stats *stats) {
    if (!c || !stats || stats->size < sizeof stats->size || stats->size > sizeof c->stats)
        return GEISTR_INVALID;
    size_t size = stats->size;
    memcpy(stats, &c->stats, size);
    stats->size = size;
    return GEISTR_OK;
}

const char *geistr_chat_error(const geistr_chat *c) {
    return c ? c->error : "no chat";
}

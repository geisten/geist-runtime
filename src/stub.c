/*
 * stub.c — a test double of the geistr API, without geistlib (#1).
 *
 * It honours the whole contract of include/geistr.h, so tests/test_api.c is a
 * conformance suite that later runs unchanged against the real runtime:
 * complete UTF-8 pieces from tokens that split code points, <think> markers
 * split across tokens, stop strings, cancellation from another thread, a
 * conversation kept across sends, rewind, context limits and ABI-sized options.
 *
 * Models: "stub:echo" answers "Echo: <last user message>"; "stub:slow" is
 * the same, repeated, at 1 ms per token; "stub:raw" generates the last
 * message verbatim, markers included (for the text stages); "stub:noformat"
 * has no chat format. A token is 3 bytes of the answer, so tokens cut UTF-8 apart. Input
 * costs (bytes + 3) / 4 tokens per message plus 4 for its markers.
 */
#include "geistr.h"
#include "common.h"
#include "geistr_decision.h"
#include "stream.h"
#include "template.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#define DEFAULT_CONTEXT 256u
#define TOKEN_BYTES 3u

const char *geistr_version(void) {
    return "0.3.2-stub";
}

static void put_error(char *error, size_t cap, const char *text) {
    if (error && cap)
        snprintf(error, cap, "%s", text);
}

/* ------------------------------------------------------------------------ */
/* Model                                                                     */
/* ------------------------------------------------------------------------ */

struct geistr_model {
    atomic_int        refs;
    geistr_model_opts opts;
    geistr_decision_policy decision;
    bool              slow, raw, has_format;
    const char       *format; /* a static family name */
    uint32_t          context;
    char              error[256];
};

static geistr_status model_new(const char              *name,
                               const geistr_model_opts *opts,
                               geistr_model           **out,
                               char                    *error,
                               size_t                   cap) {
    geistr_model_opts o = GEISTR_MODEL_OPTS_INIT;
    if (!opts_copy(&o, opts, sizeof o) ||
        (opts && opts->size > offsetof(geistr_model_opts, decision) && opts->size < sizeof o)) {
        put_error(error, cap, "model options: unknown size");
        return GEISTR_INVALID;
    }
    if (o.decision && geistr_decision_policy_validate(o.decision) != GEISTR_OK) {
        put_error(error, cap, "invalid decision policy");
        return GEISTR_INVALID;
    }
    if ((o.processor != GEISTR_PROCESSOR_AUTO && o.processor != GEISTR_PROCESSOR_CPU &&
         o.processor != GEISTR_PROCESSOR_GPU) || o.threads > INT_MAX) {
        put_error(error, cap, "invalid processor or thread count");
        return GEISTR_INVALID;
    }
    if (o.decision && o.decision->enabled) {
        put_error(error, cap, "the stub cannot verify a pretrained decision artifact");
        return GEISTR_FORMAT;
    }
    bool echo = !strcmp(name, "stub:echo"), slow = !strcmp(name, "stub:slow"),
         noformat = !strcmp(name, "stub:noformat"), raw = !strcmp(name, "stub:raw");
    if (!echo && !slow && !noformat && !raw) {
        put_error(error, cap, "the stub opens only stub:echo, stub:slow, stub:raw and stub:noformat");
        return GEISTR_FORMAT;
    }
    if (o.chat_format && tpl_family_from_name(o.chat_format) == TPL_UNKNOWN) {
        put_error(error, cap, "unknown chat_format override");
        return GEISTR_INVALID;
    }
    geistr_model *m = calloc(1, sizeof *m);
    if (!m)
        return GEISTR_NO_MEMORY;
    atomic_init(&m->refs, 1);
    m->opts       = o;
    if (o.decision) {
        m->decision = *o.decision;
        m->opts.decision = &m->decision;
    }
    m->slow       = slow;
    m->raw        = raw;
    m->has_format = !noformat || o.chat_format;
    m->format     = o.chat_format ? tpl_family_name(tpl_family_from_name(o.chat_format)) : "chatml";
    m->context    = o.context ? o.context : DEFAULT_CONTEXT;
    *out          = m;
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
    if (strncmp(path, "stub:", 5) != 0) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            put_error(error, cap, "cannot open the model file");
            return GEISTR_IO;
        }
        fclose(f);
    }
    return model_new(path, opts, out, error, cap);
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
    char name[32] = "";
    memcpy(name, data, len < sizeof name - 1 ? len : sizeof name - 1);
    return model_new(name, opts, out, error, cap);
}

static void model_release(geistr_model *m) {
    if (m && atomic_fetch_sub(&m->refs, 1) == 1)
        free(m);
}

/* No stub model embeds: as a model that generates text (#91). */
geistr_status geistr_embed(geistr_model *m, const char *text, float *out, size_t cap, size_t *dims,
                           uint32_t *tokens) {
    if (!m || !text || !dims || (cap && !out))
        return GEISTR_INVALID;
    *dims = 0;
    if (tokens)
        *tokens = 0;
    return GEISTR_FORMAT;
}

void geistr_model_close(geistr_model *m) {
    model_release(m);
}

geistr_status geistr_model_info_get(const geistr_model *m, geistr_model_info *info) {
    if (!m || !info)
        return GEISTR_INVALID;
    geistr_model_info full = {
        .size        = sizeof full,
        .arch        = "stub",
        .chat_format = m->has_format ? m->format : "unknown",
        .backend     = m->opts.processor == GEISTR_PROCESSOR_GPU ? "stub-gpu" : "cpu",
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
    bool     system;
    uint32_t tokens; /* in the context, markers included */
};

struct geistr_chat {
    geistr_model    *model;
    geistr_chat_opts opts;
    char           **stops; /* copies of opts.stop */
    atomic_bool      cancel;
    /* the conversation in the context; the last turn is the answer once sent */
    struct turn *turns;
    size_t       n_turns, cap_turns;
    uint32_t     used; /* tokens in the context */
    bool         sent, ended, stopped, cancel_reported;
    /* the raw answer the "model" produces, and the read position */
    char    *raw;
    size_t   raw_len, raw_pos;
    uint32_t limit; /* tokens allowed for this answer */
    /* the real text stages (src/stream.c) and the pieces they produced */
    struct str_utf8   utf8;
    struct str_output output;
    struct str_stops  stop;
    struct str_pieces pieces;
    geistr_stats  stats;
    double        started;
    char          error[256];
};

static uint32_t message_tokens(const char *content) {
    return (uint32_t) ((strlen(content) + 3) / 4) + 4;
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
    if (!m->has_format) {
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
    atomic_fetch_add(&m->refs, 1);
    c->model = m;
    c->opts  = o;
    c->stops = stops;
    c->opts.stop = nullptr; /* the caller's array is not kept */
    atomic_init(&c->cancel, false);
    c->stats = (geistr_stats) {.size = sizeof c->stats, .prefill_ms = -1, .first_answer_ms = -1};
    *out     = c;
    return GEISTR_OK;
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

void geistr_chat_close(geistr_chat *c) {
    if (!c)
        return;
    stops_free(c->stops, c->opts.n_stop);
    free(c->turns);
    free(c->raw);
    str_pieces_free(&c->pieces);
    str_stops_free(&c->stop);
    model_release(c->model);
    free(c);
}

static bool is_role(const geistr_message *msg, const char *role) {
    return msg->role && !strcmp(msg->role, role);
}

static geistr_status fail(geistr_chat *c, geistr_status s, const char *why) {
    snprintf(c->error, sizeof c->error, "%s", why);
    return s;
}

size_t geistr_chat_length(const geistr_chat *c) {
    return c ? c->n_turns : 0;
}

static void recount(geistr_chat *c) {
    c->used = 0;
    for (size_t i = 0; i < c->n_turns; i++)
        c->used += c->turns[i].tokens;
}

geistr_status geistr_chat_limit(geistr_chat *c, uint32_t max_tokens) {
    if (!c || (c->sent && !c->ended))
        return GEISTR_INVALID;
    c->opts.max_tokens = max_tokens;
    return GEISTR_OK;
}

geistr_status geistr_chat_rewind(geistr_chat *c, size_t keep) {
    if (!c || keep > c->n_turns)
        return GEISTR_INVALID;
    c->n_turns = keep;
    recount(c);
    c->sent = false; /* an unfinished answer is over; next needs a new send */
    return GEISTR_OK;
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
    if (is_role(&messages[count - 1], "assistant"))
        return fail(c, GEISTR_INVALID, "the last message must not be from the assistant");

    double   start   = now_ms();
    uint32_t context = c->model->context;
    uint32_t adding  = 4 + 2; /* the answer's markers and the generation prompt */
    for (size_t i = 0; i < count; i++)
        adding += message_tokens(messages[i].content);
    /* Plan the drop first, so a refusal leaves the chat unchanged. */
    size_t   first = c->n_turns && c->turns[0].system ? 1 : 0, drop = 0;
    uint32_t used  = c->used;
    while (used + adding + 1 > context && c->opts.overflow == GEISTR_OVERFLOW_DROP_OLDEST &&
           first + drop < c->n_turns)
        used -= c->turns[first + drop++].tokens;
    if (used + adding + 1 > context)
        return fail(c, GEISTR_CONTEXT, "the conversation does not fit the context window");
    if (c->n_turns + count + 1 > c->cap_turns) {
        size_t       cap  = (c->n_turns + count + 1) * 2;
        struct turn *grow = realloc(c->turns, cap * sizeof *grow);
        if (!grow)
            return fail(c, GEISTR_NO_MEMORY, "out of memory");
        c->turns     = grow;
        c->cap_turns = cap;
    }

    /* The "model": optional thinking, then an echo of the last message. */
    const char *last  = messages[count - 1].content;
    const char *think = c->opts.reasoning == GEISTR_REASONING_THINK_TAGS && !c->model->raw ? "<think>Weighing the question.</think>" : "";
    size_t      reps  = c->model->slow ? 400 : 1;
    size_t      need  = strlen(think) + reps * (strlen(last) + 8) + 1;
    char       *raw   = malloc(need);
    if (!raw)
        return fail(c, GEISTR_NO_MEMORY, "out of memory");
    size_t at = (size_t) snprintf(raw, need, "%s", think);
    for (size_t r = 0; r < reps; r++)
        at += (size_t) snprintf(raw + at, need - at, c->model->raw ? "%s%s" : "%sEcho: %s", r ? " " : "", last);
    free(c->raw);
    c->raw     = raw;
    c->raw_len = at;
    c->raw_pos = 0;

    /* Apply: drop, append the new messages and the answer turn. A drop means
     * every kept turn is processed again; otherwise only the new messages. */
    memmove(c->turns + first, c->turns + first + drop, (c->n_turns - first - drop) * sizeof *c->turns);
    c->n_turns -= drop;
    for (size_t i = 0; i < count; i++)
        c->turns[c->n_turns++] = (struct turn) {.system = is_role(&messages[i], "system"),
                                                .tokens = message_tokens(messages[i].content)};
    c->turns[c->n_turns++] = (struct turn) {.tokens = 4 + 2};
    recount(c);

    uint32_t room      = context - c->used - 1;
    c->limit           = c->opts.max_tokens && c->opts.max_tokens < room ? c->opts.max_tokens : room;
    str_pieces_clear(&c->pieces);
    c->utf8 = (struct str_utf8) {};
    str_output_init(&c->output, c->opts.reasoning == GEISTR_REASONING_THINK_TAGS);
    str_output_thinking(&c->output, c->opts.thinking ? queue_thinking : nullptr);
    str_stops_free(&c->stop);
    str_stops_init(&c->stop, c->opts.n_stop, (const char *const *) c->stops);
    c->sent            = true;
    c->ended           = false;
    c->stopped         = false;
    c->cancel_reported = false;
    c->started         = start;
    c->error[0]        = 0;
    c->stats           = (geistr_stats) {
        .size             = sizeof c->stats,
        .input_tokens     = drop ? c->used : adding,
        .context_tokens   = c->used,
        .dropped_messages = (uint32_t) drop,
        .prefill_ms       = now_ms() - start,
        .first_answer_ms  = -1,
    };
    return GEISTR_OK;
}


static geistr_status end(geistr_chat *c, geistr_piece *piece, geistr_finish finish) {
    if (!c->ended) {
        c->ended               = true;
        c->stats.finish        = finish;
        c->stats.total_ms      = now_ms() - c->started;
        c->stats.generation_ms = c->stats.total_ms - c->stats.prefill_ms;
        c->stats.context_tokens = c->used;
    }
    piece->part = GEISTR_PART_END;
    piece->text = "";
    piece->len  = 0;
    return GEISTR_OK;
}

/* One token of the "model" through the text stages. */
static bool feed_token(geistr_chat *c) {
    char   token[TOKEN_BYTES + 1], text[TOKEN_BYTES + 8];
    size_t n = c->raw_len - c->raw_pos < TOKEN_BYTES ? c->raw_len - c->raw_pos : TOKEN_BYTES;
    memcpy(token, c->raw + c->raw_pos, n);
    token[n] = 0;
    c->raw_pos += n;
    return str_utf8_feed(&c->utf8, token, text, sizeof text) && str_output_feed(&c->output, text, to_stops, c);
}

geistr_status geistr_chat_next(geistr_chat *c, geistr_piece *piece) {
    if (!c || !piece || piece->size < sizeof(geistr_piece))
        return GEISTR_INVALID;
    if (!c->sent)
        return fail(c, GEISTR_INVALID, "send first");
    for (;;) {
        if (c->ended && c->stats.finish == GEISTR_FINISH_CANCELLED && !c->cancel_reported) {
            c->cancel_reported = true;
            return GEISTR_CANCELLED;
        }
        if (!c->ended && atomic_load(&c->cancel)) {
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
        if (c->ended)
            return end(c, piece, c->stats.finish);
        bool stopped = c->stop.stopped;
        if (stopped || c->raw_pos >= c->raw_len || c->stats.output_tokens >= c->limit) {
            /* Incomplete thinking or prefixes are dropped; held answer text is released. */
            str_output_finish(&c->output);
            if (!str_stops_finish(&c->stop, queue_answer, c))
                return fail(c, GEISTR_NO_MEMORY, "out of memory");
            c->stats.finish = stopped || c->raw_pos >= c->raw_len ? GEISTR_FINISH_STOP
                              : c->limit == c->opts.max_tokens   ? GEISTR_FINISH_LENGTH
                                                                 : GEISTR_FINISH_CONTEXT;
            end(c, piece, c->stats.finish);
            continue; /* deliver what the finish released, then END */
        }
        if (c->model->slow) {
            struct timespec ms = {.tv_nsec = 1000000};
            nanosleep(&ms, nullptr);
        }
        c->stats.output_tokens++;
        c->turns[c->n_turns - 1].tokens++;
        c->used++;
        if (!feed_token(c)) {
            end(c, piece, GEISTR_FINISH_ERROR);
            return fail(c, c->utf8.failed ? GEISTR_BACKEND : GEISTR_NO_MEMORY,
                        c->utf8.failed ? "the model produced invalid UTF-8" : "out of memory");
        }
    }
}

geistr_status geistr_chat_cancel(geistr_chat *c) {
    if (!c)
        return GEISTR_INVALID;
    atomic_store(&c->cancel, true);
    return GEISTR_OK;
}

geistr_status geistr_chat_stats(const geistr_chat *c, geistr_stats *stats) {
    if (!c || !stats || stats->size < sizeof stats->size || stats->size > sizeof c->stats)
        return GEISTR_INVALID;
    size_t size = stats->size;
    memcpy(stats, &c->stats, size);
    stats->size = size;
    return GEISTR_OK;
}

/* No stub model sees: as a model without a vision tower (#92). */
geistr_status geistr_chat_image(geistr_chat *c, const void *data, size_t len) {
    if (!c || !data || !len)
        return GEISTR_INVALID;
    return fail(c, GEISTR_FORMAT, "this model has no vision");
}

const char *geistr_chat_error(const geistr_chat *c) {
    return c ? c->error : "no chat";
}

/* Honest feature-off surface: no synthetic model claims pretrained scoring. */
bool geistr_decision_available(void) { return false; }
geistr_status geistr_decision_capability_get(const geistr_model *m, geistr_decision_capability *out) {
    if (!out || out->size != sizeof *out)
        return GEISTR_INVALID;
    *out = (geistr_decision_capability){.size = sizeof *out};
    if (!m)
        return GEISTR_INVALID;
    out->configured = m->decision.enabled;
    out->profile = m->decision.profile;
    out->backend = "stub";
    return GEISTR_OK;
}
geistr_status geistr_decision_resources_get(const geistr_model *m, geistr_decision_resources *out) {
    if (!out || out->size != sizeof *out)
        return GEISTR_INVALID;
    *out = (geistr_decision_resources){.size = sizeof *out};
    return m ? GEISTR_OK : GEISTR_INVALID;
}
geistr_status geistr_decision_open(geistr_model *m, size_t error_cap, const geistr_decision_opts *opts,
                                   geistr_decision **out, char *error) {
    (void)opts;
    if (out)
        *out = nullptr;
    if (error && error_cap)
        snprintf(error, error_cap, "decision engine is disabled");
    return m && out ? GEISTR_FORMAT : GEISTR_INVALID;
}
void geistr_decision_close(geistr_decision *d) { (void)d; }
geistr_status geistr_decision_score(geistr_decision *d, const geistr_decision_request *r,
                                    geistr_decision_result *out) {
    (void)d;
    (void)r;
    if (!out || out->size != sizeof *out)
        return GEISTR_INVALID;
    *out = (geistr_decision_result){.size = sizeof *out, .best_index = SIZE_MAX};
    return d ? GEISTR_FORMAT : GEISTR_INVALID;
}
geistr_status geistr_decision_reset(geistr_decision *d) { return d ? GEISTR_FORMAT : GEISTR_INVALID; }
geistr_status geistr_decision_cancel(geistr_decision *d) { return d ? GEISTR_FORMAT : GEISTR_INVALID; }
const char *geistr_decision_error(const geistr_decision *d) {
    (void)d;
    return "decision unavailable in stub";
}
geistr_status geistr_decision_plan_get(const geistr_decision *d, geistr_decision_plan *out) {
    (void)d;
    if (!out || out->size != sizeof *out)
        return GEISTR_INVALID;
    *out = (geistr_decision_plan){.size = sizeof *out};
    return GEISTR_FORMAT;
}

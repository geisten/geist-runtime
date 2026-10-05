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
 * the same, repeated, at 1 ms per token; "stub:noformat" has no chat
 * format. A token is 3 bytes of the answer, so tokens cut UTF-8 apart. Input
 * costs (bytes + 3) / 4 tokens per message plus 4 for its markers.
 */
#include "geistr.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_CONTEXT 256u
#define TOKEN_BYTES 3u

const char *geistr_version(void) {
    return "0.1.0-stub";
}

const char *geistr_status_text(geistr_status s) {
    switch (s) {
    case GEISTR_OK:
        return "ok";
    case GEISTR_INVALID:
        return "invalid argument or call order";
    case GEISTR_NO_MEMORY:
        return "out of memory";
    case GEISTR_IO:
        return "model file missing or unreadable";
    case GEISTR_FORMAT:
        return "not a supported model or chat format";
    case GEISTR_CONTEXT:
        return "the conversation does not fit the context window";
    case GEISTR_BACKEND:
        return "the engine failed; reopen the model";
    case GEISTR_CANCELLED:
        return "cancelled";
    }
    return "unknown status";
}

/* Copy caller options of any known size over the defaults (ABI rule). */
static bool opts_copy(void *dst, const void *src, size_t known) {
    if (src == nullptr)
        return true;
    size_t size;
    memcpy(&size, src, sizeof size);
    if (size < sizeof size || size > known)
        return false;
    memcpy(dst, src, size);
    memcpy(dst, &known, sizeof known); /* the copy is a full struct now */
    return true;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec * 1e3 + (double) t.tv_nsec / 1e6;
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
    bool              slow, has_format;
    uint32_t          context;
    char              error[256];
};

static geistr_status model_new(const char              *name,
                               const geistr_model_opts *opts,
                               geistr_model           **out,
                               char                    *error,
                               size_t                   cap) {
    geistr_model_opts o = GEISTR_MODEL_OPTS_INIT;
    if (!opts_copy(&o, opts, sizeof o)) {
        put_error(error, cap, "model options: unknown size");
        return GEISTR_INVALID;
    }
    bool echo = !strcmp(name, "stub:echo"), slow = !strcmp(name, "stub:slow"),
         noformat = !strcmp(name, "stub:noformat");
    if (!echo && !slow && !noformat) {
        put_error(error, cap, "the stub opens only stub:echo, stub:slow and stub:noformat");
        return GEISTR_FORMAT;
    }
    geistr_model *m = calloc(1, sizeof *m);
    if (!m)
        return GEISTR_NO_MEMORY;
    atomic_init(&m->refs, 1);
    m->opts       = o;
    m->slow       = slow;
    m->has_format = !noformat;
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

void geistr_model_close(geistr_model *m) {
    model_release(m);
}

geistr_status geistr_model_info_get(const geistr_model *m, geistr_model_info *info) {
    if (!m || !info)
        return GEISTR_INVALID;
    geistr_model_info full = {
        .size        = sizeof full,
        .arch        = "stub",
        .chat_format = m->has_format ? "chatml" : "unknown",
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

enum phase { PHASE_START, PHASE_THINK, PHASE_ANSWER };

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
    /* decoding: an incomplete UTF-8 tail, decoded text not yet delivered */
    unsigned char carry[4];
    size_t        n_carry;
    char         *text;
    size_t        text_len, text_cap;
    enum phase    phase;
    char         *piece; /* storage behind the last returned piece */
    geistr_stats  stats;
    double        started;
    char          error[256];
};

static uint32_t message_tokens(const char *content) {
    return (uint32_t) ((strlen(content) + 3) / 4) + 4;
}

static void stops_free(char **stops, size_t n) {
    for (size_t i = 0; stops && i < n; i++)
        free(stops[i]);
    free(stops);
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
    geistr_chat *c = calloc(1, sizeof *c);
    char       **stops = o.n_stop ? calloc(o.n_stop, sizeof *stops) : nullptr;
    bool         ok    = c && (!o.n_stop || stops);
    for (size_t i = 0; ok && i < o.n_stop; i++)
        ok = o.stop[i] && *o.stop[i] && (stops[i] = strdup(o.stop[i]));
    if (!ok) {
        geistr_status s = c && (!o.n_stop || stops) ? GEISTR_INVALID : GEISTR_NO_MEMORY;
        stops_free(stops, o.n_stop);
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

void geistr_chat_close(geistr_chat *c) {
    if (!c)
        return;
    stops_free(c->stops, c->opts.n_stop);
    free(c->turns);
    free(c->raw);
    free(c->text);
    free(c->piece);
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
    const char *think = c->opts.reasoning == GEISTR_REASONING_THINK_TAGS ? "<think>Weighing the question.</think>\n" : "";
    size_t      reps  = c->model->slow ? 400 : 1;
    size_t      need  = strlen(think) + reps * (strlen(last) + 8) + 1;
    char       *raw   = malloc(need);
    if (!raw)
        return fail(c, GEISTR_NO_MEMORY, "out of memory");
    size_t at = (size_t) snprintf(raw, need, "%s", think);
    for (size_t r = 0; r < reps; r++)
        at += (size_t) snprintf(raw + at, need - at, "%sEcho: %s", r ? " " : "", last);
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
    c->n_carry         = 0;
    c->text_len        = 0;
    c->phase           = c->opts.reasoning == GEISTR_REASONING_THINK_TAGS ? PHASE_START : PHASE_ANSWER;
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

static bool text_append(geistr_chat *c, const char *bytes, size_t n) {
    if (c->text_len + n + 1 > c->text_cap) {
        size_t cap  = (c->text_len + n + 1) * 2;
        char  *grow = realloc(c->text, cap);
        if (!grow)
            return false;
        c->text     = grow;
        c->text_cap = cap;
    }
    memcpy(c->text + c->text_len, bytes, n);
    c->text_len += n;
    c->text[c->text_len] = 0;
    return true;
}

/* One token of raw bytes → complete code points appended to text. */
static bool decode_token(geistr_chat *c) {
    size_t        n = c->raw_len - c->raw_pos < TOKEN_BYTES ? c->raw_len - c->raw_pos : TOKEN_BYTES;
    unsigned char buf[4 + TOKEN_BYTES];
    memcpy(buf, c->carry, c->n_carry);
    memcpy(buf + c->n_carry, c->raw + c->raw_pos, n);
    size_t total = c->n_carry + n, done = 0;
    c->raw_pos += n;
    while (done < total) {
        unsigned char b    = buf[done];
        size_t        need = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : 4;
        if (done + need > total)
            break;
        done += need;
    }
    c->n_carry = total - done;
    memcpy(c->carry, buf + done, c->n_carry);
    return text_append(c, (const char *) buf, done);
}

/* Length of the longest suffix of text[0..len) that starts tag. */
static size_t partial_tag(const char *text, size_t len, const char *tag) {
    size_t t = strlen(tag);
    for (size_t k = t - 1; k > 0; k--)
        if (len >= k && !memcmp(text + len - k, tag, k))
            return k;
    return 0;
}

static void consume(geistr_chat *c, size_t n) {
    memmove(c->text, c->text + n, c->text_len - n + 1);
    c->text_len -= n;
}

/* Take the next deliverable piece out of text; false if none yet. */
static bool classify(geistr_chat *c, geistr_part *part, size_t *len, size_t *skip, bool final) {
    static const char open[] = "<think>", close[] = "</think>";
    *skip = 0;
    if (c->text_len == 0) /* nothing decoded yet; text may still be nullptr */
        return false;
    if (c->phase == PHASE_START) {
        size_t ws = 0;
        while (ws < c->text_len && strchr(" \n\r\t", c->text[ws]))
            ws++;
        size_t rest = c->text_len - ws;
        if (rest >= sizeof open - 1 && !memcmp(c->text + ws, open, sizeof open - 1)) {
            consume(c, ws + sizeof open - 1);
            c->phase = PHASE_THINK;
        } else if (!final && rest < sizeof open - 1 && !memcmp(c->text + ws, open, rest))
            return false; /* could still become <think> */
        else
            c->phase = PHASE_ANSWER;
    }
    if (c->phase == PHASE_THINK) {
        char *end = strstr(c->text, close);
        if (end) {
            *part = GEISTR_PART_THINKING;
            *len  = (size_t) (end - c->text);
            *skip = sizeof close - 1;
            while (*len + *skip < c->text_len && c->text[*len + *skip] == '\n')
                (*skip)++;
            c->phase = PHASE_ANSWER;
            return true;
        }
        if (final) { /* unfinished thinking is discarded, never shown as answer */
            c->text_len = 0;
            return false;
        }
        *part = GEISTR_PART_THINKING;
        *len  = c->text_len - partial_tag(c->text, c->text_len, close);
        return *len > 0;
    }
    /* Answer: end before the first stop string; hold back a possible start of one. */
    *part       = GEISTR_PART_ANSWER;
    size_t hold = 0;
    for (size_t i = 0; i < c->opts.n_stop; i++) {
        char *hit = strstr(c->text, c->stops[i]);
        if (hit) {
            *len       = (size_t) (hit - c->text);
            c->stopped = true;
            c->text_len = *len; /* the stop string and anything after it are dropped */
            c->text[*len] = 0;
            return *len > 0;
        }
        size_t k = partial_tag(c->text, c->text_len, c->stops[i]);
        hold     = k > hold ? k : hold;
    }
    *len = c->text_len - (final ? 0 : hold);
    return *len > 0;
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
        if (c->ended)
            return end(c, piece, c->stats.finish);
        if (atomic_load(&c->cancel)) {
            end(c, piece, GEISTR_FINISH_CANCELLED);
            snprintf(c->error, sizeof c->error, "cancelled");
            continue;
        }
        bool        exhausted = c->stopped || c->raw_pos >= c->raw_len || c->stats.output_tokens >= c->limit;
        geistr_part part;
        size_t      len, skip;
        if (classify(c, &part, &len, &skip, exhausted)) {
            free(c->piece);
            c->piece = malloc(len + 1);
            if (!c->piece)
                return fail(c, GEISTR_NO_MEMORY, "out of memory");
            memcpy(c->piece, c->text, len);
            c->piece[len] = 0;
            consume(c, len + skip);
            if (part == GEISTR_PART_THINKING && !c->opts.thinking)
                continue;
            if (part == GEISTR_PART_ANSWER && c->stats.first_answer_ms < 0)
                c->stats.first_answer_ms = now_ms() - c->started;
            piece->part = part;
            piece->text = c->piece;
            piece->len  = len;
            return GEISTR_OK;
        }
        exhausted = c->stopped || exhausted;
        if (exhausted) {
            geistr_finish f = c->stopped || c->raw_pos >= c->raw_len ? GEISTR_FINISH_STOP
                              : c->limit == c->opts.max_tokens       ? GEISTR_FINISH_LENGTH
                                                                     : GEISTR_FINISH_CONTEXT;
            return end(c, piece, f);
        }
        if (c->model->slow) {
            struct timespec ms = {.tv_nsec = 1000000};
            nanosleep(&ms, nullptr);
        }
        c->stats.output_tokens++;
        c->turns[c->n_turns - 1].tokens++;
        c->used++;
        if (!decode_token(c))
            return fail(c, GEISTR_NO_MEMORY, "out of memory");
    }
}

geistr_status geistr_chat_cancel(geistr_chat *c) {
    if (!c)
        return GEISTR_INVALID;
    atomic_store(&c->cancel, true);
    return GEISTR_OK;
}

geistr_status geistr_chat_run(geistr_chat         *c,
                              size_t               count,
                              const geistr_message messages[],
                              geistr_emit_fn       emit,
                              void                *context) {
    if (!emit)
        return GEISTR_INVALID;
    geistr_status s     = geistr_chat_send(c, count, messages);
    geistr_piece  piece = {.size = sizeof piece};
    while (s == GEISTR_OK && (s = geistr_chat_next(c, &piece)) == GEISTR_OK) {
        if (piece.part == GEISTR_PART_END)
            return GEISTR_OK;
        if (!emit(context, &piece))
            geistr_chat_cancel(c);
    }
    return s;
}

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

/* stream.c — see stream.h. str_utf8_feed and str_output_* are moved from
 * geist-serve (src/app/core.c, src/app/output.c) with only names changed. */
#include "stream.h"

#include <stdlib.h>
#include <string.h>

bool str_utf8_feed(struct str_utf8 *s, const char *piece, char *out, size_t cap) {
    size_t written = 0;
    if (!cap || s->failed)
        return false;
    out[0] = 0;
    for (const unsigned char *p = (const unsigned char *) piece; *p; ++p) {
        unsigned char c = *p;
        if (!s->used) {
            s->need = c < 0x80                 ? 1
                      : c >= 0xc2 && c <= 0xdf ? 2
                      : c >= 0xe0 && c <= 0xef ? 3
                      : c >= 0xf0 && c <= 0xf4 ? 4
                                               : 0;
            if (!s->need)
                goto invalid;
        } else if (c < 0x80 || c > 0xbf ||
                   (s->used == 1 &&
                    ((s->bytes[0] == 0xe0 && c < 0xa0) || (s->bytes[0] == 0xed && c > 0x9f) ||
                     (s->bytes[0] == 0xf0 && c < 0x90) || (s->bytes[0] == 0xf4 && c > 0x8f))))
            goto invalid;
        s->bytes[s->used++] = c;
        if (s->used == s->need) {
            if (cap - written <= s->used)
                goto invalid;
            memcpy(out + written, s->bytes, s->used);
            written += s->used;
            s->used = s->need = 0;
        }
    }
    out[written] = 0;
    return true;
invalid:
    s->failed = true;
    return false;
}

void str_output_thinking(struct str_output *o, str_emit_fn think) {
    o->think = think;
}
void str_output_init(struct str_output *o, bool think_tags) {
    *o = (struct str_output) {.enabled = think_tags};
    if (!o->enabled)
        o->state = STR_ANSWER;
}
static bool whitespace(char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}
static bool answer(const char *s, str_emit_fn emit, void *ctx) {
    return !*s || emit(ctx, s);
}
bool str_output_feed(struct str_output *o,
                     const char        *s,
                     str_emit_fn emit,
                     void *ctx) {
    static const char open[] = "<think>", close[] = "</think>";
    if (o->state == STR_FINISHED)
        return false;
    while (*s) {
        if (o->state == STR_DISCARD)
            return true;
        if (o->state == STR_ANSWER)
            return answer(s, emit, ctx);
        if (o->state == STR_REASONING) {
            /* The text goes to the optional thinking callback; the last o->closing
             * bytes may start "</think>" and are held back until resolved (#93).
             * Chunks end on UTF-8 character boundaries. */
            char   chunk[512];
            size_t n = o->held_len;
            memcpy(chunk, o->held, n);
            o->held_len = 0;
            bool done   = false;
            while (*s && o->state == STR_REASONING) {
                char c = *s++;
                if (c == open[o->opening])
                    ++o->opening;
                else
                    o->opening = c == open[0] ? 1 : 0;
                if (o->opening == sizeof open - 1) {
                    o->opening = 0;
                    if (++o->depth > 16) {
                        o->state = STR_DISCARD;
                        break;
                    }
                }
                chunk[n++] = c;
                if (c == close[o->closing])
                    ++o->closing;
                else
                    o->closing = c == close[0] ? 1 : 0;
                if (o->closing == sizeof close - 1) {
                    n -= sizeof close - 1; /* the marker is protocol, not thought */
                    o->closing = 0;
                    if (--o->depth)
                        continue;
                    o->opening   = 0;
                    o->state     = STR_PREFIX;
                    o->used      = 0;
                    o->prefix[0] = 0;
                    done         = true;
                    break;
                }
                if (n >= 448 && ((unsigned char) *s & 0xC0) != 0x80) {
                    size_t keep = o->closing;
                    chunk[n - keep] = 0;
                    if (o->think && n > keep && !o->think(ctx, chunk))
                        return false;
                    memmove(chunk, chunk + n - keep, keep);
                    n = keep;
                }
            }
            size_t keep = done || o->state == STR_DISCARD ? 0 : o->closing;
            memcpy(o->held, chunk + n - keep, keep);
            o->held_len = keep;
            chunk[n - keep] = 0;
            if (o->think && n > keep && o->state != STR_DISCARD && !o->think(ctx, chunk))
                return false;
            continue;
        }
        size_t leading = 0;
        while (leading < o->used && whitespace(o->prefix[leading]))
            ++leading;
        /* Retain at most 32 leading whitespace bytes, regardless of generation length. */
        if (leading == o->used && whitespace(*s) && leading >= 32) {
            ++s;
            continue;
        }
        size_t matched = o->used - leading;
        if ((matched == 0 && whitespace(*s)) || *s == open[matched]) {
            o->prefix[o->used++] = *s++;
            o->prefix[o->used]   = 0;
            if (!whitespace(o->prefix[o->used - 1]) && o->used - leading == sizeof open - 1) {
                o->state     = STR_REASONING;
                o->reasoning = true;
                o->depth     = 1;
                o->used      = 0;
                o->prefix[0] = 0;
            }
        } else {
            o->state = STR_ANSWER;
            /* The line break after </think> is layout, not answer. */
            if (!answer(o->prefix + (o->reasoning ? leading : 0), emit, ctx))
                return false;
            o->used = 0;
            return answer(s, emit, ctx);
        }
    }
    return true;
}
void str_output_finish(struct str_output *o) {
    o->used = o->closing = o->opening = o->held_len = 0;
    o->depth = 0;
    memset(o->prefix, 0, sizeof o->prefix);
    o->state = STR_FINISHED;
}

/* ------------------------------------------------------------------------ */
/* Stop strings (new, D11)                                                   */
/* ------------------------------------------------------------------------ */

void str_stops_init(struct str_stops *s, size_t n, const char *const stops[]) {
    *s = (struct str_stops) {.stops = stops, .n_stops = n};
}

void str_stops_free(struct str_stops *s) {
    free(s->held);
    *s = (struct str_stops) {};
}

/* Length of the longest suffix of text[0..len) that is a proper prefix of stop. */
static size_t partial(const char *text, size_t len, const char *stop) {
    size_t n = strlen(stop);
    for (size_t k = n - 1; k > 0; k--)
        if (len >= k && !memcmp(text + len - k, stop, k))
            return k;
    return 0;
}

/* Emit held[0..n) and drop it from held. */
static bool release(struct str_stops *s, size_t n, str_emit_fn emit, void *ctx) {
    if (!n)
        return true;
    char save = s->held[n];
    s->held[n] = 0;
    bool ok    = emit(ctx, s->held);
    s->held[n] = save;
    memmove(s->held, s->held + n, s->held_len - n + 1);
    s->held_len -= n;
    return ok;
}

bool str_stops_feed(struct str_stops *s, const char *text, str_emit_fn emit, void *ctx) {
    if (s->stopped)
        return true;
    if (!s->n_stops)
        return !*text || emit(ctx, text);
    size_t n = strlen(text);
    if (s->held_len + n + 1 > s->held_cap) {
        size_t cap  = (s->held_len + n + 1) * 2;
        char  *grow = realloc(s->held, cap);
        if (!grow)
            return false;
        s->held     = grow;
        s->held_cap = cap;
    }
    memcpy(s->held + s->held_len, text, n + 1);
    s->held_len += n;
    /* The earliest match of any stop string ends the answer. */
    size_t at = s->held_len;
    for (size_t i = 0; i < s->n_stops; i++) {
        char *hit = strstr(s->held, s->stops[i]);
        if (hit && (size_t) (hit - s->held) < at)
            at = (size_t) (hit - s->held);
    }
    if (at < s->held_len) {
        s->stopped = true;
        bool ok    = release(s, at, emit, ctx);
        s->held_len = 0;
        s->held[0]  = 0;
        return ok;
    }
    size_t hold = 0;
    for (size_t i = 0; i < s->n_stops; i++) {
        size_t k = partial(s->held, s->held_len, s->stops[i]);
        hold     = k > hold ? k : hold;
    }
    /* Never split a character: a held-back stop prefix starts on a boundary. */
    return release(s, s->held_len - hold, emit, ctx);
}

bool str_stops_finish(struct str_stops *s, str_emit_fn emit, void *ctx) {
    bool ok = s->stopped || !s->held_len || release(s, s->held_len, emit, ctx);
    s->held_len = 0;
    return ok;
}

/* Each of the last span tokens equals the one p before it. */
static bool periodic(const int32_t *t, size_t n, size_t p, size_t span) {
    size_t i = 0;
    while (i < span && t[n - 1 - i] == t[n - 1 - i - p])
        i++;
    return i == span;
}

bool str_repeats(const int32_t *t, size_t n) {
    enum { MIN_PERIOD = 4, MAX_PERIOD = 256, MIN_TIMES = 3, MIN_TOKENS = 48 };
    for (size_t p = MIN_PERIOD; p <= MAX_PERIOD; p++) {
        size_t times = (MIN_TOKENS + p - 1) / p;
        times        = times < MIN_TIMES ? MIN_TIMES : times;
        if (times * p > n)
            break;
        size_t span = (times - 1) * p;
        if (!periodic(t, n, p, span))
            continue;
        /* A run of 1 to 3 tokens again and again (64 zeros, "| 0 |" cells, a
         * rule of =====) is a unit of data, not a loop: not counted. */
        bool short_unit = false;
        for (size_t d = 1; d < MIN_PERIOD && !short_unit; d++)
            short_unit = periodic(t, n, d, span + p - d);
        return !short_unit;
    }
    return false;
}

bool str_pieces_push(struct str_pieces *q, int part, const char *text) {
    if (!*text)
        return true;
    if (q->n == q->cap) {
        size_t            cap  = q->cap ? q->cap * 2 : 8;
        struct str_piece *grow = realloc(q->items, cap * sizeof *grow);
        if (!grow)
            return false;
        q->items = grow;
        q->cap   = cap;
    }
    size_t len  = strlen(text);
    char  *copy = malloc(len + 1);
    if (!copy)
        return false;
    memcpy(copy, text, len + 1);
    q->items[q->n++] = (struct str_piece) {part, copy, len};
    return true;
}

bool str_pieces_take(struct str_pieces *q, struct str_piece *out) {
    if (q->head == q->n)
        return false;
    *out = q->items[q->head++];
    free(q->taken);
    q->taken = out->text;
    if (q->head == q->n) /* all taken: start over, the array stays small */
        q->n = q->head = 0;
    return true;
}

void str_pieces_clear(struct str_pieces *q) {
    for (size_t i = q->head; i < q->n; i++)
        free(q->items[i].text);
    q->n = q->head = 0;
}

void str_pieces_free(struct str_pieces *q) {
    str_pieces_clear(q);
    free(q->items);
    free(q->taken);
    *q = (struct str_pieces) {};
}

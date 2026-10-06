/* service.c — see service.h. The parts of geist-serve's geistd that a model
 * service needs (socket, one request at a time, cancel on disconnect, a
 * clean stop) and geist-app's conversation matching (#148), nothing else. */
#include "service.h"
#define JSMN_STATIC
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "jsmn.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define REQUEST_MAX (16u << 20)

/* ---- JSON --------------------------------------------------------------- */

struct buf {
    char  *p;
    size_t n, cap;
    bool   failed;
};

static void put_n(struct buf *b, const char *s, size_t n) {
    if (b->failed)
        return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + n + 1)
            cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) {
            b->failed = true;
            return;
        }
        b->p = p, b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void put(struct buf *b, const char *s) {
    put_n(b, s, strlen(s));
}

static void put_json(struct buf *b, const char *s) {
    put(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char) *s;
        char          esc[8];
        if (c == '"' || c == '\\')
            esc[0] = '\\', esc[1] = (char) c, put_n(b, esc, 2);
        else if (c < 0x20)
            snprintf(esc, sizeof esc, "\\u%04x", c), put_n(b, esc, 6);
        else
            put_n(b, s, 1);
    }
    put(b, "\"");
}

struct json {
    const char *s;
    jsmntok_t  *t;
    int         n;
};

static bool parse(struct json *j, const char *s, size_t len) {
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, s, len, nullptr, 0);
    if (n < 1)
        return false;
    j->s = s;
    j->t = malloc((size_t) n * sizeof *j->t);
    jsmn_init(&p);
    j->n = j->t ? jsmn_parse(&p, s, len, j->t, (unsigned) n) : -1;
    return j->n >= 1 && j->t[0].type == JSMN_OBJECT;
}

/* The value token of key in object obj, or -1. */
static int field(const struct json *j, int obj, const char *key) {
    size_t kl = strlen(key);
    for (int i = obj + 1; i + 1 < j->n && j->t[i].start < j->t[obj].end; i++)
        if (j->t[i].parent == obj && j->t[i].type == JSMN_STRING && (size_t) (j->t[i].end - j->t[i].start) == kl &&
            !memcmp(j->s + j->t[i].start, key, kl))
            return i + 1;
    return -1;
}

/* A JSON string token, unescaped (UTF-8; \uXXXX incl. surrogate pairs). */
static char *string(const struct json *j, int t) {
    if (t < 0 || j->t[t].type != JSMN_STRING)
        return nullptr;
    const char *s   = j->s + j->t[t].start;
    size_t      n   = (size_t) (j->t[t].end - j->t[t].start), o = 0;
    char       *out = malloc(n + 1);
    for (size_t i = 0; out && i < n; i++) {
        if (s[i] != '\\' || i + 1 >= n) {
            out[o++] = s[i];
            continue;
        }
        char e = s[++i];
        if (e == 'u' && i + 4 < n) {
            char hex[5] = {s[i + 1], s[i + 2], s[i + 3], s[i + 4], 0};
            unsigned cp = (unsigned) strtoul(hex, nullptr, 16);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < n && s[i + 1] == '\\' && s[i + 2] == 'u') {
                char     lo_hex[5] = {s[i + 3], s[i + 4], s[i + 5], s[i + 6], 0};
                unsigned lo        = (unsigned) strtoul(lo_hex, nullptr, 16);
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00), i += 6;
            }
            if (cp < 0x80)
                out[o++] = (char) cp;
            else if (cp < 0x800)
                out[o++] = (char) (0xC0 | (cp >> 6)), out[o++] = (char) (0x80 | (cp & 0x3F));
            else if (cp < 0x10000)
                out[o++] = (char) (0xE0 | (cp >> 12)), out[o++] = (char) (0x80 | ((cp >> 6) & 0x3F)),
                out[o++] = (char) (0x80 | (cp & 0x3F));
            else
                out[o++] = (char) (0xF0 | (cp >> 18)), out[o++] = (char) (0x80 | ((cp >> 12) & 0x3F)),
                out[o++] = (char) (0x80 | ((cp >> 6) & 0x3F)), out[o++] = (char) (0x80 | (cp & 0x3F));
            continue;
        }
        out[o++] = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
    }
    if (out)
        out[o] = 0;
    return out;
}

static double number(const struct json *j, int t, double dflt) {
    return t >= 0 && j->t[t].type == JSMN_PRIMITIVE ? strtod(j->s + j->t[t].start, nullptr) : dflt;
}

void service_field(const char *line, const char *key, char *out, size_t cap) {
    struct json j = {};
    out[0]        = 0;
    if (parse(&j, line, strlen(line))) {
        int   t = field(&j, 0, key);
        char *s = string(&j, t);
        if (s)
            snprintf(out, cap, "%s", s);
        else if (t >= 0)
            snprintf(out, cap, "%.*s", j.t[t].end - j.t[t].start, line + j.t[t].start);
        free(s);
    }
    free(j.t);
}

/* ---- socket I/O ---------------------------------------------------------- */

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE on the socket instead */
#endif

static bool write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = send(fd, s, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        s += w, n -= (size_t) w;
    }
    return true;
}

static bool send_line(int fd, struct buf *b) {
    put(b, "\n");
    bool ok = !b->failed && write_all(fd, b->p, b->n);
    b->n    = 0;
    return ok;
}

/* One line (without '\n') into a malloc'd buffer; nullptr at EOF or too long. */
static char *read_line(int fd, size_t max) {
    struct buf b = {};
    char       c;
    for (;;) {
        ssize_t r = read(fd, &c, 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0 || b.n >= max) {
            if (r <= 0 && b.n)
                break;
            free(b.p);
            return nullptr;
        }
        if (c == '\n')
            break;
        put_n(&b, &c, 1);
    }
    if (!b.p)
        put(&b, "");
    return b.p;
}

static bool address(const char *path, struct sockaddr_un *a) {
    *a = (struct sockaddr_un) {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof a->sun_path)
        return false;
    strcpy(a->sun_path, path);
    return true;
}

static int dial(const char *path) {
    struct sockaddr_un a;
    if (!address(path, &a))
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (connect(fd, (struct sockaddr *) &a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- the service: conversations ---------------------------------------- */

struct conversation {
    geistr_chat *chat;
    double       temperature;
    size_t       n;
    char       **role, **content;
    uint64_t     used;
};

static void conversation_keep(struct conversation *c, size_t n) {
    for (size_t i = n; i < c->n; i++)
        free(c->role[i]), free(c->content[i]);
    if (n < c->n)
        c->n = n;
}

static bool conversation_push(struct conversation *c, const char *role, const char *content) {
    char **r  = realloc(c->role, (c->n + 1) * sizeof *r);
    if (r)
        c->role = r;
    char **ct = r ? realloc(c->content, (c->n + 1) * sizeof *ct) : nullptr;
    if (!ct)
        return false;
    c->content       = ct;
    c->role[c->n]    = strdup(role);
    c->content[c->n] = strdup(content);
    if (!c->role[c->n] || !c->content[c->n]) {
        free(c->role[c->n]), free(c->content[c->n]);
        return false;
    }
    c->n++;
    return true;
}

static void conversation_close(struct conversation *c) {
    geistr_chat_close(c->chat);
    conversation_keep(c, 0);
    free(c->role), free(c->content);
    *c = (struct conversation) {};
}

static bool same_role(const char *a, const char *b) {
    bool sa = !strcmp(a, "system"), aa = !strcmp(a, "assistant"), sb = !strcmp(b, "system"),
         ab = !strcmp(b, "assistant");
    return sa == sb && aa == ab; /* anything else counts as user, as in the runtime */
}

/* How many of the request's messages (all but the last) this conversation holds. */
static size_t common(const struct conversation *c, const struct svc_message *m, size_t n) {
    size_t k = 0;
    while (k < c->n && k + 1 < n && same_role(c->role[k], m[k].role) && !strcmp(c->content[k], m[k].content))
        k++;
    return k;
}

static void reply_error(int fd, const char *status, const char *text) {
    struct buf b = {};
    put(&b, "{\"error\":"), put_json(&b, text), put(&b, ",\"status\":"), put_json(&b, status), put(&b, "}");
    (void) send_line(fd, &b);
    free(b.p);
}

static void chat_request(const struct svc_options *o, struct conversation *pool, int fd, const struct json *j) {
    static uint64_t clock;
    int             list = field(j, 0, "messages");
    if (list < 0 || j->t[list].type != JSMN_ARRAY || j->t[list].size < 1) {
        reply_error(fd, "invalid", "messages: a list with at least one message");
        return;
    }
    size_t              n = (size_t) j->t[list].size, k = 0;
    struct svc_message *m = calloc(n, sizeof *m);
    for (int i = list + 1; m && i < j->n && k < n; i++)
        if (j->t[i].parent == list) {
            m[k].role    = string(j, field(j, i, "role"));
            m[k].content = string(j, field(j, i, "content"));
            k++;
        }
    bool ok = m && k == n;
    for (size_t i = 0; ok && i < n; i++)
        ok = m[i].role && m[i].content;
    double   temperature = number(j, field(j, 0, "temperature"), 0);
    unsigned max         = (unsigned) number(j, field(j, 0, "max"), 0);
    if (!ok || !(temperature >= 0 && temperature <= 2)) {
        reply_error(fd, "invalid", "each message needs role and content; temperature 0 to 2");
        goto done;
    }
    /* The conversation that holds the most of this one (same temperature);
     * else a free slot, else the least recently used. */
    /* ponytail: a linear scan with full compares; fine for a handful of chats */
    struct conversation *c = nullptr;
    size_t               keep = 0;
    for (size_t i = 0; i < o->chats; i++)
        if (pool[i].chat && pool[i].temperature == temperature) {
            size_t h = common(&pool[i], m, n);
            if (!c || h > keep)
                c = &pool[i], keep = h;
        }
    if (!c || !keep) {
        struct conversation *slot = nullptr;
        for (size_t i = 0; i < o->chats && !slot; i++)
            if (!pool[i].chat)
                slot = &pool[i];
        for (size_t i = 0; i < o->chats && !slot; i++)
            if (!slot || pool[i].used < slot->used)
                slot = &pool[i];
        c    = slot;
        keep = 0;
        if (c->chat && c->temperature != temperature)
            conversation_close(c);
    }
    if (!c->chat) {
        geistr_chat_opts opts = GEISTR_CHAT_OPTS_INIT;
        opts.reasoning        = o->reasoning;
        opts.temperature      = (float) temperature;
        opts.overflow         = GEISTR_OVERFLOW_DROP_OLDEST;
        if (geistr_chat_open(o->model, &opts, &c->chat) != GEISTR_OK) {
            reply_error(fd, "error", "cannot open a chat");
            goto done;
        }
        c->temperature = temperature;
    }
    c->used = ++clock;
    if (keep < c->n && geistr_chat_rewind(c->chat, keep) != GEISTR_OK) {
        geistr_chat_rewind(c->chat, 0); /* the runtime dropped turns: start over */
        keep = 0;
    }
    conversation_keep(c, keep);
    geistr_message *turn = calloc(n - keep, sizeof *turn);
    for (size_t i = keep; turn && i < n; i++)
        turn[i - keep] = (geistr_message) {m[i].role, m[i].content};
    geistr_status s = turn ? geistr_chat_limit(c->chat, max) : GEISTR_NO_MEMORY;
    if (s == GEISTR_OK)
        s = geistr_chat_send(c->chat, n - keep, turn);
    free(turn);
    if (s != GEISTR_OK) {
        reply_error(fd, s == GEISTR_CONTEXT ? "context" : s == GEISTR_INVALID ? "invalid" : "error",
                    geistr_chat_error(c->chat));
        goto done;
    }
    for (size_t i = keep; i < n; i++)
        (void) conversation_push(c, m[i].role, m[i].content);
    /* Stream the answer; a client that goes away stops it. */
    struct buf   line = {}, answer = {};
    geistr_piece p    = {.size = sizeof p};
    bool         gone = false;
    put(&answer, "");
    while ((s = geistr_chat_next(c->chat, &p)) == GEISTR_OK && p.part != GEISTR_PART_END) {
        if (p.part != GEISTR_PART_ANSWER)
            continue;
        put_n(&answer, p.text, p.len);
        put(&line, "{\"part\":\"answer\",\"text\":"), put_json(&line, p.text), put(&line, "}");
        if (!send_line(fd, &line)) {
            gone = true;
            geistr_chat_cancel(c->chat);
            while (geistr_chat_next(c->chat, &p) == GEISTR_OK && p.part != GEISTR_PART_END) {
            }
            break;
        }
    }
    (void) conversation_push(c, "assistant", answer.p ? answer.p : "");
    geistr_stats st = {.size = sizeof st};
    (void) geistr_chat_stats(c->chat, &st);
    if (st.dropped_messages) /* the runtime's conversation is shorter now: match anew next time */
        conversation_keep(c, 0), (void) geistr_chat_rewind(c->chat, 0);
    if (!gone && (s == GEISTR_OK || s == GEISTR_CANCELLED)) {
        static const char *const finish[] = {"none", "stop", "length", "context", "cancelled", "error"};
        char                     tail[512];
        snprintf(tail, sizeof tail,
                 "{\"done\":true,\"finish\":\"%s\",\"input_tokens\":%u,\"context_tokens\":%u,\"output_tokens\":%u,"
                 "\"prefill_ms\":%.1f,\"generation_ms\":%.1f,\"total_ms\":%.1f}",
                 finish[st.finish], st.input_tokens, st.context_tokens, st.output_tokens, st.prefill_ms,
                 st.generation_ms, st.total_ms);
        put(&line, tail);
        (void) send_line(fd, &line);
    } else if (!gone) {
        reply_error(fd, "error", geistr_chat_error(c->chat));
        conversation_close(c);
    }
    free(line.p), free(answer.p);
done:
    for (size_t i = 0; m && i < n; i++)
        free((char *) m[i].role), free((char *) m[i].content);
    free(m);
}

static void info_request(const struct svc_options *o, int fd) {
    geistr_model_info i = {.size = sizeof i};
    (void) geistr_model_info_get(o->model, &i);
    struct buf b = {};
    char       tail[128];
    put(&b, "{\"model\":"), put_json(&b, o->name);
    put(&b, ",\"backend\":"), put_json(&b, i.backend ? i.backend : "cpu");
    put(&b, ",\"chat_format\":"), put_json(&b, i.chat_format ? i.chat_format : "");
    snprintf(tail, sizeof tail, ",\"context\":%u,\"chats\":%zu}", i.context, o->chats);
    put(&b, tail);
    (void) send_line(fd, &b);
    free(b.p);
}

int service_run(const struct svc_options *o) {
    struct sockaddr_un a;
    if (!address(o->socket, &a)) {
        fprintf(stderr, "geistr: socket path too long: %s\n", o->socket);
        return 1;
    }
    int other = dial(o->socket);
    if (other >= 0) { /* never take over a running service's socket */
        close(other);
        fprintf(stderr, "geistr: a service already answers on %s\n", o->socket);
        return 1;
    }
    unlink(o->socket); /* a stale one from a service that died */
    int    fd  = socket(AF_UNIX, SOCK_STREAM, 0);
    mode_t old = umask(0177); /* the socket is the owner's alone (0600) */
    bool   ok  = fd >= 0 && bind(fd, (struct sockaddr *) &a, sizeof a) == 0 && listen(fd, 16) == 0;
    umask(old);
    if (!ok) {
        fprintf(stderr, "geistr: cannot listen on %s: %s\n", o->socket, strerror(errno));
        if (fd >= 0)
            close(fd);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN); /* a vanished client is a failed write, not the end */
    struct conversation *pool = calloc(o->chats, sizeof *pool);
    fprintf(stderr, "geistr: serving %s on %s (%zu conversations)\n", o->name, o->socket, o->chats);
    while (pool && !*o->stop) {
        struct pollfd in = {.fd = fd, .events = POLLIN};
        if (poll(&in, 1, 200) <= 0)
            continue;
        int client = accept(fd, nullptr, nullptr);
        if (client < 0)
            continue;
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        struct timeval wait = {.tv_sec = 30}; /* a client that never sends or never reads */
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof wait);
        char       *request = read_line(client, REQUEST_MAX);
        struct json j       = {};
        if (!request || !parse(&j, request, strlen(request)))
            reply_error(client, "invalid", "one JSON object per line");
        else {
            char *op = string(&j, field(&j, 0, "op"));
            if (op && !strcmp(op, "info"))
                info_request(o, client);
            else if (!op || !strcmp(op, "chat"))
                chat_request(o, pool, client, &j);
            else
                reply_error(client, "invalid", "op is chat or info");
            free(op);
        }
        free(j.t);
        free(request);
        close(client);
    }
    for (size_t i = 0; pool && i < o->chats; i++)
        if (pool[i].chat)
            conversation_close(&pool[i]);
    free(pool);
    close(fd);
    unlink(o->socket);
    fprintf(stderr, "geistr: service stopped\n");
    return 0;
}

/* ---- a client ------------------------------------------------------------ */

geistr_status service_info(const char *path, char *out, size_t cap) {
    int fd = dial(path);
    if (fd < 0)
        return GEISTR_IO;
    char *line = write_all(fd, "{\"op\":\"info\"}\n", 14) ? read_line(fd, 1 << 16) : nullptr;
    close(fd);
    if (!line)
        return GEISTR_IO;
    snprintf(out, cap, "%s", line);
    free(line);
    return GEISTR_OK;
}

geistr_status service_chat(const char *path, size_t n, const struct svc_message *m, unsigned max,
                           double temperature, svc_part_fn part, svc_cancel_fn cancel, void *ctx,
                           struct svc_stats *stats, char *error, size_t cap) {
    *stats = (struct svc_stats) {};
    if (cap)
        error[0] = 0;
    int fd = dial(path);
    if (fd < 0) {
        snprintf(error, cap, "no service on %s", path);
        return GEISTR_IO;
    }
    struct buf b = {};
    char       head[96];
    snprintf(head, sizeof head, "{\"op\":\"chat\",\"max\":%u,\"temperature\":%g,\"messages\":[", max, temperature);
    put(&b, head);
    for (size_t i = 0; i < n; i++) {
        put(&b, i ? ",{\"role\":" : "{\"role\":"), put_json(&b, m[i].role);
        put(&b, ",\"content\":"), put_json(&b, m[i].content), put(&b, "}");
    }
    put(&b, "]}\n");
    bool sent = !b.failed && write_all(fd, b.p, b.n);
    free(b.p);
    geistr_status s = sent ? GEISTR_BACKEND : GEISTR_IO;
    struct buf    in = {};
    while (sent) {
        struct pollfd ready = {.fd = fd, .events = POLLIN};
        if (cancel && cancel(ctx)) { /* closing the connection stops the answer */
            s = GEISTR_CANCELLED;
            snprintf(stats->finish, sizeof stats->finish, "cancelled");
            break;
        }
        if (poll(&ready, 1, 100) <= 0)
            continue;
        char    chunk[4096];
        ssize_t r = read(fd, chunk, sizeof chunk);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            snprintf(error, cap, "the service closed the connection");
            break;
        }
        put_n(&in, chunk, (size_t) r);
        char *nl;
        bool  done = false;
        while (!done && in.p && (nl = memchr(in.p, '\n', in.n))) {
            *nl         = 0;
            struct json j = {};
            if (parse(&j, in.p, (size_t) (nl - in.p))) {
                int   t    = field(&j, 0, "text");
                char *text = string(&j, t);
                if (text) {
                    if (part && !part(ctx, text))
                        cancel = nullptr, done = true, s = GEISTR_CANCELLED;
                } else if (field(&j, 0, "done") >= 0) {
                    char *finish = string(&j, field(&j, 0, "finish"));
                    snprintf(stats->finish, sizeof stats->finish, "%s", finish ? finish : "");
                    free(finish);
                    stats->input_tokens   = (unsigned) number(&j, field(&j, 0, "input_tokens"), 0);
                    stats->context_tokens = (unsigned) number(&j, field(&j, 0, "context_tokens"), 0);
                    stats->output_tokens  = (unsigned) number(&j, field(&j, 0, "output_tokens"), 0);
                    stats->prefill_ms     = number(&j, field(&j, 0, "prefill_ms"), -1);
                    stats->generation_ms  = number(&j, field(&j, 0, "generation_ms"), -1);
                    stats->total_ms       = number(&j, field(&j, 0, "total_ms"), -1);
                    s                     = GEISTR_OK;
                    done                  = true;
                } else {
                    char *status = string(&j, field(&j, 0, "status"));
                    char *why    = string(&j, field(&j, 0, "error"));
                    snprintf(error, cap, "%s", why ? why : "error");
                    s = status && !strcmp(status, "context") ? GEISTR_CONTEXT : GEISTR_BACKEND;
                    free(status), free(why);
                    done = true;
                }
                free(text);
            }
            free(j.t);
            size_t used = (size_t) (nl - in.p) + 1;
            memmove(in.p, in.p + used, in.n - used + 1);
            in.n -= used;
        }
        if (done)
            break;
    }
    free(in.p);
    close(fd);
    return s;
}

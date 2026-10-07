/* service.c — see service.h. The parts of geist-serve's geistd that a model
 * service needs (socket, one request at a time, cancel on disconnect, a
 * clean stop) and geist-app's conversation matching (#148), nothing else. */
#include "service.h"
#include "json.h"
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

/* One line (without '\n') into a malloc'd string; nullptr at EOF or longer than max. */
static char *read_line(int fd, size_t max) {
    char   *line = nullptr, c;
    size_t  len = 0, got = 0;
    FILE   *m = open_memstream(&line, &len);
    ssize_t r = 0;
    while (m && got <= max && ((r = read(fd, &c, 1)) == 1 || (r < 0 && errno == EINTR)))
        if (r == 1 && c == '\n')
            break;
        else if (r == 1)
            fputc(c, m), got++;
    if (m)
        fclose(m);
    if (got > max || (r != 1 && !got)) { /* too long, or nothing before the end */
        free(line);
        return nullptr;
    }
    return line;
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
static size_t common(const struct conversation *c, const geistr_message *m, size_t n) {
    size_t k = 0;
    while (k < c->n && k + 1 < n && same_role(c->role[k], m[k].role) && !strcmp(c->content[k], m[k].content))
        k++;
    return k;
}

static void reply_error(FILE *out, const char *status, const char *text) {
    fputs("{\"error\":", out), json_write(out, text), fputs(",\"status\":", out), json_write(out, status);
    fputs("}\n", out);
}

static void chat_request(const struct svc_options *o, struct conversation *pool, FILE *out, const struct json *j) {
    static uint64_t clock;
    int             list = json_field(j, 0, "messages");
    if (json_count(j, list) < 1) {
        reply_error(out, "invalid", "messages: a list with at least one message");
        return;
    }
    size_t          n = (size_t) json_count(j, list);
    geistr_message *m = calloc(n, sizeof *m);
    bool            ok = m;
    for (size_t k = 0; ok && k < n; k++) {
        int item     = json_item(j, list, (int) k);
        m[k].role    = json_string(j, json_field(j, item, "role"));
        m[k].content = json_string(j, json_field(j, item, "content"));
        ok           = m[k].role && m[k].content;
    }
    double   temperature = json_number(j, json_field(j, 0, "temperature"), 0);
    unsigned max         = (unsigned) json_number(j, json_field(j, 0, "max"), 0);
    if (!ok || !(temperature >= 0 && temperature <= 2)) {
        reply_error(out, "invalid", "each message needs role and content; temperature 0 to 2");
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
            reply_error(out, "error", "cannot open a chat");
            goto done;
        }
        c->temperature = temperature;
    }
    c->used = ++clock;
    if (keep < c->n && geistr_chat_rewind(c->chat, keep) != GEISTR_OK) {
        (void) geistr_chat_rewind(c->chat, 0); /* the runtime dropped turns: start over (0 always works) */
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
        reply_error(out, s == GEISTR_CONTEXT ? "context" : s == GEISTR_INVALID ? "invalid" : "error",
                    geistr_chat_error(c->chat));
        goto done;
    }
    for (size_t i = keep; i < n; i++)
        (void) conversation_push(c, m[i].role, m[i].content);
    /* Stream the answer; a client that goes away stops it. */
    char        *text = nullptr;
    size_t       text_len = 0;
    FILE        *answer   = open_memstream(&text, &text_len);
    geistr_piece p        = {.size = sizeof p};
    bool         gone     = false;
    while ((s = geistr_chat_next(c->chat, &p)) == GEISTR_OK && p.part != GEISTR_PART_END) {
        if (p.part != GEISTR_PART_ANSWER)
            continue;
        if (answer)
            fwrite(p.text, 1, p.len, answer);
        fputs("{\"part\":\"answer\",\"text\":", out), json_write(out, p.text), fputs("}\n", out);
        if (fflush(out) == EOF) {
            gone = true;
            geistr_chat_cancel(c->chat);
            while (geistr_chat_next(c->chat, &p) == GEISTR_OK && p.part != GEISTR_PART_END) {
            }
            break;
        }
    }
    if (answer)
        fclose(answer);
    (void) conversation_push(c, "assistant", text ? text : "");
    free(text);
    geistr_stats st = {.size = sizeof st};
    (void) geistr_chat_stats(c->chat, &st);
    if (st.dropped_messages) /* the runtime's conversation is shorter now: match anew next time */
        conversation_keep(c, 0), (void) geistr_chat_rewind(c->chat, 0);
    if (!gone && (s == GEISTR_OK || s == GEISTR_CANCELLED)) {
        static const char *const finish[] = {"none", "stop", "length", "context", "cancelled", "error"};
        fprintf(out,
                 "{\"done\":true,\"finish\":\"%s\",\"input_tokens\":%u,\"context_tokens\":%u,\"output_tokens\":%u,"
                 "\"prefill_ms\":%.1f,\"generation_ms\":%.1f,\"total_ms\":%.1f}\n",
                 finish[st.finish], st.input_tokens, st.context_tokens, st.output_tokens, st.prefill_ms,
                 st.generation_ms, st.total_ms);
    } else if (!gone) {
        reply_error(out, "error", geistr_chat_error(c->chat));
        conversation_close(c);
    }
done:
    for (size_t i = 0; m && i < n; i++)
        free((char *) m[i].role), free((char *) m[i].content);
    free(m);
}

static void info_request(const struct svc_options *o, FILE *out) {
    geistr_model_info i = {.size = sizeof i};
    (void) geistr_model_info_get(o->model, &i);
    fputs("{\"model\":", out), json_write(out, o->name);
    fputs(",\"backend\":", out), json_write(out, i.backend ? i.backend : "cpu");
    fputs(",\"chat_format\":", out), json_write(out, i.chat_format ? i.chat_format : "");
    fprintf(out, ",\"context\":%u,\"chats\":%zu}\n", i.context, o->chats);
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
        FILE       *out     = fdopen(client, "w"); /* closes client */
        struct json j       = {};
        if (!out)
            close(client);
        else if (!request || !json_parse(&j, request, strlen(request)))
            reply_error(out, "invalid", "one JSON object per line");
        else {
            char *op = json_string(&j, json_field(&j, 0, "op"));
            if (op && !strcmp(op, "info"))
                info_request(o, out);
            else if (!op || !strcmp(op, "chat"))
                chat_request(o, pool, out, &j);
            else
                reply_error(out, "invalid", "op is chat or info");
            free(op);
        }
        json_free(&j);
        free(request);
        if (out)
            fclose(out);
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

geistr_status service_chat(const char *path, size_t n, const geistr_message *m, unsigned max,
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
    char  *request = nullptr;
    size_t len     = 0;
    FILE  *req     = open_memstream(&request, &len);
    if (req) {
        fprintf(req, "{\"op\":\"chat\",\"max\":%u,\"temperature\":%g,\"messages\":[", max, temperature);
        for (size_t i = 0; i < n; i++) {
            fputs(i ? ",{\"role\":" : "{\"role\":", req), json_write(req, m[i].role);
            fputs(",\"content\":", req), json_write(req, m[i].content), fputs("}", req);
        }
        fputs("]}\n", req);
    }
    bool sent = req && fclose(req) == 0 && write_all(fd, request, len);
    free(request);
    geistr_status s  = sent ? GEISTR_BACKEND : GEISTR_IO;
    char         *in = nullptr; /* what came, up to the next line */
    size_t        n_in = 0;
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
        char *grown = realloc(in, n_in + (size_t) r + 1);
        if (!grown)
            break;
        in = grown;
        memcpy(in + n_in, chunk, (size_t) r);
        n_in += (size_t) r;
        in[n_in] = 0;
        char *nl;
        bool  done = false;
        while (!done && (nl = memchr(in, '\n', n_in))) {
            *nl           = 0;
            struct json j = {};
            if (json_parse(&j, in, (size_t) (nl - in))) {
                char *text = json_string(&j, json_field(&j, 0, "text"));
                if (text) {
                    if (part && !part(ctx, text))
                        cancel = nullptr, done = true, s = GEISTR_CANCELLED;
                } else if (json_field(&j, 0, "done") >= 0) {
                    char *finish = json_string(&j, json_field(&j, 0, "finish"));
                    snprintf(stats->finish, sizeof stats->finish, "%s", finish ? finish : "");
                    free(finish);
                    stats->input_tokens   = (unsigned) json_number(&j, json_field(&j, 0, "input_tokens"), 0);
                    stats->context_tokens = (unsigned) json_number(&j, json_field(&j, 0, "context_tokens"), 0);
                    stats->output_tokens  = (unsigned) json_number(&j, json_field(&j, 0, "output_tokens"), 0);
                    stats->prefill_ms     = json_number(&j, json_field(&j, 0, "prefill_ms"), -1);
                    stats->generation_ms  = json_number(&j, json_field(&j, 0, "generation_ms"), -1);
                    stats->total_ms       = json_number(&j, json_field(&j, 0, "total_ms"), -1);
                    s                     = GEISTR_OK;
                    done                  = true;
                } else {
                    char *status = json_string(&j, json_field(&j, 0, "status"));
                    char *why    = json_string(&j, json_field(&j, 0, "error"));
                    snprintf(error, cap, "%s", why ? why : "error");
                    s = status && !strcmp(status, "context") ? GEISTR_CONTEXT : GEISTR_BACKEND;
                    free(status), free(why);
                    done = true;
                }
                free(text);
            }
            json_free(&j);
            size_t used = (size_t) (nl - in) + 1;
            memmove(in, in + used, n_in - used + 1);
            n_in -= used;
        }
        if (done)
            break;
    }
    free(in);
    close(fd);
    return s;
}

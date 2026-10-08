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
#include <time.h>
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

/* One line (without '\n') into a malloc'd string; nullptr at EOF or longer
 * than max. What follows the '\n' is dropped: one line per connection. */
static char *read_line(int fd, size_t max) {
    char    buf[4096], *line = nullptr, *nl = nullptr;
    size_t  len = 0, got = 0;
    FILE   *m = open_memstream(&line, &len);
    ssize_t r = 0;
    while (m && !nl && got <= max && ((r = read(fd, buf, sizeof buf)) > 0 || (r < 0 && errno == EINTR)))
        if (r > 0) {
            nl       = memchr(buf, '\n', (size_t) r);
            size_t n = nl ? (size_t) (nl - buf) : (size_t) r;
            fwrite(buf, 1, n, m), got += n;
        }
    if (m)
        fclose(m);
    if (got > max || (!nl && !got)) { /* too long, or nothing before the end */
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

/* A conversation the service holds: its chat (KV cache), the options it was
 * opened with, and the messages in it. */
struct held {
    geistr_chat *chat;
    double       temperature;
    char        *stop; /* the stop strings, joined by \x1f; nullptr for none */
    size_t       n;
    char       **role, **content;
    uint64_t     used;
};

static void held_keep(struct held *c, size_t n) {
    for (size_t i = n; i < c->n; i++)
        free(c->role[i]), free(c->content[i]);
    if (n < c->n)
        c->n = n;
}

static bool held_push(struct held *c, const char *role, const char *content) {
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

static void held_close(struct held *c) {
    geistr_chat_close(c->chat);
    held_keep(c, 0);
    free(c->role), free(c->content), free(c->stop);
    *c = (struct held) {};
}

/* The stop strings as one key (nullptr for none). */
static char *stop_key(const struct svc_request *r) {
    char  *key = nullptr;
    size_t len = 0;
    FILE  *f   = r->n_stop ? open_memstream(&key, &len) : nullptr;
    for (size_t i = 0; f && i < r->n_stop; i++)
        fprintf(f, "%s%s", i ? "\x1f" : "", r->stop[i]);
    if (f)
        fclose(f);
    return key;
}

static bool same_options(const struct held *c, double temperature, const char *stop) {
    return c->temperature == temperature && (c->stop ? stop && !strcmp(c->stop, stop) : !stop);
}

static bool same_role(const char *a, const char *b) {
    bool sa = !strcmp(a, "system"), aa = !strcmp(a, "assistant"), sb = !strcmp(b, "system"),
         ab = !strcmp(b, "assistant");
    return sa == sb && aa == ab; /* anything else counts as user, as in the runtime */
}

/* How many of the request's messages (all but the last) this conversation holds. */
static size_t common(const struct held *c, const geistr_message *m, size_t n) {
    size_t k = 0;
    while (k < c->n && k + 1 < n && same_role(c->role[k], m[k].role) && !strcmp(c->content[k], m[k].content))
        k++;
    return k;
}

const char *svc_finish(geistr_finish finish) {
    static const char *const names[] = {"none", "stop", "length", "context", "cancelled", "error", "repetition"};
    return finish < sizeof names / sizeof *names ? names[finish] : "error";
}

void svc_free_request(struct svc_request *r) {
    for (size_t i = 0; r->messages && i < r->n; i++)
        free((char *) r->messages[i].role), free((char *) r->messages[i].content);
    for (size_t i = 0; r->stop && i < r->n_stop; i++)
        free((char *) r->stop[i]);
    free(r->messages), free((void *) r->stop), free(r->image);
    *r = (struct svc_request) {};
}

static void reply_error(FILE *out, const char *status, const char *text) {
    fputs("{\"error\":", out), json_write(out, text), fputs(",\"status\":", out), json_write(out, status);
    fputs("}\n", out);
}

void svc_chat(const struct svc_options *o, struct held *pool, const struct svc_request *r,
              const struct svc_sink *out) {
    static uint64_t       clock;
    const geistr_message *m = r->messages;
    size_t                n = r->n;
    if (!n || !(r->temperature >= 0 && r->temperature <= 2)) {
        out->error(out->ctx, GEISTR_INVALID, "at least one message; temperature 0 to 2");
        return;
    }
    if (o->embedding) {
        out->error(out->ctx, GEISTR_INVALID, "an embedding model does not chat: POST /v1/embeddings or /api/embed");
        return;
    }
    char *stop = stop_key(r);
    /* The conversation that holds the most of this one (same options); else
     * a free slot, else the least recently used. */
    /* ponytail: a linear scan with full compares; fine for a handful of chats */
    struct held *c    = nullptr;
    size_t       keep = 0;
    for (size_t i = 0; i < o->chats; i++)
        if (pool[i].chat && same_options(&pool[i], r->temperature, stop)) {
            size_t h = common(&pool[i], m, n);
            if (!c || h > keep)
                c = &pool[i], keep = h;
        }
    if (!c || !keep) {
        struct held *slot = nullptr;
        for (size_t i = 0; i < o->chats && !slot; i++)
            if (!pool[i].chat)
                slot = &pool[i];
        for (size_t i = 0; i < o->chats && !slot; i++)
            if (!slot || pool[i].used < slot->used)
                slot = &pool[i];
        c    = slot;
        keep = 0;
        if (c->chat && !same_options(c, r->temperature, stop))
            held_close(c);
    }
    if (!c->chat) {
        geistr_chat_opts opts = GEISTR_CHAT_OPTS_INIT;
        opts.reasoning        = o->reasoning;
        opts.temperature      = (float) r->temperature;
        opts.overflow         = GEISTR_OVERFLOW_DROP_OLDEST;
        opts.thinking         = 1; /* its pieces keep a stream alive */
        opts.stop             = r->stop;
        opts.n_stop           = r->n_stop;
        if (geistr_chat_open(o->model, &opts, &c->chat) != GEISTR_OK) {
            out->error(out->ctx, GEISTR_INVALID, geistr_model_error(o->model)); /* e.g. an empty stop string */
            free(stop);
            return;
        }
        c->temperature = r->temperature;
        c->stop        = stop, stop = nullptr;
    }
    free(stop);
    c->used = ++clock;
    if (keep < c->n && geistr_chat_rewind(c->chat, keep) != GEISTR_OK) {
        (void) geistr_chat_rewind(c->chat, 0); /* the runtime dropped turns: start over (0 always works) */
        keep = 0;
    }
    held_keep(c, keep);
    geistr_status s = geistr_chat_limit(c->chat, r->max);
    if (s == GEISTR_OK && r->image && geistr_chat_image(c->chat, r->image, r->image_len) != GEISTR_OK) {
        out->error(out->ctx, GEISTR_INVALID, geistr_chat_error(c->chat)); /* no vision, not an image */
        return;
    }
    if (s == GEISTR_OK)
        s = geistr_chat_send(c->chat, n - keep, m + keep);
    if (s != GEISTR_OK) {
        out->error(out->ctx, s, geistr_chat_error(c->chat));
        return;
    }
    for (size_t i = keep; i < n; i++)
        (void) held_push(c, m[i].role, m[i].content);
    /* Stream the answer; a client that goes away stops it. */
    char        *text     = nullptr;
    size_t       text_len = 0;
    FILE        *answer   = open_memstream(&text, &text_len);
    geistr_piece p        = {.size = sizeof p};
    bool         gone     = out->alive && !out->alive(out->ctx); /* accepted: a stream's headers go now */
    time_t       beat     = time(nullptr);
    while (!gone && (s = geistr_chat_next(c->chat, &p)) == GEISTR_OK && p.part != GEISTR_PART_END) {
        if (*o->stop) /* SIGTERM: the answer ends now, as stopped, and the service after it */
            geistr_chat_cancel(c->chat);
        if (p.part == GEISTR_PART_THINKING) { /* hidden reasoning: a sign of life about once a second */
            if (out->alive && time(nullptr) != beat)
                beat = time(nullptr), gone = !out->alive(out->ctx);
        } else if (p.part == GEISTR_PART_ANSWER) {
            if (answer)
                fwrite(p.text, 1, p.len, answer);
            gone = !out->part(out->ctx, p.text, p.len);
        }
    }
    if (gone) {
        geistr_chat_cancel(c->chat);
        while (geistr_chat_next(c->chat, &p) == GEISTR_OK && p.part != GEISTR_PART_END) {
        }
    }
    if (answer)
        fclose(answer);
    (void) held_push(c, "assistant", text ? text : "");
    geistr_stats st = {.size = sizeof st};
    (void) geistr_chat_stats(c->chat, &st);
    if (st.dropped_messages) /* the runtime's conversation is shorter now: match anew next time */
        held_keep(c, 0), (void) geistr_chat_rewind(c->chat, 0);
    if (!gone && (s == GEISTR_OK || s == GEISTR_CANCELLED))
        out->done(out->ctx, &st, text ? text : "");
    else if (!gone) {
        out->error(out->ctx, GEISTR_BACKEND, geistr_chat_error(c->chat));
        held_close(c);
    }
    free(text);
}

/* ---- the socket protocol: one JSON object per line ----------------------- */

static bool line_part(void *ctx, const char *text, size_t len) {
    (void) len;
    FILE *out = ctx;
    fputs("{\"part\":\"answer\",\"text\":", out), json_write(out, text), fputs("}\n", out);
    return fflush(out) != EOF;
}

static void line_done(void *ctx, const geistr_stats *st, const char *answer) {
    (void) answer;
    fprintf(ctx,
            "{\"done\":true,\"finish\":\"%s\",\"input_tokens\":%u,\"context_tokens\":%u,\"output_tokens\":%u,"
            "\"prefill_ms\":%.1f,\"generation_ms\":%.1f,\"total_ms\":%.1f}\n",
            svc_finish(st->finish), st->input_tokens, st->context_tokens, st->output_tokens, st->prefill_ms,
            st->generation_ms, st->total_ms);
}

static void line_error(void *ctx, geistr_status s, const char *text) {
    reply_error(ctx, s == GEISTR_CONTEXT ? "context" : s == GEISTR_INVALID ? "invalid" : "error", text);
}

static void chat_request(const struct svc_options *o, struct held *pool, FILE *out, const struct json *j) {
    int    list = json_field(j, 0, "messages");
    size_t n    = json_count(j, list) > 0 ? (size_t) json_count(j, list) : 0;
    struct svc_request r = {.messages    = calloc(n ? n : 1, sizeof *r.messages),
                            .n           = n,
                            .temperature = json_number(j, json_field(j, 0, "temperature"), 0),
                            .max         = (unsigned) json_number(j, json_field(j, 0, "max"), 0)};
    bool ok   = r.messages && n;
    int  item = -1;
    for (size_t k = 0; ok && k < n; k++) {
        item                  = json_next(j, list, item);
        r.messages[k].role    = json_string(j, json_field(j, item, "role"));
        r.messages[k].content = json_string(j, json_field(j, item, "content"));
        ok                    = r.messages[k].role && r.messages[k].content;
    }
    if (ok)
        svc_chat(o, pool, &r, &(struct svc_sink) {out, line_part, line_done, line_error, nullptr});
    else
        reply_error(out, "invalid", "messages: a list of messages, each with role and content");
    svc_free_request(&r);
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
    bool loopback = true;
    int  web      = o->http ? http_listen(o->http, &loopback) : -1;
    if (o->http && web < 0) {
        close(fd), unlink(o->socket);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN); /* a vanished client is a failed write, not the end */
    struct held *pool = calloc(o->chats, sizeof *pool);
    fprintf(stderr, "geistr: serving %s on %s%s%s (%zu conversations)\n", o->name, o->socket,
            o->http ? " and http://" : "", o->http ? o->http : "", o->chats);
    while (pool && !*o->stop) {
        struct pollfd in[2] = {{.fd = fd, .events = POLLIN}, {.fd = web, .events = POLLIN}};
        if (poll(in, web >= 0 ? 2 : 1, 200) <= 0)
            continue;
        bool from_web = web >= 0 && (in[1].revents & POLLIN);
        int  client   = accept(from_web ? web : fd, nullptr, nullptr);
        if (client < 0)
            continue;
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        struct timeval wait = {.tv_sec = 30}; /* a client that never sends or never reads */
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof wait);
        if (from_web) {
            http_serve(o, pool, client, loopback);
            continue;
        }
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
            held_close(&pool[i]);
    free(pool);
    close(fd);
    if (web >= 0)
        close(web);
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

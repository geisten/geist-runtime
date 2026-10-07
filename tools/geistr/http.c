/* http.c — geistr serve --http: the OpenAI and Ollama chat APIs on the
 * service's core (see service.h). HTTP/1.1, one request per connection,
 * Content-Length bodies; a stream ends with the connection.
 *
 * No authentication: it listens on loopback unless told otherwise, and then
 * answers only requests whose Host is a loopback name, so a web page cannot
 * reach it through DNS rebinding. No CORS headers: other origins' pages
 * cannot read its answers. */
#include "geistr.h"
#include "json.h"
#include "service.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HEAD_MAX (64u << 10)
#define BODY_MAX (16u << 20)

/* ---- listening ----------------------------------------------------------- */

static bool is_loopback(const struct sockaddr *a) {
    if (a->sa_family == AF_INET)
        return ntohl(((const struct sockaddr_in *) (const void *) a)->sin_addr.s_addr) >> 24 == 127;
    return a->sa_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&((const struct sockaddr_in6 *) (const void *) a)->sin6_addr);
}

int http_listen(const char *where, bool *loopback) {
    const char *colon = where[0] == '[' ? strstr(where, "]:") : strrchr(where, ':');
    char        host[256];
    if (!colon) {
        fprintf(stderr, "geistr: --http=ADDR:PORT, e.g. 127.0.0.1:11434\n");
        return -1;
    }
    if (where[0] == '[') /* [::1]:11434 */
        snprintf(host, sizeof host, "%.*s", (int) (colon - where - 1), where + 1), colon++;
    else
        snprintf(host, sizeof host, "%.*s", (int) (colon - where), where);
    struct addrinfo hints = {.ai_flags = AI_PASSIVE | AI_NUMERICSERV, .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res  = nullptr;
    int              e    = getaddrinfo(host[0] ? host : "127.0.0.1", colon + 1, &hints, &res);
    if (e) {
        fprintf(stderr, "geistr: --http=%s: %s\n", where, gai_strerror(e));
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, 0), one = 1;
    if (fd >= 0)
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    bool ok   = fd >= 0 && bind(fd, res->ai_addr, res->ai_addrlen) == 0 && listen(fd, 16) == 0;
    *loopback = is_loopback(res->ai_addr);
    freeaddrinfo(res);
    if (!ok) {
        fprintf(stderr, "geistr: cannot listen on %s: %s\n", where, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    if (!*loopback)
        fprintf(stderr, "geistr: warning: the HTTP API on %s has no authentication: whoever reaches it can use "
                        "the model\n",
                where);
    return fd;
}

/* ---- a request ------------------------------------------------------------- */

struct request {
    char  method[8], path[256], host[256];
    char *body; /* NUL-terminated */
    long  length;
};

/* Read one request; 0, or the HTTP status to answer with. */
static int read_request(int fd, struct request *r) {
    char  *head = malloc(HEAD_MAX + 1), *end = nullptr;
    size_t n    = 0;
    while (head && !end && n < HEAD_MAX) {
        ssize_t got = read(fd, head + n, HEAD_MAX - n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        n += (size_t) got;
        head[n] = 0;
        end     = strstr(head, "\r\n\r\n");
    }
    int status = !head ? 500 : !end ? (n >= HEAD_MAX ? 431 : 400) : 0;
    if (!status && sscanf(head, "%7s %255s HTTP/1.", r->method, r->path) != 2)
        status = 400;
    bool chunked = false;
    r->length    = 0;
    for (char *line = status ? end : strstr(head, "\r\n") + 2; line && line < end;) {
        char *next = strstr(line, "\r\n");
        *next      = 0;
        char *colon = strchr(line, ':');
        if (colon) {
            *colon      = 0;
            char *value = colon + 1 + strspn(colon + 1, " \t");
            if (!strcasecmp(line, "content-length"))
                r->length = strtol(value, nullptr, 10);
            else if (!strcasecmp(line, "host"))
                snprintf(r->host, sizeof r->host, "%s", value);
            else if (!strcasecmp(line, "transfer-encoding"))
                chunked = strstr(value, "chunked") || strstr(value, "Chunked"); /* no strcasestr in POSIX */
        }
        line = next + 2;
    }
    if (!status && chunked)
        status = 411; /* a body needs Content-Length */
    if (!status && (r->length < 0 || r->length > BODY_MAX))
        status = 413;
    if (!status) {
        r->path[strcspn(r->path, "?")] = 0;
        size_t have = n - (size_t) (end + 4 - head), want = (size_t) r->length;
        r->body     = malloc(want + 1);
        if (!r->body)
            status = 500;
        else {
            memcpy(r->body, end + 4, have < want ? have : want);
            for (size_t got = have; got < want;) {
                ssize_t more = read(fd, r->body + got, want - got);
                if (more < 0 && errno == EINTR)
                    continue;
                if (more <= 0) {
                    status = 400;
                    break;
                }
                got += (size_t) more;
            }
            r->body[want] = 0;
        }
    }
    free(head);
    return status;
}

/* The Host header names this computer's loopback (or is absent: not a browser). */
static bool host_is_loopback(const char *host) {
    char name[256];
    if (!host[0])
        return true;
    if (host[0] == '[')
        snprintf(name, sizeof name, "%.*s", (int) strcspn(host + 1, "]"), host + 1);
    else
        snprintf(name, sizeof name, "%.*s", (int) strcspn(host, ":"), host);
    return !strcasecmp(name, "localhost") || !strncmp(name, "127.", 4) || !strcmp(name, "::1");
}

/* ---- responses ------------------------------------------------------------- */

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    default: return "Internal Server Error";
    }
}

static void respond(FILE *out, int status, const char *type, const char *body) {
    fprintf(out, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", status,
            reason(status), type, strlen(body), body);
}

/* A stream: no length, it ends with the connection. */
static void stream_begin(FILE *out, const char *type) {
    fprintf(out, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", type);
}

/* JSON built in memory, then answered with its length. */
struct text {
    char  *s;
    size_t n;
    FILE  *f;
};

static FILE *text_open(struct text *t) {
    *t = (struct text) {};
    return t->f = open_memstream(&t->s, &t->n);
}

static void respond_json(FILE *out, int status, struct text *t) {
    if (t->f)
        fclose(t->f), t->f = nullptr;
    respond(out, status, "application/json", t->s ? t->s : "{}");
    free(t->s), t->s = nullptr;
}

static void error_json(FILE *out, int status, bool openai, const char *message, const char *code) {
    struct text t;
    FILE       *f = text_open(&t);
    if (!f) {
        respond(out, 500, "application/json", "{}");
        return;
    }
    if (openai) {
        fputs("{\"error\":{\"message\":", f), json_write(f, message);
        fprintf(f, ",\"type\":\"%s\",\"code\":", status >= 500 ? "server_error" : "invalid_request_error");
        code ? json_write(f, code) : (void) fputs("null", f);
        fputs("}}", f);
    } else
        fputs("{\"error\":", f), json_write(f, message), fputc('}', f);
    respond_json(out, status, &t);
}

static void iso_time(char out[32]) {
    time_t now = time(nullptr);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
}

/* ---- requests both APIs share --------------------------------------------- */

/* A message's content: a string, or OpenAI's [{type: text, text}] parts joined. */
static char *content_of(const struct json *j, int t) {
    char *s = json_string(j, t);
    if (s || json_count(j, t) < 0)
        return s ? s : t >= 0 ? strdup("") : nullptr; /* null content: an assistant's tool call */
    struct text all;
    FILE       *f = text_open(&all);
    for (int k = 0; f && k < json_count(j, t); k++) {
        char *part = json_string(j, json_field(j, json_item(j, t, k), "text"));
        if (part)
            fputs(part, f);
        free(part);
    }
    if (f)
        fclose(f);
    return all.s ? all.s : strdup("");
}

static bool read_messages(const struct json *j, struct svc_request *r) {
    int list = json_field(j, 0, "messages");
    int n    = json_count(j, list);
    if (n < 1 || !(r->messages = calloc((size_t) n, sizeof *r->messages)))
        return false;
    r->n = (size_t) n;
    for (int k = 0; k < n; k++) {
        int   item = json_item(j, list, k);
        char *role = json_string(j, json_field(j, item, "role"));
        if (role && !strcmp(role, "developer")) /* OpenAI's newer name for system */
            free(role), role = strdup("system");
        r->messages[k].role    = role;
        r->messages[k].content = content_of(j, json_field(j, item, "content"));
        if (!r->messages[k].role || !r->messages[k].content)
            return false;
    }
    return true;
}

/* stop: a string or a list of strings. */
static void read_stop(const struct json *j, int t, struct svc_request *r) {
    char *one = json_string(j, t);
    int   n   = one ? 1 : json_count(j, t);
    char **stop = n > 0 ? calloc((size_t) n, sizeof *stop) : nullptr;
    for (int k = 0; stop && k < n; k++)
        stop[k] = one ? one : json_string(j, json_item(j, t, k));
    if (!stop)
        free(one);
    r->stop   = (const char *const *) stop;
    r->n_stop = stop ? (size_t) n : 0;
    for (size_t k = 0; k < r->n_stop; k++)
        if (!stop[k]) /* not a string: no stop at all */
            r->n_stop = k;
}

static double clamp(double t) {
    return t < 0 ? 0 : t > 2 ? 2 : t;
}

/* ---- OpenAI: /v1/chat/completions ------------------------------------------- */

struct openai {
    FILE       *out;
    const char *model;
    char        id[32];
    long long   created;
    bool        stream, usage, started;
    struct text text; /* the answer, without a stream */
};

static void openai_chunk(struct openai *a, const char *delta, const char *finish) {
    fprintf(a->out, "data: {\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%lld,\"model\":", a->id,
            a->created);
    json_write(a->out, a->model);
    fprintf(a->out, ",\"choices\":[{\"index\":0,\"delta\":%s,\"finish_reason\":", delta);
    finish ? (void) fprintf(a->out, "\"%s\"", finish) : (void) fputs("null", a->out);
    fputs("}]}\n\n", a->out);
}

static void openai_start(struct openai *a) {
    if (a->started)
        return;
    a->started = true;
    stream_begin(a->out, "text/event-stream");
    openai_chunk(a, "{\"role\":\"assistant\",\"content\":\"\"}", nullptr);
}

static bool openai_part(void *ctx, const char *text, size_t len) {
    struct openai *a = ctx;
    if (!a->stream)
        return a->text.f && fwrite(text, 1, len, a->text.f) == len;
    openai_start(a);
    struct text delta;
    FILE       *f = text_open(&delta);
    if (!f)
        return false;
    fputs("{\"content\":", f), json_write(f, text), fputc('}', f), fclose(f);
    openai_chunk(a, delta.s, nullptr);
    free(delta.s);
    return fflush(a->out) != EOF;
}

static void openai_done(void *ctx, const geistr_stats *st) {
    struct openai *a      = ctx;
    const char    *finish = st->finish == GEISTR_FINISH_LENGTH || st->finish == GEISTR_FINISH_CONTEXT ? "length" : "stop";
    unsigned       prompt = st->context_tokens > st->output_tokens ? st->context_tokens - st->output_tokens : 0;
    char           usage[128];
    snprintf(usage, sizeof usage, "{\"prompt_tokens\":%u,\"completion_tokens\":%u,\"total_tokens\":%u}", prompt,
             st->output_tokens, prompt + st->output_tokens);
    if (a->stream) {
        openai_start(a);
        openai_chunk(a, "{}", finish);
        if (a->usage)
            fprintf(a->out,
                    "data: {\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%lld,\"choices\":[],"
                    "\"usage\":%s}\n\n",
                    a->id, a->created, usage);
        fputs("data: [DONE]\n\n", a->out);
        return;
    }
    if (a->text.f)
        fclose(a->text.f), a->text.f = nullptr;
    struct text body;
    FILE       *f = text_open(&body);
    if (!f) {
        error_json(a->out, 500, true, "out of memory", nullptr);
        return;
    }
    fprintf(f, "{\"id\":\"%s\",\"object\":\"chat.completion\",\"created\":%lld,\"model\":", a->id, a->created);
    json_write(f, a->model);
    fputs(",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":", f);
    json_write(f, a->text.s ? a->text.s : "");
    fprintf(f, "},\"finish_reason\":\"%s\"}],\"usage\":%s}", finish, usage);
    respond_json(a->out, 200, &body);
}

static void openai_error(void *ctx, const char *status, const char *text) {
    struct openai *a       = ctx;
    const char    *code    = !strcmp(status, "context") ? "context_length_exceeded" : nullptr;
    int            http    = !strcmp(status, "error") ? 500 : 400;
    if (!a->started) {
        error_json(a->out, http, true, text, code);
        return;
    }
    fputs("data: {\"error\":{\"message\":", a->out), json_write(a->out, text), fputs("}}\n\n", a->out);
}

static void openai_chat(const struct svc_options *o, struct held *pool, FILE *out, const struct json *j) {
    static unsigned    serial;
    struct svc_request r   = {};
    int                max = json_field(j, 0, "max_completion_tokens");
    if (!read_messages(j, &r)) {
        svc_free_request(&r);
        error_json(out, 400, true, "messages: a list of messages with role and content", nullptr);
        return;
    }
    r.temperature = clamp(json_number(j, json_field(j, 0, "temperature"), 1)); /* OpenAI's default */
    r.max         = (unsigned) json_number(j, max >= 0 ? max : json_field(j, 0, "max_tokens"), 0);
    read_stop(j, json_field(j, 0, "stop"), &r);
    int           options = json_field(j, 0, "stream_options");
    struct openai a       = {.out     = out,
                             .model   = o->name,
                             .created = (long long) time(nullptr),
                             .stream  = json_bool(j, json_field(j, 0, "stream"), false),
                             .usage   = json_bool(j, json_field(j, options, "include_usage"), false)};
    snprintf(a.id, sizeof a.id, "chatcmpl-%x%x", (unsigned) a.created, ++serial);
    if (!a.stream && !text_open(&a.text))
        error_json(out, 500, true, "out of memory", nullptr);
    else
        svc_chat(o, pool, &r, &(struct svc_sink) {&a, openai_part, openai_done, openai_error});
    if (a.text.f)
        fclose(a.text.f);
    free(a.text.s);
    svc_free_request(&r);
}

static void openai_models(const struct svc_options *o, FILE *out) {
    struct text t;
    FILE       *f = text_open(&t);
    if (!f) {
        respond(out, 500, "application/json", "{}");
        return;
    }
    fputs("{\"object\":\"list\",\"data\":[{\"id\":", f), json_write(f, o->name);
    fputs(",\"object\":\"model\",\"created\":0,\"owned_by\":\"geistr\"}]}", f);
    respond_json(out, 200, &t);
}

/* ---- Ollama: /api/chat, /api/tags -------------------------------------------- */

struct ollama {
    FILE       *out;
    const char *model;
    bool        stream, started;
    struct text text;
};

static void ollama_head(struct ollama *a, FILE *f) {
    char now[32];
    iso_time(now);
    fputs("{\"model\":", f), json_write(f, a->model), fprintf(f, ",\"created_at\":\"%s\"", now);
}

static bool ollama_part(void *ctx, const char *text, size_t len) {
    struct ollama *a = ctx;
    if (!a->stream)
        return a->text.f && fwrite(text, 1, len, a->text.f) == len;
    if (!a->started)
        a->started = true, stream_begin(a->out, "application/x-ndjson");
    ollama_head(a, a->out);
    fputs(",\"message\":{\"role\":\"assistant\",\"content\":", a->out), json_write(a->out, text);
    fputs("},\"done\":false}\n", a->out);
    return fflush(a->out) != EOF;
}

static void ollama_done(void *ctx, const geistr_stats *st) {
    struct ollama *a = ctx;
    struct text    body;
    FILE          *f = a->stream ? a->out : text_open(&body);
    if (!f) {
        error_json(a->out, 500, false, "out of memory", nullptr);
        return;
    }
    if (a->stream && !a->started)
        a->started = true, stream_begin(a->out, "application/x-ndjson");
    if (!a->stream && a->text.f)
        fclose(a->text.f), a->text.f = nullptr;
    unsigned prompt = st->context_tokens > st->output_tokens ? st->context_tokens - st->output_tokens : 0;
    ollama_head(a, f);
    fputs(",\"message\":{\"role\":\"assistant\",\"content\":", f);
    json_write(f, a->stream ? "" : a->text.s ? a->text.s : "");
    fprintf(f,
            "},\"done\":true,\"done_reason\":\"%s\",\"total_duration\":%.0f,\"load_duration\":0,"
            "\"prompt_eval_count\":%u,\"prompt_eval_duration\":%.0f,\"eval_count\":%u,\"eval_duration\":%.0f}%s",
            st->finish == GEISTR_FINISH_LENGTH || st->finish == GEISTR_FINISH_CONTEXT ? "length" : "stop",
            st->total_ms * 1e6, prompt, st->prefill_ms > 0 ? st->prefill_ms * 1e6 : 0, st->output_tokens,
            st->generation_ms > 0 ? st->generation_ms * 1e6 : 0, a->stream ? "\n" : "");
    if (!a->stream)
        respond_json(a->out, 200, &body);
}

static void ollama_error(void *ctx, const char *status, const char *text) {
    struct ollama *a = ctx;
    if (!a->started) {
        error_json(a->out, !strcmp(status, "error") ? 500 : 400, false, text, nullptr);
        return;
    }
    fputs("{\"error\":", a->out), json_write(a->out, text), fputs("}\n", a->out);
}

static void ollama_chat(const struct svc_options *o, struct held *pool, FILE *out, const struct json *j) {
    struct svc_request r       = {};
    int                options = json_field(j, 0, "options");
    if (!read_messages(j, &r)) {
        svc_free_request(&r);
        error_json(out, 400, false, "messages: a list of messages with role and content", nullptr);
        return;
    }
    r.temperature  = clamp(json_number(j, json_field(j, options, "temperature"), 0.8)); /* Ollama's default */
    double predict = json_number(j, json_field(j, options, "num_predict"), 0);
    r.max          = predict > 0 ? (unsigned) predict : 0; /* -1: unlimited */
    read_stop(j, json_field(j, options, "stop"), &r);
    struct ollama a = {.out = out, .model = o->name, .stream = json_bool(j, json_field(j, 0, "stream"), true)};
    if (!a.stream && !text_open(&a.text))
        error_json(out, 500, false, "out of memory", nullptr);
    else
        svc_chat(o, pool, &r, &(struct svc_sink) {&a, ollama_part, ollama_done, ollama_error});
    if (a.text.f)
        fclose(a.text.f);
    free(a.text.s);
    svc_free_request(&r);
}

static void ollama_tags(const struct svc_options *o, FILE *out) {
    geistr_model_info i = {.size = sizeof i};
    (void) geistr_model_info_get(o->model, &i);
    char now[32];
    iso_time(now);
    struct text t;
    FILE       *f = text_open(&t);
    if (!f) {
        respond(out, 500, "application/json", "{}");
        return;
    }
    fputs("{\"models\":[{\"name\":", f), json_write(f, o->name);
    fputs(",\"model\":", f), json_write(f, o->name);
    fprintf(f, ",\"modified_at\":\"%s\",\"size\":0,\"digest\":\"\",\"details\":{\"format\":\"gguf\",\"family\":", now);
    json_write(f, i.arch ? i.arch : "");
    fputs("}}]}", f);
    respond_json(out, 200, &t);
}

/* ---- routing ----------------------------------------------------------------- */

void http_serve(const struct svc_options *o, struct held *pool, int client, bool loopback) {
    struct request r      = {};
    int            status = read_request(client, &r);
    FILE          *out    = fdopen(client, "w"); /* closes client */
    if (!out) {
        close(client);
        free(r.body);
        return;
    }
    bool get = !strcmp(r.method, "GET"), post = !strcmp(r.method, "POST"), openai = !strncmp(r.path, "/v1/", 4);
    bool chat = !strcmp(r.path, "/v1/chat/completions") || !strcmp(r.path, "/api/chat");
    if (status)
        error_json(out, status, openai, reason(status), nullptr);
    else if (loopback && !host_is_loopback(r.host))
        error_json(out, 403, openai, "Host is not this computer (DNS rebinding?)", nullptr);
    else if (get && (!strcmp(r.path, "/") || !strcmp(r.path, "/api/version"))) {
        char version[64];
        snprintf(version, sizeof version, "{\"version\":\"%s\"}", geistr_version());
        r.path[1] ? respond(out, 200, "application/json", version)
                  : respond(out, 200, "text/plain", "geistr is running (OpenAI and Ollama APIs)\n");
    } else if (get && !strcmp(r.path, "/v1/models"))
        openai_models(o, out);
    else if (get && !strcmp(r.path, "/api/tags"))
        ollama_tags(o, out);
    else if (chat && post) {
        struct json j = {};
        if (!json_parse(&j, r.body, (size_t) r.length))
            error_json(out, 400, openai, "the body is not a JSON object", nullptr);
        else if (openai)
            openai_chat(o, pool, out, &j);
        else
            ollama_chat(o, pool, out, &j);
        json_free(&j);
    } else if (chat)
        error_json(out, 405, openai, "use POST", nullptr);
    else
        error_json(out, 404, openai, "no such endpoint", nullptr);
    fclose(out);
    free(r.body);
}

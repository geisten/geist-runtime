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
    for (int item = json_next(j, t, -1); f && item >= 0; item = json_next(j, t, item)) {
        char *part = json_string(j, json_field(j, item, "text"));
        if (part)
            fputs(part, f);
        free(part);
    }
    if (f)
        fclose(f);
    return all.s ? all.s : strdup("");
}

/* ---- images (#92): the last message's, base64 ------------------------------
 * OpenAI: a content part {"type":"image_url","image_url":{"url":"data:…;base64,…"}};
 * Ollama: "images": ["…"]. ponytail: only the last message's image reaches the
 * model; an earlier one lives on in the held conversation that saw it (matched
 * by text: a client that sends another image under the same words and answers
 * gets the old one; a hash per held turn if that matters). */

static unsigned char *base64(const char *s, size_t n, size_t *len) {
    unsigned char *out = malloc(n / 4 * 3 + 3);
    uint32_t       acc = 0;
    size_t         o = 0, bits = 0;
    for (size_t i = 0; out && i < n && s[i] != '='; i++) {
        const char *at = strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", s[i]);
        if (!at || !s[i]) {
            free(out);
            return nullptr;
        }
        acc = acc << 6 | (uint32_t) (at - "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
        if ((bits += 6) >= 8)
            bits -= 8, out[o++] = (unsigned char) (acc >> bits);
    }
    *len = o;
    return out;
}

/* The image of the request's last message into r; nullptr, or why not. */
static const char *read_image(const struct json *j, struct svc_request *r) {
    int list = json_field(j, 0, "messages"), last = -1, found = -1, count = 0;
    for (int item = json_next(j, list, -1); item >= 0; item = json_next(j, list, item))
        last = item;
    int images = json_field(j, last, "images"), content = json_field(j, last, "content");
    for (int item = json_next(j, images, -1); item >= 0; item = json_next(j, images, item))
        found = item, count++;
    for (int part = json_next(j, content, -1); json_count(j, content) > 0 && part >= 0;
         part = json_next(j, content, part)) {
        char *type = json_string(j, json_field(j, part, "type"));
        if (type && !strcmp(type, "image_url")) {
            int   url    = json_field(j, part, "image_url");
            char *direct = json_string(j, url); /* a string, or {"url": …} */
            found        = direct ? url : json_field(j, url, "url"), count++;
            free(direct);
        }
        free(type);
    }
    if (!count)
        return nullptr;
    if (count > 1)
        return "one image per message";
    char       *text = json_string(j, found);
    const char *data = text;
    if (data && found != json_next(j, images, -1)) { /* OpenAI: a data URL */
        const char *comma = strstr(data, ";base64,");
        data              = !strncmp(data, "data:", 5) && comma ? comma + strlen(";base64,") : nullptr;
    }
    r->image = data ? base64(data, strlen(data), &r->image_len) : nullptr;
    free(text);
    return !data ? "image_url: a data: URL with base64 (no web addresses)" : !r->image ? "image: not base64" : nullptr;
}

/* ---- tools: Qwen3's format (Hermes style), as its chat template renders it --
 * The tool list goes into the system message; an assistant's earlier calls
 * become <tool_call> blocks, tool results a user turn of <tool_response>
 * blocks. The answer's calls come back as <tool_call> blocks (calls_parse). */

/* The model has a tool format geistr renders. ponytail: Qwen3 only, the
 * catalog's tool-trained family; another family needs its own format. */
static bool tools_supported(const struct svc_options *o) {
    geistr_model_info i = {.size = sizeof i};
    return geistr_model_info_get(o->model, &i) == GEISTR_OK && i.arch && !strcmp(i.arch, "qwen3");
}

static const char tools_head[] =
        "# Tools\n\nYou may call one or more functions to assist with the user query.\n\nYou are provided with "
        "function signatures within <tools></tools> XML tags:\n<tools>";
static const char tools_tail[] =
        "\n</tools>\n\nFor each function call, return a json object with function name and arguments within "
        "<tool_call></tool_call> XML tags:\n<tool_call>\n{\"name\": <function-name>, \"arguments\": "
        "<args-json-object>}\n</tool_call>";

/* An assistant message's tool_calls after its content, as <tool_call> blocks.
 * OpenAI's arguments are a JSON string, Ollama's an object. */
static char *with_calls(const struct json *j, int calls, const char *content) {
    struct text t;
    FILE       *f = text_open(&t);
    if (!f)
        return nullptr;
    fputs(content, f);
    for (int call = json_next(j, calls, -1); call >= 0; call = json_next(j, calls, call)) {
        int    fn   = json_field(j, call, "function");
        char  *name = json_string(j, json_field(j, fn, "name"));
        int    a    = json_field(j, fn, "arguments");
        char  *text = json_string(j, a); /* OpenAI: the arguments' JSON as a string */
        size_t len  = 0;
        const char *raw = text ? text : json_raw(j, a, &len);
        fputs(ftell(f) ? "\n<tool_call>\n{\"name\": " : "<tool_call>\n{\"name\": ", f);
        json_write(f, name ? name : "");
        fputs(", \"arguments\": ", f);
        text ? (void) fputs(text, f) : raw ? (void) fwrite(raw, 1, len, f) : (void) fputs("{}", f);
        fputs("}\n</tool_call>", f);
        free(name), free(text);
    }
    fclose(f);
    return t.s;
}

/* messages, and with tools (a list, already checked) their rendering. */
static bool read_messages(const struct json *j, struct svc_request *r, int tools) {
    int list = json_field(j, 0, "messages");
    int n    = json_count(j, list);
    if (n < 1 || !(r->messages = calloc((size_t) n + 1, sizeof *r->messages))) /* +1: a system message for tools */
        return false;
    size_t k = 0, results = SIZE_MAX; /* results: the user turn that collects tool results */
    int    item = -1;
    for (int m = 0; m < n; m++) {
        item       = json_next(j, list, item);
        char *role = json_string(j, json_field(j, item, "role"));
        if (role && !strcmp(role, "developer")) /* OpenAI's newer name for system */
            free(role), role = strdup("system");
        char *content = content_of(j, json_field(j, item, "content"));
        if (!content && role && !strcmp(role, "assistant")) /* only tool_calls: no content at all */
            content = strdup("");
        if (!role || !content) {
            free(role), free(content);
            return r->n = k, false;
        }
        int calls = json_field(j, item, "tool_calls");
        if (tools >= 0 && !strcmp(role, "assistant") && json_count(j, calls) > 0) {
            char *both = with_calls(j, calls, content);
            free(content), content = both;
        }
        if (tools >= 0 && !strcmp(role, "tool")) { /* a result: into the user turn of results */
            struct text t;
            FILE       *f = text_open(&t);
            if (f) {
                if (results == k - 1 && k)
                    fprintf(f, "%s\n", r->messages[k - 1].content);
                fprintf(f, "<tool_response>\n%s\n</tool_response>", content);
                fclose(f);
            }
            free(content), free(role);
            if (!t.s)
                return r->n = k, false;
            if (results == k - 1 && k) {
                free((char *) r->messages[k - 1].content);
                r->messages[k - 1].content = t.s;
                continue;
            }
            role = strdup("user"), content = t.s, results = k;
        }
        r->messages[k].role    = role;
        r->messages[k].content = content;
        k++;
        if (!role || !content)
            return r->n = k, false;
    }
    r->n = k;
    if (tools < 0)
        return true;
    /* the tool list into the system message: after its text, or a new one first */
    struct text t;
    FILE       *f      = text_open(&t);
    bool        system = !strcmp(r->messages[0].role, "system");
    if (!f)
        return false;
    if (system)
        fprintf(f, "%s\n\n", r->messages[0].content);
    fputs(tools_head, f);
    for (int tool = json_next(j, tools, -1); tool >= 0; tool = json_next(j, tools, tool)) {
        size_t      len = 0;
        const char *raw = json_raw(j, tool, &len);
        fputc('\n', f), fwrite(raw, 1, len, f);
    }
    fputs(tools_tail, f);
    fclose(f);
    if (!t.s)
        return false;
    if (system)
        free((char *) r->messages[0].content), r->messages[0].content = t.s;
    else {
        memmove(r->messages + 1, r->messages, r->n * sizeof *r->messages);
        r->messages[0] = (geistr_message) {strdup("system"), t.s};
        r->n++;
        if (!r->messages[0].role)
            return false;
    }
    return true;
}

/* The request's tools: their list's token, -1 without (or tool_choice none);
 * -2 when the model has no tool format (the client is told so, 400). */
static int request_tools(const struct svc_options *o, const struct json *j) {
    int   tools  = json_field(j, 0, "tools");
    char *choice = json_string(j, json_field(j, 0, "tool_choice"));
    bool  none   = choice && !strcmp(choice, "none");
    free(choice);
    if (json_count(j, tools) < 1 || none)
        return -1;
    return tools_supported(o) ? tools : -2;
}

/* ---- the answer's calls ------------------------------------------------------ */

#define CALLS_MAX 16
struct calls {
    size_t n;
    char  *name[CALLS_MAX], *args[CALLS_MAX]; /* args: a JSON object's text */
};

static void calls_free(struct calls *c) {
    for (size_t i = 0; i < c->n; i++)
        free(c->name[i]), free(c->args[i]);
    c->n = 0;
}

/* The <tool_call> blocks of text (the last one may be unclosed): their name
 * and arguments; the count of valid ones. */
static size_t calls_parse(const char *text, struct calls *c) {
    *c = (struct calls) {};
    for (const char *p = text; (p = strstr(p, "<tool_call>")) && c->n < CALLS_MAX;) {
        p += strlen("<tool_call>");
        const char *end  = strstr(p, "</tool_call>");
        size_t      len  = end ? (size_t) (end - p) : strlen(p);
        char       *body = strndup(p, len);
        struct json j    = {};
        if (body && json_parse(&j, body, len)) {
            char       *name = json_string(&j, json_field(&j, 0, "name"));
            size_t      n    = 0;
            const char *args = json_raw(&j, json_field(&j, 0, "arguments"), &n);
            if (name && args && body[args - body - 1] != '"') { /* an object, not a string */
                c->name[c->n] = name;
                c->args[c->n] = strndup(args, n);
                c->n += c->args[c->n] != nullptr;
            } else
                free(name);
        }
        json_free(&j);
        free(body);
        p += len;
    }
    return c->n;
}

/* Streaming with tools: text goes out until "<tool_call>" begins (a possible
 * start of it is held); from there on it is calls, read at the end. */
struct hold {
    bool   tools, calling;
    char  *text; /* held: a possible start of "<tool_call>", or the calls */
    size_t n;
};

/* What of text can go out now (into out[0..*n)); false when out of memory. */
static bool hold_feed(struct hold *h, const char *text, char **out, size_t *n) {
    size_t add  = strlen(text);
    char  *grow = realloc(h->text, h->n + add + 1);
    if (!grow)
        return false;
    h->text = grow;
    memcpy(h->text + h->n, text, add + 1);
    h->n += add;
    *out = h->text, *n = 0;
    if (h->calling)
        return true;
    char *at = strstr(h->text, "<tool_call>");
    if (at) {
        *n         = (size_t) (at - h->text);
        h->calling = true;
        return true;
    }
    size_t keep = 0; /* the longest end that may begin "<tool_call>" */
    for (size_t k = h->n < 10 ? h->n : 10; k > 0 && !keep; k--)
        if (!strncmp(h->text + h->n - k, "<tool_call>", k))
            keep = k;
    *n = h->n - keep;
    return true;
}

/* After hold_feed: drop what went out. */
static void hold_sent(struct hold *h, size_t n) {
    memmove(h->text, h->text + n, h->n - n + 1);
    h->n -= n;
}

/* stop: a string or a list of strings. */
static void read_stop(const struct json *j, int t, struct svc_request *r) {
    char *one = json_string(j, t);
    int   n   = one ? 1 : json_count(j, t);
    char **stop = n > 0 ? calloc((size_t) n, sizeof *stop) : nullptr;
    for (int k = 0, item = -1; stop && k < n; k++)
        stop[k] = one ? one : json_string(j, item = json_next(j, t, item));
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
    struct hold hold; /* with tools: what may be a call */
};

/* OpenAI's tool_calls: an array of {id, type, function: {name, arguments}}
 * (arguments as a JSON string); in a stream delta, with their index. */
static void openai_calls(FILE *f, const char *id, const struct calls *c, bool delta) {
    fputc('[', f);
    for (size_t i = 0; i < c->n; i++) {
        fprintf(f, "%s{", i ? "," : "");
        if (delta)
            fprintf(f, "\"index\":%zu,", i);
        fprintf(f, "\"id\":\"call_%s_%zu\",\"type\":\"function\",\"function\":{\"name\":", id, i);
        json_write(f, c->name[i]);
        fputs(",\"arguments\":", f), json_write(f, c->args[i]), fputs("}}", f);
    }
    fputc(']', f);
}

/* A chunk whose delta has the role (the first), content, or neither (the last). */
static void openai_chunk(struct openai *a, bool role, const char *content, const char *finish) {
    fprintf(a->out, "data: {\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%lld,\"model\":", a->id,
            a->created);
    json_write(a->out, a->model);
    fputs(",\"choices\":[{\"index\":0,\"delta\":{", a->out);
    if (role)
        fputs("\"role\":\"assistant\",", a->out);
    if (content)
        fputs("\"content\":", a->out), json_write(a->out, content);
    fputs("},\"finish_reason\":", a->out);
    finish ? (void) fprintf(a->out, "\"%s\"", finish) : (void) fputs("null", a->out);
    fputs("}]}\n\n", a->out);
}

static void openai_start(struct openai *a) {
    if (a->started)
        return;
    a->started = true;
    stream_begin(a->out, "text/event-stream");
    openai_chunk(a, true, "", nullptr);
}

static bool openai_part(void *ctx, const char *text, size_t len) {
    (void) len;
    struct openai *a = ctx;
    if (!a->stream) /* the answer comes whole, to done */
        return true;
    openai_start(a);
    if (!a->hold.tools)
        openai_chunk(a, false, text, nullptr);
    else { /* text until a call begins */
        char  *ready;
        size_t n;
        if (!hold_feed(&a->hold, text, &ready, &n))
            return false;
        if (n) {
            char *content = strndup(ready, n);
            if (content)
                openai_chunk(a, false, content, nullptr);
            free(content);
            hold_sent(&a->hold, n);
        }
    }
    return fflush(a->out) != EOF;
}

/* Accepted, or still reasoning: the headers and role first, then an SSE comment. */
static bool openai_alive(void *ctx) {
    struct openai *a = ctx;
    if (!a->stream)
        return true;
    if (a->started)
        fputs(": thinking\n\n", a->out);
    openai_start(a);
    return fflush(a->out) != EOF;
}

/* Both APIs: why the answer ended, and what the prompt took of the context. */
static const char *finish_reason(const geistr_stats *st) {
    return st->finish == GEISTR_FINISH_LENGTH || st->finish == GEISTR_FINISH_CONTEXT ? "length" : "stop";
}

static unsigned prompt_tokens(const geistr_stats *st) {
    return st->context_tokens > st->output_tokens ? st->context_tokens - st->output_tokens : 0;
}

static void openai_done(void *ctx, const geistr_stats *st, const char *answer) {
    struct openai *a      = ctx;
    const char    *finish = finish_reason(st);
    unsigned       prompt = prompt_tokens(st);
    char           usage[128];
    snprintf(usage, sizeof usage, "{\"prompt_tokens\":%u,\"completion_tokens\":%u,\"total_tokens\":%u}", prompt,
             st->output_tokens, prompt + st->output_tokens);
    struct calls calls = {};
    if (a->hold.tools) { /* the answer's calls, if it made any */
        const char *text = a->stream ? (a->hold.calling ? a->hold.text : "") : answer;
        if (calls_parse(text ? text : "", &calls))
            finish = "tool_calls";
        else if (a->stream && a->hold.n) { /* no call after all: the held text is content */
            openai_start(a);
            openai_chunk(a, false, a->hold.text, nullptr);
        }
    }
    if (a->stream) {
        openai_start(a);
        if (calls.n) {
            fprintf(a->out, "data: {\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%lld,\"model\":",
                    a->id, a->created);
            json_write(a->out, a->model);
            fputs(",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":", a->out);
            openai_calls(a->out, a->id, &calls, true);
            fputs("},\"finish_reason\":null}]}\n\n", a->out);
        }
        calls_free(&calls);
        openai_chunk(a, false, nullptr, finish);
        if (a->usage)
            fprintf(a->out,
                    "data: {\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%lld,\"choices\":[],"
                    "\"usage\":%s}\n\n",
                    a->id, a->created, usage);
        fputs("data: [DONE]\n\n", a->out);
        return;
    }
    struct text body;
    FILE       *f = text_open(&body);
    if (!f) {
        error_json(a->out, 500, true, "out of memory", nullptr);
        return;
    }
    fprintf(f, "{\"id\":\"%s\",\"object\":\"chat.completion\",\"created\":%lld,\"model\":", a->id, a->created);
    json_write(f, a->model);
    fputs(",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":", f);
    if (calls.n) { /* the text before the calls, if any, and the calls */
        const char *at = strstr(answer, "<tool_call>");
        size_t      n  = at ? (size_t) (at - answer) : 0;
        while (n && strchr(" \t\r\n", answer[n - 1]))
            n--;
        char *before = strndup(answer, n);
        before && *before ? json_write(f, before) : (void) fputs("null", f);
        free(before);
        fputs(",\"tool_calls\":", f), openai_calls(f, a->id, &calls, false);
    } else
        json_write(f, answer);
    calls_free(&calls);
    fprintf(f, "},\"finish_reason\":\"%s\"}],\"usage\":%s}", finish, usage);
    respond_json(a->out, 200, &body);
}

/* Both APIs: the client's fault (400), else the service's (500). */
static int http_status(geistr_status s) {
    return s == GEISTR_INVALID || s == GEISTR_CONTEXT ? 400 : 500;
}

static void openai_error(void *ctx, geistr_status status, const char *text) {
    struct openai *a    = ctx;
    const char    *code = status == GEISTR_CONTEXT ? "context_length_exceeded" : nullptr;
    int            http = http_status(status);
    if (!a->started) {
        error_json(a->out, http, true, text, code);
        return;
    }
    fputs("data: {\"error\":{\"message\":", a->out), json_write(a->out, text), fputs("}}\n\n", a->out);
}

static void openai_chat(const struct svc_options *o, struct held *pool, FILE *out, const struct json *j) {
    static unsigned    serial;
    struct svc_request r     = {};
    int                max   = json_field(j, 0, "max_completion_tokens");
    int                tools = request_tools(o, j);
    if (tools == -2) {
        error_json(out, 400, true, "this model has no tool-calling format (tools work with Qwen3 models)", nullptr);
        return;
    }
    if (!read_messages(j, &r, tools)) {
        svc_free_request(&r);
        error_json(out, 400, true, "messages: a list of messages with role and content", nullptr);
        return;
    }
    const char *why = read_image(j, &r);
    if (why) {
        svc_free_request(&r);
        error_json(out, 400, true, why, nullptr);
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
    a.hold.tools = tools >= 0;
    svc_chat(o, pool, &r, &(struct svc_sink) {&a, openai_part, openai_done, openai_error, openai_alive});
    free(a.hold.text);
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
    struct hold hold; /* with tools: what may be a call */
};

/* Ollama's tool_calls: [{function: {name, arguments}}], arguments an object. */
static void ollama_calls(FILE *f, const struct calls *c) {
    fputc('[', f);
    for (size_t i = 0; i < c->n; i++) {
        fprintf(f, "%s{\"function\":{\"name\":", i ? "," : "");
        json_write(f, c->name[i]);
        fprintf(f, ",\"arguments\":%s}}", c->args[i]);
    }
    fputc(']', f);
}

static void ollama_head(struct ollama *a, FILE *f) {
    char now[32];
    iso_time(now);
    fputs("{\"model\":", f), json_write(f, a->model), fprintf(f, ",\"created_at\":\"%s\"", now);
}

static bool ollama_part(void *ctx, const char *text, size_t len) {
    (void) len;
    struct ollama *a = ctx;
    if (!a->stream) /* the answer comes whole, to done */
        return true;
    if (!a->started)
        a->started = true, stream_begin(a->out, "application/x-ndjson");
    char  *ready = nullptr;
    size_t n     = strlen(text);
    if (a->hold.tools) { /* text until a call begins */
        if (!hold_feed(&a->hold, text, &ready, &n))
            return false;
        text = ready;
    }
    if (n) {
        char *content = strndup(text, n);
        ollama_head(a, a->out);
        fputs(",\"message\":{\"role\":\"assistant\",\"content\":", a->out), json_write(a->out, content ? content : "");
        fputs("},\"done\":false}\n", a->out);
        free(content);
        if (a->hold.tools)
            hold_sent(&a->hold, n);
    }
    return fflush(a->out) != EOF;
}

/* Accepted: the headers (NDJSON has no comments for later signs of life). */
static bool ollama_alive(void *ctx) {
    struct ollama *a = ctx;
    if (a->stream && !a->started)
        a->started = true, stream_begin(a->out, "application/x-ndjson");
    return fflush(a->out) != EOF;
}

static void ollama_done(void *ctx, const geistr_stats *st, const char *answer) {
    struct ollama *a = ctx;
    struct text    body;
    FILE          *f = a->stream ? a->out : text_open(&body);
    if (!f) {
        error_json(a->out, 500, false, "out of memory", nullptr);
        return;
    }
    if (a->stream && !a->started)
        a->started = true, stream_begin(a->out, "application/x-ndjson");
    struct calls calls = {};
    const char  *rest  = "";
    if (a->hold.tools) { /* the answer's calls, if it made any */
        const char *text = a->stream ? (a->hold.calling ? a->hold.text : "") : answer;
        if (!calls_parse(text ? text : "", &calls) && a->stream && a->hold.n)
            rest = a->hold.text; /* no call after all: the held text is content */
    }
    char *before = nullptr; /* without a stream: the text before the calls */
    if (!a->stream && calls.n) {
        const char *at = strstr(answer, "<tool_call>");
        size_t      n  = at ? (size_t) (at - answer) : 0;
        while (n && strchr(" \t\r\n", answer[n - 1]))
            n--;
        before = strndup(answer, n);
    }
    ollama_head(a, f);
    fputs(",\"message\":{\"role\":\"assistant\",\"content\":", f);
    json_write(f, a->stream ? rest : calls.n ? (before ? before : "") : answer);
    free(before);
    if (calls.n)
        fputs(",\"tool_calls\":", f), ollama_calls(f, &calls);
    calls_free(&calls);
    fprintf(f,
            "},\"done\":true,\"done_reason\":\"%s\",\"total_duration\":%.0f,\"load_duration\":0,"
            "\"prompt_eval_count\":%u,\"prompt_eval_duration\":%.0f,\"eval_count\":%u,\"eval_duration\":%.0f}%s",
            finish_reason(st), st->total_ms * 1e6, prompt_tokens(st), st->prefill_ms > 0 ? st->prefill_ms * 1e6 : 0, st->output_tokens,
            st->generation_ms > 0 ? st->generation_ms * 1e6 : 0, a->stream ? "\n" : "");
    if (!a->stream)
        respond_json(a->out, 200, &body);
}

static void ollama_error(void *ctx, geistr_status status, const char *text) {
    struct ollama *a = ctx;
    if (!a->started) {
        error_json(a->out, http_status(status), false, text, nullptr);
        return;
    }
    fputs("{\"error\":", a->out), json_write(a->out, text), fputs("}\n", a->out);
}

static void ollama_chat(const struct svc_options *o, struct held *pool, FILE *out, const struct json *j) {
    struct svc_request r       = {};
    int                options = json_field(j, 0, "options");
    int                tools   = request_tools(o, j);
    if (tools == -2) {
        error_json(out, 400, false, "this model has no tool-calling format (tools work with Qwen3 models)", nullptr);
        return;
    }
    if (!read_messages(j, &r, tools)) {
        svc_free_request(&r);
        error_json(out, 400, false, "messages: a list of messages with role and content", nullptr);
        return;
    }
    const char *why = read_image(j, &r);
    if (why) {
        svc_free_request(&r);
        error_json(out, 400, false, why, nullptr);
        return;
    }
    r.temperature  = clamp(json_number(j, json_field(j, options, "temperature"), 0.8)); /* Ollama's default */
    double predict = json_number(j, json_field(j, options, "num_predict"), 0);
    r.max          = predict > 0 ? (unsigned) predict : 0; /* -1: unlimited */
    read_stop(j, json_field(j, options, "stop"), &r);
    struct ollama a = {.out    = out,
                       .model  = o->name,
                       .stream = json_bool(j, json_field(j, 0, "stream"), true),
                       .hold   = {.tools = tools >= 0}};
    svc_chat(o, pool, &r, &(struct svc_sink) {&a, ollama_part, ollama_done, ollama_error, ollama_alive});
    free(a.hold.text);
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

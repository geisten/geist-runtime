/* fuzz_http.c — libFuzzer: one HTTP request to geistr serve --http (#133):
 * request line, headers, Content-Length, the body's JSON (OpenAI and Ollama
 * chat requests: messages, tools, stop, images) and the answer written back.
 * The input is what a client sends, over a socketpair. svc_chat is a fake
 * that streams the last message back, so no model runs. */
#define svc_chat fuzz_chat
#include "http.c"
#include <pthread.h>
#include <signal.h>
#include <stdint.h>

void fuzz_chat(const struct svc_options *o, struct held *pool, const struct svc_request *r,
               const struct svc_sink *out) {
    (void) o, (void) pool;
    size_t bytes = r->image_len;
    for (size_t i = 0; i < r->n; i++)
        bytes += strlen(r->messages[i].role) + strlen(r->messages[i].content);
    for (size_t i = 0; i < r->n_stop; i++)
        bytes += strlen(r->stop[i]);
    if (!r->n || bytes > (64u << 20)) {
        out->error(out->ctx, GEISTR_INVALID, "at least one message");
        return;
    }
    if (out->alive && !out->alive(out->ctx))
        return;
    const char *answer = r->messages[r->n - 1].content; /* in two pieces: a <tool_call> may span them */
    size_t      half   = strlen(answer) / 2;
    if (!out->part(out->ctx, answer, half) || !out->part(out->ctx, answer + half, strlen(answer + half)))
        return;
    geistr_stats st = {.size = sizeof st, .finish = GEISTR_FINISH_STOP};
    out->done(out->ctx, &st, answer);
}

struct client {
    int            fd;
    const uint8_t *data;
    size_t         size;
};

static void *client_run(void *arg) { /* sends the request, then reads the answer to its end */
    struct client *c = arg;
    for (size_t sent = 0; sent < c->size;) {
        ssize_t w = write(c->fd, c->data + sent, c->size - sent);
        if (w <= 0)
            break;
        sent += (size_t) w;
    }
    shutdown(c->fd, SHUT_WR);
    char buf[4096];
    while (read(c->fd, buf, sizeof buf) > 0) {
    }
    return nullptr;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static geistr_model         *model;
    static volatile sig_atomic_t stop;
    if (!model) {
        signal(SIGPIPE, SIG_IGN); /* the server may answer before the request is all sent */
        if (geistr_model_open("stub:echo", nullptr, &model, nullptr, 0) != GEISTR_OK)
            abort();
    }
    struct svc_options o = {.model = model, .name = "fuzz", .chats = 1, .max_request = 1u << 20, .stop = &stop};
    int                fd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0)
        abort();
    struct client c = {fd[1], data, size};
    pthread_t     t;
    if (pthread_create(&t, nullptr, client_run, &c) != 0)
        abort();
    http_serve(&o, nullptr, fd[0], true); /* closes fd[0] */
    pthread_join(t, nullptr);
    close(fd[1]);
    return 0;
}

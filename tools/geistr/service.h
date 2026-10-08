/* service.h — geistr as a service: one model, many short-lived clients over a
 * Unix socket (geistr serve / geistr chat --socket).
 *
 * Protocol: one JSON object per line. A client connects, sends one request
 * line and reads lines until the last one, then the connection closes.
 *
 *   → {"op":"info"}
 *   ← {"model":"gemma4-e2b","backend":"metal","chat_format":"gemma4","context":8192,"chats":2}
 *   → {"op":"chat","messages":[{"role":"user","content":"…"},…],"max":0,"temperature":0}
 *   ← {"part":"answer","text":"…"}            (as often as the answer has pieces)
 *   ← {"done":true,"finish":"stop","input_tokens":14,"context_tokens":52,"output_tokens":7,
 *      "prefill_ms":40.1,"generation_ms":95.3,"total_ms":136.2}
 *   ← {"error":"…","status":"context"}        (instead, on failure)
 *
 * A client sends the whole conversation every time; the service keeps up to
 * N conversations (KV caches) and continues the one that matches best: it
 * goes back to where the request differs and processes only the rest. A
 * client that disconnects stops its answer. Requests are served one at a
 * time. */
#pragma once
#include "geistr.h"
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct svc_stats {
    char     finish[16];
    unsigned input_tokens, context_tokens, output_tokens;
    double   prefill_ms, generation_ms, total_ms;
};

/* ---- the service -------------------------------------------------------- */

struct svc_options {
    geistr_model    *model;
    geistr_reasoning reasoning;
    const char      *name;   /* shown by info */
    const char      *socket; /* path */
    const char      *http;   /* ADDR:PORT for the HTTP API, or nullptr */
    size_t           chats;  /* conversations kept, ≥ 1 */
    volatile sig_atomic_t *stop;
};

/* Serve until *stop is set. Returns a geistr exit code (0 ok, 1 error). */
int service_run(const struct svc_options *o);

/* ---- inside the service: one chat request, whatever the protocol ---------- */

/* The request, its strings malloc'd (svc_free_request frees them). */
struct svc_request {
    geistr_message     *messages;
    size_t              n;
    double              temperature; /* 0 to 2 */
    unsigned            max;         /* answer tokens; 0 = the rest of the context */
    const char *const  *stop;
    size_t              n_stop;
    unsigned char      *image; /* the last message's image (#92): PNG, JPEG or BMP bytes */
    size_t              image_len;
};
void svc_free_request(struct svc_request *r);

/* Where the answer goes. part returns false when the client is gone (the
 * answer stops); then neither done nor error follows. done gets the whole
 * answer; error's status is GEISTR_INVALID, GEISTR_CONTEXT or another
 * (the service failed). alive, if set, comes when the request is accepted
 * and about once a second of hidden reasoning; false: the client is gone. */
struct svc_sink {
    void *ctx;
    bool (*part)(void *ctx, const char *text, size_t len);
    void (*done)(void *ctx, const geistr_stats *stats, const char *answer);
    void (*error)(void *ctx, geistr_status status, const char *text);
    bool (*alive)(void *ctx);
};

struct held; /* the conversations the service keeps */
void        svc_chat(const struct svc_options *o, struct held *pool, const struct svc_request *r,
                     const struct svc_sink *out);
const char *svc_finish(geistr_finish finish); /* "stop", "length", … */

/* http.c: a listener for ADDR:PORT (-1 on failure, reported), and one HTTP
 * request on a connection (OpenAI and Ollama APIs); it closes client. */
int  http_listen(const char *where, bool *loopback);
void http_serve(const struct svc_options *o, struct held *pool, int client, bool loopback);

/* ---- a client ----------------------------------------------------------- */

/* The service's info line into out (JSON). GEISTR_IO if none answers. */
geistr_status service_info(const char *socket, char *out, size_t cap);

typedef bool (*svc_part_fn)(void *ctx, const char *text);
typedef bool (*svc_cancel_fn)(void *ctx);

/* Send a conversation, stream the answer to part(); cancel() is polled while
 * waiting and closes the connection (the service stops the answer).
 * GEISTR_OK, GEISTR_CANCELLED, GEISTR_CONTEXT, GEISTR_IO (no service) or
 * GEISTR_BACKEND (error text in error). */
geistr_status service_chat(const char *socket, size_t n, const geistr_message *messages, unsigned max,
                           double temperature, svc_part_fn part, svc_cancel_fn cancel, void *ctx,
                           struct svc_stats *stats, char *error, size_t cap);

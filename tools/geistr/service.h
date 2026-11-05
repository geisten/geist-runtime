/* service.h — geistr as a service: one model, many short-lived clients over a
 * Unix socket (geistr serve / geistr chat --socket). Without a model (#195)
 * the HTTP API offers every installed model and loads the one a request
 * names, in place of the one loaded before: one model in memory at a time.
 *
 * Protocol: one JSON object per line. A client connects, sends one request
 * line and reads lines until the last one, then the connection closes.
 *
 *   → {"op":"info"}
 *   ← {"model":"gemma4-e2b","backend":"metal","chat_format":"gemma4","context":8192,"chats":2}
 *   → {"op":"chat","messages":[{"role":"user","content":"…"},…],"max":0,"temperature":0,
 *      "think":false,"effort":"low","thinking":true}
 *     (an assistant message may carry its "reasoning", as the thinking lines brought it, #282)
 *   ← {"part":"answer","text":"…"}            (as often as the answer has pieces)
 *   ← {"part":"thinking","text":"…"}          (only if the request asked: "thinking":true)
 *   ← {"done":true,"finish":"stop","input_tokens":14,"context_tokens":52,"output_tokens":7,
 *      "prefill_ms":40.1,"generation_ms":95.3,"total_ms":136.2}
 *   ← {"error":"…","status":"context"}        (instead, on failure)
 *
 * status: "context" (the conversation does not fit), "invalid" (not a request),
 * "too_large" (a line over --max-request), "error" (anything else).
 *
 * A client sends the whole conversation every time; the service keeps up to
 * N conversations (KV caches) and continues the one that matches best: it
 * goes back to where the request differs and processes only the rest. A
 * client that disconnects stops its answer. Requests are served one at a
 * time.
 *
 * The reasoning a client sends back with an earlier answer (#282: OpenAI
 * reasoning_content or reasoning, Ollama thinking, the socket's reasoning)
 * goes to the template as reasoning_content, unless the service ignores it
 * (serve --client-reasoning=ignore). An answer is held with its reasoning, so
 * a request matches it only with the same reasoning, or none for an answer
 * without. */
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
    geistr_model    *model;  /* nullptr: none loaded yet (a service of every model) */
    geistr_reasoning reasoning; /* a request's without its own think (#266): the server's default */
    const char      *name;   /* shown by info; "" while none is loaded */
    const char      *socket; /* path */
    const char      *http;   /* ADDR:PORT for the HTTP API, or nullptr */
    size_t           chats;  /* conversations kept, ≥ 1 */
    size_t           max_request; /* bytes of one request: the HTTP body, a socket line */
    volatile sig_atomic_t *stop;
    bool             embedding; /* an embedding model (#91): /v1/embeddings, no chats */
    /* A service of every installed model (#195); nullptr: the one above.
     * installed: the ids a request may name (at most cap). load: close the
     * model (its chats are closed already), then open id unless nullptr, and
     * set model, reasoning, name and embedding; false after saying why. */
    size_t (*installed)(char (*ids)[64], size_t cap);
    bool (*load)(struct svc_options *o, const char *id);
    int think; /* serve --thinking (#266), for requests without their own: 0 the settings' reasoning
                  above, 1 on, 2 off; a model that does not think, or cannot stop, ignores it */
    bool ignore_reasoning; /* serve --client-reasoning=ignore (#282): the reasoning clients send back
                              is dropped, as before; else it goes to the template */
};

/* Serve until *stop is set. Returns a geistr exit code (0 ok, 1 error). */
int service_run(struct svc_options *o);

/* ---- inside the service: one chat request, whatever the protocol ---------- */

/* The request, its strings malloc'd (svc_free_request frees them). */
struct svc_request {
    geistr_message     *messages;
    char              **reasoning; /* messages[i]'s reasoning_content (#282), nullptr: none; the
                                      list may be nullptr: none has any */
    size_t              n;
    double              temperature; /* 0 to 2 */
    unsigned            max;         /* answer tokens; 0 = the rest of the context */
    const char *const  *stop;
    size_t              n_stop;
    unsigned char      *image; /* the last message's image (#92): PNG, JPEG or BMP bytes */
    size_t              image_len;
    int                 think;  /* #266: -1 the server's default, 0 off, 1 on */
    geistr_effort       effort; /* with think on: how much (Qwen3.6+'s reasoning_effort) */
    bool                strict; /* think on for a model that cannot think: 400 (Ollama), else ignored */
};
void svc_free_request(struct svc_request *r);

/* Where the answer goes. part returns false when the client is gone (the
 * answer stops); then neither done nor error follows. done gets the whole
 * answer and the whole reasoning ("" without); error's status is
 * GEISTR_INVALID, GEISTR_CONTEXT or another (the service failed). thought,
 * if set, gets the reasoning as it comes (#266), like part; without it,
 * alive comes about once a second of reasoning. alive, if set, comes when
 * the request is accepted; false: the client is gone.
 * fd, if >= 0, is the client's connection: while the prompt is processed
 * (which can take minutes) it is watched for the peer closing it, and that,
 * like the service's stop flag, cancels the work (#146). */
struct svc_sink {
    void *ctx;
    bool (*part)(void *ctx, const char *text, size_t len);
    void (*done)(void *ctx, const geistr_stats *stats, const char *answer, const char *thinking);
    void (*error)(void *ctx, geistr_status status, const char *text);
    bool (*alive)(void *ctx);
    int  fd;
    bool (*thought)(void *ctx, const char *text, size_t len);
};

/* A request's think and effort from a client's value (#266): true/false,
 * OpenAI's reasoning_effort or Ollama's think level ("none", "minimal", "low",
 * "medium", "high", "xhigh"). false for a value that is none of these. */
bool svc_think_level(const char *level, int *think, geistr_effort *effort);

struct held; /* the conversations the service keeps */
void        svc_chat(const struct svc_options *o, struct held *pool, const struct svc_request *r,
                     const struct svc_sink *out);
const char *svc_finish(geistr_finish finish); /* "stop", "length", … */
void        svc_unload(struct svc_options *o, struct held *pool); /* the chats, then the model (#195) */

/* http.c: a listener for ADDR:PORT (-1 on failure, reported), and one HTTP
 * request on a connection (OpenAI and Ollama APIs); it closes client. */
int  http_listen(const char *where, bool *loopback);
void http_serve(struct svc_options *o, struct held *pool, int client, bool loopback);

/* ---- a client ----------------------------------------------------------- */

/* The service's info line into out (JSON). GEISTR_IO if none answers. */
geistr_status service_info(const char *socket, char *out, size_t cap);

typedef bool (*svc_part_fn)(void *ctx, const char *text);
typedef bool (*svc_cancel_fn)(void *ctx);

/* Send a conversation, stream the answer to part(); cancel() is polled while
 * waiting and closes the connection (the service stops the answer). think:
 * -1 the service's default, 0 off, 1 on (#266).
 * GEISTR_OK, GEISTR_CANCELLED, GEISTR_CONTEXT, GEISTR_IO (no service) or
 * GEISTR_BACKEND (error text in error). */
geistr_status service_chat(const char *socket, size_t n, const geistr_message *messages, unsigned max,
                           double temperature, int think, svc_part_fn part, svc_cancel_fn cancel, void *ctx,
                           struct svc_stats *stats, char *error, size_t cap);

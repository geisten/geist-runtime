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
    size_t           chats;  /* conversations kept, ≥ 1 */
    volatile sig_atomic_t *stop;
};

/* Serve until *stop is set. Returns a geistr exit code (0 ok, 1 error). */
int service_run(const struct svc_options *o);

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

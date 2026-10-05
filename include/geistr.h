/*
 * geistr.h — the geist-runtime C API (draft for geist-runtime#1).
 *
 * An embeddable model runner on top of geistlib: open a model, hold a chat,
 * read the answer as complete UTF-8 text pieces. Templates, stop handling,
 * thinking output, context limits and KV reuse are the runtime's job; the
 * caller only deals with messages and text.
 *
 *   geistr_model_open → geistr_chat_open → geistr_chat_send → geistr_chat_next … END
 *
 * Every call to geistr_chat_send carries the whole conversation (stateless,
 * like the OpenAI chat API). The runtime keeps the KV cache of the unchanged
 * prefix, so a follow-up turn only pays for what is new.
 *
 * Conventions
 * - Status codes, never errno. Text for a status: geistr_status_text; detail
 *   for the last failure on an object: geistr_model_error / geistr_chat_error.
 * - Options and result structs start with `size`, set by the caller to
 *   sizeof the struct it was compiled with. Fields beyond `size` take their
 *   default, so a caller built against an older header keeps working; a size
 *   larger than this library knows is refused (GEISTR_INVALID). Pass nullptr
 *   for default options.
 * - Strings are UTF-8 and NUL-terminated unless a length is given. Strings
 *   returned by the runtime are borrowed: valid until the documented point.
 * - No global mutable state. Thread rules are stated per function.
 *
 * Stability: everything here is EXPERIMENTAL until 1.0 (see docs/API.md).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GEISTR_VERSION_MAJOR 0
#define GEISTR_VERSION_MINOR 1
#define GEISTR_VERSION_PATCH 0

/* "0.1.0" — the library's version, which may be newer than this header's. */
const char *geistr_version(void);

/* ------------------------------------------------------------------------ */
/* Status                                                                    */
/* ------------------------------------------------------------------------ */

typedef enum geistr_status {
    GEISTR_OK = 0,
    GEISTR_INVALID,   /* bad argument, unknown option size, call out of order */
    GEISTR_NO_MEMORY, /* allocation failed; the object is unchanged */
    GEISTR_IO,        /* model file missing or unreadable */
    GEISTR_FORMAT,    /* not a supported model, or its chat format is unknown */
    GEISTR_CONTEXT,   /* the conversation does not fit the context window */
    GEISTR_BACKEND,   /* the engine failed while running; reopen the model */
    GEISTR_CANCELLED, /* geistr_chat_cancel, or the emit callback said stop */
} geistr_status;

/* Static English text for a status, e.g. "the conversation does not fit". */
const char *geistr_status_text(geistr_status status);

/* ------------------------------------------------------------------------ */
/* Model                                                                     */
/* ------------------------------------------------------------------------ */

typedef enum geistr_processor {
    GEISTR_PROCESSOR_AUTO = 0, /* the runtime's recommendation for this model */
    GEISTR_PROCESSOR_CPU,
    GEISTR_PROCESSOR_GPU, /* Metal or Vulkan; GEISTR_BACKEND if none is available */
} geistr_processor;

typedef struct geistr_model_opts {
    size_t           size;      /* sizeof(geistr_model_opts) */
    geistr_processor processor; /* default AUTO */
    uint32_t         threads;   /* CPU threads; 0 = engine default */
    uint32_t         context;   /* context window in tokens; 0 = model default */
} geistr_model_opts;

#define GEISTR_MODEL_OPTS_INIT {sizeof(geistr_model_opts), GEISTR_PROCESSOR_AUTO, 0, 0}

typedef struct geistr_model geistr_model;

/* Open a GGUF model. On failure *out is nullptr and, when error is not
 * nullptr, a reason is written to error[0..error_cap).
 * Thread safety: any thread; opening several models at once is allowed. */
geistr_status geistr_model_open(const char              *path,
                                const geistr_model_opts *opts,
                                geistr_model           **out,
                                char                    *error,
                                size_t                   error_cap);

/* Open a model already in memory (e.g. embedded in the executable). The
 * bytes are borrowed, not copied: they must outlive the model. */
geistr_status geistr_model_open_memory(const void              *data,
                                       size_t                   len,
                                       const geistr_model_opts *opts,
                                       geistr_model           **out,
                                       char                    *error,
                                       size_t                   error_cap);

/* Release the caller's reference. Chats opened on this model keep it alive
 * until they are closed, so the order of closing does not matter.
 * nullptr is ignored. */
void geistr_model_close(geistr_model *model);

typedef struct geistr_model_info {
    size_t      size;        /* sizeof(geistr_model_info), set by the caller */
    const char *arch;        /* "gemma4", "qwen3", … */
    const char *chat_format; /* "gemma4", "chatml", "llama3", "bitnet", … */
    const char *backend;     /* the processor in use: "cpu", "metal", "vulkan" */
    uint32_t    context;     /* context window in tokens */
} geistr_model_info;

/* Fill *info. Its strings are borrowed until the model is released.
 * Thread safety: any thread, concurrently with chats on this model. */
geistr_status geistr_model_info_get(const geistr_model *model, geistr_model_info *info);

/* Detail of the last failure caused by this model handle (never nullptr). */
const char *geistr_model_error(const geistr_model *model);

/* ------------------------------------------------------------------------ */
/* Chat                                                                      */
/* ------------------------------------------------------------------------ */

typedef enum geistr_reasoning {
    GEISTR_REASONING_NONE = 0,  /* the model writes only an answer */
    GEISTR_REASONING_THINK_TAGS /* <think>…</think> before the answer */
} geistr_reasoning;

typedef enum geistr_overflow {
    GEISTR_OVERFLOW_REFUSE = 0, /* GEISTR_CONTEXT if the conversation is too long */
    GEISTR_OVERFLOW_DROP_OLDEST /* drop the oldest turns; a leading system message stays */
} geistr_overflow;

typedef struct geistr_chat_opts {
    size_t           size;        /* sizeof(geistr_chat_opts) */
    float            temperature; /* 0 = greedy (default) */
    float            top_p;       /* 1 = off (default) */
    uint32_t         max_tokens;  /* answer limit; 0 = the rest of the context */
    geistr_reasoning reasoning;   /* default NONE */
    geistr_overflow  overflow;    /* default REFUSE */
    int              thinking;    /* nonzero: deliver thinking as GEISTR_PART_THINKING;
                                     default 0: thinking is discarded, never kept */
} geistr_chat_opts;

#define GEISTR_CHAT_OPTS_INIT                                                                      \
    {sizeof(geistr_chat_opts), 0.0f, 1.0f, 0, GEISTR_REASONING_NONE, GEISTR_OVERFLOW_REFUSE, 0}

typedef struct geistr_message {
    const char *role;    /* "system", "user" or "assistant"; anything else counts as user */
    const char *content; /* UTF-8 */
} geistr_message;

typedef struct geistr_chat geistr_chat;

/* Open a chat on a model; it holds a reference to the model.
 * GEISTR_FORMAT if the model's chat format is unknown.
 * Thread safety: any thread. Several chats on one model may run on different
 * threads at the same time; the runtime serialises the engine where a
 * backend requires it. */
geistr_status
geistr_chat_open(geistr_model *model, const geistr_chat_opts *opts, geistr_chat **out);

/* Close the chat and release its model reference. nullptr is ignored.
 * Must not run concurrently with another call on the same chat. */
void geistr_chat_close(geistr_chat *chat);

/* Start an answer to messages[0..count): render with the model's template,
 * reuse the cached prefix, process the rest of the input. The messages are
 * copied; the caller may free them on return. An unfinished previous answer
 * is abandoned.
 * GEISTR_CONTEXT: too long (REFUSE), or even the last message alone does not
 * fit (DROP_OLDEST). GEISTR_INVALID: count is 0 or the last message is from
 * the assistant. GEISTR_CANCELLED: cancelled during input processing. */
geistr_status geistr_chat_send(geistr_chat *chat, size_t count, const geistr_message messages[]);

typedef enum geistr_part {
    GEISTR_PART_ANSWER = 0,
    GEISTR_PART_THINKING, /* only with opts.thinking */
    GEISTR_PART_END       /* the answer is complete; see geistr_chat_stats */
} geistr_part;

typedef struct geistr_piece {
    size_t      size; /* sizeof(geistr_piece), set by the caller */
    geistr_part part;
    const char *text; /* complete UTF-8, NUL-terminated; empty for END */
    size_t      len;  /* bytes in text */
} geistr_piece;

/* Produce the next piece of the answer (pull model; one token or more).
 * text is borrowed until the next call on this chat.
 * After END, further calls return END again until the next send.
 * GEISTR_CANCELLED once after a cancel; the chat stays usable for a new send.
 * GEISTR_INVALID before the first send. */
geistr_status geistr_chat_next(geistr_chat *chat, geistr_piece *piece);

/* Ask the running send/next to stop as soon as possible.
 * Thread safety: the one call that may run on any thread, at any time,
 * concurrently with send/next on the same chat. Idempotent. */
geistr_status geistr_chat_cancel(geistr_chat *chat);

/* Convenience: send, then deliver every piece to emit until END.
 * emit returning 0 cancels (the result is GEISTR_CANCELLED). */
typedef int (*geistr_emit_fn)(void *context, const geistr_piece *piece);
geistr_status geistr_chat_run(geistr_chat         *chat,
                              size_t               count,
                              const geistr_message messages[],
                              geistr_emit_fn       emit,
                              void                *context);

typedef enum geistr_finish {
    GEISTR_FINISH_NONE = 0,  /* still running, or nothing sent yet */
    GEISTR_FINISH_STOP,      /* the model ended its turn */
    GEISTR_FINISH_LENGTH,    /* max_tokens reached */
    GEISTR_FINISH_CONTEXT,   /* the context window is full */
    GEISTR_FINISH_CANCELLED, /* geistr_chat_cancel or emit */
    GEISTR_FINISH_ERROR      /* see geistr_chat_error */
} geistr_finish;

typedef struct geistr_stats {
    size_t        size; /* sizeof(geistr_stats), set by the caller */
    geistr_finish finish;
    uint32_t      prompt_tokens;    /* input tokens of the last send */
    uint32_t      reused_tokens;    /* of those, taken from the KV cache */
    uint32_t      dropped_messages; /* removed by GEISTR_OVERFLOW_DROP_OLDEST */
    uint32_t      output_tokens;    /* generated, thinking included */
    double        prefill_ms;       /* input processing; -1 if not reached */
    double        first_answer_ms;  /* send → first answer text; -1 if none */
    double        generation_ms;    /* first token → END */
    double        total_ms;         /* send → END */
} geistr_stats;

/* Statistics of the current or last answer.
 * Thread safety: same thread as send/next. */
geistr_status geistr_chat_stats(const geistr_chat *chat, geistr_stats *stats);

/* Detail of the last failure on this chat (never nullptr). */
const char *geistr_chat_error(const geistr_chat *chat);

#ifdef __cplusplus
}
#endif

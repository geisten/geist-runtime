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
 * A chat holds its conversation. geistr_chat_send takes only the new
 * messages; the answer becomes part of the conversation by itself. So every
 * token is processed once, and a turn costs only what it adds.
 * geistr_chat_rewind goes back to an earlier message, e.g. to regenerate or
 * to follow a client that edited its history.
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
#define GEISTR_VERSION_MINOR 2
#define GEISTR_VERSION_PATCH 0

/* Every call that can fail returns geistr_status: ignoring it is a warning
 * with C23 or C++17 (attribute [[nodiscard]]), nothing on older compilers. */
#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L) || (defined(__cplusplus) && __cplusplus >= 201703L)
#define GEISTR_NODISCARD [[nodiscard]]
#else
#define GEISTR_NODISCARD
#endif

/* "0.2.0" — the library's version, which may be newer than this header's. */
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

typedef struct geistr_decision_policy geistr_decision_policy;

typedef struct geistr_model_opts {
    size_t           size;      /* sizeof(geistr_model_opts) */
    geistr_processor processor; /* default AUTO */
    uint32_t         threads;   /* CPU threads; 0 = engine default */
    uint32_t         context;   /* tokens; 0 = the model's full window, reduced to
                                   what fits into memory (see info.context) */
    const char      *chat_format; /* override the detected template: "gemma3", "gemma4",
                                     "chatml", "llama3", "bitnet"; nullptr = detect from
                                     the model file. An unknown name is GEISTR_INVALID. */
    const geistr_decision_policy *decision; /* optional immutable policy from geistr_decision.h;
                                              copied at open; nullptr = decision disabled.
                                              Enabling requires the exact verified artifact. */
} geistr_model_opts;

#define GEISTR_MODEL_OPTS_INIT {sizeof(geistr_model_opts), GEISTR_PROCESSOR_AUTO, 0, 0, nullptr, nullptr}

typedef struct geistr_model geistr_model;

/* Open a GGUF model. On failure *out is nullptr and, when error is not
 * nullptr, a reason is written to error[0..error_cap).
 * Thread safety: any thread; opening several models at once is allowed. */
GEISTR_NODISCARD geistr_status geistr_model_open(const char              *path,
                                const geistr_model_opts *opts,
                                geistr_model           **out,
                                char                    *error,
                                size_t                   error_cap);

/* Open a model already in memory (e.g. embedded in the executable). The
 * bytes are borrowed, not copied: they must outlive the model. */
GEISTR_NODISCARD geistr_status geistr_model_open_memory(const void              *data,
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
    uint32_t    context;     /* the context window in use, in tokens */
} geistr_model_info;

/* Fill *info. Its strings are borrowed until the model is released.
 * Thread safety: any thread, concurrently with chats on this model. */
GEISTR_NODISCARD geistr_status geistr_model_info_get(const geistr_model *model, geistr_model_info *info);

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
    const char *const *stop;      /* extra stop strings for the answer (copied at open);
                                     the answer ends before the first match */
    size_t             n_stop;
} geistr_chat_opts;

#define GEISTR_CHAT_OPTS_INIT                                                                      \
    {sizeof(geistr_chat_opts), 0.0f, 1.0f, 0, GEISTR_REASONING_NONE, GEISTR_OVERFLOW_REFUSE, 0,       \
     nullptr, 0}

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
GEISTR_NODISCARD geistr_status
geistr_chat_open(geistr_model *model, const geistr_chat_opts *opts, geistr_chat **out);

/* Close the chat and release its model reference. nullptr is ignored.
 * Must not run concurrently with another call on the same chat. */
void geistr_chat_close(geistr_chat *chat);

/* Append messages[0..count) to the conversation and start the answer: only
 * these messages are rendered and processed. A system message belongs first,
 * in the first send. The messages are copied; the caller may free them on
 * return. An unfinished previous answer is ended and kept as it is.
 * GEISTR_CONTEXT: the conversation would not fit (REFUSE), or not even after
 * dropping the oldest turns (DROP_OLDEST); the chat is then unchanged.
 * GEISTR_INVALID: count is 0 or the last message is from the assistant.
 * GEISTR_CANCELLED: cancelled during input processing; the new messages are
 * then not part of the conversation. */
GEISTR_NODISCARD geistr_status geistr_chat_send(geistr_chat *chat, size_t count, const geistr_message messages[]);

/* Messages in the conversation: every sent message and every answer (an
 * answer counts once it has started). Turns dropped by DROP_OLDEST are gone. */
size_t geistr_chat_length(const geistr_chat *chat);

/* Go back to the first keep messages; later ones and their cache are
 * dropped, an unfinished answer is ended. rewind(chat, 0) starts a new
 * conversation. GEISTR_INVALID if keep > geistr_chat_length. */
GEISTR_NODISCARD geistr_status geistr_chat_rewind(geistr_chat *chat, size_t keep);

/* Answer limit for the following sends, as opts.max_tokens (0 = the rest of
 * the context). For callers whose limit differs per request. Not while an
 * answer is running: GEISTR_INVALID then. */
GEISTR_NODISCARD geistr_status geistr_chat_limit(geistr_chat *chat, uint32_t max_tokens);

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
 * text is borrowed until the next call on this chat. The answer ends at the
 * model's end of turn, max_tokens, a full context or a stop string.
 * After END, further calls return END again until the next send.
 * GEISTR_CANCELLED once after a cancel; the chat stays usable for a new send.
 * GEISTR_INVALID before the first send. */
GEISTR_NODISCARD geistr_status geistr_chat_next(geistr_chat *chat, geistr_piece *piece);

/* Ask the running send/next to stop as soon as possible.
 * Thread safety: the one call that may run on any thread, at any time,
 * concurrently with send/next on the same chat. Idempotent. */
geistr_status geistr_chat_cancel(geistr_chat *chat);

/* Convenience: send, then deliver every piece to emit until END.
 * emit returning 0 cancels (the result is GEISTR_CANCELLED). */
typedef int (*geistr_emit_fn)(void *context, const geistr_piece *piece);
GEISTR_NODISCARD geistr_status geistr_chat_run(geistr_chat         *chat,
                              size_t               count,
                              const geistr_message messages[],
                              geistr_emit_fn       emit,
                              void                *context);

typedef enum geistr_finish {
    GEISTR_FINISH_NONE = 0,  /* still running, or nothing sent yet */
    GEISTR_FINISH_STOP,      /* the model ended its turn, or a stop string matched */
    GEISTR_FINISH_LENGTH,    /* max_tokens reached */
    GEISTR_FINISH_CONTEXT,   /* the context window is full */
    GEISTR_FINISH_CANCELLED, /* geistr_chat_cancel or emit */
    GEISTR_FINISH_ERROR,     /* see geistr_chat_error */
    GEISTR_FINISH_REPETITION /* the answer repeated one passage back to back; ended there */
} geistr_finish;

typedef struct geistr_stats {
    size_t        size; /* sizeof(geistr_stats), set by the caller */
    geistr_finish finish;
    uint32_t      input_tokens;     /* processed by the last send (only what was new;
                                       all kept turns again after a DROP_OLDEST) */
    uint32_t      context_tokens;   /* in the context now, the answer included */
    uint32_t      dropped_messages; /* removed by GEISTR_OVERFLOW_DROP_OLDEST */
    uint32_t      output_tokens;    /* generated, thinking included */
    double        prefill_ms;       /* input processing; -1 if not reached */
    double        first_answer_ms;  /* send → first answer text; -1 if none */
    double        generation_ms;    /* first token → END */
    double        total_ms;         /* send → END */
} geistr_stats;

/* Statistics of the current or last answer.
 * Thread safety: same thread as send/next. */
GEISTR_NODISCARD geistr_status geistr_chat_stats(const geistr_chat *chat, geistr_stats *stats);

/* Detail of the last failure on this chat (never nullptr). */
const char *geistr_chat_error(const geistr_chat *chat);

#ifdef __cplusplus
}
#endif

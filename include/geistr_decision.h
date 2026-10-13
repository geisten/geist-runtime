/* Fixed-option decision contracts. EXPERIMENTAL until 1.0.
 * Configuration/preparation belongs to geistr; geistlib supplies scoring.
 * No API here changes ordinary chat or supplies calibrated confidence. */
#pragma once
#include "geistr.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Byte limits, excluding terminators. More restrictive loaded-model token
 * limits still apply. No request may expand these bounds. */
enum {
    GEISTR_DECISION_MAX_ID = 128,
    GEISTR_DECISION_MAX_QUESTION = 65536,
    GEISTR_DECISION_MAX_CONTEXT = 65536,
    GEISTR_DECISION_MAX_DESCRIPTION = 16384,
    GEISTR_DECISION_MAX_REQUEST = 262144,
    GEISTR_DECISION_MAX_SERIALIZED = 1048576,
    GEISTR_DECISION_MAX_OPTIONS = 26,
    GEISTR_DECISION_MAX_CONFIG = 65536,
    GEISTR_DECISION_MAX_POLICIES = 64
};

typedef enum geistr_operation { GEISTR_OPERATION_CHAT = 0, GEISTR_OPERATION_DECISION = 1 } geistr_operation;

typedef enum geistr_decision_mode {
    GEISTR_DECISION_DEFAULT = 0, /* use the immutable model policy */
    GEISTR_DECISION_DENSE = 1,
    GEISTR_DECISION_SELECTED_ROWS = 2
} geistr_decision_mode;

typedef enum geistr_decision_profile {
    GEISTR_DECISION_PROFILE_NONE = 0,
    GEISTR_DECISION_BONSAI2_27B_PQ2 = 1,
    GEISTR_DECISION_GEMMA4_E2B_Q4 = 2
} geistr_decision_profile;

/* Immutable value, copied by model_open. Hash is lowercase hex plus NUL.
 * A disabled policy never enables scoring. Device choice is independent. */
typedef struct geistr_decision_policy {
    char sha256[65];
    bool enabled;
    geistr_decision_profile profile;
    geistr_decision_mode mode;
} geistr_decision_policy;

typedef struct geistr_decision_config geistr_decision_config;

/* Separate, strict schema-1 JSON; never alters the schema-2 model catalog.
 * At most MAX_CONFIG bytes. Configuration uses ASCII literal keys/values;
 * escaped keys/values, duplicate/unknown fields and trailing JSON fail.
 * On failure *out is nullptr. Error text may be shortened to error_cap.
 * On success the config owns all entries, independent of input JSON.
 * Thread-safe; parsed configs are immutable, concurrent readers allowed. */
[[nodiscard]] geistr_status geistr_decision_config_parse(size_t len, size_t error_cap, const char *json,
                                                         geistr_decision_config **out, char *error);
void geistr_decision_config_free(geistr_decision_config *config);
[[nodiscard]] size_t geistr_decision_config_count(const geistr_decision_config *config);
/* Borrowed until config_free; nullptr for a missing/out-of-range entry.
 * A missing entry means default-off, not an inferred family profile. */
const geistr_decision_policy *geistr_decision_config_get(size_t index, const geistr_decision_config *config);
const geistr_decision_policy *geistr_decision_config_find(const char *sha256,
                                                          const geistr_decision_config *config);
const char *geistr_decision_profile_name(geistr_decision_profile profile);
const char *geistr_decision_profile_sha256(geistr_decision_profile profile);
/* Disabled policies may omit a profile. Enabled policies must match its
 * exact artifact, not a family, filename or caller-supplied model name. */
[[nodiscard]] geistr_status geistr_decision_policy_validate(const geistr_decision_policy *policy);

typedef struct geistr_decision_option {
    size_t id_len, description_len;
    const char *id, *description; /* lengths authoritative, no embedded NUL */
} geistr_decision_option;

typedef struct geistr_decision_request {
    size_t size;
    geistr_operation operation; /* must explicitly be DECISION */
    size_t question_len, context_len, n_options;
    const char *question;
    const char *context; /* optional only when context_len == 0 */
    const geistr_decision_option *options;
} geistr_decision_request;

/* Validates all lengths/UTF-8/IDs before preparation or inference. IDs are
 * byte-exact; controls U+0000..001F, U+007F..009F and U+2028/2029 fail.
 * Question/descriptions must be nonempty. Context may be absent. Caller
 * memory is borrowed for the synchronous call and must not change during it.
 * Status/size must be checked; no sanitization, normalization or truncation. */
[[nodiscard]] geistr_status
geistr_decision_request_validate(size_t error_cap, const geistr_decision_request *request, char *error);

typedef struct geistr_decision geistr_decision;
typedef struct geistr_decision_opts {
    size_t size;
    geistr_decision_mode mode; /* DEFAULT inherits the model's mode */
    size_t max_prompt_tokens;  /* 0 = min(model context, 512); never expands context */
} geistr_decision_opts;
#define GEISTR_DECISION_OPTS_INIT {sizeof(geistr_decision_opts), GEISTR_DECISION_DEFAULT, 0}

typedef struct geistr_decision_capability {
    size_t size;
    bool configured, available, dense, selected_rows;
    geistr_decision_profile profile;
    const char *backend;         /* actual engine name; borrowed until model released */
    const char *engine_revision; /* compile-time verified engine pin */
    const char *engine_version;  /* library-owned static string */
} geistr_decision_capability;

typedef struct geistr_decision_result {
    size_t size;
    size_t n_options, best_index, prompt_tokens;
    geistr_decision_mode mode;
    geistr_decision_profile profile;
    char external_id[GEISTR_DECISION_MAX_ID + 1]; /* copied, byte-exact */
    /* Owned by the decision instance, not by geistlib. Borrowed until its
     * next score/reset (including failure) or close; copy for longer use. */
    const float *logits;
    const double *selection_probabilities;
    size_t model_calls;                /* includes scoring attempts that fail/cancel */
    double preparation_ms, scoring_ms; /* synchronization included in scoring */
} geistr_decision_result;

/* Read-only reproducibility diagnostics, not model-generated output. These
 * loans last until the next score/reset/close; getters must not race scoring.
 * Empty if preparation failed. A completed plan may exist after a score error. */
typedef struct geistr_decision_plan {
    size_t size, n_prompt, n_candidates;
    const char *prompt;
    const int32_t *prompt_ids, *candidate_ids;
    const char *template_sha256;
} geistr_decision_plan;
[[nodiscard]] geistr_status geistr_decision_plan_get(const geistr_decision *decision,
                                                     geistr_decision_plan *out);

[[nodiscard]] bool geistr_decision_available(void);
/* No inference; a loaded model is required. Permission and actual linked
 * engine/mode support are separate. Concurrent reads allowed. */
[[nodiscard]] geistr_status geistr_decision_capability_get(const geistr_model *model,
                                                           geistr_decision_capability *out);
/* Resource observations: decision wrapper allocations only, not engine/KV
 * allocations or unique physical residency. Sample after quiescing callers.
 * Metal is a provider device counter, not RSS; never add it to unified RSS. */
typedef struct geistr_decision_resources {
    size_t size, live_allocations, live_bytes;
    bool provider_known, unified_memory;
    uint64_t provider_allocated_bytes;
} geistr_decision_resources;
[[nodiscard]] geistr_status geistr_decision_resources_get(const geistr_model *model,
                                                          geistr_decision_resources *out);

/* Owns independent tokenizer/scoring state and holds a model reference;
 * immutable weights are shared. Known older whole-field option sizes default
 * absent fields; unknown sizes/modes fail. Disabled/unsupported = FORMAT.
 * Setup/teardown and backend-required operations are serialized per model.
 * No generation or numeric/device fallback. On failure *out is nullptr.
 * Setup diagnostics go to the caller buffer, never shared model error state. */
[[nodiscard]] geistr_status geistr_decision_open(geistr_model *model, size_t error_cap,
                                                 const geistr_decision_opts *opts, geistr_decision **out,
                                                 char *error);
void geistr_decision_close(geistr_decision *decision);
/* Synchronous, one caller per instance (except cancel). Inputs must remain
 * immutable; result.size must equal sizeof the V1 result. Results must not
 * overlap inputs or this instance's borrowed arrays. Each call starts fresh.
 * Failure clears selection/borrowed arrays (best_index SIZE_MAX); attempted
 * call counts/timing remain defined. No incomplete selection is successful. */
[[nodiscard]] geistr_status geistr_decision_score(geistr_decision *decision,
                                                  const geistr_decision_request *request,
                                                  geistr_decision_result *result);
[[nodiscard]] geistr_status geistr_decision_reset(geistr_decision *decision);
/* Any thread; atomic, idempotent. Checked before preparation, between complete
 * tokenizer calls and before/after scoring. Cannot interrupt the synchronous
 * engine kernel; a cancelled result is discarded. A pending cancellation is
 * consumed by the next score, and subsequent requests remain usable. */
[[nodiscard]] geistr_status geistr_decision_cancel(geistr_decision *decision);
const char *geistr_decision_error(const geistr_decision *decision);

/* ---- Reusable contexts and atomic question batches (#163) ----------------
 * EXPERIMENTAL. One immutable context, several independent questions: the
 * context's common prefix is prefilled once and kept as a complete engine
 * snapshot (KV, recurrent state, positions, pending logits); every question
 * restores it, prefills only its own suffix and reads its options exactly as
 * geistr_decision_score would read the same complete prompt. Requires the
 * model's decision permission and an engine built with DECISION=1
 * DECISION_CONTEXT=1; DENSE only, FP32 KV, no fallback. A changed context
 * needs a new handle; snapshots are in-process only and never shared. */
enum { GEISTR_DECISION_MAX_BATCH = 32, GEISTR_DECISION_SNAPSHOT_BYTES = 268435456 };

typedef struct geistr_decision_context geistr_decision_context;

typedef struct geistr_decision_context_opts {
    size_t size;
    geistr_decision_mode mode; /* DEFAULT/DENSE; SELECTED_ROWS (also via the policy) = FORMAT */
    size_t max_prompt_tokens;  /* each full prompt; 0 = min(model context, 512); never expands context */
    size_t max_snapshot_bytes; /* 0 = GEISTR_DECISION_SNAPSHOT_BYTES; a larger snapshot = NO_MEMORY */
} geistr_decision_context_opts;
#define GEISTR_DECISION_CONTEXT_OPTS_INIT {sizeof(geistr_decision_context_opts), GEISTR_DECISION_DEFAULT, 0, 0}

/* One question; the ID follows the option ID rules and is unique per batch. */
typedef struct geistr_decision_question {
    size_t id_len, question_len, n_options;
    const char *id, *question; /* lengths authoritative, no embedded NUL */
    const geistr_decision_option *options;
} geistr_decision_question;

typedef struct geistr_decision_batch {
    size_t size;
    geistr_operation operation; /* must explicitly be DECISION */
    size_t n_questions;         /* 1..GEISTR_DECISION_MAX_BATCH */
    const geistr_decision_question *questions;
} geistr_decision_batch;

typedef struct geistr_decision_answer {
    size_t n_options, best_index;
    size_t prompt_tokens, uncached_tokens; /* full prompt; prefilled after the restore */
    char question_id[GEISTR_DECISION_MAX_ID + 1], external_id[GEISTR_DECISION_MAX_ID + 1];
    const float *logits;                    /* n_options, input order */
    const double *selection_probabilities; /* softmax over the options only */
} geistr_decision_answer;

/* answers/arrays are owned by the context and borrowed until its next batch
 * (including a failed one) or close; copy them for longer use. */
typedef struct geistr_decision_batch_result {
    size_t size;
    size_t n_answers;                      /* n_questions on success, else 0 */
    const geistr_decision_answer *answers; /* input order; nullptr on failure */
    size_t failed_index;                   /* SIZE_MAX: none or not question-specific */
    char failed_id[GEISTR_DECISION_MAX_ID + 1]; /* empty unless the failing ID itself was valid */
    size_t prepared_questions;             /* passed preflight */
    size_t model_calls;                    /* restore+suffix+readout attempts; 0 after a rejection */
    double preparation_ms, scoring_ms, batch_ms;
} geistr_decision_batch_result;

/* Read-only, fixed at open. Wrapper allocations stay in
 * geistr_decision_resources_get; snapshot_bytes is engine-owned memory. */
typedef struct geistr_decision_context_info {
    size_t size;
    geistr_decision_profile profile;
    geistr_decision_mode mode;  /* resolved: DENSE */
    size_t cached_tokens;       /* restored per question; 0 is valid */
    size_t snapshot_bytes, max_snapshot_bytes, prefill_chunk, max_prompt_tokens;
    size_t setup_model_calls;   /* prefix prefills at open (0 when nothing is cached) */
    double setup_ms;            /* open: rendering, tokenizing, engine setup, prefix, snapshot */
    const char *backend, *engine_revision, *engine_version; /* borrowed until close */
    const char *kv_cache;       /* "fp32" */
    const char *activation;     /* nullptr: not reported by the engine; never implied FP32 */
    char model_sha256[65], template_sha256[65];
    char context_sha256[65];    /* of the rendered common prefix bytes */
} geistr_decision_context_info;

/* Build capability: false unless the linked engine has DECISION_CONTEXT=1.
 * Model permission is geistr_decision_capability_get's `configured`; whether
 * the loaded model/backend can snapshot is known only from a successful open.
 * None of these implies quality eligibility. */
[[nodiscard]] bool geistr_decision_context_available(void);
/* Copies the context, holds a model reference, creates private tokenizer and
 * scoring state, preallocates every per-batch buffer, prefills and snapshots
 * the cached prefix. Never changes the model's policy, chats or weights.
 * Disabled permission, unsupported engine/model/snapshot or a conflicting mode
 * = FORMAT; invalid context = INVALID; too long = CONTEXT; snapshot budget =
 * NO_MEMORY. On failure *out is nullptr; diagnostics go to the caller buffer.
 * Setup/teardown are serialized with every operation on the model. */
[[nodiscard]] geistr_status geistr_decision_context_open(geistr_model *model, size_t context_len, size_t error_cap,
                                                         const char *context,
                                                         const geistr_decision_context_opts *opts,
                                                         geistr_decision_context **out, char *error);
/* Synchronous, one caller per handle (except cancel). Every question is
 * validated, rendered, tokenized and checked against the cached prefix before
 * any is scored; then each restores the snapshot and is scored serially.
 * Only a complete success publishes answers. Any failure (validation, engine,
 * restore, non-finite, cancellation) returns zero answers, names the failing
 * question where there is one and keeps the counts/timings; the context stays
 * usable. No allocation after open. result.size must be the V1 size. */
[[nodiscard]] geistr_status geistr_decision_context_score_batch(geistr_decision_context *context,
                                                                const geistr_decision_batch *batch,
                                                                geistr_decision_batch_result *result);
/* Any thread; atomic, idempotent. Checked before preflight, per question, between
 * tokenizer calls and before/after each restore+score; it cannot interrupt a
 * running kernel. Consumed by the batch it cancels; the next batch runs. */
[[nodiscard]] geistr_status geistr_decision_context_cancel(geistr_decision_context *context);
[[nodiscard]] geistr_status geistr_decision_context_info_get(const geistr_decision_context *context,
                                                             geistr_decision_context_info *out);
/* The last batch's error; borrowed until the next batch or close. */
const char *geistr_decision_context_error(const geistr_decision_context *context);
/* Must not race score/cancel/getters. Releases the model reference. */
void geistr_decision_context_close(geistr_decision_context *context);

#ifdef __cplusplus
}
#endif

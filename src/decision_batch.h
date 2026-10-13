/* Engine-free core of a reusable decision context (#163): the copied context,
 * its cached cut, the preallocated workspace and the atomic batch loop. The
 * engine enters only through callbacks, so release/sanitizer tests can inject
 * failures at any question. */
#pragma once
#include "decision_profile.h"

struct decision_batch_engine {
    void *context;
    decision_tokenize_fn tokenize; /* returns GEISTR_CANCELLED while a cancel is pending */
    bool (*cancelled)(void *context); /* consumes a pending cancellation */
    /* Restores the prefix, prefills the suffix, reads the candidates. */
    geistr_status (*score)(void *context, size_t n_suffix, size_t n_candidates, const int32_t *suffix,
                           const int32_t *candidates, float *logits, double *probabilities, size_t *best);
};

struct decision_batch {
    geistr_decision_profile profile;
    size_t context_len, token_capacity, n_cached, workspace_bytes;
    char *workspace; /* one allocation: context copy, prompt text, cached/scratch/prompt IDs */
    char *context, *text;
    int32_t *cached, *scratch, *prompts; /* prompts: GEISTR_DECISION_MAX_BATCH x token_capacity */
    size_t n_prompt[GEISTR_DECISION_MAX_BATCH];
    int32_t candidates[GEISTR_DECISION_MAX_BATCH][GEISTR_DECISION_MAX_OPTIONS];
    float logits[GEISTR_DECISION_MAX_BATCH][GEISTR_DECISION_MAX_OPTIONS];
    double probabilities[GEISTR_DECISION_MAX_BATCH][GEISTR_DECISION_MAX_OPTIONS];
    geistr_decision_answer answers[GEISTR_DECISION_MAX_BATCH];
    char context_sha256[65];
    char error[256];
    char detail[128]; /* the engine's reason for a failed score, set by its callback */
};

/* Validates and copies the context, renders its common prefix into text and
 * allocates the bounded workspace (checked products). b is zeroed first;
 * decision_batch_free releases it on success and failure. */
[[nodiscard]] geistr_status decision_batch_init(struct decision_batch *b, geistr_decision_profile profile,
                                                size_t max_prompt_tokens, size_t context_len, const char *context);
/* Tokenizes the rendered prefix into cached and sets n_cached for the chunk. */
[[nodiscard]] geistr_status decision_batch_cut(struct decision_batch *b, size_t chunk,
                                               const struct decision_batch_engine *engine);
void decision_batch_free(struct decision_batch *b);
/* Preflight of every question, then serial scoring; publishes only on success. */
[[nodiscard]] geistr_status decision_batch_score(struct decision_batch *b, const struct decision_batch_engine *engine,
                                                 const geistr_decision_batch *batch,
                                                 geistr_decision_batch_result *result);

#pragma once
#include "geistr_decision.h"
#include <stdint.h>

enum { DECISION_PROMPT_CAP = GEISTR_DECISION_MAX_REQUEST + 1024 };
const char *decision_pre_prompt(void);
const char *decision_template_hash(geistr_decision_profile profile);
/* Output empty on failure. BOS is textual only in the Gemma native render;
 * tokenizer inserts no extra BOS. No model-control strings in request data. */
[[nodiscard]] geistr_status decision_render(size_t *written, size_t capacity, geistr_decision_profile profile,
                                            const geistr_decision_request *request, char *out);
typedef geistr_status (*decision_tokenize_fn)(size_t *written, size_t capacity, const char *text,
                                              int32_t *ids, void *context);
/* Workspace arrays have capacity elements. Verification uses the COMPLETE
 * prefix for every key, not isolated-label or first-token approximations. */
[[nodiscard]] geistr_status decision_map(size_t *n_prompt, size_t capacity, size_t n_options,
                                         size_t text_capacity, decision_tokenize_fn tokenize, char *text,
                                         int32_t *prompt, int32_t *scratch, int32_t *candidates,
                                         void *context);

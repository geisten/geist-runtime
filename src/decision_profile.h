#pragma once
#include "geistr_decision.h"
#include <stdint.h>

enum { DECISION_PROMPT_CAP = GEISTR_DECISION_MAX_REQUEST + 1024 };
/* A verified profile (#162): the exact artifact, its native template and the
 * thinking-off generation prompt. New models add an entry; nothing here is
 * inferred from a family or file name, and V1 has no template override. */
struct decision_profile {
    const char *name, *artifact_sha256, *template_sha256;
    const char *bos, *system, *user, *assistant; /* native BOS/role markers */
    const char *thinking_off;              /* after the assistant role */
};
const struct decision_profile *decision_profile_get(geistr_decision_profile profile); /* nullptr: unknown */
/* An unverified profile from a chat template family (enum tpl_family): the
 * markers tpl_render_send writes around a system and a user message, the
 * generation prompt, and the thinking-off block when the model thinks. BOS is
 * not textual here: the caller prepends the BOS token as the chat does. For
 * Gemma 4 and ChatML this renders byte-identically to the verified profiles
 * (tests/test_decision_profile.c). false for an unknown family. */
struct decision_family_profile {
    struct decision_profile profile;
    char markers[1024];
};
[[nodiscard]] bool decision_profile_from_family(struct decision_family_profile *out, int family, bool thinks);
/* decision_render with an explicit profile (verified or from a family). */
[[nodiscard]] geistr_status decision_render_with(size_t *written, size_t capacity, const struct decision_profile *profile,
                                                 const geistr_decision_request *request, char *out);
const char *decision_pre_prompt(void);
const char *decision_template_hash(geistr_decision_profile profile);
/* Output empty on failure. BOS is textual only in the Gemma native render;
 * tokenizer inserts no extra BOS. No model-control strings in request data. */
[[nodiscard]] geistr_status decision_render(size_t *written, size_t capacity, geistr_decision_profile profile,
                                            const geistr_decision_request *request, char *out);
/* The bytes every full prompt over this context starts with: roles, pre-prompt
 * and the context block. Question, options and IDs never enter it. */
[[nodiscard]] geistr_status decision_render_prefix(size_t *written, size_t capacity,
                                                   geistr_decision_profile profile, size_t context_len,
                                                   const char *context, char *out);
typedef geistr_status (*decision_tokenize_fn)(size_t *written, size_t capacity, const char *text,
                                              int32_t *ids, void *context);
/* Workspace arrays have capacity elements. Verification uses the COMPLETE
 * prefix for every key, not isolated-label or first-token approximations. */
[[nodiscard]] geistr_status decision_map(size_t *n_prompt, size_t capacity, size_t n_options,
                                         size_t text_capacity, decision_tokenize_fn tokenize, char *text,
                                         int32_t *prompt, int32_t *scratch, int32_t *candidates,
                                         void *context);
/* Tokenizes the rendered common prefix into ids. The cached cut ends strictly
 * before its last token (text after it may merge with that token) and is
 * aligned down to the resolved prefill chunk; zero is a valid cut. */
[[nodiscard]] geistr_status decision_prefix_cut(size_t *n_cached, size_t capacity, size_t chunk,
                                                decision_tokenize_fn tokenize, const char *prefix,
                                                int32_t *ids, void *context);
/* Batch preflight of one full prompt: the cached IDs must be its exact prefix.
 * The suffix is prompt[n_cached..n_prompt), the uncached context tail included. */
[[nodiscard]] geistr_status decision_suffix(size_t *n_suffix, size_t n_cached, const int32_t *cached,
                                            size_t n_prompt, const int32_t *prompt);

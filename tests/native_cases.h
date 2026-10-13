#pragma once
#include <stddef.h>
#include <stdint.h>
struct native_case {
    const char *question, *context;
    size_t n_options;
    struct {
        const char *id, *description;
    } options[4];
    const char *prompt;
    const int32_t *ids;
    size_t n_ids;
    const int32_t *candidates;
};
/* #162: one context, independently rendered questions; cuts[k] is the cached
 * cut for <profile>_context_chunks[k]. */
struct native_context {
    const char *context, *prefix;
    const int32_t *prefix_ids;
    size_t n_prefix, cuts[2];
    const struct native_case *questions;
    size_t n_questions;
};
#include "fixtures/decisions/bonsai2.h"
#include "fixtures/decisions/gemma4.h"

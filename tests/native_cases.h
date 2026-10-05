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
#include "fixtures/decisions/bonsai2.h"
#include "fixtures/decisions/gemma4.h"

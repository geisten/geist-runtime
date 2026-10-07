/* common.h — what the stub (stub.c) and the runtime (runtime.c) share.
 * Internal; common.c has the API functions both implement alike. */
#pragma once
#include "geistr.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Copy caller options of any known size over the defaults (ABI rule). */
static inline bool opts_copy(void *dst, const void *src, size_t known) {
    if (src == nullptr)
        return true;
    size_t size;
    memcpy(&size, src, sizeof size);
    if (size < sizeof size || size > known)
        return false;
    memcpy(dst, src, size);
    memcpy(dst, &known, sizeof known); /* the copy is a full struct now */
    return true;
}

static inline double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec * 1e3 + (double) t.tv_nsec / 1e6;
}

static inline void stops_free(char **stops, size_t n) {
    for (size_t i = 0; stops && i < n; i++)
        free(stops[i]);
    free(stops);
}

/* Copies of a chat's stop strings (nullptr for none); GEISTR_INVALID for an
 * empty or missing one. */
static inline geistr_status stops_copy(const geistr_chat_opts *o, char ***out) {
    char **stops = o->n_stop ? calloc(o->n_stop, sizeof *stops) : nullptr;
    *out         = stops;
    if (o->n_stop && !stops)
        return GEISTR_NO_MEMORY;
    for (size_t i = 0; i < o->n_stop; i++)
        if (!o->stop[i] || !*o->stop[i] || !(stops[i] = strdup(o->stop[i]))) {
            stops_free(stops, o->n_stop);
            *out = nullptr;
            return o->stop[i] && *o->stop[i] ? GEISTR_NO_MEMORY : GEISTR_INVALID;
        }
    return GEISTR_OK;
}

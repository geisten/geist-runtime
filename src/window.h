/*
 * window.h — the context window a model is loaded with (#4, docs/API.md D8).
 * Pure arithmetic, so the policy is testable without an engine.
 */
#pragma once
#include <stdint.h>

#define WINDOW_STEP 256u /* a chosen window is a multiple of this */
#define WINDOW_MIN 512u  /* below this a model is refused as not fitting */

/* The caller's window (wanted > 0), capped at the trained one; else the
 * trained one (4096 if unknown) reduced to what fits: memory (the budget,
 * e.g. 3/4 of physical memory) less the weights, over the bytes a position
 * costs, rounded down to WINDOW_STEP. 0 means the model does not fit with
 * WINDOW_MIN positions. memory 0 or per_position 0: no reduction. */
static inline uint32_t window_choose(uint64_t trained,
                                     uint32_t wanted,
                                     uint64_t weight_bytes,
                                     uint64_t per_position,
                                     uint64_t memory) {
    if (trained == 0)
        trained = 4096;
    if (trained > UINT32_MAX / 2)
        trained = UINT32_MAX / 2;
    if (wanted)
        return (uint32_t) (wanted < trained ? wanted : trained);
    uint64_t fit = trained;
    if (memory && per_position) {
        fit = memory > weight_bytes ? (memory - weight_bytes) / per_position : 0;
        fit = fit / WINDOW_STEP * WINDOW_STEP;
    }
    if (fit >= trained)
        return (uint32_t) trained;
    return fit < WINDOW_MIN ? 0 : (uint32_t) fit;
}

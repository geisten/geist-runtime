/* test_window.c — the window policy (#4, D8) without an engine. */
#include "window.h"

#include <stdio.h>

static int failures;
static void check(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

int main(void) {
    const uint64_t G = 1ull << 30;
    /* SmolLM2 on 64 GiB: the trained 8192 fits. */
    check(window_choose(8192, 0, 386u << 20, 41984, 48 * G) == 8192, "fits: the trained window");
    /* Qwen3.8 27B on 64 GiB: 262144 positions at ~74.7 kB fit beside 16 GB. */
    check(window_choose(262144, 0, 16 * G, 74752, 48 * G) == 262144, "a large model's full window fits");
    /* The same on 32 GiB (24 GiB budget): (24 - 16) GiB / 74752 B, in steps of 256. */
    const uint32_t w = window_choose(262144, 0, 16 * G, 74752, 24 * G);
    check(w == (8 * G / 74752) / 256 * 256 && w % 256 == 0 && w < 262144, "reduced to what fits, a multiple of 256");
    /* Weights alone exceed the budget, or leave room for fewer than 512 positions. */
    check(window_choose(262144, 0, 16 * G, 74752, 12 * G) == 0, "weights over budget: does not fit");
    check(window_choose(262144, 0, 16 * G, 74752, 16 * G + 300 * 74752) == 0, "fewer than 512 positions: does not fit");
    /* A model trained shorter than WINDOW_MIN still loads with its own window. */
    check(window_choose(256, 0, 1 * G, 1 << 20, 2 * G) == 256, "a short trained window is kept");
    /* An explicit window wins, capped at the trained one. */
    check(window_choose(8192, 1024, 16 * G, 74752, 1 * G) == 1024, "explicit window kept, memory not consulted");
    check(window_choose(8192, 100000, 0, 1, 0) == 8192, "explicit window capped at the trained one");
    /* Unknown trained length and memory: 4096, no reduction. */
    check(window_choose(0, 0, 1 * G, 1000, 0) == 4096, "unknown: 4096");
    /* The GPU budget: free device memory less max(a tenth, 512 MiB), capped by RAM. */
    const uint64_t M = 1ull << 20;
    check(window_gpu_budget(48 * G, 11264 * M, 9000 * M) == 9000 * M - 11264 * M / 10, "GPU: free less a tenth");
    check(window_gpu_budget(4 * G, 11264 * M, 9000 * M) == 4 * G, "GPU: RAM is the smaller budget");
    check(window_gpu_budget(48 * G, 2 * G, 2 * G) == 2 * G - 512 * M, "GPU: at least 512 MiB headroom");
    check(window_gpu_budget(48 * G, 11264 * M, 300 * M) == 1, "GPU: nothing to spare is 1 byte, not unlimited");
    check(window_choose(131072, 0, 3 * G, 45056, window_gpu_budget(48 * G, 11264 * M, 300 * M)) == 0,
          "GPU: a full device fits nothing");
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("window: fits, large model, reduced, too big, short trained, explicit, unknown, GPU budget passed");
    return 0;
}

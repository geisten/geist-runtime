/* Test dispatcher only. The product's single geistr binary belongs to #11.
 * argv[1] "decide-batch" selects the batch command (#164). */
#include "../tools/geistr/decide.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void cancel_at_start(geistr_decision *d, void *context) {
    (void)context;
    if (d && geistr_decision_cancel(d) != GEISTR_OK)
        abort();
}
static void cancel_context(geistr_decision_context *c, void *context) {
    (void)context;
    if (c && geistr_decision_context_cancel(c) != GEISTR_OK)
        abort();
}
int main(int argc, char **argv) {
    const bool cancel = getenv("GEISTR_TEST_CANCEL");
    const geistr_decide_host host = {.active = cancel ? cancel_at_start : nullptr,
                                     .context_active = cancel ? cancel_context : nullptr};
    if (argc == 2 && !strcmp(argv[1], "early-exit-default")) { /* geistlib#286: the default per profile */
        for (int p = GEISTR_DECISION_PROFILE_NONE; p <= GEISTR_DECISION_GEMMA4_E2B_Q4; ++p)
            printf("%s %d\n", geistr_decision_profile_name((geistr_decision_profile)p),
                   geistr_decide_early_exit_default((geistr_decision_profile)p));
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "decide-batch"))
        return geistr_decide_batch_command((size_t)(argc - 2), (const char *const *)(argv + 2), &host);
    return geistr_decide_command((size_t)(argc - 1), (const char *const *)(argv + 1), &host);
}

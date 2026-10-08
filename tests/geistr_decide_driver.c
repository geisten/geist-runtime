/* Test dispatcher only. The product's single geistr binary belongs to #11. */
#include "../tools/geistr/decide.h"
#include <stdlib.h>
static void cancel_at_start(geistr_decision *d, void *context) {
    (void)context;
    if (d && geistr_decision_cancel(d) != GEISTR_OK)
        abort();
}
int main(int argc, char **argv) {
    const geistr_decide_host host = {.active = getenv("GEISTR_TEST_CANCEL") ? cancel_at_start : nullptr};
    return geistr_decide_command((size_t)(argc - 1), (const char *const *)(argv + 1), &host);
}

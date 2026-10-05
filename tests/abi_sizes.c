/* abi_sizes.c — struct sizes of the public headers, for the Python binding's
 * ctypes layout check (tests/test_python.py). */
#include "geistr.h"
#include "geistr_catalog.h"
#include "geistr_decision.h"
#include <stdio.h>

#define SIZE(t) printf("%s %zu\n", #t, sizeof(t))

int main(void) {
    SIZE(geistr_model_opts);
    SIZE(geistr_model_info);
    SIZE(geistr_chat_opts);
    SIZE(geistr_message);
    SIZE(geistr_piece);
    SIZE(geistr_stats);
    SIZE(geistr_catalog_entry);
    SIZE(geistr_device);
    SIZE(geistr_fit);
    SIZE(geistr_decision_policy);
    SIZE(geistr_decision_opts);
    SIZE(geistr_decision_option);
    SIZE(geistr_decision_request);
    SIZE(geistr_decision_result);
    SIZE(geistr_decision_capability);
    SIZE(geistr_decision_plan);
    SIZE(geistr_decision_resources);
    return 0;
}

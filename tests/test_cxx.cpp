// test_cxx.cpp — the header compiles and links as C++ (extern "C", no C-only syntax).
#include "geistr.h"
#include "geistr_catalog.h"
#include "geistr_decision.h"

#include <cstdio>
#include <string>

int main() {
    const geistr_decision_option choice{1, 3, "x", "yes"};
    const geistr_decision_request request{sizeof(geistr_decision_request), GEISTR_OPERATION_DECISION,
                                         2, 0, 1, "Q?", nullptr, &choice};
    if (geistr_decision_request_validate(0, &request, nullptr) != GEISTR_OK)
        return 1;
    geistr_model     *model = nullptr;
    geistr_model_opts mopts = GEISTR_MODEL_OPTS_INIT;
    if (geistr_model_open("stub:echo", &mopts, &model, nullptr, 0) != GEISTR_OK)
        return 1;
    geistr_chat     *chat  = nullptr;
    geistr_chat_opts copts = GEISTR_CHAT_OPTS_INIT;
    if (geistr_chat_open(model, &copts, &chat) != GEISTR_OK)
        return 1;
    std::string    answer;
    geistr_message msg = {"user", "C++"};
    auto emit          = [](void *out, const geistr_piece *piece) -> int {
        static_cast<std::string *>(out)->append(piece->text, piece->len);
        return 1;
    };
    geistr_status s = geistr_chat_run(chat, 1, &msg, emit, &answer);
    geistr_chat_close(chat);
    geistr_model_close(model);
    if (s != GEISTR_OK || answer != "Echo: C++")
        return 1;
    std::puts("geistr API: C++ build passed");
    return 0;
}

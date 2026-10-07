/* common.c — the API functions the stub and the runtime implement alike. */
#include "geistr.h"

const char *geistr_status_text(geistr_status s) {
    switch (s) {
    case GEISTR_OK:
        return "ok";
    case GEISTR_INVALID:
        return "invalid argument or call order";
    case GEISTR_NO_MEMORY:
        return "out of memory";
    case GEISTR_IO:
        return "model file missing or unreadable";
    case GEISTR_FORMAT:
        return "not a supported model or chat format";
    case GEISTR_CONTEXT:
        return "the conversation does not fit the context window";
    case GEISTR_BACKEND:
        return "the engine failed; reopen the model";
    case GEISTR_CANCELLED:
        return "cancelled";
    }
    return "unknown status";
}

geistr_status geistr_chat_run(geistr_chat         *c,
                              size_t               count,
                              const geistr_message messages[],
                              geistr_emit_fn       emit,
                              void                *context) {
    if (!emit)
        return GEISTR_INVALID;
    geistr_status s     = geistr_chat_send(c, count, messages);
    geistr_piece  piece = {.size = sizeof piece};
    while (s == GEISTR_OK && (s = geistr_chat_next(c, &piece)) == GEISTR_OK) {
        if (piece.part == GEISTR_PART_END)
            return GEISTR_OK;
        if (!emit(context, &piece))
            geistr_chat_cancel(c);
    }
    return s;
}

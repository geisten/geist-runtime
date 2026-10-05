/*
 * chat.c — a terminal chat: the whole embedding story.
 *
 *   build/chat <model.gguf | stub:echo>
 *
 * The chat holds the conversation: each line is sent on its own, and the
 * runtime processes only that line. Ctrl-C stops the answer, not the chat.
 */
#include "geistr.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static geistr_chat *volatile running; /* for the Ctrl-C handler */

static void on_interrupt(int) {
    if (running)
        geistr_chat_cancel(running); /* an atomic store: safe in a handler */
}

static int print_piece(void *, const geistr_piece *piece) {
    fwrite(piece->text, 1, piece->len, stdout);
    fflush(stdout);
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <model.gguf | stub:echo>\n", argv[0]);
        return 2;
    }
    geistr_model *model = nullptr;
    char          error[256];
    if (geistr_model_open(argv[1], nullptr, &model, error, sizeof error) != GEISTR_OK) {
        fprintf(stderr, "cannot open %s: %s\n", argv[1], error);
        return 1;
    }
    geistr_chat_opts opts = GEISTR_CHAT_OPTS_INIT;
    opts.overflow         = GEISTR_OVERFLOW_DROP_OLDEST;
    geistr_chat  *chat    = nullptr;
    geistr_status s       = geistr_chat_open(model, &opts, &chat);
    geistr_model_close(model); /* the chat holds its own reference */
    if (s != GEISTR_OK) {
        fprintf(stderr, "cannot chat: %s\n", geistr_status_text(s));
        return 1;
    }
    signal(SIGINT, on_interrupt);
    running = chat;

    char line[2048];
    while (fputs("> ", stdout) >= 0 && fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        geistr_message turn       = {"user", line};
        s = geistr_chat_run(chat, 1, &turn, print_piece, nullptr);
        puts(s == GEISTR_CANCELLED ? " [stopped]" : "");
        if (s != GEISTR_OK && s != GEISTR_CANCELLED)
            fprintf(stderr, "%s: %s\n", geistr_status_text(s), geistr_chat_error(chat));
    }
    running = nullptr;
    geistr_chat_close(chat);
    return 0;
}

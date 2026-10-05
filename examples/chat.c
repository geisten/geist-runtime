/*
 * chat.c — a terminal chat in about 80 lines: the whole embedding story.
 *
 *   build/chat <model.gguf | stub:echo>
 *
 * Each line is a user turn; the conversation is sent in full every time and
 * the runtime reuses what it already processed. Ctrl-C stops the answer.
 */
#include "geistr.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static geistr_chat *volatile running; /* for the Ctrl-C handler */

static void on_interrupt(int) {
    if (running)
        geistr_chat_cancel(running); /* an atomic store: safe in a handler */
}

struct answer {
    char  *text;
    size_t len;
};

/* Print the piece and keep it: the answer is the next turn's history. */
static int print_piece(void *context, const geistr_piece *piece) {
    struct answer *a    = context;
    char          *grow = realloc(a->text, a->len + piece->len + 1);
    if (!grow)
        return 0; /* stop the answer rather than lose part of it */
    memcpy(grow + a->len, piece->text, piece->len + 1);
    a->text = grow;
    a->len += piece->len;
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

    geistr_message turns[64];
    char          *owned[64];
    size_t         n = 0;
    char           line[2048];
    while (n + 2 <= 64 && fputs("> ", stdout) >= 0 && fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        owned[n]                  = strdup(line);
        turns[n]                  = (geistr_message) {"user", owned[n]};
        n++;
        struct answer answer = {};
        s = geistr_chat_run(chat, n, turns, print_piece, &answer);
        puts(s == GEISTR_CANCELLED ? " [stopped]" : "");
        if (s != GEISTR_OK && s != GEISTR_CANCELLED) {
            fprintf(stderr, "%s: %s\n", geistr_status_text(s), geistr_chat_error(chat));
            n--; /* drop the turn that failed */
            free(owned[n]);
            free(answer.text);
            continue;
        }
        owned[n] = answer.text ? answer.text : strdup("");
        turns[n] = (geistr_message) {"assistant", owned[n]};
        n++;
    }
    running = nullptr;
    geistr_chat_close(chat);
    for (size_t i = 0; i < n; i++)
        free(owned[i]);
    return 0;
}

/* lineedit.h — a small line editor for geistr chat: UTF-8 aware editing,
 * history, Tab completion with a candidate list, and a dim inline hint
 * (fish-style) that → or Tab accepts. No dependency (readline, libedit).
 *
 * The core (le_feed) is a state machine over input bytes that writes its
 * screen updates to a FILE, so it is tested without a terminal; le_read
 * puts the terminal in raw mode around it. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* A completion: the whole line it would make, and a short description
 * shown when several match (may be nullptr). */
struct le_candidate {
    const char *line, *help;
};
/* Fill up to max candidates for the line as typed; returns how many. */
typedef size_t (*le_complete_fn)(void *ctx, const char *line, struct le_candidate *out, size_t max);

enum le_event { LE_MORE, LE_SUBMIT, LE_EOF, LE_INTERRUPT };

struct le {
    FILE          *out;
    const char    *prompt; /* may contain ANSI escapes */
    unsigned       width;  /* terminal columns */
    le_complete_fn complete;
    void          *ctx;
    char           buf[4096];
    size_t         len, pos;        /* bytes; pos is at a character boundary */
    unsigned       rows, cursor_row; /* of the last drawing, for the next */
    char           esc[8];          /* an escape sequence in progress */
    size_t         n_esc;
    char         **history; /* oldest first; owned */
    size_t         n_history, browsing;
    char           draft[4096]; /* the line being typed while browsing history */
};

void          le_init(struct le *e, FILE *out, unsigned width, le_complete_fn complete, void *ctx);
void          le_free(struct le *e);
void          le_begin(struct le *e, const char *prompt); /* a new line: draw the prompt */
enum le_event le_feed(struct le *e, unsigned char byte);
void          le_remember(struct le *e, const char *line); /* add to the history */

/* Read a line from a terminal on fd 0: LE_SUBMIT (line in e->buf), LE_EOF
 * (Ctrl-D on an empty line) or LE_INTERRUPT (Ctrl-C). */
enum le_event le_read(struct le *e, const char *prompt);

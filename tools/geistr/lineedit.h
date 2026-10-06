/* lineedit.h — a small line editor for geistr chat: UTF-8 aware editing,
 * history, and a selection list under the line while a / command is typed
 * (filtered as you type; ↑/↓ choose, Tab takes, Enter takes and submits
 * unless the choice needs an argument, Esc closes), with the chosen entry's
 * rest shown dim in the line (→ takes it). Keys as in Claude Code: Ctrl-C
 * clears the line (on an empty line: LE_INTERRUPT, the caller asks for a
 * second one), ? on an empty line is LE_HELP. No dependency (readline, libedit).
 *
 * The core (le_feed) is a state machine over input bytes that writes its
 * screen updates to a FILE, so it is tested without a terminal; le_read
 * puts the terminal in raw mode around it. */
#pragma once
#include <signal.h>
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

enum le_event { LE_MORE, LE_SUBMIT, LE_EOF, LE_INTERRUPT, LE_HELP };

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
    size_t         sel;         /* the chosen entry of the list */
    bool           closed;      /* Esc closed the list until the line changes */
    unsigned       menu_rows;   /* list rows of the last drawing */
    unsigned char  ahead[256];  /* keys typed while the answer ran: read first */
    size_t         n_ahead;
    /* Set by the caller's SIGINT handler: a Ctrl-C that came while the
     * terminal was cooked (between two reads). le_read clears it and
     * returns LE_INTERRUPT once the terminal is raw, so none is lost. */
    volatile sig_atomic_t *interrupted;
};

void          le_init(struct le *e, FILE *out, unsigned width, le_complete_fn complete, void *ctx);
void          le_free(struct le *e);
void          le_begin(struct le *e, const char *prompt); /* a new line: draw the prompt */
enum le_event le_feed(struct le *e, unsigned char byte);
void          le_remember(struct le *e, const char *line); /* add to the history */
void          le_escape(struct le *e); /* the Esc key alone: close the list */
void          le_type_ahead(struct le *e, const unsigned char *keys, size_t n);

/* Read a line from a terminal on fd 0: LE_SUBMIT (line in e->buf), LE_EOF
 * (Ctrl-D on an empty line) or LE_INTERRUPT (Ctrl-C). */
enum le_event le_read(struct le *e, const char *prompt);

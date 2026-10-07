/* test_lineedit.c — geistr's line editor (tools/geistr/lineedit.c) without a
 * terminal: key bytes in, the line and the screen updates out. */
#include "lineedit.h"
#include <locale.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, what)                                                    \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, what);     \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static const struct le_candidate commands[] = {
        {"/gpu", "processor"}, {"/cpu", "processor"},  {"/clear", "new conversation"}, {"/model ", "another model"},
        {"/temp ", "temperature"}, {"/system ", "system prompt"}, {"/info", "what runs"}, {"/help", "help"},
        {"/save", "keep"}, {"/exit", "end"}};
static const char *const models[] = {"gemma4-e2b", "gemma4-e4b", "qwen3-0.6b"};

static size_t complete(void *ctx, const char *line, struct le_candidate *out, size_t max) {
    (void) ctx;
    size_t n = 0;
    if (!strncmp(line, "/model ", 7)) {
        static char lines[3][64];
        for (size_t i = 0; i < 3 && n < max; i++)
            if (!strncmp(models[i], line + 7, strlen(line + 7))) {
                snprintf(lines[n], sizeof lines[n], "/model %s", models[i]);
                out[n] = (struct le_candidate) {lines[n], nullptr};
                n++;
            }
        return n;
    }
    if (line[0] != '/' || strchr(line, ' '))
        return 0;
    for (size_t i = 0; i < sizeof commands / sizeof *commands && n < max; i++)
        if (!strncmp(commands[i].line, line, strlen(line)))
            out[n++] = commands[i];
    return n;
}

/* Feed keys; returns the last event and the screen output. */
static enum le_event keys(struct le *e, const char *bytes, char **screen) {
    size_t len = 0;
    FILE  *f   = open_memstream(screen, &len);
    e->out     = f;
    le_begin(e, "\033[2m⚡\033[0m > ");
    enum le_event ev = LE_MORE;
    for (const char *b = bytes; *b && ev == LE_MORE; b++)
        ev = le_feed(e, (unsigned char) *b);
    fclose(f);
    return ev;
}

static void line_is(struct le *e, const char *bytes, const char *want, const char *what) {
    char         *screen = nullptr;
    enum le_event ev     = keys(e, bytes, &screen);
    if (ev != LE_SUBMIT || strcmp(e->buf, want)) {
        fprintf(stderr, "%s: got \"%s\" (event %d), want \"%s\"\n", what, e->buf, ev, want);
        failures++;
    }
    free(screen);
}

int main(void) {
    if (!setlocale(LC_CTYPE, "C.UTF-8") && !setlocale(LC_CTYPE, "en_US.UTF-8")) {
        puts("lineedit: SKIPPED (no UTF-8 locale)");
        return 0;
    }
    struct le e;
    le_init(&e, stdout, 80, complete, nullptr);

    /* typing and editing, UTF-8 by characters */
    line_is(&e, "Hallo Welt\r", "Hallo Welt", "typing");
    line_is(&e, "Grüße\x7f\x7f\r", "Grü", "backspace removes whole characters");
    line_is(&e, "abc\x1b[D\x1b[DX\r", "aXbc", "left arrow, insert");
    line_is(&e, "über\x01Ä\r", "Äüber", "Ctrl-A, insert before an umlaut");
    line_is(&e, "eins zwei\x17" "drei\r", "eins drei", "Ctrl-W deletes a word");
    line_is(&e, "abcdef\x1b[D\x1b[D\x0b\r", "abcd", "Ctrl-K");
    line_is(&e, "abc\x15x\r", "x", "Ctrl-U");
    line_is(&e, "ab\x1b[H\x1b[3~\r", "b", "Home, Delete");

    /* completion */
    line_is(&e, "/mo\tx\r", "/model x", "Tab completes a command with its space");
    line_is(&e, "/model q\t\r", "/model qwen3-0.6b", "model ids complete");
    line_is(&e, "/model gem\t\r", "/model gemma4-e2b", "Tab takes the chosen entry (the first)");
    line_is(&e, "/model gem\x1b[B\t\r", "/model gemma4-e4b", "↓ chooses in the list");
    line_is(&e, "/model gem\x1b[B\x1b[B\t\r", "/model gemma4-e2b", "the list wraps around");
    line_is(&e, "/he\r", "/help", "Enter takes and runs a complete command");
    line_is(&e, "/mo\rq\r", "/model qwen3-0.6b", "Enter on a command with an argument waits for it");
    line_is(&e, "/c\x1b[B\r", "/clear", "↓ then Enter");
    line_is(&e, "/i\x1b[C\r", "/info", "→ takes the hint");
    char *screen = nullptr;
    keys(&e, "/c", &screen); /* the list opens while typing: /cpu and /clear with their help */
    CHECK(strstr(screen, "\033[7m/cpu") && strstr(screen, "/clear") && strstr(screen, "new conversation"),
          "a selection list under the line, the first entry chosen");
    CHECK(e.menu_rows == 2, "two entries listed");
    free(screen);
    keys(&e, "/", &screen);
    CHECK(e.menu_rows == 8 + 1 && strstr(screen, "more"), "/ alone: all commands, a window and '… more'");
    free(screen);
    keys(&e, "/inf", &screen);
    CHECK(strstr(screen, "\033[2mo\033[0m"), "dim hint for the rest of the chosen entry");
    free(screen);
    size_t len = 0;
    FILE  *f   = open_memstream(&screen, &len); /* le_escape draws: a live stream */
    e.out      = f;
    le_begin(&e, "> ");
    le_feed(&e, '/'), le_feed(&e, 'c');
    le_escape(&e);
    fclose(f);
    free(screen);
    CHECK(e.menu_rows == 0, "Esc closes the list");
    keys(&e, "hello", &screen);
    CHECK(!strstr(screen, "\033[2m") || strstr(screen, "\033[2m⚡"), "no hint for ordinary text");
    free(screen);
    keys(&e, "/xyz\t", &screen);
    CHECK(strchr(screen, '\a'), "no candidate: bell");
    free(screen);

    /* history */
    le_remember(&e, "erste Frage");
    le_remember(&e, "zweite Frage");
    le_remember(&e, "zweite Frage"); /* no duplicate in a row */
    CHECK(e.n_history == 2, "history keeps distinct lines");
    line_is(&e, "\x1b[A\r", "zweite Frage", "↑ the last line");
    line_is(&e, "\x1b[A\x1b[A\r", "erste Frage", "↑↑ the one before");
    line_is(&e, "neu\x1b[A\x1b[B\r", "neu", "↓ back to the line being typed");

    /* events */
    CHECK(keys(&e, "\x04", &screen) == LE_EOF, "Ctrl-D on an empty line ends");
    free(screen);
    CHECK(keys(&e, "abc\x03", &screen) == LE_MORE && e.len == 0, "Ctrl-C clears the line");
    free(screen);
    CHECK(keys(&e, "\x03", &screen) == LE_INTERRUPT, "Ctrl-C on an empty line: the caller decides");
    free(screen);
    CHECK(keys(&e, "?", &screen) == LE_HELP, "? on an empty line: the shortcuts");
    free(screen);
    line_is(&e, "a?\r", "a?", "? inside a line is text");
    line_is(&e, "ab\x01\x04\r", "b", "Ctrl-D in a line deletes");

    /* a line longer than the terminal wraps: the cursor goes back over rows */
    e.width = 20;
    keys(&e, "0123456789012345678901234567890123456789\x01", &screen);
    CHECK(e.rows == 3 && e.cursor_row == 0, "40 characters after a 5-column prompt: 3 rows, cursor on the first");
    free(screen);
    le_free(&e);
    /* a bracketed paste keeps its line breaks (Tab: spaces, ? a character); Enter after it sends */
    line_is(&e, "Look:\033[200~line one\r\nline two?\tend\033[201~\r", "Look:line one\nline two?    end",
            "paste");
    {
        char *shown = nullptr;
        keys(&e, "\033[200~a\nb\033[201~", &shown);
        if (!strstr(shown, "a↵b") || strchr(shown, '\n')) {
            fprintf(stderr, "paste: a line break shows as ↵, got %s\n", shown);
            failures++;
        }
        free(shown);
    }
    if (failures) {
        fprintf(stderr, "lineedit: %d failures\n", failures);
        return 1;
    }
    puts("lineedit: UTF-8 editing, keys, selection list, hint, Esc, Ctrl-C, ?, history, wrapping, paste passed");
    return 0;
}

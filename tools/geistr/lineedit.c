/* lineedit.c — see lineedit.h. */
#define _XOPEN_SOURCE 700 /* wcwidth */
#include "lineedit.h"
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#define HISTORY_MAX 500
#define CANDIDATES_MAX 64

/* ---- text measures ------------------------------------------------------ */

static size_t next_char(const struct le *e, size_t i) {
    if (i < e->len)
        i++;
    while (i < e->len && ((unsigned char) e->buf[i] & 0xc0) == 0x80)
        i++;
    return i;
}

static size_t prev_char(const struct le *e, size_t i) {
    if (i)
        i--;
    while (i && ((unsigned char) e->buf[i] & 0xc0) == 0x80)
        i--;
    return i;
}

/* Terminal columns of s[0..n): wide characters count two, ANSI escapes none. */
static unsigned columns(const char *s, size_t n) {
    unsigned  w  = 0;
    mbstate_t st = {};
    for (size_t i = 0; i < n;) {
        if (s[i] == '\033') {
            i++;
            if (i < n && s[i] == '[') {
                i++;
                while (i < n && !(s[i] >= '@' && s[i] <= '~'))
                    i++;
                i++;
            }
            continue;
        }
        wchar_t wc;
        size_t  k = mbrtowc(&wc, s + i, n - i, &st);
        if (k == (size_t) -1 || k == (size_t) -2 || k == 0) {
            memset(&st, 0, sizeof st);
            w++, i++;
            continue;
        }
        int cw = wcwidth(wc);
        w += cw < 0 ? 1 : (unsigned) cw;
        i += k;
    }
    return w;
}

/* Whether the character just typed is still incomplete (UTF-8 arrives a byte
 * at a time): then wait for the rest before drawing. */
static bool partial(const struct le *e) {
    size_t start = e->pos ? e->pos - 1 : 0;
    while (start && ((unsigned char) e->buf[start] & 0xc0) == 0x80)
        start--;
    unsigned char lead = (unsigned char) e->buf[start];
    size_t        need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
    return e->pos - start < need;
}

/* ---- candidates and the hint -------------------------------------------- */

static size_t candidates(struct le *e, struct le_candidate *c) {
    if (!e->complete)
        return 0;
    char line[sizeof e->buf];
    memcpy(line, e->buf, e->len);
    line[e->len] = 0;
    return e->complete(e->ctx, line, c, CANDIDATES_MAX);
}

/* The longest prefix all candidates share (at least the line itself). */
static size_t common(const struct le_candidate *c, size_t n) {
    size_t k = strlen(c[0].line);
    for (size_t i = 1; i < n; i++) {
        size_t j = 0;
        while (j < k && c[i].line[j] && c[i].line[j] == c[0].line[j])
            j++;
        k = j;
    }
    while (k && ((unsigned char) c[0].line[k] & 0xc0) == 0x80) /* never inside a character */
        k--;
    return k;
}

/* What would complete the line, shown dim after it: only at its end. */
static const char *hint(struct le *e, char out[static 256]) {
    out[0] = 0;
    struct le_candidate c[CANDIDATES_MAX];
    size_t              n = e->pos == e->len && e->len ? candidates(e, c) : 0;
    if (!n)
        return out;
    size_t k = common(c, n);
    if (k > e->len && !strncmp(c[0].line, e->buf, e->len))
        snprintf(out, 256, "%.*s", (int) (k - e->len), c[0].line + e->len);
    return out;
}

/* ---- drawing ------------------------------------------------------------ */

static void draw(struct le *e, bool with_hint) {
    char        h[256];
    const char *tail = with_hint ? hint(e, h) : "";
    if (e->cursor_row)
        fprintf(e->out, "\033[%uA", e->cursor_row);
    fputs("\r\033[J", e->out);
    fputs(e->prompt, e->out);
    fwrite(e->buf, 1, e->len, e->out);
    if (*tail)
        fprintf(e->out, "\033[2m%s\033[0m", tail);
    unsigned w      = e->width ? e->width : 80;
    unsigned prompt = columns(e->prompt, strlen(e->prompt));
    unsigned total  = prompt + columns(e->buf, e->len) + columns(tail, strlen(tail));
    unsigned cursor = prompt + columns(e->buf, e->pos);
    if (total && total % w == 0)
        fputs("\r\n", e->out); /* leave the pending wrap: the cursor is on the next row */
    unsigned end_row = total / w, row = cursor / w, col = cursor % w;
    if (end_row > row)
        fprintf(e->out, "\033[%uA", end_row - row);
    fputs("\r", e->out);
    if (col)
        fprintf(e->out, "\033[%uC", col);
    e->rows       = end_row + 1;
    e->cursor_row = row;
    fflush(e->out);
}

/* ---- editing ------------------------------------------------------------ */

static void set_line(struct le *e, const char *s) {
    size_t n = strlen(s);
    if (n >= sizeof e->buf)
        n = sizeof e->buf - 1;
    memcpy(e->buf, s, n);
    e->buf[n] = 0;
    e->len = e->pos = n;
}

static void erase(struct le *e, size_t from, size_t to) {
    memmove(e->buf + from, e->buf + to, e->len - to);
    e->len -= to - from;
    e->buf[e->len] = 0;
    if (e->pos > to)
        e->pos -= to - from;
    else if (e->pos > from)
        e->pos = from;
}

static void history_move(struct le *e, int step) {
    if (step < 0 && e->browsing == 0)
        return;
    if (step > 0 && e->browsing >= e->n_history)
        return;
    if (e->browsing == e->n_history) { /* leaving the line being typed: keep it */
        memcpy(e->draft, e->buf, e->len);
        e->draft[e->len] = 0;
    }
    e->browsing = (size_t) ((long) e->browsing + step);
    set_line(e, e->browsing == e->n_history ? e->draft : e->history[e->browsing]);
}

static void complete(struct le *e) {
    e->pos = e->len;
    struct le_candidate c[CANDIDATES_MAX];
    size_t              n = candidates(e, c);
    if (!n) {
        fputc('\a', e->out);
        return;
    }
    size_t k = common(c, n);
    if (k > e->len && !strncmp(c[0].line, e->buf, e->len)) { /* extend to what all share */
        char line[sizeof e->buf];
        snprintf(line, sizeof line, "%.*s", (int) k, c[0].line);
        set_line(e, line);
        return;
    }
    if (n == 1)
        return;
    draw(e, false); /* several: list them under the line, then the line again */
    fputs("\r\n", e->out);
    for (size_t i = 0; i < n; i++)
        fprintf(e->out, "  %-24s\033[2m%s\033[0m\r\n", c[i].line, c[i].help ? c[i].help : "");
    e->rows = e->cursor_row = 0;
}

static void escape(struct le *e) {
    const char *s = e->esc + 1; /* after ESC */
    char        final = e->esc[e->n_esc - 1];
    if (*s == '[' || *s == 'O') {
        if (final == 'A')
            history_move(e, -1);
        else if (final == 'B')
            history_move(e, +1);
        else if (final == 'C') {
            if (e->pos == e->len) { /* at the end, → takes the hint */
                char h[256];
                if (*hint(e, h)) {
                    char line[sizeof e->buf];
                    snprintf(line, sizeof line, "%.*s%s", (int) e->len, e->buf, h);
                    set_line(e, line);
                }
            } else
                e->pos = next_char(e, e->pos);
        } else if (final == 'D')
            e->pos = prev_char(e, e->pos);
        else if (final == 'H' || !strcmp(s, "[1~") || !strcmp(s, "[7~"))
            e->pos = 0;
        else if (final == 'F' || !strcmp(s, "[4~") || !strcmp(s, "[8~"))
            e->pos = e->len;
        else if (!strcmp(s, "[3~") && e->pos < e->len)
            erase(e, e->pos, next_char(e, e->pos));
    }
    e->n_esc = 0;
}

void le_init(struct le *e, FILE *out, unsigned width, le_complete_fn fn, void *ctx) {
    *e = (struct le) {.out = out, .width = width, .complete = fn, .ctx = ctx, .prompt = ""};
}

void le_free(struct le *e) {
    for (size_t i = 0; i < e->n_history; i++)
        free(e->history[i]);
    free(e->history);
    e->history   = nullptr;
    e->n_history = 0;
}

void le_remember(struct le *e, const char *line) {
    if (!*line || (e->n_history && !strcmp(e->history[e->n_history - 1], line)))
        return;
    if (e->n_history == HISTORY_MAX) {
        free(e->history[0]);
        memmove(e->history, e->history + 1, (HISTORY_MAX - 1) * sizeof *e->history);
        e->n_history--;
    }
    char **grown = realloc(e->history, (e->n_history + 1) * sizeof *grown);
    char  *copy  = strdup(line);
    if (!grown || !copy) {
        free(copy);
        if (grown)
            e->history = grown;
        return;
    }
    e->history                 = grown;
    e->history[e->n_history++] = copy;
}

void le_begin(struct le *e, const char *prompt) {
    e->prompt   = prompt;
    e->len      = e->pos = 0;
    e->buf[0]   = 0;
    e->rows     = e->cursor_row = 0;
    e->n_esc    = 0;
    e->browsing = e->n_history;
    draw(e, true);
}

enum le_event le_feed(struct le *e, unsigned char c) {
    if (e->n_esc) {
        if (e->n_esc < sizeof e->esc)
            e->esc[e->n_esc++] = (char) c;
        bool done = e->n_esc == 2 ? (c != '[' && c != 'O') : (c >= '@' && c <= '~');
        if (e->n_esc == sizeof e->esc || done) {
            if (e->n_esc >= 3) /* ESC [ … or ESC O …; ESC + key (Alt) is ignored */
                escape(e);
            e->n_esc = 0;
            draw(e, true);
        }
        return LE_MORE;
    }
    switch (c) {
    case '\r':
    case '\n':
        e->pos = e->len;
        draw(e, false);
        fputs("\r\n", e->out);
        fflush(e->out);
        return LE_SUBMIT;
    case 3: /* Ctrl-C */
        e->pos = e->len;
        draw(e, false);
        fputs("^C\r\n", e->out);
        fflush(e->out);
        return LE_INTERRUPT;
    case 4: /* Ctrl-D */
        if (!e->len) {
            fputs("\r\n", e->out);
            fflush(e->out);
            return LE_EOF;
        }
        if (e->pos < e->len)
            erase(e, e->pos, next_char(e, e->pos));
        break;
    case 27:
        e->esc[0] = 27;
        e->n_esc  = 1;
        return LE_MORE;
    case '\t':
        complete(e);
        break;
    case 127:
    case 8:
        if (e->pos)
            erase(e, prev_char(e, e->pos), e->pos);
        break;
    case 1:
        e->pos = 0;
        break;
    case 5:
        e->pos = e->len;
        break;
    case 2:
        e->pos = prev_char(e, e->pos);
        break;
    case 6:
        e->pos = next_char(e, e->pos);
        break;
    case 11: /* Ctrl-K */
        erase(e, e->pos, e->len);
        break;
    case 21: /* Ctrl-U */
        erase(e, 0, e->pos);
        break;
    case 23: { /* Ctrl-W: the word before the cursor */
        size_t from = e->pos;
        while (from && e->buf[from - 1] == ' ')
            from--;
        while (from && e->buf[from - 1] != ' ')
            from--;
        erase(e, from, e->pos);
        break;
    }
    case 12: /* Ctrl-L */
        fputs("\033[H\033[2J", e->out);
        e->rows = e->cursor_row = 0;
        break;
    case 16:
        history_move(e, -1);
        break;
    case 14:
        history_move(e, +1);
        break;
    default:
        if (c < 32 || e->len + 1 >= sizeof e->buf)
            return LE_MORE;
        memmove(e->buf + e->pos + 1, e->buf + e->pos, e->len - e->pos);
        e->buf[e->pos++] = (char) c;
        e->buf[++e->len] = 0;
        if (partial(e)) /* wait for the rest of the character */
            return LE_MORE;
    }
    draw(e, true);
    return LE_MORE;
}

enum le_event le_read(struct le *e, const char *prompt) {
    struct termios cooked, raw;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col)
        e->width = ws.ws_col;
    if (tcgetattr(STDIN_FILENO, &cooked) != 0)
        return LE_EOF;
    raw = cooked;
    raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw); /* not FLUSH: keep what was typed ahead */
    le_begin(e, prompt);
    enum le_event ev = LE_MORE;
    unsigned char c;
    while (ev == LE_MORE) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            ev = LE_EOF;
            break;
        }
        ev = le_feed(e, c);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &cooked);
    return ev;
}

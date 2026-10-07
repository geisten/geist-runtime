/* lineedit.c — see lineedit.h. */
#define _XOPEN_SOURCE 700 /* wcwidth */
#include "lineedit.h"
#include <stdlib.h>
#include <string.h>
#include <poll.h>
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

/* The list under the line: while a / command is typed (at its end), unless
 * Esc closed it or the line already is the only entry. */
#define MENU_ROWS 8
static size_t menu(struct le *e, struct le_candidate c[CANDIDATES_MAX]) {
    if (e->closed || e->pos != e->len || !e->len || e->buf[0] != '/')
        return 0;
    size_t n = candidates(e, c);
    if (n == 1 && !strcmp(c[0].line, e->buf))
        return 0;
    if (e->sel >= n)
        e->sel = n ? n - 1 : 0;
    return n;
}

/* The chosen entry's rest, dim after the line. */
static const char *hint(struct le *e, char out[static 256]) {
    out[0] = 0;
    struct le_candidate c[CANDIDATES_MAX];
    size_t              n = menu(e, c);
    if (n && !strncmp(c[e->sel].line, e->buf, e->len))
        snprintf(out, 256, "%s", c[e->sel].line + e->len);
    return out;
}

/* ---- drawing ------------------------------------------------------------ */

/* s in at most w columns; cut, it ends with "…". */
static void put_cut(FILE *out, const char *s, unsigned w) {
    unsigned room = columns(s, strlen(s)) <= w ? w : w ? w - 1 : 0, used = 0;
    const char *p = s;
    while (*p) {
        size_t k = 1;
        while (((unsigned char) p[k] & 0xc0) == 0x80)
            k++;
        unsigned cw = columns(p, k);
        if (used + cw > room)
            break;
        fwrite(p, 1, k, out);
        used += cw;
        p += k;
    }
    if (*p && w)
        fputs("…", out);
}

/* The line as typed; a pasted line break shows as ↵ (one column, as columns() counts it). */
static void put_line(struct le *e) {
    for (size_t i = 0; i < e->len; i++)
        e->buf[i] == '\n' ? (void) fputs("↵", e->out) : (void) fputc(e->buf[i], e->out);
}

static void draw(struct le *e, bool with_menu) {
    char                h[256];
    struct le_candidate c[CANDIDATES_MAX];
    size_t              n    = with_menu ? menu(e, c) : 0;
    const char         *tail = with_menu ? hint(e, h) : "";
    if (e->cursor_row)
        fprintf(e->out, "\033[%uA", e->cursor_row);
    fputs("\r\033[J", e->out);
    fputs(e->prompt, e->out);
    put_line(e);
    if (*tail)
        fprintf(e->out, "\033[2m%s\033[0m", tail);
    unsigned w      = e->width ? e->width : 80;
    unsigned prompt = columns(e->prompt, strlen(e->prompt));
    unsigned total  = prompt + columns(e->buf, e->len) + columns(tail, strlen(tail));
    unsigned cursor = prompt + columns(e->buf, e->pos);
    if (total && total % w == 0)
        fputs("\r\n", e->out); /* leave the pending wrap: the cursor is on the next row */
    unsigned end_row = total / w, row = cursor / w, col = cursor % w;
    /* the list: a window of MENU_ROWS around the chosen entry */
    unsigned shown = 0;
    if (n) {
        size_t   first = e->sel >= MENU_ROWS ? e->sel - MENU_ROWS + 1 : 0;
        unsigned name  = 0;
        for (size_t i = 0; i < n; i++) {
            unsigned cw = columns(c[i].line, strlen(c[i].line));
            name        = cw > name ? cw : name;
        }
        for (size_t i = first; i < n && shown < MENU_ROWS; i++, shown++) {
            fputs("\r\n  ", e->out);
            bool chosen = i == e->sel;
            fputs(chosen ? "\033[7m" : "", e->out);
            put_cut(e->out, c[i].line, w > 4 ? w - 4 : 1);
            unsigned used = 2 + columns(c[i].line, strlen(c[i].line));
            for (unsigned k = columns(c[i].line, strlen(c[i].line)); k < name && used < w - 1; k++, used++)
                fputc(' ', e->out);
            fputs(chosen ? "\033[0m" : "", e->out);
            if (c[i].help && used + 3 < w) {
                fputs("  \033[2m", e->out);
                put_cut(e->out, c[i].help, w - used - 3);
                fputs("\033[0m", e->out);
            }
        }
        if (n > shown)
            fprintf(e->out, "\r\n  \033[2m… %zu more\033[0m", n - shown), shown++;
    }
    unsigned up = end_row - row + shown;
    if (up)
        fprintf(e->out, "\033[%uA", up);
    fputs("\r", e->out);
    if (col)
        fprintf(e->out, "\033[%uC", col);
    e->rows       = end_row + 1 + shown;
    e->menu_rows  = shown;
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

/* ↑/↓: through the list while it is open, else the history. */
static void up_down(struct le *e, int step) {
    struct le_candidate c[CANDIDATES_MAX];
    size_t              n = menu(e, c);
    if (n)
        e->sel = (e->sel + n + (size_t) (step > 0 ? 1 : n - 1)) % n;
    else
        history_move(e, step);
}

/* Take the chosen entry; true if it is complete (no argument to follow). */
static bool take(struct le *e) {
    struct le_candidate c[CANDIDATES_MAX];
    size_t              n = menu(e, c);
    if (!n)
        return true;
    set_line(e, c[e->sel].line);
    e->sel = 0;
    return e->len && e->buf[e->len - 1] != ' ';
}

static void escape(struct le *e) {
    const char *s = e->esc + 1; /* after ESC */
    char        final = e->esc[e->n_esc - 1];
    if (e->n_esc == 6 && !memcmp(e->esc, "\033[20", 4) && final == '~') /* ESC[200~ … ESC[201~: a paste */
        e->pasting = e->esc[4] == '0';
    else if (*s == '[' || *s == 'O') {
        if (final == 'A')
            up_down(e, -1);
        else if (final == 'B')
            up_down(e, +1);
        else if (final == 'C') {
            if (e->pos == e->len) { /* at the end, → takes the hint */
                char h[256];
                if (*hint(e, h))
                    (void) take(e);
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

void le_escape(struct le *e) {
    e->n_esc  = 0;
    e->closed = true;
    draw(e, true);
}

void le_type_ahead(struct le *e, const unsigned char *keys, size_t n) {
    if (n > sizeof e->ahead - e->n_ahead)
        n = sizeof e->ahead - e->n_ahead;
    memcpy(e->ahead + e->n_ahead, keys, n);
    e->n_ahead += n;
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
    e->sel      = 0;
    e->closed   = false;
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
    bool after_cr = e->pasted_cr;
    e->pasted_cr  = e->pasting && c == '\r';
    if (e->pasting && (c == '\r' || c == '\n' || c == '\t')) { /* pasted: a line break or spaces, never send */
        const char *add = c == '\t' ? "    " : c == '\n' && after_cr ? "" : "\n";
        for (; *add && e->len + 1 < sizeof e->buf; add++) {
            memmove(e->buf + e->pos + 1, e->buf + e->pos, e->len - e->pos);
            e->buf[e->pos++] = *add;
            e->buf[++e->len] = 0;
        }
        draw(e, true);
        return LE_MORE;
    }
    if (c != '\t' && c != 16 && c != 14 && c != '\r' && c != '\n' && c != 27) { /* the line changes: a new list */
        e->sel    = 0;
        e->closed = false;
    }
    switch (c) {
    case '\r':
    case '\n': {
        struct le_candidate list[CANDIDATES_MAX];
        if (menu(e, list)) { /* Enter runs the chosen command; one that takes an argument shows its state */
            (void) take(e);
            while (e->len && e->buf[e->len - 1] == ' ')
                e->buf[--e->len] = 0;
        }
        e->pos = e->len;
        draw(e, false);
        fputs("\r\n", e->out);
        fflush(e->out);
        return LE_SUBMIT;
    }
    case 3: /* Ctrl-C: clears the line; on an empty one the caller decides */
        if (e->len) {
            e->len = e->pos = 0;
            e->buf[0]       = 0;
            e->closed       = false;
            break;
        }
        draw(e, false);
        fputs("\r\n", e->out);
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
    case '\t': {
        struct le_candidate list[CANDIDATES_MAX];
        e->pos = e->len;
        if (menu(e, list))
            (void) take(e);
        else
            fputc('\a', e->out);
        break;
    }
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
        up_down(e, -1);
        break;
    case 14:
        up_down(e, +1);
        break;
    default:
        if (c < 32 || e->len + 1 >= sizeof e->buf)
            return LE_MORE;
        if (c == '?' && !e->len && !e->pasting) { /* ? on an empty line: the shortcuts */
            draw(e, false);
            fputs("\r\n", e->out);
            fflush(e->out);
            return LE_HELP;
        }
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
    if (e->interrupted && *e->interrupted) { /* from now on Ctrl-C is a key, not a signal */
        *e->interrupted = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &cooked);
        return LE_INTERRUPT;
    }
    fputs("\033[?2004h", e->out); /* bracketed paste: pasted text is marked */
    e->pasting = false;
    le_begin(e, prompt);
    enum le_event ev = LE_MORE;
    unsigned char c;
    for (size_t i = 0; i < e->n_ahead && ev == LE_MORE; i++) /* typed while the answer ran */
        ev = le_feed(e, e->ahead[i]);
    e->n_ahead = 0;
    while (ev == LE_MORE) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            ev = LE_EOF;
            break;
        }
        struct pollfd more = {.fd = STDIN_FILENO, .events = POLLIN};
        if (c == 27 && !e->n_esc && poll(&more, 1, 30) == 0) { /* Esc alone, not a key sequence */
            le_escape(e);
            continue;
        }
        ev = le_feed(e, c);
    }
    fputs("\033[?2004l", e->out);
    fflush(e->out);
    tcsetattr(STDIN_FILENO, TCSANOW, &cooked);
    return ev;
}

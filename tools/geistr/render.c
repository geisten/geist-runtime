/* render.c — see render.h. A character-level state machine: every byte is
 * handled with only the bytes before it, so piece boundaries never matter. */
#define _XOPEN_SOURCE 700 /* wcwidth */
#include "render.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---- styles ------------------------------------------------------------- */

static void style(struct md *m) {
    char now[sizeof m->last] = "";
    if (m->mode == MD_RAW)
        return;
    if (m->bold)
        strcat(now, "b");
    if (m->italic)
        strcat(now, "i");
    if (m->code || m->block)
        strcat(now, "c");
    if (m->heading)
        strcat(now, "h");
    if (m->quote)
        strcat(now, "q");
    if (m->math)
        strcat(now, "m");
    if (m->underline)
        strcat(now, "u");
    if (!strcmp(now, m->last))
        return;
    strcpy(m->last, now);
    if (m->mode == MD_TAGS) {
        fprintf(m->out, "«%s»", now);
        return;
    }
    fputs("\033[0m", m->out);
    if (m->bold)
        fputs("\033[1m", m->out);
    if (m->italic)
        fputs("\033[3m", m->out);
    if (m->code || m->block)
        fputs("\033[36m", m->out);
    if (m->heading)
        fputs("\033[1;35m", m->out);
    if (m->quote)
        fputs("\033[2m", m->out);
    if (m->math)
        fputs("\033[33m", m->out);
    if (m->underline)
        fputs("\033[4m", m->out);
}

void md_init(struct md *m, enum md_mode mode, FILE *out) {
    *m = (struct md) {.mode = mode, .out = out, .sink = out, .line_start = true, .head = true};
}

/* ---- math --------------------------------------------------------------- */

struct mo {
    char  *out;
    size_t cap, n;
};

static void put_n(struct mo *o, const char *s, size_t len) {
    if (o->n + len < o->cap) {
        memcpy(o->out + o->n, s, len);
        o->n += len;
        o->out[o->n] = 0;
    }
}
static void put(struct mo *o, const char *s) {
    put_n(o, s, strlen(s));
}

static const char *const symbols[][2] = {
        {"alpha", "α"},   {"beta", "β"},     {"gamma", "γ"},     {"delta", "δ"},    {"epsilon", "ε"},
        {"varepsilon", "ε"}, {"zeta", "ζ"},  {"eta", "η"},       {"theta", "θ"},    {"vartheta", "ϑ"},
        {"iota", "ι"},    {"kappa", "κ"},    {"lambda", "λ"},    {"mu", "μ"},       {"nu", "ν"},
        {"xi", "ξ"},      {"pi", "π"},       {"rho", "ρ"},       {"sigma", "σ"},    {"tau", "τ"},
        {"upsilon", "υ"}, {"phi", "φ"},      {"varphi", "φ"},    {"chi", "χ"},      {"psi", "ψ"},
        {"omega", "ω"},   {"Gamma", "Γ"},    {"Delta", "Δ"},     {"Theta", "Θ"},    {"Lambda", "Λ"},
        {"Xi", "Ξ"},      {"Pi", "Π"},       {"Sigma", "Σ"},     {"Phi", "Φ"},      {"Psi", "Ψ"},
        {"Omega", "Ω"},   {"cdot", "·"},     {"times", "×"},     {"div", "÷"},      {"pm", "±"},
        {"mp", "∓"},      {"le", "≤"},       {"leq", "≤"},       {"ge", "≥"},       {"geq", "≥"},
        {"neq", "≠"},     {"ne", "≠"},       {"approx", "≈"},    {"equiv", "≡"},    {"sim", "∼"},
        {"propto", "∝"},  {"infty", "∞"},    {"sum", "∑"},       {"prod", "∏"},     {"int", "∫"},
        {"oint", "∮"},    {"partial", "∂"},  {"nabla", "∇"},     {"to", "→"},       {"rightarrow", "→"},
        {"leftarrow", "←"}, {"Rightarrow", "⇒"}, {"Leftarrow", "⇐"}, {"iff", "⇔"}, {"implies", "⇒"},
        {"mapsto", "↦"},  {"in", "∈"},       {"notin", "∉"},     {"subset", "⊂"},   {"subseteq", "⊆"},
        {"supset", "⊃"},  {"cup", "∪"},      {"cap", "∩"},       {"forall", "∀"},   {"exists", "∃"},
        {"emptyset", "∅"}, {"varnothing", "∅"}, {"neg", "¬"},   {"land", "∧"},     {"lor", "∨"},
        {"cdots", "⋯"},   {"ldots", "…"},    {"dots", "…"},      {"circ", "∘"},     {"degree", "°"},
        {"angle", "∠"},   {"perp", "⊥"},     {"parallel", "∥"},  {"hbar", "ℏ"},     {"ell", "ℓ"},
        {"langle", "⟨"},  {"rangle", "⟩"},   {"lfloor", "⌊"},    {"rfloor", "⌋"},   {"lceil", "⌈"},
        {"rceil", "⌉"},   {"quad", "  "},    {"qquad", "    "},  {"lim", "lim"},    {"log", "log"},
        {"ln", "ln"},     {"sin", "sin"},    {"cos", "cos"},     {"tan", "tan"},    {"exp", "exp"},
        {"max", "max"},   {"min", "min"},
};
static const char *const skipped[] = {"left", "right", "big", "Big", "bigl", "bigr", "Bigl", "Bigr",
                                      "displaystyle", "textstyle", "limits", "nolimits", "boxed"};
static const char *const sup[][2] = {{"0", "⁰"}, {"1", "¹"}, {"2", "²"}, {"3", "³"}, {"4", "⁴"}, {"5", "⁵"},
                                     {"6", "⁶"}, {"7", "⁷"}, {"8", "⁸"}, {"9", "⁹"}, {"+", "⁺"}, {"-", "⁻"},
                                     {"=", "⁼"}, {"(", "⁽"}, {")", "⁾"}, {"n", "ⁿ"}, {"i", "ⁱ"}, {"x", "ˣ"},
                                     {"T", "ᵀ"}, {"*", "*"}, {"′", "′"}};
static const char *const sub[][2] = {{"0", "₀"}, {"1", "₁"}, {"2", "₂"}, {"3", "₃"}, {"4", "₄"}, {"5", "₅"},
                                     {"6", "₆"}, {"7", "₇"}, {"8", "₈"}, {"9", "₉"}, {"+", "₊"}, {"-", "₋"},
                                     {"=", "₌"}, {"(", "₍"}, {")", "₎"}, {"a", "ₐ"}, {"e", "ₑ"}, {"o", "ₒ"},
                                     {"x", "ₓ"}, {"h", "ₕ"}, {"k", "ₖ"}, {"l", "ₗ"}, {"m", "ₘ"}, {"n", "ₙ"},
                                     {"p", "ₚ"}, {"s", "ₛ"}, {"t", "ₜ"}, {"i", "ᵢ"}, {"j", "ⱼ"}, {"r", "ᵣ"},
                                     {"u", "ᵤ"}, {"v", "ᵥ"}};
static const char *const blackboard[][2] = {{"R", "ℝ"}, {"N", "ℕ"}, {"Z", "ℤ"}, {"Q", "ℚ"}, {"C", "ℂ"}};

static void expr(const char *s, const char *e, struct mo *o);

/* The next argument: {…} (without braces), a \command, or one UTF-8 char. */
static const char *group(const char **p, const char *e, const char **end) {
    const char *s = *p;
    while (s < e && *s == ' ')
        s++;
    if (s >= e) {
        *p = *end = s;
        return s;
    }
    if (*s == '{') {
        int depth = 0;
        for (const char *q = s; q < e; q++) {
            depth += *q == '{' ? 1 : *q == '}' ? -1 : 0;
            if (!depth) {
                *end = q;
                *p   = q + 1;
                return s + 1;
            }
        }
        *p = *end = e; /* unbalanced: the rest */
        return s + 1;
    }
    const char *q = s + 1;
    if (*s == '\\')
        while (q < e && isalpha((unsigned char) *q))
            q++;
    else
        while (q < e && ((unsigned char) *q & 0xc0) == 0x80)
            q++;
    *p = *end = q;
    return s;
}

static void render(const char *s, const char *e, char *buf, size_t cap) {
    struct mo t = {buf, cap, 0};
    buf[0]      = 0;
    expr(s, e, &t);
}

/* A part that reads unambiguously without parentheses next to / or √. */
static bool simple(const char *t) {
    return strlen(t) && !strpbrk(t, " +-*/=<>,");
}

/* t, in parentheses unless bare. */
static void operand(struct mo *o, const char *t, bool bare) {
    put(o, bare ? "" : "("), put(o, t), put(o, bare ? "" : ")");
}

static bool script(const char *t, const char *const table[][2], size_t n, struct mo *o) {
    char out[256] = "";
    for (const char *c = t; *c;) {
        size_t len = 1;
        while (((unsigned char) c[len] & 0xc0) == 0x80)
            len++;
        size_t k = 0;
        for (; k < n; k++)
            if (strlen(table[k][0]) == len && !strncmp(table[k][0], c, len))
                break;
        if (k == n || strlen(out) + strlen(table[k][1]) >= sizeof out)
            return false;
        strcat(out, table[k][1]);
        c += len;
    }
    put(o, out);
    return true;
}

static void expr(const char *s, const char *e, struct mo *o) {
    char a[512], b[512];
    while (s < e) {
        if (*s == '\\') {
            const char *n = ++s;
            while (s < e && isalpha((unsigned char) *s))
                s++;
            size_t len = (size_t) (s - n);
            if (!len) { /* \, \; \  \{ \} \\ and other single characters */
                if (s < e) {
                    put_n(o, strchr(",;: !\\", *s) ? " " : s, 1);
                    s++;
                }
                continue;
            }
#define IS(word) (len == strlen(word) && !strncmp(n, word, len))
            bool done = false;
            for (size_t k = 0; !done && k < sizeof symbols / sizeof *symbols; k++)
                if (IS(symbols[k][0]))
                    put(o, symbols[k][1]), done = true;
            for (size_t k = 0; !done && k < sizeof skipped / sizeof *skipped; k++)
                done = IS(skipped[k]);
            if (done)
                continue;
            const char *g, *ge;
            if (IS("frac") || IS("dfrac") || IS("tfrac")) {
                g = group(&s, e, &ge), render(g, ge, a, sizeof a);
                g = group(&s, e, &ge), render(g, ge, b, sizeof b);
                operand(o, a, simple(a)), put(o, "/"), operand(o, b, simple(b));
            } else if (IS("sqrt")) {
                if (s < e && *s == '[') { /* \sqrt[n]{x}: the index as superscript */
                    const char *close = memchr(s, ']', (size_t) (e - s));
                    if (close) {
                        render(s + 1, close, b, sizeof b);
                        if (!script(b, sup, sizeof sup / sizeof *sup, o))
                            put(o, b);
                        s = close + 1;
                    }
                }
                g = group(&s, e, &ge), render(g, ge, a, sizeof a);
                put(o, "√"), operand(o, a, simple(a));
            } else if (IS("mathbb")) {
                g = group(&s, e, &ge);
                bool hit = false;
                for (size_t k = 0; k < sizeof blackboard / sizeof *blackboard; k++)
                    if (ge - g == 1 && *g == blackboard[k][0][0])
                        put(o, blackboard[k][1]), hit = true;
                if (!hit)
                    put_n(o, g, (size_t) (ge - g));
            } else if (IS("text") || IS("textrm") || IS("mathrm") || IS("mathbf") || IS("mathit") ||
                       IS("operatorname") || IS("mathcal") || IS("boldsymbol")) {
                g = group(&s, e, &ge);
                if (n[0] == 't')
                    put_n(o, g, (size_t) (ge - g)); /* text is literal */
                else
                    expr(g, ge, o);
            } else
                put_n(o, n, len); /* unknown: its name */
#undef IS
        } else if (*s == '^' || *s == '_') {
            bool        up = *s++ == '^';
            const char *g, *ge;
            g = group(&s, e, &ge);
            render(g, ge, a, sizeof a);
            if (!(up ? script(a, sup, sizeof sup / sizeof *sup, o) : script(a, sub, sizeof sub / sizeof *sub, o))) {
                put(o, up ? "^" : "_"), operand(o, a, strlen(a) <= 1);
            }
        } else if (*s == '{') {
            const char *g, *ge;
            g = group(&s, e, &ge);
            expr(g, ge, o);
        } else if (*s == '}') {
            s++;
        } else {
            put_n(o, s, 1);
            s++;
        }
    }
}

void md_math(const char *tex, char *out, size_t cap) {
    if (cap)
        render(tex, tex + strlen(tex), out, cap);
}

/* ---- tables ------------------------------------------------------------- */

static void feed_char(struct md *m, char c);

/* Display columns: wide characters count two; ANSI escapes and the test
 * mode's «…» tags count none. */
static unsigned cols(const char *s, size_t n) {
    unsigned  w  = 0;
    mbstate_t st = {};
    for (size_t i = 0; i < n;) {
        if (s[i] == '\033' && i + 1 < n && s[i + 1] == ']') { /* OSC (a hyperlink): up to BEL or ESC \ */
            for (i += 2; i < n && s[i] != '\a' && !(s[i] == '\033' && i + 1 < n && s[i + 1] == '\\'); i++) {
            }
            i += i < n && s[i] == '\a' ? 1 : 2;
            continue;
        }
        if (s[i] == '\033') {
            i++;
            if (i < n && s[i] == '[')
                for (i++; i < n && !(s[i] >= '@' && s[i] <= '~'); i++) {
                }
            i++;
            continue;
        }
        if (!strncmp(s + i, "«", 2)) {
            const char *close = strstr(s + i, "»");
            i                 = close ? (size_t) (close - s) + 2 : n;
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

static void table_add(struct md *m, char c) {
    if (m->n_table + 2 > m->cap_table) {
        size_t cap  = m->cap_table ? m->cap_table * 2 : 1024;
        char  *grow = realloc(m->table, cap);
        if (!grow)
            return;
        m->table = grow, m->cap_table = cap;
    }
    m->table[m->n_table++] = c;
    m->table[m->n_table]   = 0;
}

/* Cells of one row: '|' separates, except escaped (\|) or inside `code`. */
#define MAX_COLS 32
static size_t split_row(const char *line, size_t len, char *cells[MAX_COLS]) {
    size_t n = 0, i = 0;
    while (i < len && line[i] == ' ')
        i++;
    if (i < len && line[i] == '|')
        i++;
    char  *cell = malloc(len + 1);
    size_t k    = 0;
    bool   code = false;
    for (; cell && i <= len; i++) {
        char c   = i < len ? line[i] : 0;
        bool end = i == len || (c == '|' && !code);
        if (!end) {
            if (c == '\\' && i + 1 < len && line[i + 1] == '|' && !code)
                c = line[++i];
            else if (c == '`')
                code = !code;
            cell[k++] = c;
            continue;
        }
        cell[k] = 0;
        char *a = cell, *b = cell + k;
        while (*a == ' ')
            a++;
        while (b > a && b[-1] == ' ')
            b--;
        *b = 0;
        if (n < MAX_COLS && (i < len || *a || n == 0)) /* a trailing '|' ends the row */
            cells[n++] = strdup(a);
        k = 0;
    }
    free(cell);
    return n;
}

static bool delimiter_row(char *cells[], size_t n) {
    for (size_t i = 0; i < n; i++) {
        const char *c = cells[i];
        if (*c == ':')
            c++;
        size_t dashes = strspn(c, "-");
        c += dashes;
        if (*c == ':')
            c++;
        if (!dashes || *c)
            return false;
    }
    return n > 0;
}

/* A cell with inline Markdown, as the terminal shows it. */
static char *cell_render(const struct md *m, const char *text, bool bold) {
    char  *buf = nullptr;
    size_t len = 0;
    FILE  *f   = open_memstream(&buf, &len);
    if (!f)
        return strdup(text);
    struct md inner;
    md_init(&inner, m->mode, f);
    inner.line_start = false; /* no headings or bullets inside a cell */
    inner.bold       = bold;
    style(&inner);
    md_feed(&inner, text);
    md_finish(&inner);
    fclose(f);
    return buf;
}

/* The longest word of a rendered cell, in columns. */
static unsigned longest_word(const char *s) {
    unsigned best = 0;
    for (const char *p = s; *p;) {
        const char *e = strchr(p, ' ');
        size_t      n = e ? (size_t) (e - p) : strlen(p);
        unsigned    w = cols(p, n);
        best          = w > best ? w : best;
        p += n;
        while (*p == ' ')
            p++;
    }
    return best;
}

/* Word-wrap a rendered cell to w columns. A style that spans a break is
 * closed at the line end and opened again on the next line. */
struct lines {
    char  *line[64];
    size_t n;
};

static void wrap(const struct md *m, const char *s, unsigned w, struct lines *out) {
    out->n         = 0;
    char  *cur     = calloc(1, strlen(s) * 2 + 64);
    size_t len     = 0;
    unsigned width = 0;
    char   active[64] = ""; /* the last style sequence seen */
    const char *reset = m->mode == MD_TAGS ? "«»" : "\033[0m";
    for (const char *p = s; cur && *p && out->n < 63;) {
        /* the next word with the style sequences inside it */
        const char *e = p;
        while (*e && *e != ' ')
            e++;
        size_t   n = (size_t) (e - p);
        unsigned ww = cols(p, n);
        if (width && width + 1 + ww > w) { /* a new line */
            if (*active && strcmp(active, reset))
                strcpy(cur + len, reset), len += strlen(reset);
            cur[len]         = 0;
            out->line[out->n++] = cur;
            cur              = calloc(1, strlen(s) * 2 + 64);
            if (!cur)
                break;
            len = width = 0;
            if (*active && strcmp(active, reset))
                strcpy(cur, active), len = strlen(active);
        }
        if (width)
            cur[len++] = ' ', width++;
        for (size_t i = 0; i < n;) { /* copy, hard-breaking a word longer than w */
            if (p[i] == '\033' || !strncmp(p + i, "«", 2)) {
                const char *end = p[i] == '\033' ? p + i + 1 : strstr(p + i, "»");
                if (p[i] == '\033' && end < e && *end == '[')
                    for (end++; end < e && !(*end >= '@' && *end <= '~'); end++) {
                    }
                size_t k = end && end < e ? (size_t) (end - (p + i)) + (p[i] == '\033' ? 1 : 2) : n - i;
                if (k < sizeof active) { /* runs of escapes: style() starts each with a reset */
                    size_t have = strlen(active);
                    if (!strncmp(p + i, reset, strlen(reset)) || m->mode == MD_TAGS || have + k >= sizeof active)
                        have = 0;
                    memcpy(active + have, p + i, k);
                    active[have + k] = 0;
                }
                memcpy(cur + len, p + i, k), len += k, i += k;
                continue;
            }
            size_t k = 1;
            while (i + k < n && ((unsigned char) p[i + k] & 0xc0) == 0x80)
                k++;
            unsigned cw = cols(p + i, k);
            if (width + cw > w && width) {
                cur[len]            = 0;
                out->line[out->n++] = cur;
                cur                 = calloc(1, strlen(s) * 2 + 64);
                if (!cur || out->n >= 63)
                    break;
                len = width = 0;
            }
            memcpy(cur + len, p + i, k), len += k, i += k, width += cw;
        }
        p = e;
        while (*p == ' ')
            p++;
    }
    if (cur) {
        cur[len]            = 0;
        out->line[out->n++] = cur;
    }
}

static void lines_free(struct lines *l) {
    for (size_t i = 0; i < l->n; i++)
        free(l->line[i]);
    l->n = 0;
}

static void pad(FILE *out, unsigned n) {
    while (n--)
        fputc(' ', out);
}

static void table_draw(struct md *m) {
    char  *rows[256][MAX_COLS] = {};
    size_t n_cells[256]        = {}, n_rows = 0;
    for (char *line = m->table; line && *line && n_rows < 256;) {
        char  *nl  = strchr(line, '\n');
        size_t len = nl ? (size_t) (nl - line) : strlen(line);
        n_cells[n_rows] = split_row(line, len, rows[n_rows]);
        n_rows++;
        line += len + (nl ? 1 : 0);
    }
    size_t n = n_cells[0];
    char   align[MAX_COLS]; /* l, c, r from the delimiter row */
    for (size_t c = 0; c < n; c++) {
        const char *d = c < n_cells[1] ? rows[1][c] : "";
        size_t      l = strlen(d);
        align[c]      = l && d[0] == ':' && d[l - 1] == ':' ? 'c' : l && d[l - 1] == ':' ? 'r' : 'l';
    }
    /* the rendered cells (row 1 is the delimiter: skipped) */
    static char *cell[256][MAX_COLS];
    unsigned     natural[MAX_COLS] = {}, least[MAX_COLS] = {};
    for (size_t r = 0; r < n_rows; r++)
        for (size_t c = 0; c < n; c++) {
            if (r == 1)
                continue;
            cell[r][c]   = cell_render(m, c < n_cells[r] ? rows[r][c] : "", r == 0);
            unsigned w   = cols(cell[r][c], strlen(cell[r][c]));
            unsigned lw  = longest_word(cell[r][c]);
            natural[c]   = w > natural[c] ? w : natural[c];
            lw           = lw < 12 ? lw : 12;
            least[c]     = lw > least[c] ? lw : least[c];
        }
    unsigned avail = (m->width ? m->width : 80) - 1, width[MAX_COLS], total = 0;
    for (size_t c = 0; c < n; c++) {
        least[c] = least[c] < 3 ? 3 : least[c];
        least[c] = least[c] > natural[c] ? natural[c] : least[c];
        width[c] = natural[c];
        total += width[c] + 2 + (c ? 1 : 0);
    }
    while (total > avail) { /* narrow the widest column that can give */
        size_t widest = n;
        for (size_t c = 0; c < n; c++)
            if (width[c] > least[c] && (widest == n || width[c] > width[widest]))
                widest = c;
        if (widest == n)
            break;
        width[widest]--, total--;
    }
    if (m->mode == MD_ANSI)
        fputs("\r\033[2K", m->out); /* the placeholder goes */
    struct lines wrapped[MAX_COLS];
    if (total <= avail) {
        for (size_t r = 0; r < n_rows; r++) {
            if (r == 1) { /* under the header: a rule per column */
                for (size_t c = 0; c < n; c++) {
                    fputs(c ? " " : "", m->out);
                    for (unsigned k = 0; k < width[c] + 2; k++)
                        fputs("─", m->out);
                }
                fputc('\n', m->out);
                continue;
            }
            size_t height = 1;
            for (size_t c = 0; c < n; c++) {
                wrap(m, cell[r][c], width[c], &wrapped[c]);
                height = wrapped[c].n > height ? wrapped[c].n : height;
            }
            for (size_t h = 0; h < height; h++) {
                unsigned spaces = 0; /* written only when text follows: no trailing spaces */
                for (size_t c = 0; c < n; c++) {
                    const char *t    = h < wrapped[c].n ? wrapped[c].line[h] : "";
                    unsigned    w    = cols(t, strlen(t)), gap = width[c] > w ? width[c] - w : 0;
                    unsigned    left = align[c] == 'r' ? gap : align[c] == 'c' ? gap / 2 : 0;
                    spaces += (c ? 3 : 1) + left; /* inside each column's rule */
                    if (*t) {
                        pad(m->out, spaces);
                        fputs(t, m->out);
                        spaces = 0;
                    }
                    spaces += gap - left;
                }
                fputc('\n', m->out);
            }
            for (size_t c = 0; c < n; c++)
                lines_free(&wrapped[c]);
        }
    } else { /* too many columns: one record per row */
        unsigned head = 0;
        for (size_t c = 0; c < n; c++) {
            unsigned w = cols(cell[0][c], strlen(cell[0][c]));
            head       = w > head ? w : head;
        }
        unsigned value = avail > head + 12 ? avail - head - 2 : 10;
        for (size_t r = 2; r < n_rows; r++) {
            if (r > 2)
                fputs(m->mode == MD_ANSI ? "\033[2m─────\033[0m\n" : "─────\n", m->out);
            for (size_t c = 0; c < n; c++) {
                struct lines v;
                wrap(m, cell[r][c], value, &v);
                for (size_t h = 0; h < v.n; h++) {
                    if (h == 0) {
                        fputs(cell[0][c], m->out);
                        pad(m->out, head - cols(cell[0][c], strlen(cell[0][c])) + 2);
                    } else
                        pad(m->out, head + 2);
                    fputs(v.line[h], m->out);
                    fputc('\n', m->out);
                }
                lines_free(&v);
            }
        }
    }
    for (size_t r = 0; r < n_rows; r++) {
        for (size_t c = 0; c < n_cells[r]; c++)
            free(rows[r][c]);
        for (size_t c = 0; c < n && r != 1; c++)
            free(cell[r][c]), cell[r][c] = nullptr;
    }
    m->n_table = m->table_rows = 0;
    m->table_state = 0;
}

/* Not a table after all: its lines as text. */
static void table_replay(struct md *m) {
    char *text = m->table;
    size_t n   = m->n_table;
    m->table   = nullptr;
    m->n_table = m->cap_table = m->table_rows = 0;
    m->table_state = 0;
    m->replaying   = true;
    for (size_t i = 0; i < n; i++)
        feed_char(m, text[i]);
    m->replaying = false;
    free(text);
}

static void placeholder(struct md *m) {
    if (m->mode == MD_ANSI)
    {
        size_t rows = m->table_rows - 2; /* without header and delimiter */
        if (rows)
            fprintf(m->out, "\r\033[2K\033[2m▦ table · %zu row%s…\033[0m", rows, rows == 1 ? "" : "s");
        else
            fputs("\r\033[2K\033[2m▦ table…\033[0m", m->out);
    }
}

/* A table line is complete. */
static void table_line_end(struct md *m) {
    m->table_line = false;
    table_add(m, '\n');
    m->table_rows++;
    if (m->table_state == 0) {
        m->table_state = 1;
        return;
    }
    if (m->table_state == 1) { /* the second line decides */
        const char *second = strchr(m->table, '\n') + 1;
        char       *cells[MAX_COLS];
        size_t      n  = split_row(second, strlen(second) - 1, cells);
        bool        ok = delimiter_row(cells, n);
        for (size_t i = 0; i < n; i++)
            free(cells[i]);
        if (!ok) {
            table_replay(m);
            return;
        }
        m->table_state = 2;
    }
    placeholder(m);
}

static void table_end(struct md *m) {
    if (m->table_line)
        table_line_end(m);
    if (m->table_state == 1)
        table_replay(m);
    else if (m->table_state == 2)
        table_draw(m);
}

/* ---- the stream --------------------------------------------------------- */

static void text(struct md *m, char c) {
    fputc(c, m->out);
}

static const char *opener(int math) {
    return math == '$' ? "$" : math == 'D' ? "$$" : math == '(' ? "\\(" : "\\[";
}

/* Not math after all (no closer before the line or buffer ended): as written. */
static void math_cancel(struct md *m) {
    int math = m->math;
    m->math  = 0;
    style(m);
    fputs(opener(math), m->out);
    fwrite(m->math_buf, 1, m->n_math, m->out);
    m->n_math = 0;
}

static void math_finish(struct md *m) {
    char out[4096];
    m->math_buf[m->n_math] = 0;
    md_math(m->math_buf, out, sizeof out);
    bool display           = m->math == 'D' || m->math == '[';
    style(m);
    fputs(display ? "  " : "", m->out);
    fputs(out, m->out);
    m->math   = 0;
    m->n_math = 0;
    style(m);
}

static void inline_char(struct md *m, char c);

static void math_char(struct md *m, char c) {
    if (m->closing) { /* the '$' before: a closer unless a digit follows */
        m->closing = false;
        if (!isdigit((unsigned char) c)) {
            math_finish(m);
            inline_char(m, c);
            return;
        }
        m->math_buf[m->n_math++] = '$';
    }
    if (m->math == '$' && c == '$') {
        if (m->n_math && m->math_buf[m->n_math - 1] != ' ')
            m->closing = true;
        else
            m->math_buf[m->n_math++] = c;
        return;
    }
    bool closes =
                  (m->math == 'D' && c == '$' && m->n_math && m->math_buf[m->n_math - 1] == '$') ||
                  (m->math == '(' && c == ')' && m->n_math && m->math_buf[m->n_math - 1] == '\\') ||
                  (m->math == '[' && c == ']' && m->n_math && m->math_buf[m->n_math - 1] == '\\');
    if (closes) {
        if (m->math != '$')
            m->n_math--;
        math_finish(m);
        return;
    }
    if (m->n_math + 2 >= sizeof m->math_buf) {
        math_cancel(m);
        text(m, c);
        return;
    }
    m->math_buf[m->n_math++] = c;
}

/* A held marker meets the next character (or the end: c == 0). */
static void pending_resolve(struct md *m, char c) {
    char p     = m->pending;
    m->pending = 0;
    switch (p) {
    case '*':
        if (c == '*') {
            m->bold = !m->bold;
            style(m);
            return;
        }
        if (m->italic) {
            m->italic = false;
            style(m);
        } else if (c && c != ' ' && c != '\n') {
            m->italic = true;
            style(m);
        } else
            text(m, '*');
        break;
    case '$':
        if (c == '$') {
            m->math = 'D';
            return;
        }
        if (c && c != ' ' && c != '\n') {
            m->math = '$';
            math_char(m, c);
            return;
        }
        text(m, '$');
        break;
    case '\\':
        if (c == '(' || c == '[') {
            m->math = c;
            return;
        }
        if (c && strchr("*_`$\\#", c)) { /* an escaped marker is the character itself */
            text(m, c);
            return;
        }
        text(m, '\\');
        break;
    }
    if (c)
        inline_char(m, c);
}

/* ---- links ---------------------------------------------------------------- */

/* A hyperlink (OSC 8): the text underlined and clickable where the terminal
 * supports it; the URL dim after it when it differs, so it stays readable
 * (and copyable) where it does not. */
static void link_emit(struct md *m, const char *text, size_t n_text, const char *url) {
    if (m->mode == MD_TAGS)
        fprintf(m->out, "«link %s»", url);
    else
        fprintf(m->out, "\033]8;;%s\033\\", url);
    m->underline = true;
    style(m);
    bool literal = m->literal;
    m->literal   = true;
    for (size_t i = 0; i < n_text; i++) /* the text keeps its inline marks (`code`, **bold**) */
        inline_char(m, text[i]);
    if (m->pending)
        pending_resolve(m, 0);
    m->literal   = literal;
    m->underline = false;
    style(m);
    fputs(m->mode == MD_TAGS ? "«/link»" : "\033]8;;\033\\", m->out);
    if (strlen(url) != n_text || strncmp(text, url, n_text)) {
        bool faint = m->mode == MD_ANSI;
        fprintf(m->out, " %s(%s)%s", faint ? "\033[2m" : "", url, faint ? "\033[22m" : "");
    }
}

/* Not a link after all: what was held, as written. */
static void link_literal(struct md *m) {
    int  stage   = m->link;
    bool literal = m->literal;
    m->link      = 0;
    m->literal   = true;
    text(m, '[');
    for (size_t i = 0; i < m->n_link_text; i++)
        inline_char(m, m->link_text[i]);
    if (stage >= 2)
        text(m, ']');
    if (stage == 3) {
        text(m, '(');
        for (size_t i = 0; i < m->n_link_url; i++)
            inline_char(m, m->link_url[i]);
    }
    m->literal = literal;
}

/* Held bare-URL characters that are not one, as written. */
static void bare_literal(struct md *m, const char *s, size_t n) {
    bool literal = m->literal;
    m->literal   = true;
    for (size_t i = 0; i < n; i++)
        inline_char(m, s[i]);
    m->literal = literal;
}

/* A character while a [text](url) is open; false when it ended the link and
 * c is still to be handled. */
static bool link_char(struct md *m, char c) {
    if (m->link == 1) {
        if (c == ']')
            m->link = 2;
        else if (m->n_link_text + 1 < sizeof m->link_text)
            m->link_text[m->n_link_text++] = c;
        else
            return link_literal(m), false;
        return true;
    }
    if (m->link == 2) {
        if (c == '(') {
            m->link = 3;
            return true;
        }
        return link_literal(m), false;
    }
    if (c == ')' && m->n_link_url) {
        m->link_url[m->n_link_url] = 0;
        m->link                    = 0;
        link_emit(m, m->link_text, m->n_link_text, m->link_url);
        return true;
    }
    if (c == ' ' || m->n_link_url + 1 >= sizeof m->link_url)
        return link_literal(m), false;
    m->link_url[m->n_link_url++] = c;
    return true;
}

/* A bare http(s):// URL: held while it can still be one, linked at its end
 * (trailing punctuation stays outside). */
static bool bare_possible(const char *s, size_t n) {
    static const char *const schemes[] = {"https://", "http://"};
    for (size_t k = 0; k < 2; k++) {
        size_t len = strlen(schemes[k]);
        if (!strncmp(s, schemes[k], n < len ? n : len))
            return true;
    }
    return false;
}

static void bare_end(struct md *m) {
    size_t n = m->n_bare, scheme = strncmp(m->bare, "https://", 8) ? 7 : 8;
    m->n_bare = 0;
    size_t url = n;
    while (url > scheme && strchr(".,;:!?)\"'", m->bare[url - 1]))
        url--;
    if (url > scheme && !strncmp(m->bare, "http", 4) && n >= scheme && m->bare[scheme - 1] == '/') {
        char link[sizeof m->bare];
        memcpy(link, m->bare, url), link[url] = 0;
        link_emit(m, link, url, link);
    } else
        url = 0;
    bare_literal(m, m->bare + url, n - url);
}

static void inline_char(struct md *m, char c) {
    char before = m->before;
    m->before   = c;
    if (m->link && link_char(m, c))
        return;
    if (m->n_bare) {
        if (c != ' ' && c != '<' && c != '>' && m->n_bare + 1 < sizeof m->bare) {
            m->bare[m->n_bare++] = c;
            if (!bare_possible(m->bare, m->n_bare)) { /* not a URL after all: as written */
                size_t n  = m->n_bare;
                m->n_bare = 0;
                bare_literal(m, m->bare, n);
            }
            return;
        }
        bare_end(m);
    }
    if (m->pending) {
        pending_resolve(m, c);
        return;
    }
    if (m->math) {
        math_char(m, c);
        return;
    }
    if (m->code) {
        if (c == '`') {
            m->code = false;
            style(m);
        } else
            text(m, c);
        return;
    }
    if (c == '[' && !m->literal) {
        m->link        = 1;
        m->n_link_text = m->n_link_url = 0;
        return;
    }
    if (c == 'h' && !m->literal && (!before || before == ' ' || before == '(' || before == '\n')) {
        m->bare[0] = c, m->n_bare = 1;
        return;
    }
    if (c == '*' || c == '$' || c == '\\') {
        m->pending = c;
        return;
    }
    if (c == '`') {
        m->code = true;
        style(m);
        return;
    }
    text(m, c);
}

/* ---- code blocks: a frame that shows where they start and end ---------------- */

static bool wrapping(const struct md *m);

/* A code line's dim "│ " (part of the block style: the terminal's own wrap of
 * a long line does not repeat it). */
static void gutter(struct md *m) {
    fputs(m->mode == MD_ANSI ? "\033[2m│\033[22m " : "│ ", m->out);
}

static void fence_rule(struct md *m) {
    bool faint = m->mode == MD_ANSI;
    fputs(faint ? "\033[2m" : "", m->out);
    if (m->block && m->n_lang) /* opening, with a language */
        fprintf(m->out, "── %.*s ──", (int) m->n_lang, m->lang);
    else
        fputs("──", m->out);
    fputs(faint ? "\033[22m" : "", m->out);
    text(m, '\n');
}

enum prefix { P_MORE, P_TEXT, P_HEADING, P_BULLET, P_QUOTE, P_FENCE };

static enum prefix classify(const struct md *m, bool final, size_t *indent, size_t *marker) {
    const char *s = m->prefix;
    size_t      n = m->n_prefix, i = 0;
    while (i < n && s[i] == ' ')
        i++;
    *indent = i;
    if (i == n)
        return final || i > 8 ? P_TEXT : P_MORE;
    const char *r = s + i;
    size_t      k = n - i;
    if (r[0] == '`') {
        size_t ticks = 0;
        while (ticks < k && r[ticks] == '`')
            ticks++;
        if (ticks == k && k < 3)
            return final ? P_TEXT : P_MORE;
        *marker = 3;
        return ticks >= 3 ? P_FENCE : P_TEXT;
    }
    if (m->block)
        return P_TEXT;
    if (r[0] == '#') {
        size_t hashes = 0;
        while (hashes < k && r[hashes] == '#')
            hashes++;
        if (hashes == k)
            return final || hashes > 6 ? P_TEXT : P_MORE;
        *marker = hashes + 1;
        return r[hashes] == ' ' && hashes <= 6 ? P_HEADING : P_TEXT;
    }
    if (strchr("-*+>", r[0])) {
        if (k == 1)
            return final ? P_TEXT : P_MORE;
        *marker = 2;
        return r[1] != ' ' ? P_TEXT : r[0] == '>' ? P_QUOTE : P_BULLET;
    }
    return P_TEXT;
}

/* The line start is decided: apply it, then hand on what is left. */
static void prefix_release(struct md *m, bool final) {
    size_t      indent = 0, marker = 0;
    enum prefix p      = classify(m, final, &indent, &marker);
    if (p == P_MORE)
        return;
    char   held[sizeof m->prefix];
    size_t n = m->n_prefix;
    memcpy(held, m->prefix, n);
    m->n_prefix   = 0;
    m->line_start = false;
    size_t from   = 0;
    if (p != P_TEXT) {
        fwrite(held, 1, indent, m->out);
        from = indent + marker;
        if (p == P_FENCE) { /* the fence line becomes a rule with the language (at its end) */
            m->block     = !m->block;
            m->skip_line = true;
            m->n_lang    = 0;
            if (wrapping(m)) /* code is not word-wrapped: a mark for the wrap filter */
                fputc(m->block ? '\001' : '\002', m->out);
            style(m);
            return;
        }
        if (p == P_HEADING)
            m->heading = true;
        if (p == P_QUOTE)
            m->quote = true;
        style(m);
        fputs(p == P_BULLET ? "• " : p == P_QUOTE ? "│ " : "", m->out);
    }
    if (m->block)
        gutter(m);
    for (size_t i = from; i < n; i++)
        if (m->block)
            text(m, held[i]);
        else
            inline_char(m, held[i]);
}

/* A line or the answer ends: what waits for more resolves as it is. */
static void flush_pending(struct md *m) {
    if (m->line_start && m->n_prefix)
        prefix_release(m, true);
    if (m->link)
        link_literal(m);
    if (m->n_bare)
        bare_end(m);
    if (m->pending)
        pending_resolve(m, 0);
    if (m->closing) {
        m->closing = false;
        math_finish(m);
    }
}

static void newline(struct md *m) {
    flush_pending(m);
    if (m->math == '$' || m->math == '(')
        math_cancel(m);
    bool skip  = m->skip_line;
    m->skip_line = false;
    if (skip) /* a fence: "── python ──" opening a block, "──" closing it */
        fence_rule(m);
    else if (m->block && m->line_start) /* an empty code line keeps the gutter */
        gutter(m);
    m->bold = m->italic = m->code = m->heading = m->quote = false;
    style(m);
    if (!skip)
        text(m, '\n');
    m->line_start = true;
}

static void feed_char(struct md *m, char c) {
    if (m->mode == MD_RAW) {
        text(m, c);
        return;
    }
    if (m->table_line) { /* a table line is collected whole */
        if (c == '\n')
            table_line_end(m);
        else
            table_add(m, c);
        return;
    }
    if (m->line_start && !m->n_prefix && !m->block && !m->math && !m->replaying) {
        if (c == '|') {
            m->table_line = true;
            table_add(m, c);
            return;
        }
        if (m->table_state) /* any other line ends the table */
            table_end(m);
    }
    if ((m->math == 'D' || m->math == '[') && !m->pending) {
        math_char(m, c == '\n' ? ' ' : c); /* display math may span lines */
        return;
    }
    if (c == '\n') {
        newline(m);
        return;
    }
    if (m->skip_line) { /* the fence line: its language */
        if (m->block && c != '`' && c != ' ' && m->n_lang + 1 < sizeof m->lang)
            m->lang[m->n_lang++] = c;
        return;
    }
    if (m->line_start) {
        m->prefix[m->n_prefix++] = c;
        prefix_release(m, m->n_prefix == sizeof m->prefix);
        return;
    }
    if (m->block)
        text(m, c);
    else
        inline_char(m, c);
}

/* ---- wrapping: words onto lines -------------------------------------------- */

static void word_flush(struct md *m) {
    if (!m->n_word)
        return;
    unsigned w = cols(m->word, m->n_word);
    if (!m->head && m->col > m->hang && m->col + m->spaces + w > m->width) { /* on the next line, under the text */
        fputc('\n', m->sink);
        pad(m->sink, m->bar ? m->hang - 2 : m->hang);
        fputs(m->bar ? "│ " : "", m->sink);
        m->col = m->hang;
    } else {
        pad(m->sink, m->spaces);
        m->col += m->spaces;
    }
    m->spaces = 0;
    fwrite(m->word, 1, m->n_word, m->sink);
    m->col += w;
    if (m->head) { /* the line's first word: a bullet or a quote bar sets the indent */
        m->word[m->n_word] = 0;
        bool bar           = strstr(m->word, "•") || strstr(m->word, "│");
        m->hang            = bar && w == 1 ? m->col + 1 : m->lead;
        m->bar             = bar && w == 1 && strstr(m->word, "│");
        m->head            = false;
    }
    m->n_word = 0;
}

static void wrap_put(struct md *m, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\001' || c == '\002') { /* a code block begins or ends: no word wrap inside */
            word_flush(m);
            m->nowrap = c == '\001';
            continue;
        }
        if (m->nowrap) {
            fputc(c, m->sink);
            if (c == '\n')
                m->col = m->lead = m->hang = 0, m->bar = false, m->head = true;
            continue;
        }
        if (c == '\n' || c == '\r') {
            word_flush(m);
            m->spaces = 0; /* none at a line's end */
            fputc(c, m->sink);
            m->col = m->lead = 0;
            m->hang          = 0;
            m->bar           = false;
            m->head          = true;
        } else if (c == ' ' && m->head && !m->n_word) {
            fputc(' ', m->sink);
            m->col++, m->lead++;
        } else if (c == ' ') {
            word_flush(m);
            m->spaces++;
        } else {
            if (m->n_word == sizeof m->word - 1) /* a word longer than that: as it comes */
                word_flush(m);
            m->word[m->n_word++] = c;
        }
    }
}

static bool wrapping(const struct md *m) {
    return m->wrap && m->width && m->mode != MD_RAW;
}

/* Run fn with out pointing at a buffer, then wrap what it wrote. */
static void through_wrap(struct md *m, const char *s, void (*fn)(struct md *, const char *)) {
    char  *buf = nullptr;
    size_t len = 0;
    FILE  *b   = open_memstream(&buf, &len);
    if (!b) {
        fn(m, s);
        return;
    }
    m->out = b;
    fn(m, s);
    fclose(b);
    m->out = m->sink;
    wrap_put(m, buf, len);
    free(buf);
}

static void feed_all(struct md *m, const char *s) {
    for (; *s; s++)
        feed_char(m, *s);
}

static void finish_all(struct md *m, const char *unused);

void md_feed(struct md *m, const char *s) {
    if (wrapping(m))
        through_wrap(m, s, feed_all);
    else
        feed_all(m, s);
    fflush(m->sink);
}

void md_finish(struct md *m) {
    if (m->mode == MD_RAW)
        return;
    if (wrapping(m)) {
        through_wrap(m, nullptr, finish_all);
        word_flush(m);
    } else
        finish_all(m, nullptr);
    fflush(m->sink);
}

static void finish_all(struct md *m, const char *unused) {
    (void) unused;
    table_end(m);
    free(m->table);
    m->table   = nullptr;
    m->n_table = m->cap_table = 0;
    flush_pending(m);
    if (m->math)
        math_cancel(m);
    m->bold = m->italic = m->code = m->block = m->heading = m->quote = false;
    style(m);
}

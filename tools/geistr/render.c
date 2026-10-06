/* render.c — see render.h. A character-level state machine: every byte is
 * handled with only the bytes before it, so piece boundaries never matter. */
#include "render.h"
#include <ctype.h>
#include <string.h>

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
}

void md_init(struct md *m, enum md_mode mode, FILE *out) {
    *m = (struct md) {.mode = mode, .out = out, .line_start = true};
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
                                      "displaystyle", "textstyle", "limits", "nolimits"};
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
                put(o, simple(a) ? a : "("), put(o, simple(a) ? "" : a), put(o, simple(a) ? "" : ")");
                put(o, "/");
                put(o, simple(b) ? b : "("), put(o, simple(b) ? "" : b), put(o, simple(b) ? "" : ")");
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
                put(o, "√"), put(o, simple(a) ? a : "("), put(o, simple(a) ? "" : a), put(o, simple(a) ? "" : ")");
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
                put(o, up ? "^" : "_");
                bool one = strlen(a) <= 1;
                put(o, one ? a : "("), put(o, one ? "" : a), put(o, one ? "" : ")");
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
    if (!cap)
        return;
    out[0]       = 0;
    struct mo o = {out, cap, 0};
    expr(tex, tex + strlen(tex), &o);
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

static void inline_char(struct md *m, char c) {
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
        if (p == P_FENCE) {
            m->block     = !m->block;
            m->skip_line = true; /* the fence line itself (and its language) is not shown */
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
    for (size_t i = from; i < n; i++)
        if (m->block)
            text(m, held[i]);
        else
            inline_char(m, held[i]);
}

static void newline(struct md *m) {
    if (m->line_start && m->n_prefix)
        prefix_release(m, true);
    if (m->pending)
        pending_resolve(m, 0);
    if (m->closing) {
        m->closing = false;
        math_finish(m);
    }
    if (m->math == '$' || m->math == '(')
        math_cancel(m);
    bool skip  = m->skip_line;
    m->skip_line = false;
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
    if ((m->math == 'D' || m->math == '[') && !m->pending) {
        math_char(m, c == '\n' ? ' ' : c); /* display math may span lines */
        return;
    }
    if (c == '\n') {
        newline(m);
        return;
    }
    if (m->skip_line)
        return;
    if (m->line_start) {
        m->prefix[m->n_prefix++] = c;
        if (m->n_prefix == sizeof m->prefix)
            prefix_release(m, true);
        else
            prefix_release(m, false);
        return;
    }
    if (m->block)
        text(m, c);
    else
        inline_char(m, c);
}

void md_feed(struct md *m, const char *s) {
    for (; *s; s++)
        feed_char(m, *s);
    fflush(m->out);
}

void md_finish(struct md *m) {
    if (m->mode == MD_RAW)
        return;
    if (m->line_start && m->n_prefix)
        prefix_release(m, true);
    if (m->pending)
        pending_resolve(m, 0);
    if (m->closing) {
        m->closing = false;
        math_finish(m);
    }
    if (m->math)
        math_cancel(m);
    m->bold = m->italic = m->code = m->block = m->heading = m->quote = false;
    style(m);
    fflush(m->out);
}

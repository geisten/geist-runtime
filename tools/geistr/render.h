/* render.h — Markdown and LaTeX math for a terminal, streamed (#11).
 *
 * Text arrives in pieces of any size; the output does not depend on where a
 * piece ends (markers split across pieces are held back until decided).
 * Block level at line starts: # headings, - * + bullets, > quotes, ```
 * fences. Inline: **bold**, *italic*, `code`, $math$, $$math$$, \(math\),
 * \[math\]; math becomes Unicode (α, ∑, x², xᵢ, a/b, √x). Dollars follow
 * Pandoc: no space after the opening '$', none before the closing one, and no
 * digit after it, so "$5 or $10" stays money. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

enum md_mode {
    MD_RAW,  /* pass text through unchanged (pipes, NO_COLOR) */
    MD_ANSI, /* terminal escape sequences */
    MD_TAGS, /* «…» style tags instead of escapes, for tests */
};

struct md {
    enum md_mode mode;
    FILE        *out;
    bool         bold, italic, code, block, heading, quote, skip_line;
    int          math;      /* 0, or the opener: '$', 'D' ($$), '(' or '[' */
    bool         closing;   /* a '$' that closes unless a digit follows (Pandoc's rule) */
    bool         line_start;
    char         prefix[16]; /* undecided line start ("  ##", "``", "-") */
    size_t       n_prefix;
    char         pending;    /* undecided inline marker: '*', '`', '$', '\\' */
    char         math_buf[2048];
    size_t       n_math;
    char         last[8]; /* the style last emitted */
};

void md_init(struct md *m, enum md_mode mode, FILE *out);
void md_feed(struct md *m, const char *text);
void md_finish(struct md *m); /* flushes what is held, resets the style */

/* LaTeX → Unicode into out[0..cap) (always NUL-terminated). */
void md_math(const char *tex, char *out, size_t cap);

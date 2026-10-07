/* render.h — Markdown and LaTeX math for a terminal, streamed (#11).
 *
 * Text arrives in pieces of any size; the output does not depend on where a
 * piece ends (markers split across pieces are held back until decided).
 * Block level at line starts: # headings, - * + bullets, > quotes, ```
 * fences. Inline: **bold**, *italic*, `code`, $math$, $$math$$, \(math\),
 * \[math\]; math becomes Unicode (α, ∑, x², xᵢ, a/b, √x). Dollars follow
 * Pandoc: no space after the opening '$', none before the closing one, and no
 * digit after it, so "$5 or $10" stays money.
 *
 * Tables (GitHub style: a line starting with '|', then a delimiter row such
 * as |---|:-:|--:|) need every row before their widths are known: they are
 * collected, a one-line placeholder counts the rows, and the table replaces
 * it when it ends, compact (no outer frame), aligned, cells with inline
 * Markdown, wrapped to the terminal width or as one record per row when the
 * columns cannot fit. A first line not followed by a delimiter is text.
 *
 * With wrap set (and a width), prose wraps at word boundaries: a word is
 * shown once it is complete, on the next line when it does not fit, under
 * the text of its bullet or quote. */
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
    bool         bold, italic, code, block, heading, quote, skip_line, underline;
    int          math;      /* 0, or the opener: '$', 'D' ($$), '(' or '[' */
    bool         closing;   /* a '$' that closes unless a digit follows (Pandoc's rule) */
    bool         line_start;
    char         prefix[16]; /* undecided line start ("  ##", "``", "-") */
    size_t       n_prefix;
    char         pending;    /* undecided inline marker: '*', '`', '$', '\\' */
    char         math_buf[2048];
    size_t       n_math;
    char         last[8]; /* the style last emitted */
    /* links: [text](url) held until it closes (stage 1 text, 2 after ']', 3 url);
     * a bare http(s):// URL held until its end */
    int          link;
    char         link_text[256], link_url[512], bare[512];
    size_t       n_link_text, n_link_url, n_bare;
    char         before;  /* the input character before this one: a bare URL starts a word */
    bool         literal; /* replaying held text, or a link's own: no new link starts */
    char         lang[32]; /* a code fence's language, shown on its line */
    size_t       n_lang;
    unsigned     width;   /* terminal columns for tables (0: 80) */
    char        *table;   /* the table's lines so far, raw */
    size_t       n_table, cap_table, table_rows;
    int          table_state; /* 0, 1 (a first line), 2 (confirmed) */
    bool         table_line;  /* collecting a table line */
    bool         replaying;   /* not a table after all: its text again */
    /* wrapping (wrap and width set): what the parser wrote goes through it */
    bool     wrap;
    FILE    *sink;            /* the real output; out is a buffer meanwhile */
    unsigned col, hang, lead; /* the line's columns, its continuation indent, its leading spaces */
    unsigned spaces;          /* held until the next word: dropped at a line's end */
    bool     head;            /* no word on this line yet */
    bool     bar;             /* a quote: its continuation lines repeat the │ */
    bool     nowrap;          /* inside a code block: lines as they are */
    unsigned say_hang;        /* md_say: the continuation indent (0: after a leading symbol) */
    bool     saying;          /* md_say: the program's own text, not an answer */
    char     word[512];       /* the word being written (escapes included) */
    size_t   n_word;
};

void md_init(struct md *m, enum md_mode mode, FILE *out);
void md_feed(struct md *m, const char *text);
void md_finish(struct md *m); /* flushes what is held (a table: drawn), resets the style */

/* The program's own text (escapes included) word-wrapped to width:
 * continuation lines indented by hang, or with hang 0 under the text after
 * a leading symbol ("↻ …", "  ⟲ …"), else under the line's leading spaces. */
void md_say(FILE *out, const char *text, unsigned width, unsigned hang);

/* LaTeX → Unicode into out[0..cap) (always NUL-terminated). */
void md_math(const char *tex, char *out, size_t cap);

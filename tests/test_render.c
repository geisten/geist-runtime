/* test_render.c — geistr's terminal Markdown and math (tools/geistr/render.c).
 * Every case also checks that the output does not depend on where the
 * text is split into pieces: whole, every 2-split, and char by char. */
#include "render.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int      failures;
static unsigned test_width; /* terminal columns for tables; 0: the default */
static bool     test_wrap;  /* prose wrapped at words, to test_width */

static char *run(const char *const *pieces, size_t n, enum md_mode mode) {
    char  *buf = nullptr;
    size_t len = 0;
    FILE  *f   = open_memstream(&buf, &len);
    struct md m;
    md_init(&m, mode, f);
    m.width = test_width;
    m.wrap  = test_wrap;
    for (size_t i = 0; i < n; i++)
        md_feed(&m, pieces[i]);
    md_finish(&m);
    fclose(f);
    return buf;
}

static void check(const char *in, const char *want) {
    const char *whole[] = {in};
    char       *got     = run(whole, 1, MD_TAGS);
    if (strcmp(got, want)) {
        fprintf(stderr, "render %s\n  got  %s\n  want %s\n", in, got, want);
        failures++;
    }
    size_t n = strlen(in);
    for (size_t k = 1; k < n; k++) { /* every 2-split */
        char a[512], b[512];
        snprintf(a, sizeof a, "%.*s", (int) k, in);
        snprintf(b, sizeof b, "%s", in + k);
        const char *two[] = {a, b};
        char       *split = run(two, 2, MD_TAGS);
        if (strcmp(split, got)) {
            fprintf(stderr, "split %zu of %s\n  got  %s\n  want %s\n", k, in, split, got);
            failures++;
        }
        free(split);
    }
    const char *chars[512];
    char        one[512][2];
    for (size_t i = 0; i < n && i < 512; i++)
        one[i][0] = in[i], one[i][1] = 0, chars[i] = one[i];
    char *each = run(chars, n < 512 ? n : 512, MD_TAGS);
    if (strcmp(each, got)) {
        fprintf(stderr, "char by char %s\n  got  %s\n", in, each);
        failures++;
    }
    const char *raw_in[] = {in};
    char       *raw      = run(raw_in, 1, MD_RAW);
    if (strcmp(raw, in)) {
        fprintf(stderr, "raw mode changed %s\n", in);
        failures++;
    }
    free(each), free(raw), free(got);
}

static void math(const char *tex, const char *want) {
    char out[512];
    md_math(tex, out, sizeof out);
    if (strcmp(out, want)) {
        fprintf(stderr, "math %s\n  got  %s\n  want %s\n", tex, out, want);
        failures++;
    }
}

int main(void) {
    if (!setlocale(LC_CTYPE, "C.UTF-8") && !setlocale(LC_CTYPE, "en_US.UTF-8")) {
        puts("render: SKIPPED (no UTF-8 locale)");
        return 0;
    }
    /* inline */
    check("Die Hauptstadt ist **Ottawa**.", "Die Hauptstadt ist «b»Ottawa«».");
    check("ein *kursives* Wort", "ein «i»kursives«» Wort");
    check("nutze `make test` jetzt", "nutze «c»make test«» jetzt");
    check("5 * 3 = 15", "5 * 3 = 15");
    check("kostet $5 oder $ 10", "kostet $5 oder $ 10");
    check("$5 und $10 bitte", "$5 und $10 bitte");
    check("π ist etwa $3,14159$.", "π ist etwa «m»3,14159«».");
    check("**$A$**: die Fläche", "«b»«bm»A«b»«»: die Fläche");
    check("\\*kein\\* fett", "*kein* fett");
    check("**fett** und *kursiv*\nneue Zeile", "«b»fett«» und «i»kursiv«»\nneue Zeile");
    check("**offen am Zeilenende\nweiter", "«b»offen am Zeilenende«»\nweiter");
    /* line starts */
    check("# Titel\nText", "«h»Titel«»\nText");
    check("### Klein", "«h»Klein«»");
    check("#hashtag", "#hashtag");
    check("- eins\n- zwei", "• eins\n• zwei");
    check("  * eingerückt", "  • eingerückt");
    check("> zitiert", "«q»│ zitiert«»");
    check("---", "---");
    check("```c\nint x = 1; // **nicht fett**\n```\nnach", "«c»int x = 1; // **nicht fett**\n«»nach");
    check("1. erstens", "1. erstens");
    /* math */
    check("Euler: $e^{i\\pi} + 1 = 0$.", "Euler: «m»e^(iπ) + 1 = 0«»."); /* no superscript π */
    check("$$\\frac{a+b}{2}$$", "«m»  (a+b)/2«»");
    check("\\(x_1^2\\) und \\[\\sum_{i=1}^{n} i\\]", "«m»x₁²«» und «m»  ∑ᵢ₌₁ⁿ i«»");
    check("$nicht geschlossen\nweiter", "$nicht geschlossen\nweiter");
    /* tables: compact, aligned, wrapped to the width, records when too wide */
    test_width = 80;
    check("| A | B |\n|:-|-:|\n| x | 1 |\n| **y** | 22 |\nText",
          " «b»A«»    «b»B«»\n─── ────\n x    1\n «b»y«»   22\nText"); /* alignment, bold header, text right after */
    test_width = 80;
    check("| a | b |\nkein Trenner\n",
          "| a | b |\nkein Trenner\n"); /* not a table: text as written */
    test_width = 80;
    check("| Code | Pipe |\n|--|--|\n| `a|b` | x \\| y |\n",
          " «b»Code«»   «b»Pipe«»\n────── ───────\n «c»a|b«»    x | y\n"); /* pipes in code and escaped */
    test_width = 14;
    check("| Name | Wert |\n|--|--|\n| lang lang | 1 |\n",
          " «b»Name«»   «b»Wert«»\n────── ──────\n lang   1\n lang\n"); /* wrapped to the width */
    test_width = 10;
    check("| Kopf | Lang |\n|--|--|\n| abcdefgh | ijklmnop |\n| a | b |\n",
          "«b»Kopf«»  abcdefgh\n«b»Lang«»  ijklmnop\n─────\n«b»Kopf«»  a\n«b»Lang«»  b\n"); /* records when columns cannot fit */
    test_width = 80;
    check("| Größe | ⚡ |\n|--|:-:|\n| ä | $x^2$ |",
          " «b»Größe«»   «b»⚡«»\n─────── ────\n ä       «m»x²«»\n"); /* display widths, centred, math, at the end */
    test_width = 80;
    check("Vorher:\n| a | b |\n|--|--|\n| 1 | 2 |\n\nNachher **fett**.\n",
          "Vorher:\n «b»a«»   «b»b«»\n─── ───\n 1   2\n\nNachher «b»fett«».\n"); /* between paragraphs */
    test_width = 0;
    math("\\alpha + \\beta \\leq \\gamma", "α + β ≤ γ");
    math("\\sqrt{x^2 + y^2}", "√(x² + y²)");
    math("\\sqrt[3]{8}", "³√8");
    math("\\boxed{391}", "391"); /* qwen3 marks its result so */
    math("\\frac{1}{2}", "1/2");
    math("x \\in \\mathbb{R}", "x ∈ ℝ");
    math("\\lim_{x \\to \\infty} f(x)", "lim_(x → ∞) f(x)"); /* no subscript arrow */
    math("a^{bc}", "a^(bc)");
    math("E = mc^2", "E = mc²");
    math("\\text{if } x > 0", "if  x > 0");
    math("\\int_0^1 x\\,dx", "∫₀¹ x dx");
    if (failures) {
        fprintf(stderr, "render: %d failures\n", failures);
        return 1;
    }
    /* wrapping: at word boundaries, under a bullet's text, styles and Unicode
     * measured by what shows, no spaces at line ends, a line that fills the
     * width exactly stays, an over-long word on its own line */
    test_wrap = true, test_width = 30;
    check("The keeper of the lighthouse had been tending the light for nigh on forty years.\n",
          "The keeper of the lighthouse\nhad been tending the light for\nnigh on forty years.\n");
    test_width = 24;
    check("- a first point that is far too long for one line\n- short\n",
          "• a first point that is\n  far too long for one\n  line\n• short\n");
    test_width = 22;
    check("Energy is **mass times** the speed of light squared: $E=mc^2$ as always.\n",
          "Energy is «b»mass times«»\nthe speed of light\nsquared: «m»E=mc²«» as\nalways.\n");
    test_width = 20;
    check("Grüße aus München, wo die Brezeln größer sind.\n", "Grüße aus München,\nwo die Brezeln\ngrößer sind.\n");
    test_width = 16;
    check("Short then Donaudampfschifffahrtsgesellschaftskapitän ends.\n",
          "Short then\nDonaudampfschifffahrtsgesellschaftskapitän\nends.\n");
    test_wrap = false, test_width = 0;
    puts("render: Markdown, tables and math for the terminal, split-invariant, raw mode untouched, word wrap passed");
    return 0;
}

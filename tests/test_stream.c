/*
 * test_stream.c — the text stages (#3).
 *
 * The UTF-8 and output protocol checks are geist-serve's
 * tests/app/output_test.c and the UTF-8 part of tests/app/core_test.c,
 * ported with only names changed; the stop string checks are new.
 */
#include "stream.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
struct sink {
    char   text[2048];
    size_t n;
};
static bool emit(void *ctx, const char *s) {
    struct sink *o = ctx;
    size_t       n = strlen(s);
    assert(o->n + n < sizeof o->text);
    memcpy(o->text + o->n, s, n + 1);
    o->n += n;
    assert(!strstr(o->text, "SECRET"));
    return true;
}
static void check(const char *input, const char *expected, bool reasoning, const char *format) {
    for (size_t split = 0; split <= strlen(input); ++split) {
        char left[1024];
        memcpy(left, input, split);
        left[split]         = 0;
        struct sink       s = {0};
        struct str_output p;
        str_output_init(&p, !strcmp(format, "think_tags"));
        assert(str_output_feed(&p, left, emit, &s));
        assert(str_output_feed(&p, input + split, emit, &s));
        str_output_finish(&p);
        assert(!strcmp(s.text, expected));
        assert(p.reasoning == reasoning);
        assert(!str_output_feed(&p, "late", emit, &s));
    }
    struct sink       s = {0};
    struct str_output p;
    str_output_init(&p, !strcmp(format, "think_tags"));
    for (const char *c = input; *c; ++c) {
        char chunk[2] = {*c, 0};
        assert(str_output_feed(&p, chunk, emit, &s));
    }
    str_output_finish(&p);
    assert(!strcmp(s.text, expected));
}
struct pair {
    struct sink answer;
    char        thought[1 << 21];
    size_t      thought_n, chunks;
    bool        boundaries;
};
static bool emit_answer(void *ctx, const char *s) {
    return emit(&((struct pair *) ctx)->answer, s);
}
/* Thinking chunks: plain text without markers, each a whole run of UTF-8 characters. */
static bool emit_thought(void *ctx, const char *s) {
    struct pair *o = ctx;
    size_t       n = strlen(s);
    assert(n > 0 && n <= 512 && o->thought_n + n < sizeof o->thought);
    if (((unsigned char) s[0] & 0xC0) == 0x80)
        o->boundaries = false;
    size_t tail = n, need = 0;
    while (tail && ((unsigned char) s[tail - 1] & 0xC0) == 0x80)
        --tail;
    if (tail) {
        unsigned char lead = (unsigned char) s[tail - 1];
        need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (n - (tail - 1) != need)
            o->boundaries = false;
    }
    memcpy(o->thought + o->thought_n, s, n + 1);
    o->thought_n += n;
    ++o->chunks;
    return true;
}
/* #93: with a thinking callback, the reasoning text arrives without markers, for every split. */
static void check_thinking(const char *input, const char *answer, const char *thought) {
    for (size_t split = 0; split <= strlen(input); ++split) {
        char left[1024];
        memcpy(left, input, split);
        left[split]               = 0;
        static struct pair  pair;
        pair                      = (struct pair) {.boundaries = true};
        struct str_output   p;
        str_output_init(&p, true);
        str_output_thinking(&p, emit_thought);
        assert(str_output_feed(&p, left, emit_answer, &pair));
        assert(str_output_feed(&p, input + split, emit_answer, &pair));
        str_output_finish(&p);
        assert(!strcmp(pair.answer.text, answer));
        assert(!strcmp(pair.thought, thought));
        /* Production feeds whole characters (str_utf8_feed); only such splits must keep boundaries. */
        if (((unsigned char) input[split] & 0xC0) != 0x80)
            assert(pair.boundaries);
    }
}
static void utf8(void) { /* ported from geist-serve tests/app/core_test.c */
    struct str_utf8 utf8 = {};
    char            decoded[64];
    assert(str_utf8_feed(&utf8, "Gr\xc3", decoded, sizeof decoded) && !strcmp(decoded, "Gr"));
    assert(str_utf8_feed(&utf8, "\xbc\xc3", decoded, sizeof decoded) && !strcmp(decoded, "\xc3\xbc"));
    assert(str_utf8_feed(&utf8, "\x9f" "e \xf0\x9f", decoded, sizeof decoded) && !strcmp(decoded, "\xc3\x9f" "e "));
    assert(str_utf8_feed(&utf8, "\x8c\xb1", decoded, sizeof decoded) && !strcmp(decoded, "\xf0\x9f\x8c\xb1"));
    assert(!utf8.used);
    const char *invalid[] = {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        struct str_utf8 bad = {};
        assert(!str_utf8_feed(&bad, invalid[i], decoded, sizeof decoded) && bad.failed);
        assert(!str_utf8_feed(&bad, "ok", decoded, sizeof decoded));
    }
    struct str_utf8 small = {};
    assert(!str_utf8_feed(&small, "\xc3\xbc\xc3\xbc", decoded, 3) && small.failed); /* out too small */
}

/* Stop strings (new): every split of the answer gives the same result. */
static void stop_case(const char *answer, size_t n, const char *const stops[], const char *want) {
    for (size_t split = 0; split <= strlen(answer); ++split) {
        if (((unsigned char) answer[split] & 0xC0) == 0x80)
            continue; /* the stage after str_utf8 only sees whole characters */
        char left[256];
        memcpy(left, answer, split);
        left[split] = 0;
        struct sink      s = {0};
        struct str_stops f;
        str_stops_init(&f, n, stops);
        assert(str_stops_feed(&f, left, emit, &s));
        assert(str_stops_feed(&f, answer + split, emit, &s));
        assert(str_stops_finish(&f, emit, &s));
        str_stops_free(&f);
        assert(!strcmp(s.text, want));
    }
    struct sink      s = {0};
    struct str_stops f;
    str_stops_init(&f, n, stops);
    for (const char *c = answer; *c;) { /* one character at a time */
        size_t len = 1;
        while (((unsigned char) c[len] & 0xC0) == 0x80)
            len++;
        char one[8] = {};
        memcpy(one, c, len);
        assert(str_stops_feed(&f, one, emit, &s));
        c += len;
    }
    assert(str_stops_finish(&f, emit, &s));
    str_stops_free(&f);
    assert(!strcmp(s.text, want));
}

static void stop_strings(void) {
    const char *const one[]  = {"Köln"};
    const char *const two[]  = {"\n\n", "END"};
    const char *const near[] = {"abcd"};
    stop_case("Grüße aus Köln und Bonn", 1, one, "Grüße aus ");
    stop_case("no stop here 🌿", 1, one, "no stop here 🌿");
    stop_case("Köln first", 1, one, "");
    stop_case("a\nb END c\n\nd", 2, two, "a\nb ");
    stop_case("abc abcx abcd", 1, near, "abc abcx ");
    stop_case("ends with abc", 1, near, "ends with abc"); /* a held prefix is released at the end */
    stop_case("plain", 0, nullptr, "plain");
}

int main(void) {
    utf8();
    stop_strings();

    check("<think>SECRET Grüß 🌿</think>Answer", "Answer", true, "think_tags");
    check(" \n<think></think>\n<think>SECRET</think> Grüß 🌿", "Grüß 🌿", true, "think_tags");
    check("<think>\n\n</think>\n\nParis", "Paris", true, "think_tags");
    check("<think>SECRET </thin", "", true, "think_tags");
    check("<think><think>SECRET</think>SECRET</think>Answer", "Answer", true, "think_tags");
    check("<thi", "", false, "think_tags");
    check("Normal <think> literal", "Normal <think> literal", false, "think_tags");
    check("```html\n<think>literal</think>\n```",
          "```html\n<think>literal</think>\n```",
          false,
          "think_tags");
    check("`<think>` &lt;think&gt; \\<think>",
          "`<think>` &lt;think&gt; \\<think>",
          false,
          "think_tags");
    check("  α + β", "  α + β", false, "think_tags");
    check("<think>literal</think>", "<think>literal</think>", false, "none");
    check("</think>literal", "</think>literal", false, "think_tags");
    struct sink       s = {0};
    struct str_output p;
    str_output_init(&p, true);
    assert(str_output_feed(&p, "<think>", emit, &s));
    for (unsigned i = 0; i < 1000000; ++i)
        assert(str_output_feed(&p, "SECRET", emit, &s));
    assert(p.used < sizeof p.prefix && p.closing < 8 && s.n == 0);
    assert(str_output_feed(&p, "</think>done", emit, &s));
    assert(!strcmp(s.text, "done"));
    str_output_finish(&p);
    assert(p.prefix[0] == 0);
    str_output_init(&p, true);
    s = (struct sink) {0};
    for (unsigned i = 0; i < 32; ++i)
        assert(str_output_feed(&p, "<think>", emit, &s));
    for (unsigned i = 0; i < 32; ++i)
        assert(str_output_feed(&p, "</think>", emit, &s));
    assert(str_output_feed(&p, "SECRET", emit, &s));
    assert(!s.n);
    check_thinking("<think>Plan: grüß 🌿 </thinker> ok</think>Answer", "Answer", "Plan: grüß 🌿 </thinker> ok");
    check_thinking("<think>a<think>b</think>c</think>Done", "Done", "a<think>bc");
    check_thinking("<think>no close yet </th", "", "no close yet ");
    { /* 1 MB of mixed-width text: bounded chunks on character boundaries, nothing lost. */
        static struct pair big;
        big = (struct pair) {.boundaries = true};
        struct str_output q;
        str_output_init(&q, true);
        str_output_thinking(&q, emit_thought);
        assert(str_output_feed(&q, "<think>", emit_answer, &big));
        size_t sent = 0;
        for (unsigned i = 0; i < 60000; ++i) {
            assert(str_output_feed(&q, "Grüß 🌿 x", emit_answer, &big));
            sent += strlen("Grüß 🌿 x");
        }
        assert(str_output_feed(&q, "</think>fin", emit_answer, &big));
        assert(big.thought_n == sent && big.boundaries && big.chunks > 1000 && !strcmp(big.answer.text, "fin"));
    }
    { /* repetition: a cycle of 4..256 tokens, 3 times and 48 tokens at least, at the end */
        static int32_t t[2048];
        size_t         n = 0;
        for (int32_t k = 0; k < 20; k++) /* an opening that does not repeat */
            t[n++] = 1000 + k;
        for (int r = 0; r < 3; r++) /* a 12-token sentence, 3 times: 36 tokens, not yet */
            for (int32_t k = 0; k < 12; k++)
                t[n++] = k;
        assert(!str_repeats(t, n));
        for (int32_t k = 0; k < 12; k++) /* the 4th time: 48 tokens */
            t[n++] = k;
        assert(str_repeats(t, n));
        t[n++] = 99; /* it moved on: not at the end any more */
        assert(!str_repeats(t, n));
        n = 0;
        for (int r = 0; r < 11; r++) /* period 4: 12 times needed, 11 is not */
            for (int32_t k = 0; k < 4; k++)
                t[n++] = k;
        assert(!str_repeats(t, n));
        for (int32_t k = 0; k < 4; k++)
            t[n++] = k;
        assert(str_repeats(t, n));
        n = 0;
        for (int r = 0; r < 40; r++) /* period 3 ("| 0 |" cells) never counts */
            for (int32_t k = 0; k < 3; k++)
                t[n++] = k;
        assert(!str_repeats(t, n));
        n = 0;
        for (int r = 0; r < 3; r++) /* period 300: longer than 256, not watched */
            for (int32_t k = 0; k < 300; k++)
                t[n++] = k;
        assert(!str_repeats(t, n));
        for (n = 0; n < 200; n++) /* 200 equal tokens (a rule of =====) and "0, 0, 0, …": data */
            t[n] = 7;
        assert(!str_repeats(t, n));
        for (n = 0; n < 200; n++)
            t[n] = n % 2 ? 44 : 15;
        assert(!str_repeats(t, n));
        for (n = 0; n < 1000; n++) /* no repeat at all */
            t[n] = (int32_t) n;
        assert(!str_repeats(t, n) && !str_repeats(t, 0));
    }
    puts("stream: UTF-8, output protocol (all splits, literal Markdown, Unicode, EOF, 6 MB bounded discard, thinking), stop strings, repetition passed");
    return 0;
}

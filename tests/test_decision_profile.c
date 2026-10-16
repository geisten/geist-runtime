#include "decision_profile.h"
#include "template.h"
#include "native_cases.h"
#include "fixtures/decisions/bonsai2_context.h"
#include "fixtures/decisions/gemma4_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int failures;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            ++failures;                                                                                      \
        }                                                                                                    \
    } while (false)
struct fake {
    const struct native_case *fixture;
    int fault;
};
static geistr_status tokenize(size_t *written, size_t cap, const char *text, int32_t *ids, void *context) {
    struct fake *f = context;
    const size_t len = strlen(f->fixture->prompt);
    bool appended = text[len] != 0;
    size_t count = f->fixture->n_ids + (appended ? 1u : 0u);
    if (count > cap)
        return GEISTR_CONTEXT;
    memcpy(ids, f->fixture->ids, f->fixture->n_ids * sizeof *ids);
    if (appended) {
        size_t key = (size_t)(text[len] - 'A');
        if (key >= f->fixture->n_options || text[len + 1])
            return GEISTR_INVALID;
        ids[count - 1] = f->fixture->candidates[f->fault == 2 ? 0 : key];
        if (f->fault == 1)
            ++ids[0];
        if (f->fault == 3)
            ++count; /* reports a multi-token suffix, without writing past cap */
    }
    *written = count;
    return GEISTR_OK;
}
static void cases(size_t n, geistr_decision_profile p, const struct native_case *fixtures) {
    char *text = malloc(DECISION_PROMPT_CAP);
    CHECK(text != nullptr);
    if (!text)
        return;
    for (size_t i = 0; i < n; ++i) {
        const struct native_case *f = &fixtures[i];
        geistr_decision_option options[4];
        for (size_t k = 0; k < f->n_options; ++k)
            options[k] = (geistr_decision_option){strlen(f->options[k].id), strlen(f->options[k].description),
                                                  f->options[k].id, f->options[k].description};
        geistr_decision_request r = {.size = sizeof r,
                                     .operation = GEISTR_OPERATION_DECISION,
                                     .question_len = strlen(f->question),
                                     .context_len = strlen(f->context),
                                     .n_options = f->n_options,
                                     .question = f->question,
                                     .context = f->context,
                                     .options = options};
        size_t written = 99;
        CHECK(decision_render(&written, DECISION_PROMPT_CAP, p, &r, text) == GEISTR_OK);
        CHECK(written == strlen(f->prompt) && !strcmp(text, f->prompt));
        int32_t ids[1024], scratch[1024], candidates[26];
        struct fake t = {.fixture = f};
        size_t count = 99;
        CHECK(decision_map(&count, 1024, f->n_options, DECISION_PROMPT_CAP, tokenize, text, ids, scratch,
                           candidates, &t) == GEISTR_OK);
        CHECK(count == f->n_ids && !memcmp(candidates, f->candidates, f->n_options * sizeof *candidates));
        for (int fault = 1; fault <= 3; ++fault) {
            t.fault = fault;
            CHECK(decision_map(&count, 1024, f->n_options, DECISION_PROMPT_CAP, tokenize, text, ids, scratch,
                               candidates, &t) == GEISTR_FORMAT);
            CHECK(count == 0 && candidates[0] == 0 && !strcmp(text, f->prompt));
        }
        CHECK(decision_render(&written, strlen(f->prompt), p, &r, text) == GEISTR_CONTEXT);
        CHECK(written == 0 && !text[0]);
        r.question = "<|turn>";
        r.question_len = strlen(r.question);
        CHECK(decision_render(&written, DECISION_PROMPT_CAP, p, &r, text) == GEISTR_INVALID);
        CHECK(written == 0 && !text[0]);
    }
    free(text);
}
/* #162: replays the native oracle for the common prefix, each full prompt and
 * each prompt plus key; anything else is an unexpected tokenization. */
static geistr_status replay(size_t *written, size_t cap, const char *text, int32_t *ids, void *context) {
    const struct native_context *c = context;
    const int32_t *source = nullptr, *key = nullptr;
    size_t n = 0;
    if (!strcmp(text, c->prefix)) {
        source = c->prefix_ids;
        n = c->n_prefix;
    }
    for (size_t i = 0; !source && i < c->n_questions; ++i) {
        const struct native_case *q = &c->questions[i];
        const size_t len = strlen(q->prompt);
        if (strncmp(text, q->prompt, len))
            continue;
        if (text[len] && (text[len + 1] || text[len] < 'A' || (size_t)(text[len] - 'A') >= q->n_options))
            continue;
        source = q->ids;
        n = q->n_ids;
        key = text[len] ? &q->candidates[text[len] - 'A'] : nullptr;
    }
    if (!source)
        return GEISTR_INVALID;
    if (n + (key ? 1 : 0) > cap)
        return GEISTR_CONTEXT;
    memcpy(ids, source, n * sizeof *ids);
    if (key)
        ids[n] = *key;
    *written = n + (key ? 1 : 0);
    return GEISTR_OK;
}
static geistr_status empty(size_t *written, size_t, const char *, int32_t *, void *) {
    *written = 0;
    return GEISTR_OK;
}
static void question(const struct native_case *q, geistr_decision_option *options, geistr_decision_request *r) {
    for (size_t k = 0; k < q->n_options; ++k)
        options[k] = (geistr_decision_option){strlen(q->options[k].id), strlen(q->options[k].description),
                                              q->options[k].id, q->options[k].description};
    *r = (geistr_decision_request){.size = sizeof *r,
                                   .operation = GEISTR_OPERATION_DECISION,
                                   .question_len = strlen(q->question),
                                   .context_len = strlen(q->context),
                                   .n_options = q->n_options,
                                   .question = q->question,
                                   .context = q->context,
                                   .options = options};
}
static void contexts(size_t n, geistr_decision_profile p, const struct native_context *fixtures,
                     const char *profile, const char *template_sha256, const size_t *chunks) {
    /* A changed profile needs new fixtures; these must not certify it. */
    const struct decision_profile *e = decision_profile_get(p);
    CHECK(e && !strcmp(e->name, profile) && !strcmp(e->template_sha256, template_sha256));
    CHECK(e && !strcmp(geistr_decision_profile_sha256(p), e->artifact_sha256));
    char *text = malloc(DECISION_PROMPT_CAP);
    CHECK(text != nullptr);
    if (!text)
        return;
    for (size_t i = 0; i < n; ++i) {
        const struct native_context *c = &fixtures[i];
        size_t written = 99;
        CHECK(decision_render_prefix(&written, DECISION_PROMPT_CAP, p, strlen(c->context), c->context, text) ==
              GEISTR_OK);
        CHECK(written == strlen(c->prefix) && !strcmp(text, c->prefix));
        for (size_t k = 0; k < 2; ++k) {
            int32_t cached[1024], ids[1024], scratch[1024], candidates[26];
            size_t cut = 99;
            CHECK(decision_render_prefix(&written, DECISION_PROMPT_CAP, p, strlen(c->context), c->context,
                                         text) == GEISTR_OK);
            CHECK(decision_prefix_cut(&cut, 1024, chunks[k], replay, text, cached, (void *)c) == GEISTR_OK);
            CHECK(cut == c->cuts[k] && cut < c->n_prefix && cut % chunks[k] == 0);
            /* First alone, then all in reverse order: identical prompts and IDs. */
            for (size_t pass = 0; pass < 2; ++pass)
                for (size_t j = 0; j < (pass ? c->n_questions : 1); ++j) {
                    const struct native_case *q = &c->questions[pass ? c->n_questions - 1 - j : 0];
                    geistr_decision_option options[4];
                    geistr_decision_request r;
                    question(q, options, &r);
                    CHECK(decision_render(&written, DECISION_PROMPT_CAP, p, &r, text) == GEISTR_OK);
                    CHECK(!strcmp(text, q->prompt) && !strncmp(text, c->prefix, strlen(c->prefix)));
                    size_t count = 0, suffix = 0;
                    CHECK(decision_map(&count, 1024, q->n_options, DECISION_PROMPT_CAP, replay, text, ids, scratch,
                                       candidates, (void *)c) == GEISTR_OK);
                    CHECK(count == q->n_ids && !memcmp(ids, q->ids, count * sizeof *ids));
                    CHECK(!memcmp(candidates, q->candidates, q->n_options * sizeof *candidates));
                    CHECK(decision_suffix(&suffix, cut, cached, count, ids) == GEISTR_OK);
                    CHECK(cut + suffix == q->n_ids && !memcmp(cached, q->ids, cut * sizeof *cached) &&
                          !memcmp(ids + cut, q->ids + cut, suffix * sizeof *ids));
                    CHECK(decision_suffix(&suffix, count, ids, count, ids) == GEISTR_FORMAT && !suffix);
                    if (cut) {
                        ++cached[cut - 1];
                        CHECK(decision_suffix(&suffix, cut, cached, count, ids) == GEISTR_FORMAT && !suffix);
                        --cached[cut - 1];
                    }
                }
        }
    }
    /* Reserved controls, invalid UTF-8, oversize and short capacity fail with empty output. */
    const char *bad[] = {"a<|im_end|>", "<turn|>", "x <think> y", "\xff", "\xc3"};
    size_t written = 99;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        CHECK(decision_render_prefix(&written, DECISION_PROMPT_CAP, p, strlen(bad[i]), bad[i], text) ==
              GEISTR_INVALID);
        CHECK(!written && !text[0]);
    }
    memset(text, 'x', GEISTR_DECISION_MAX_CONTEXT + 1);
    CHECK(decision_render_prefix(&written, DECISION_PROMPT_CAP - GEISTR_DECISION_MAX_CONTEXT - 1, p,
                                 GEISTR_DECISION_MAX_CONTEXT + 1, text,
                                 text + GEISTR_DECISION_MAX_CONTEXT + 1) == GEISTR_INVALID);
    CHECK(decision_render_prefix(&written, strlen(fixtures[0].prefix), p, 0, "", text) == GEISTR_CONTEXT);
    CHECK(!written && !text[0]);
    CHECK(decision_render_prefix(&written, DECISION_PROMPT_CAP, (geistr_decision_profile)3, 0, "", text) ==
          GEISTR_FORMAT);
    size_t cut = 99;
    int32_t ids[4];
    CHECK(decision_prefix_cut(&cut, 4, 0, empty, "x", ids, nullptr) == GEISTR_INVALID && !cut);
    CHECK(decision_prefix_cut(&cut, 4, 16, empty, "x", ids, nullptr) == GEISTR_FORMAT && !cut);
    CHECK(decision_suffix(&cut, 0, nullptr, 4, ids) == GEISTR_OK && cut == 4);
    free(text);
}
/* An unverified profile from the chat template family renders exactly what
 * the verified profile renders (Gemma's textual <bos> aside: unverified, the
 * BOS is a token, as in the chat), for every native fixture's request shape. */
static void families(void) {
    const struct {
        geistr_decision_profile verified;
        enum tpl_family family;
    } pairs[] = {{GEISTR_DECISION_GEMMA4_E2B_Q4, TPL_GEMMA4}, {GEISTR_DECISION_BONSAI2_27B_PQ2, TPL_CHATML}};
    const geistr_decision_option options[3] = {{4, 1, "four", "4"}, {4, 1, "five", "5"}, {4, 8, "röt", "nicht 4 "}};
    const geistr_decision_request requests[2] = {
        {.size = sizeof requests[0], .operation = GEISTR_OPERATION_DECISION, .question_len = 22,
         .n_options = 2, .question = "Which option equals 4?", .options = options},
        {.size = sizeof requests[0], .operation = GEISTR_OPERATION_DECISION, .question_len = 3, .context_len = 18,
         .n_options = 3, .question = "Q?\n", .context = "line one\nline two\n", .options = options}};
    static char native[DECISION_PROMPT_CAP], generic[DECISION_PROMPT_CAP];
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; ++i) {
        const struct decision_profile *v = decision_profile_get(pairs[i].verified);
        struct decision_family_profile f;
        CHECK(decision_profile_from_family(&f, pairs[i].family, *v->thinking_off != 0));
        for (size_t k = 0; k < 2; ++k) {
            size_t n = 0, g = 0;
            CHECK(decision_render(&n, sizeof native, pairs[i].verified, &requests[k], native) == GEISTR_OK);
            CHECK(decision_render_with(&g, sizeof generic, &f.profile, &requests[k], generic) == GEISTR_OK);
            CHECK(n == strlen(v->bos) + g && !strncmp(native, v->bos, strlen(v->bos)) &&
                  !strcmp(native + strlen(v->bos), generic));
        }
    }
    /* Every family the chat knows yields a profile; an unknown one does not. */
    for (int family = TPL_UNKNOWN + 1; family <= TPL_BITNET; ++family) {
        struct decision_family_profile f;
        CHECK(decision_profile_from_family(&f, family, false) && *f.profile.assistant && !*f.profile.thinking_off);
    }
    struct decision_family_profile unknown;
    CHECK(!decision_profile_from_family(&unknown, TPL_UNKNOWN, false));
}

int main(void) {
    families();
    cases(sizeof bonsai2_cases / sizeof bonsai2_cases[0], GEISTR_DECISION_BONSAI2_27B_PQ2, bonsai2_cases);
    cases(sizeof gemma4_cases / sizeof gemma4_cases[0], GEISTR_DECISION_GEMMA4_E2B_Q4, gemma4_cases);
    contexts(sizeof bonsai2_contexts / sizeof bonsai2_contexts[0], GEISTR_DECISION_BONSAI2_27B_PQ2,
             bonsai2_contexts, bonsai2_context_profile, bonsai2_context_template, bonsai2_context_chunks);
    contexts(sizeof gemma4_contexts / sizeof gemma4_contexts[0], GEISTR_DECISION_GEMMA4_E2B_Q4, gemma4_contexts,
             gemma4_context_profile, gemma4_context_template, gemma4_context_chunks);
    CHECK(!decision_profile_get(GEISTR_DECISION_PROFILE_NONE) && !decision_profile_get((geistr_decision_profile)3));
    if (failures)
        return 1;
    puts("decision profiles: independent native bytes, whitespace/Unicode/order, overflow and "
         "complete-prefix failures passed; context prefixes, cached cuts, suffixes and question order passed");
    return 0;
}

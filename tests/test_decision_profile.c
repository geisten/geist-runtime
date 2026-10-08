#include "decision_profile.h"
#include "native_cases.h"
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
int main(void) {
    cases(sizeof bonsai2_cases / sizeof bonsai2_cases[0], GEISTR_DECISION_BONSAI2_27B_PQ2, bonsai2_cases);
    cases(sizeof gemma4_cases / sizeof gemma4_cases[0], GEISTR_DECISION_GEMMA4_E2B_Q4, gemma4_cases);
    if (failures)
        return 1;
    puts("decision profiles: independent native bytes, whitespace/Unicode/order, overflow and "
         "complete-prefix failures passed");
    return 0;
}

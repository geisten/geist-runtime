/* Reusable decision context core (#163); see decision_batch.h. */
#include "decision_batch.h"
#include "geistr_catalog.h"

#include <math.h>
#include <stdarg.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double clock_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

void decision_batch_free(struct decision_batch *b) {
    if (!b)
        return;
    free(b->workspace);
    b->workspace = b->context = b->text = nullptr;
    b->cached = b->scratch = b->prompts = nullptr;
    b->workspace_bytes = 0;
}

geistr_status decision_batch_init(struct decision_batch *b, geistr_decision_profile profile, size_t max_prompt,
                                  size_t context_len, const char *context) {
    if (!b)
        return GEISTR_INVALID;
    *b = (struct decision_batch){.profile = profile};
    if (!max_prompt || context_len > GEISTR_DECISION_MAX_CONTEXT || (context_len && !context))
        return GEISTR_INVALID;
    /* (MAX_BATCH + 2) token arrays, the prompt text and the context copy. */
    size_t tokens, ids, bytes;
    if (ckd_add(&b->token_capacity, max_prompt, 1) ||
        ckd_mul(&tokens, b->token_capacity, (size_t)GEISTR_DECISION_MAX_BATCH + 2) ||
        ckd_mul(&ids, tokens, sizeof(int32_t)) || ckd_add(&bytes, ids, (size_t)DECISION_PROMPT_CAP) ||
        ckd_add(&bytes, bytes, context_len + 1))
        return GEISTR_CONTEXT;
    b->workspace = malloc(bytes);
    if (!b->workspace)
        return GEISTR_NO_MEMORY;
    b->workspace_bytes = bytes;
    b->cached = (int32_t *)(void *)b->workspace;
    b->scratch = b->cached + b->token_capacity;
    b->prompts = b->scratch + b->token_capacity;
    b->text = b->workspace + ids;
    b->context = b->text + DECISION_PROMPT_CAP;
    if (context_len)
        memcpy(b->context, context, context_len);
    b->context[context_len] = 0;
    b->context_len = context_len;
    size_t written = 0;
    geistr_status s = decision_render_prefix(&written, DECISION_PROMPT_CAP, profile, context_len, b->context, b->text);
    if (s == GEISTR_OK)
        s = geistr_sha256_memory(written, b->text, b->context_sha256);
    if (s != GEISTR_OK)
        decision_batch_free(b);
    return s;
}

geistr_status decision_batch_cut(struct decision_batch *b, size_t chunk, const struct decision_batch_engine *e) {
    if (!b || !b->workspace || !e)
        return GEISTR_INVALID;
    return decision_prefix_cut(&b->n_cached, b->token_capacity, chunk, e->tokenize, b->text, b->cached, e->context);
}

[[gnu::format(printf, 5, 6)]] static geistr_status fail(struct decision_batch *b, geistr_decision_batch_result *r,
                                                         geistr_status status, size_t index, const char *fmt, ...) {
    r->failed_index = index;
    char what[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    if (index == SIZE_MAX)
        snprintf(b->error, sizeof b->error, "decision batch: %s", what);
    else
        /* bounded parts: at most 25 + 20 + 2 + 64 + 1 + 2 + 120 = 234 bytes, so GCC's
         * -Wformat-truncation sees it fits the 256-byte error (a long ID is cut, not the format) */
        snprintf(b->error, sizeof b->error, "decision batch: question %zu%s%.64s%s: %.120s", index,
                 r->failed_id[0] ? " (" : "", r->failed_id, r->failed_id[0] ? ")" : "", what);
    return status;
}

/* Staged state from an earlier or failed batch never survives into a result. */
static void clear(struct decision_batch *b) {
    memset(b->logits, 0, sizeof b->logits);
    memset(b->probabilities, 0, sizeof b->probabilities);
    memset(b->answers, 0, sizeof b->answers);
    memset(b->n_prompt, 0, sizeof b->n_prompt);
}

static geistr_status preflight(struct decision_batch *b, const struct decision_batch_engine *e,
                               const geistr_decision_batch *batch, geistr_decision_batch_result *r) {
    for (size_t i = 0; i < batch->n_questions; ++i) {
        const geistr_decision_question *q = &batch->questions[i];
        if (e->cancelled(e->context))
            return fail(b, r, GEISTR_CANCELLED, i, "cancelled during preflight");
        /* The question ID under exactly the option ID rules. */
        const geistr_decision_option id = {q->id_len, 1, q->id, "x"};
        const geistr_decision_request probe = {.size = sizeof probe,
                                               .operation = GEISTR_OPERATION_DECISION,
                                               .question_len = 1,
                                               .n_options = 1,
                                               .question = "x",
                                               .options = &id};
        if (geistr_decision_request_validate(0, &probe, nullptr) != GEISTR_OK)
            return fail(b, r, GEISTR_INVALID, i, "invalid question ID");
        memcpy(r->failed_id, q->id, q->id_len);
        r->failed_id[q->id_len] = 0;
        for (size_t k = 0; k < i; ++k)
            if (q->id_len == batch->questions[k].id_len && !memcmp(q->id, batch->questions[k].id, q->id_len))
                return fail(b, r, GEISTR_INVALID, i, "duplicate question ID");
        const geistr_decision_request request = {.size = sizeof request,
                                                 .operation = GEISTR_OPERATION_DECISION,
                                                 .question_len = q->question_len,
                                                 .context_len = b->context_len,
                                                 .n_options = q->n_options,
                                                 .question = q->question,
                                                 .context = b->context,
                                                 .options = q->options};
        char why[128] = "";
        if (geistr_decision_request_validate(sizeof why, &request, why) != GEISTR_OK)
            return fail(b, r, GEISTR_INVALID, i, "%s", why);
        size_t written = 0, n_prompt = 0, n_suffix = 0;
        int32_t *prompt = b->prompts + i * b->token_capacity;
        geistr_status s = decision_render(&written, DECISION_PROMPT_CAP, b->profile, &request, b->text);
        if (s != GEISTR_OK)
            return fail(b, r, s, i, "rendering: reserved control text or prompt capacity");
        s = decision_map(&n_prompt, b->token_capacity, q->n_options, DECISION_PROMPT_CAP, e->tokenize, b->text,
                         prompt, b->scratch, b->candidates[i], e->context);
        if (s != GEISTR_OK)
            return fail(b, r, s, i, "%s", s == GEISTR_CANCELLED ? "cancelled during tokenization"
                                          : s == GEISTR_CONTEXT ? "prompt exceeds max_prompt_tokens"
                                                                : "tokenization or option keys");
        s = decision_suffix(&n_suffix, b->n_cached, b->cached, n_prompt, prompt);
        if (s != GEISTR_OK)
            return fail(b, r, s, i, "the cached context is not an exact prefix of the prompt");
        b->n_prompt[i] = n_prompt;
        r->failed_id[0] = 0;
        ++r->prepared_questions;
    }
    return GEISTR_OK;
}

static geistr_status scoring(struct decision_batch *b, const struct decision_batch_engine *e,
                             const geistr_decision_batch *batch, geistr_decision_batch_result *r) {
    for (size_t i = 0; i < batch->n_questions; ++i) {
        const geistr_decision_question *q = &batch->questions[i];
        memcpy(r->failed_id, q->id, q->id_len);
        r->failed_id[q->id_len] = 0;
        if (e->cancelled(e->context))
            return fail(b, r, GEISTR_CANCELLED, i, "cancelled before restore");
        size_t best = SIZE_MAX;
        b->detail[0] = 0;
        ++r->model_calls;
        const geistr_status s =
            e->score(e->context, b->n_prompt[i] - b->n_cached, q->n_options,
                     b->prompts + i * b->token_capacity + b->n_cached, b->candidates[i], b->logits[i],
                     b->probabilities[i], &best);
        if (e->cancelled(e->context))
            return fail(b, r, GEISTR_CANCELLED, i, "cancelled during synchronous scoring");
        if (s != GEISTR_OK)
            return fail(b, r, s, i, "restore/scoring: %s", b->detail[0] ? b->detail : "engine failure");
        bool valid = best < q->n_options;
        for (size_t k = 0; valid && k < q->n_options; ++k)
            valid = isfinite(b->logits[i][k]) && isfinite(b->probabilities[i][k]);
        if (!valid)
            return fail(b, r, GEISTR_BACKEND, i, "incomplete or non-finite candidates");
        geistr_decision_answer *a = &b->answers[i];
        a->n_options = q->n_options;
        a->best_index = best;
        a->prompt_tokens = b->n_prompt[i];
        a->uncached_tokens = b->n_prompt[i] - b->n_cached;
        r->failed_id[0] = 0;
    }
    return GEISTR_OK;
}

geistr_status decision_batch_score(struct decision_batch *b, const struct decision_batch_engine *e,
                                   const geistr_decision_batch *batch, geistr_decision_batch_result *r) {
    if (!r || r->size != sizeof *r)
        return GEISTR_INVALID;
    *r = (geistr_decision_batch_result){.size = sizeof *r, .failed_index = SIZE_MAX};
    if (!b || !b->workspace || !e)
        return GEISTR_INVALID;
    clear(b);
    b->error[0] = 0;
    const double start = clock_ms();
    geistr_status s = GEISTR_OK;
    if (e->cancelled(e->context))
        s = fail(b, r, GEISTR_CANCELLED, SIZE_MAX, "cancelled before preflight");
    else if (!batch || batch->size != sizeof *batch || batch->operation != GEISTR_OPERATION_DECISION ||
             !batch->n_questions || batch->n_questions > GEISTR_DECISION_MAX_BATCH || !batch->questions)
        s = fail(b, r, GEISTR_INVALID, SIZE_MAX, "explicit operation and 1..%d questions required",
                 GEISTR_DECISION_MAX_BATCH);
    else
        s = preflight(b, e, batch, r);
    r->preparation_ms = clock_ms() - start;
    if (s == GEISTR_OK) {
        const double scored = clock_ms();
        s = scoring(b, e, batch, r);
        r->scoring_ms = clock_ms() - scored;
    }
    if (s == GEISTR_CANCELLED)
        (void)e->cancelled(e->context); /* consumed by this batch */
    if (s != GEISTR_OK) {
        clear(b);
        r->batch_ms = clock_ms() - start;
        return s;
    }
    for (size_t i = 0; i < batch->n_questions; ++i) {
        const geistr_decision_question *q = &batch->questions[i];
        geistr_decision_answer *a = &b->answers[i];
        const geistr_decision_option *o = &q->options[a->best_index];
        memcpy(a->question_id, q->id, q->id_len);
        memcpy(a->external_id, o->id, o->id_len);
        a->logits = b->logits[i];
        a->selection_probabilities = b->probabilities[i];
    }
    r->n_answers = batch->n_questions;
    r->answers = b->answers;
    r->batch_ms = clock_ms() - start;
    return GEISTR_OK;
}

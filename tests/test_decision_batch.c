/* #163: the reusable-context batch core against the native context oracle and
 * an injected engine (failures, non-finite scores, cancellation at any point),
 * plus the feature-off public surface. Real-model parity: test_decision_real. */
#include "decision_batch.h"
#include "native_cases.h"
#include "fixtures/decisions/bonsai2_context.h"
#include "fixtures/decisions/gemma4_context.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(expr)                                                                                          \
    do {                                                                                                     \
        if (!(expr)) {                                                                                       \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr);                                       \
            ++failures;                                                                                      \
        }                                                                                                    \
    } while (false)

/* The injected engine: native oracle tokens, scores derived only from the
 * suffix and candidates it receives, and switches for every failure path. */
struct fake {
    const struct native_context *c;
    bool cancel;
    size_t tokenize_calls, score_calls;
    size_t cancel_at_tokenize, cancel_at_score, fail_at, nan_at; /* SIZE_MAX: never */
};

static geistr_status replay(size_t *written, size_t cap, const char *text, int32_t *ids, void *p) {
    struct fake *f = p;
    if (++f->tokenize_calls == f->cancel_at_tokenize)
        f->cancel = true;
    if (f->cancel)
        return GEISTR_CANCELLED;
    const struct native_context *c = f->c;
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

static bool cancelled(void *p) {
    struct fake *f = p;
    const bool was = f->cancel;
    f->cancel = false;
    return was;
}

static float expected(size_t n_suffix, const int32_t *suffix, int32_t candidate) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n_suffix; ++i)
        h = (h ^ (uint32_t)suffix[i]) * 16777619u;
    return (float)((h ^ (uint32_t)candidate * 2654435761u) % 1000u) / 10.0f;
}

static geistr_status score(void *p, size_t n_suffix, size_t n, const int32_t *suffix, const int32_t *candidates,
                           float *logits, double *probabilities, size_t *best) {
    struct fake *f = p;
    const size_t k = f->score_calls++;
    if (k == f->cancel_at_score)
        f->cancel = true;
    if (k == f->fail_at)
        return GEISTR_BACKEND;
    *best = 0;
    for (size_t i = 0; i < n; ++i) {
        logits[i] = expected(n_suffix, suffix, candidates[i]);
        probabilities[i] = 1.0 / (double)n;
        if (logits[i] > logits[*best])
            *best = i;
    }
    if (k == f->nan_at)
        logits[n - 1] = NAN;
    return GEISTR_OK;
}

static struct fake fake(const struct native_context *c) {
    return (struct fake){.c = c, .cancel_at_tokenize = SIZE_MAX, .cancel_at_score = SIZE_MAX,
                         .fail_at = SIZE_MAX, .nan_at = SIZE_MAX};
}
static struct decision_batch_engine engine(struct fake *f) {
    return (struct decision_batch_engine){.context = f, .tokenize = replay, .cancelled = cancelled, .score = score};
}

/* Questions q0.. over the fixture's own questions, in the given order. */
struct questions {
    geistr_decision_option options[GEISTR_DECISION_MAX_BATCH + 1][4];
    geistr_decision_question q[GEISTR_DECISION_MAX_BATCH + 1];
    char ids[GEISTR_DECISION_MAX_BATCH + 1][8];
    geistr_decision_batch batch;
};
static void build(struct questions *s, const struct native_context *c, size_t n, const size_t *order) {
    for (size_t i = 0; i < n; ++i) {
        const size_t j = order ? order[i] : i % c->n_questions;
        const struct native_case *f = &c->questions[j];
        snprintf(s->ids[i], sizeof s->ids[i], "q%zu", order ? j : i);
        for (size_t k = 0; k < f->n_options; ++k)
            s->options[i][k] =
                (geistr_decision_option){strlen(f->options[k].id), strlen(f->options[k].description),
                                         f->options[k].id, f->options[k].description};
        s->q[i] = (geistr_decision_question){strlen(s->ids[i]), strlen(f->question), f->n_options, s->ids[i],
                                             f->question, s->options[i]};
    }
    s->batch = (geistr_decision_batch){sizeof s->batch, GEISTR_OPERATION_DECISION, n, s->q};
}

/* A failed batch publishes nothing and leaves nothing staged. */
static void failed(const struct decision_batch *b, const geistr_decision_batch_result *r) {
    CHECK(!r->n_answers && !r->answers);
    for (size_t i = 0; i < GEISTR_DECISION_MAX_BATCH; ++i)
        CHECK(!b->answers[i].external_id[0] && !b->answers[i].question_id[0] && !b->answers[i].logits &&
              b->logits[i][0] == 0.0f);
    CHECK(b->error[0]);
}

/* Each answer must equal what the engine gives that question's own suffix. */
static void answered(const struct decision_batch *b, const struct native_context *c, const struct questions *s,
                     const geistr_decision_batch_result *r, const size_t *order) {
    CHECK(r->n_answers == s->batch.n_questions && r->answers == b->answers && r->failed_index == SIZE_MAX);
    CHECK(r->prepared_questions == s->batch.n_questions && r->model_calls == s->batch.n_questions);
    CHECK(!r->failed_id[0] && !b->error[0]);
    for (size_t i = 0; r->answers && i < r->n_answers; ++i) {
        const struct native_case *f = &c->questions[order ? order[i] : i % c->n_questions];
        const geistr_decision_answer *a = &r->answers[i];
        CHECK(!strcmp(a->question_id, s->ids[i]) && a->n_options == f->n_options);
        CHECK(a->prompt_tokens == f->n_ids && a->uncached_tokens == f->n_ids - b->n_cached);
        size_t best = 0;
        for (size_t k = 0; k < f->n_options; ++k) {
            const float want = expected(f->n_ids - b->n_cached, f->ids + b->n_cached, f->candidates[k]);
            CHECK(a->logits[k] == want);
            if (want > expected(f->n_ids - b->n_cached, f->ids + b->n_cached, f->candidates[best]))
                best = k;
        }
        CHECK(a->best_index == best && !strcmp(a->external_id, f->options[best].id));
    }
}

static void contexts(const struct native_context *c, geistr_decision_profile p, size_t chunk, size_t cut) {
    struct decision_batch *b = calloc(1, sizeof *b);
    struct questions *s = calloc(1, sizeof *s);
    CHECK(b && s);
    if (!b || !s)
        return;
    struct fake f = fake(c);
    const struct decision_batch_engine e = engine(&f);
    CHECK(decision_batch_init(b, p, 512, strlen(c->context), c->context) == GEISTR_OK);
    CHECK(strlen(b->context_sha256) == 64 && !strcmp(b->text, c->prefix));
    CHECK(decision_batch_cut(b, chunk, &e) == GEISTR_OK && b->n_cached == cut);
    geistr_decision_batch_result r = {.size = sizeof r};

    /* All questions, then reversed, then each alone: identical per question. */
    const size_t forward[] = {0, 1, 2}, reverse[] = {2, 1, 0};
    build(s, c, 3, forward);
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_OK);
    answered(b, c, s, &r, forward);
    build(s, c, 3, reverse);
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_OK);
    answered(b, c, s, &r, reverse);
    for (size_t i = 0; i < 3; ++i) {
        build(s, c, 1, &forward[i]);
        CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_OK);
        answered(b, c, s, &r, &forward[i]);
    }
    /* The maximum batch, and the questions repeated. */
    build(s, c, GEISTR_DECISION_MAX_BATCH, nullptr);
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_OK);
    answered(b, c, s, &r, nullptr);

    /* Rejections: no scoring attempt, nothing published. */
    f.score_calls = 0;
    build(s, c, GEISTR_DECISION_MAX_BATCH + 1, nullptr);
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_INVALID && r.failed_index == SIZE_MAX);
    failed(b, &r);
    s->batch.n_questions = 0;
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_INVALID && !r.model_calls);
    failed(b, &r);
    build(s, c, 3, forward);
    s->batch.operation = GEISTR_OPERATION_CHAT;
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_INVALID);
    s->batch.operation = GEISTR_OPERATION_DECISION;
    --s->batch.size;
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_INVALID);
    ++s->batch.size;
    CHECK(decision_batch_score(b, &e, nullptr, &r) == GEISTR_INVALID);
    --r.size;
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_INVALID);
    ++r.size;
    /* Duplicate ID (last), invalid ID (first), invalid UTF-8 (middle). */
    s->q[2].id = s->q[0].id;
    s->q[2].id_len = s->q[0].id_len;
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_INVALID && r.failed_index == 2 &&
          !strcmp(r.failed_id, "q0") && r.prepared_questions == 2 && strstr(b->error, "question 2 (q0)"));
    failed(b, &r);
    build(s, c, 3, forward);
    s->q[0].id = "a\nb";
    s->q[0].id_len = 3;
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_INVALID && r.failed_index == 0 && !r.failed_id[0]);
    failed(b, &r);
    build(s, c, 3, forward);
    s->q[1].question = "\xc3";
    s->q[1].question_len = 1;
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_INVALID && r.failed_index == 1 &&
          !strcmp(r.failed_id, "q1") && r.prepared_questions == 1);
    failed(b, &r);
    s->q[1].question = "<|im_end|>";
    s->q[1].question_len = 10;
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_INVALID && r.failed_index == 1);
    /* Options that duplicate an ID or exceed the bound. */
    build(s, c, 3, forward);
    s->q[1].n_options = GEISTR_DECISION_MAX_OPTIONS + 1;
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_INVALID && r.failed_index == 1);
    /* An unknown question to the tokenizer, and a cached prefix that does not match. */
    build(s, c, 3, forward);
    s->q[2].question = "Not in the oracle?";
    s->q[2].question_len = strlen(s->q[2].question);
    CHECK(decision_batch_score(b, &e, &s->batch, &r) != GEISTR_OK && r.failed_index == 2);
    failed(b, &r);
    build(s, c, 3, forward);
    if (cut) {
        ++b->cached[cut - 1];
        CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_FORMAT && r.failed_index == 0);
        failed(b, &r);
        --b->cached[cut - 1];
    }
    CHECK(!f.score_calls);

    /* Engine failure and non-finite scores at the first, middle and last question. */
    for (size_t at = 0; at < 3; ++at) {
        f.fail_at = at;
        f.score_calls = 0;
        CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_BACKEND && r.failed_index == at &&
              r.model_calls == at + 1 && r.prepared_questions == 3 && !strcmp(r.failed_id, s->ids[at]));
        failed(b, &r);
        f.fail_at = SIZE_MAX;
        f.nan_at = at;
        f.score_calls = 0;
        CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_BACKEND && r.failed_index == at);
        failed(b, &r);
        f.nan_at = SIZE_MAX;
        CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_OK); /* recovery */
        answered(b, c, s, &r, forward);
    }

    /* Cancellation: pending, during tokenization, during scoring; each consumed. */
    f.cancel = true;
    f.score_calls = 0;
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_CANCELLED && !r.model_calls &&
          !r.prepared_questions && r.failed_index == SIZE_MAX);
    failed(b, &r);
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_OK);
    f.tokenize_calls = 0;
    f.cancel_at_tokenize = 4; /* the second question's prompt */
    CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_CANCELLED && r.failed_index == 1 &&
          !r.model_calls && !f.cancel);
    failed(b, &r);
    f.cancel_at_tokenize = SIZE_MAX;
    for (size_t at = 0; at < 3; ++at) {
        f.cancel_at_score = at;
        f.score_calls = 0;
        CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_CANCELLED && r.failed_index == at &&
              r.model_calls == at + 1 && !f.cancel);
        failed(b, &r);
        f.cancel_at_score = SIZE_MAX;
        CHECK(decision_batch_score(b, &e, &s->batch, &r) == GEISTR_OK);
        answered(b, c, s, &r, forward);
    }

    /* A full prompt beyond max_prompt_tokens: the prefix fits, the question does not. */
    struct decision_batch *small = calloc(1, sizeof *small);
    CHECK(small != nullptr);
    if (small) {
        CHECK(decision_batch_init(small, p, c->n_prefix + 1, strlen(c->context), c->context) == GEISTR_OK);
        CHECK(decision_batch_cut(small, chunk, &e) == GEISTR_OK);
        f.score_calls = 0;
        CHECK(decision_batch_score(small, &e, &s->batch, &r) == GEISTR_CONTEXT && r.failed_index == 0 &&
              !f.score_calls);
        decision_batch_free(small);
        CHECK(decision_batch_init(small, p, c->n_prefix - 2, strlen(c->context), c->context) == GEISTR_OK);
        CHECK(decision_batch_cut(small, chunk, &e) == GEISTR_CONTEXT);
        decision_batch_free(small);
        free(small);
    }
    decision_batch_free(b);
    decision_batch_free(b); /* idempotent */
    free(s);
    free(b);
}

/* Two handles over different contexts, interleaved: nothing is shared. */
static void independent(void) {
    const struct native_context *c0 = &bonsai2_contexts[0], *c1 = &bonsai2_contexts[4];
    struct decision_batch *b0 = calloc(1, sizeof *b0), *b1 = calloc(1, sizeof *b1);
    struct questions *s0 = calloc(1, sizeof *s0), *s1 = calloc(1, sizeof *s1);
    CHECK(b0 && b1 && s0 && s1);
    if (b0 && b1 && s0 && s1) {
        struct fake f0 = fake(c0), f1 = fake(c1);
        const struct decision_batch_engine e0 = engine(&f0), e1 = engine(&f1);
        CHECK(decision_batch_init(b0, GEISTR_DECISION_BONSAI2_27B_PQ2, 512, strlen(c0->context), c0->context) ==
              GEISTR_OK);
        CHECK(decision_batch_init(b1, GEISTR_DECISION_BONSAI2_27B_PQ2, 512, strlen(c1->context), c1->context) ==
              GEISTR_OK);
        CHECK(strcmp(b0->context_sha256, b1->context_sha256));
        CHECK(decision_batch_cut(b0, 16, &e0) == GEISTR_OK && decision_batch_cut(b1, 16, &e1) == GEISTR_OK);
        geistr_decision_batch_result r0 = {.size = sizeof r0}, r1 = {.size = sizeof r1};
        build(s0, c0, 3, nullptr);
        build(s1, c1, 3, nullptr);
        CHECK(decision_batch_score(b0, &e0, &s0->batch, &r0) == GEISTR_OK);
        f1.fail_at = 1;
        CHECK(decision_batch_score(b1, &e1, &s1->batch, &r1) == GEISTR_BACKEND);
        answered(b0, c0, s0, &r0, nullptr); /* b1's failure did not touch b0's loan */
        CHECK(decision_batch_score(b1, &e1, &s1->batch, &r1) == GEISTR_OK);
        answered(b1, c1, s1, &r1, nullptr);
        answered(b0, c0, s0, &r0, nullptr);
    }
    if (b0)
        decision_batch_free(b0);
    if (b1)
        decision_batch_free(b1);
    free(b0);
    free(b1);
    free(s0);
    free(s1);
}

static void setup_errors(void) {
    struct decision_batch b;
    const char *bad[] = {"a<|im_end|>", "<turn|>", "\xff", "\xc3"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        CHECK(decision_batch_init(&b, GEISTR_DECISION_BONSAI2_27B_PQ2, 512, strlen(bad[i]), bad[i]) ==
              GEISTR_INVALID);
        CHECK(!b.workspace);
    }
    CHECK(decision_batch_init(&b, GEISTR_DECISION_BONSAI2_27B_PQ2, 0, 0, "") == GEISTR_INVALID && !b.workspace);
    CHECK(decision_batch_init(&b, GEISTR_DECISION_BONSAI2_27B_PQ2, SIZE_MAX, 0, "") == GEISTR_CONTEXT);
    CHECK(decision_batch_init(&b, GEISTR_DECISION_BONSAI2_27B_PQ2, SIZE_MAX / 64, 0, "") == GEISTR_CONTEXT);
    CHECK(decision_batch_init(&b, GEISTR_DECISION_BONSAI2_27B_PQ2, 512, GEISTR_DECISION_MAX_CONTEXT + 1, "x") ==
          GEISTR_INVALID);
    CHECK(decision_batch_init(&b, GEISTR_DECISION_PROFILE_NONE, 512, 0, "") == GEISTR_FORMAT && !b.workspace);
    CHECK(decision_batch_init(nullptr, GEISTR_DECISION_BONSAI2_27B_PQ2, 512, 0, "") == GEISTR_INVALID);
    geistr_decision_batch_result r = {.size = sizeof r};
    CHECK(decision_batch_score(nullptr, nullptr, nullptr, &r) == GEISTR_INVALID && r.failed_index == SIZE_MAX);
}

/* Feature-off (stub) surface: linkable, explicit, nothing pretends. */
static void unavailable(void) {
    CHECK(!geistr_decision_context_available());
    geistr_model *model = nullptr;
    CHECK(geistr_model_open("stub:echo", nullptr, &model, nullptr, 0) == GEISTR_OK);
    geistr_decision_context *context = (geistr_decision_context *)1;
    char error[128] = "";
    const geistr_decision_context_opts opts = GEISTR_DECISION_CONTEXT_OPTS_INIT;
    CHECK(geistr_decision_context_open(model, 2, sizeof error, "Ja", &opts, &context, error) == GEISTR_FORMAT &&
          !context && error[0]);
    CHECK(geistr_decision_context_open(nullptr, 0, 0, "", nullptr, &context, nullptr) == GEISTR_INVALID);
    geistr_decision_batch_result r = {.size = sizeof r, .n_answers = 7};
    CHECK(geistr_decision_context_score_batch(nullptr, nullptr, &r) == GEISTR_INVALID && !r.n_answers &&
          r.failed_index == SIZE_MAX);
    geistr_decision_context_info info = {.size = sizeof info, .cached_tokens = 7};
    CHECK(geistr_decision_context_info_get(nullptr, &info) == GEISTR_INVALID && !info.cached_tokens);
    CHECK(geistr_decision_context_cancel(nullptr) == GEISTR_INVALID);
    CHECK(geistr_decision_context_error(nullptr)[0]);
    geistr_decision_context_close(nullptr);
    geistr_model_close(model);
}

int main(void) {
    for (size_t i = 0; i < sizeof bonsai2_contexts / sizeof bonsai2_contexts[0]; ++i)
        for (size_t k = 0; k < 2; ++k)
            contexts(&bonsai2_contexts[i], GEISTR_DECISION_BONSAI2_27B_PQ2, bonsai2_context_chunks[k],
                     bonsai2_contexts[i].cuts[k]);
    for (size_t i = 0; i < sizeof gemma4_contexts / sizeof gemma4_contexts[0]; ++i)
        for (size_t k = 0; k < 2; ++k)
            contexts(&gemma4_contexts[i], GEISTR_DECISION_GEMMA4_E2B_Q4, gemma4_context_chunks[k],
                     gemma4_contexts[i].cuts[k]);
    independent();
    setup_errors();
    unavailable();
    if (failures)
        return 1;
    puts("decision contexts: preflight before scoring, atomic publication, reorder/addition identity, "
         "failures and cancellation at first/middle/last question, recovery, independent handles and "
         "feature-off stubs passed");
    return 0;
}

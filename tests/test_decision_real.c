/* White-box integration: identical loaded weights/backend for wrapper and
 * direct engine controls. Do not add an engine-handle escape to the public API. */
#include "../src/runtime.c"
#include "native_cases.h"
#include "fixtures/decisions/bonsai2_context.h"
#include "fixtures/decisions/gemma4_context.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(x)                                                                                           \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (false)

/* #163: the fixture's questions over one context, in the given order. */
struct batch {
    geistr_decision_option options[3][4];
    geistr_decision_question q[3];
    geistr_decision_batch batch;
};
static const char *const question_ids[] = {"q0", "q1", "q2"};
static void build(struct batch *s, const struct native_context *c, size_t n, const size_t *order) {
    for (size_t i = 0; i < n; ++i) {
        const struct native_case *f = &c->questions[order[i]];
        for (size_t k = 0; k < f->n_options; ++k)
            s->options[i][k] = (geistr_decision_option){strlen(f->options[k].id), strlen(f->options[k].description),
                                                        f->options[k].id, f->options[k].description};
        s->q[i] = (geistr_decision_question){2, strlen(f->question), f->n_options, question_ids[order[i]],
                                             f->question, s->options[i]};
    }
    s->batch = (geistr_decision_batch){sizeof s->batch, GEISTR_OPERATION_DECISION, n, s->q};
}

/* Logits per fixture question, from the first complete batch. */
static float batched[5][3][4];

static bool same(const geistr_decision_batch_result *r, size_t context, const size_t *order) {
    for (size_t i = 0; i < r->n_answers; ++i)
        if (strcmp(r->answers[i].question_id, question_ids[order[i]]) ||
            memcmp(r->answers[i].logits, batched[context][order[i]],
                   r->answers[i].n_options * sizeof(float)))
            return false;
    return r->n_answers > 0;
}

/* Leaves the first context open in *kept, to outlive the caller's model reference. */
static int contexts(geistr_model *m, geistr_chat *chat, size_t history, bool gemma, const char *profile_name,
                    const char *backend, geistr_decision_context **kept) {
    geistr_decision_context *x = nullptr;
    geistr_decision_context_opts xo = GEISTR_DECISION_CONTEXT_OPTS_INIT;
    char message[256];
    if (!geistr_decision_context_available()) { /* DECISION_CONTEXT=0: explicit, nothing pretends */
        REQUIRE(geistr_decision_context_open(m, 0, sizeof message, "", &xo, &x, message) == GEISTR_FORMAT && !x);
        puts("{\"decision_context\":\"unavailable_explicit_error\"}");
        return 0;
    }
    /* Conflicting mode, oversize prompt, snapshot budget, invalid context: refused at open. */
    xo.mode = GEISTR_DECISION_SELECTED_ROWS;
    REQUIRE(geistr_decision_context_open(m, 0, sizeof message, "", &xo, &x, message) == GEISTR_FORMAT && !x);
    xo = (geistr_decision_context_opts)GEISTR_DECISION_CONTEXT_OPTS_INIT;
    xo.max_prompt_tokens = 513;
    REQUIRE(geistr_decision_context_open(m, 0, sizeof message, "", &xo, &x, message) == GEISTR_CONTEXT && !x);
    xo = (geistr_decision_context_opts)GEISTR_DECISION_CONTEXT_OPTS_INIT;
    xo.max_snapshot_bytes = 1;
    REQUIRE(geistr_decision_context_open(m, 0, sizeof message, "", &xo, &x, message) == GEISTR_NO_MEMORY && !x);
    xo.max_snapshot_bytes = 0;
    --xo.size;
    REQUIRE(geistr_decision_context_open(m, 0, sizeof message, "", &xo, &x, message) == GEISTR_INVALID && !x);
    ++xo.size;
    REQUIRE(geistr_decision_context_open(m, 1, sizeof message, "\xff", &xo, &x, message) == GEISTR_INVALID && !x);

    const struct native_context *fixtures = gemma ? gemma4_contexts : bonsai2_contexts;
    const size_t *chunks = gemma ? gemma4_context_chunks : bonsai2_context_chunks;
    const size_t forward[] = {0, 1, 2}, reverse[] = {2, 1, 0};
    geistr_decision_context *open[5] = {};
    for (size_t c = 0; c < 5; ++c) {
        const struct native_context *f = &fixtures[c];
        geistr_status s = geistr_decision_context_open(m, strlen(f->context), sizeof message, f->context, nullptr,
                                                       &open[c], message);
        if (s != GEISTR_OK)
            fprintf(stderr, "context open: %s\n", message);
        REQUIRE(s == GEISTR_OK);
        geistr_decision_context_info info = {.size = sizeof info};
        REQUIRE(geistr_decision_context_info_get(open[c], &info) == GEISTR_OK);
        REQUIRE(info.mode == GEISTR_DECISION_DENSE && !strcmp(info.kv_cache, "fp32") && info.snapshot_bytes &&
                info.prefill_chunk && info.cached_tokens % info.prefill_chunk == 0 &&
                info.cached_tokens < f->n_prefix && info.max_prompt_tokens == 512 &&
                info.setup_model_calls == (info.cached_tokens ? 1u : 0u) && !strcmp(info.backend, backend) &&
                !strcmp(info.model_sha256, geistr_decision_profile_sha256(info.profile)) &&
                strlen(info.context_sha256) == 64 && !info.activation);
        for (size_t k = 0; k < 2; ++k)
            if (info.prefill_chunk == chunks[k])
                REQUIRE(info.cached_tokens == f->cuts[k]); /* the native fixture's cut */
        struct batch b;
        geistr_decision_batch_result r = {.size = sizeof r};
        build(&b, f, 3, forward);
        s = geistr_decision_context_score_batch(open[c], &b.batch, &r);
        if (s != GEISTR_OK)
            fprintf(stderr, "batch: %s\n", geistr_decision_context_error(open[c]));
        REQUIRE(s == GEISTR_OK && r.n_answers == 3 && r.model_calls == 3 && r.failed_index == SIZE_MAX);
        for (size_t i = 0; i < 3; ++i) {
            const geistr_decision_answer *a = &r.answers[i];
            REQUIRE(a->prompt_tokens == f->questions[i].n_ids &&
                    a->uncached_tokens == a->prompt_tokens - info.cached_tokens);
            REQUIRE(!strcmp(a->external_id, f->questions[i].options[a->best_index].id));
            memcpy(batched[c][i], a->logits, a->n_options * sizeof(float));
        }
        printf("{\"profile\":\"%s\",\"backend\":\"%s\",\"context\":%zu,\"cached_tokens\":%zu,\"prefix_tokens\":%zu,"
               "\"prefill_chunk\":%zu,\"snapshot_bytes\":%zu,\"setup_ms\":%.3f,\"batch_ms\":%.3f,"
               "\"scoring_ms\":%.3f}\n",
               profile_name, backend, c, info.cached_tokens, f->n_prefix, info.prefill_chunk, info.snapshot_bytes,
               info.setup_ms, r.batch_ms, r.scoring_ms);
    }
    /* Every context against the single-question API (full prompt, fresh state),
     * reordered, and each question alone; the contexts interleave with each other
     * and with the chat, which keeps its history. */
    geistr_decision *d = nullptr;
    REQUIRE(geistr_decision_open(m, sizeof message, nullptr, &d, message) == GEISTR_OK);
    for (size_t c = 0; c < 5; ++c) {
        const struct native_context *f = &fixtures[c];
        struct batch b;
        geistr_decision_batch_result r = {.size = sizeof r};
        build(&b, f, 3, reverse);
        REQUIRE(geistr_decision_context_score_batch(open[c], &b.batch, &r) == GEISTR_OK && same(&r, c, reverse));
        for (size_t i = 0; i < 3; ++i) {
            build(&b, f, 1, &forward[i]);
            REQUIRE(geistr_decision_context_score_batch(open[c], &b.batch, &r) == GEISTR_OK &&
                    same(&r, c, &forward[i]));
            const geistr_decision_request request = {.size = sizeof request,
                                                     .operation = GEISTR_OPERATION_DECISION,
                                                     .question_len = b.q[0].question_len,
                                                     .context_len = strlen(f->context),
                                                     .n_options = b.q[0].n_options,
                                                     .question = b.q[0].question,
                                                     .context = f->context,
                                                     .options = b.q[0].options};
            geistr_decision_result full = {.size = sizeof full};
            REQUIRE(geistr_decision_score(d, &request, &full) == GEISTR_OK);
            float delta = 0;
            for (size_t k = 0; k < full.n_options; ++k) {
                const float diff = fabsf(full.logits[k] - batched[c][i][k]);
                delta = diff > delta ? diff : delta;
            }
            printf("{\"profile\":\"%s\",\"backend\":\"%s\",\"context\":%zu,\"question\":%zu,"
                   "\"full_prompt_max_abs_delta\":%.9g,\"same_choice\":%s}\n",
                   profile_name, backend, c, i, (double)delta,
                   full.best_index == r.answers[0].best_index ? "true" : "false");
            /* The cut sits on a prefill-chunk boundary: the same passes, the same bits. */
            REQUIRE(delta == 0.0f && full.best_index == r.answers[0].best_index);
        }
        REQUIRE(geistr_chat_length(chat) == history);
    }
    geistr_decision_close(d);
    /* Failure in the middle and cancellation: nothing published, then recovery. */
    struct batch b;
    geistr_decision_batch_result r = {.size = sizeof r};
    build(&b, &fixtures[4], 3, forward);
    geistr_decision_option twin[4];
    memcpy(twin, b.options[1], sizeof twin);
    twin[1].id = twin[0].id;
    twin[1].id_len = twin[0].id_len;
    b.q[1].options = twin;
    REQUIRE(geistr_decision_context_score_batch(open[4], &b.batch, &r) == GEISTR_INVALID && !r.n_answers &&
            !r.answers && r.failed_index == 1 && !strcmp(r.failed_id, "q1") && !r.model_calls);
    b.q[1].options = b.options[1];
    REQUIRE(geistr_decision_context_cancel(open[4]) == GEISTR_OK && geistr_decision_context_cancel(open[4]) == GEISTR_OK);
    REQUIRE(geistr_decision_context_score_batch(open[4], &b.batch, &r) == GEISTR_CANCELLED && !r.n_answers &&
            !r.model_calls);
    REQUIRE(geistr_decision_context_score_batch(open[4], &b.batch, &r) == GEISTR_OK && same(&r, 4, forward));
    for (size_t c = 1; c < 5; ++c)
        geistr_decision_context_close(open[c]);
    geistr_decision_resources resources = {.size = sizeof resources};
    REQUIRE(geistr_decision_resources_get(m, &resources) == GEISTR_OK && resources.live_allocations == 2);
    *kept = open[0];
    return 0;
}

static int run(const char *path, const char *profile_name, const char *processor) {
    const bool gemma = !strcmp(profile_name, "gemma4");
    if (!gemma && strcmp(profile_name, "bonsai2"))
        return 2;
    const bool gpu = !strcmp(processor, "gpu");
    if (!gpu && strcmp(processor, "cpu"))
        return 2;
    geistr_decision_policy p = {.enabled = true,
                                .mode = GEISTR_DECISION_DENSE,
                                .profile =
                                    gemma ? GEISTR_DECISION_GEMMA4_E2B_Q4 : GEISTR_DECISION_BONSAI2_27B_PQ2};
    memcpy(p.sha256, geistr_decision_profile_sha256(p.profile), sizeof p.sha256);
    geistr_model_opts mo = GEISTR_MODEL_OPTS_INIT;
    mo.decision = &p;
    mo.processor = gpu ? GEISTR_PROCESSOR_GPU : GEISTR_PROCESSOR_CPU;
    mo.context = 512;
    mo.threads = 6;
    geistr_model *m = nullptr;
    char message[256];
    geistr_status opened = geistr_model_open(path, &mo, &m, message, sizeof message);
    if (opened != GEISTR_OK) {
        fprintf(stderr, "open: %s\n", message);
        return 1;
    }
    p.enabled = false; /* the loaded model's permission is its own snapshot */
    geistr_decision_capability cap = {.size = sizeof cap};
    REQUIRE(geistr_decision_capability_get(m, &cap) == GEISTR_OK && cap.configured);
    REQUIRE(cap.available && cap.dense);
    const struct native_case *fixtures = gemma ? gemma4_cases : bonsai2_cases;
    const size_t n = 3;
    geistr_chat_opts co = GEISTR_CHAT_OPTS_INIT;
    co.max_tokens = 1;
    geistr_chat *chat = nullptr;
    REQUIRE(geistr_chat_open(m, &co, &chat) == GEISTR_OK);
    const geistr_message input = {"user", "Reply with OK."};
    REQUIRE(geistr_chat_send(chat, 1, &input) == GEISTR_OK);
    const size_t history = geistr_chat_length(chat);
    for (int numeric = 1; numeric <= 2; ++numeric) {
        geistr_decision_opts opts = GEISTR_DECISION_OPTS_INIT;
        opts.mode = (geistr_decision_mode)numeric;
        geistr_decision *d = nullptr;
        if (numeric == 2 && !cap.selected_rows) {
            REQUIRE(geistr_decision_open(m, sizeof message, &opts, &d, message) == GEISTR_FORMAT && !d);
            puts("{\"selected_rows\":\"unsupported_explicit_error\"}");
            continue;
        }
        REQUIRE(geistr_decision_open(m, sizeof message, &opts, &d, message) == GEISTR_OK);
        struct geist_decision *direct = nullptr;
        const struct geist_decision_opts engine_opts = {.mode = numeric == 1 ? GEIST_DECISION_DENSE
                                                                             : GEIST_DECISION_SELECTED_ROWS,
                                                        .max_prompt_tokens = 512,
                                                        .max_candidates = GEISTR_DECISION_MAX_OPTIONS,
                                                        .kv_mode = GEIST_KV_FP32}; /* as the runtime */
        engine_setup_lock(m);
        const enum geist_status created = geist_decision_create(m->m, m->be, &engine_opts, &direct);
        engine_setup_unlock(m);
        REQUIRE(created == GEIST_OK);
        for (size_t i = 0; i < n; ++i) {
            const struct native_case *f = &fixtures[i];
            geistr_decision_option options[4];
            for (size_t k = 0; k < f->n_options; ++k)
                options[k] =
                    (geistr_decision_option){strlen(f->options[k].id), strlen(f->options[k].description),
                                             f->options[k].id, f->options[k].description};
            geistr_decision_request request = {.size = sizeof request,
                                               .operation = GEISTR_OPERATION_DECISION,
                                               .question_len = strlen(f->question),
                                               .context_len = strlen(f->context),
                                               .n_options = f->n_options,
                                               .question = f->question,
                                               .context = f->context,
                                               .options = options};
            geistr_decision_result out = {.size = sizeof out};
            geistr_status s = geistr_decision_score(d, &request, &out);
            if (s != GEISTR_OK)
                fprintf(stderr, "score: %s\n", geistr_decision_error(d));
            REQUIRE(s == GEISTR_OK && out.n_options == f->n_options);
            geistr_decision_plan plan = {.size = sizeof plan};
            REQUIRE(geistr_decision_plan_get(d, &plan) == GEISTR_OK && plan.n_prompt == f->n_ids);
            REQUIRE(!strcmp(plan.prompt, f->prompt));
            REQUIRE(!memcmp(plan.prompt_ids, f->ids, f->n_ids * sizeof *f->ids));
            REQUIRE(!memcmp(plan.candidate_ids, f->candidates, f->n_options * sizeof *f->candidates));
            struct geist_decision_result reference = {};
            engine_lock(m);
            const enum geist_status es =
                geist_decision_score(direct, f->n_ids, f->n_options, f->ids, f->candidates, &reference);
            engine_unlock(m);
            REQUIRE(es == GEIST_OK && reference.best_index == out.best_index);
            REQUIRE(!memcmp(reference.logits, out.logits, f->n_options * sizeof *reference.logits));
            REQUIRE(!memcmp(reference.probabilities, out.selection_probabilities,
                            f->n_options * sizeof *reference.probabilities));
            REQUIRE(!strcmp(out.external_id, f->options[out.best_index].id));
            REQUIRE(geistr_chat_length(chat) == history);
            printf("{\"profile\":\"%s\",\"backend\":\"%s\",\"mode\":%d,\"fixture\":%zu,\"native_ids\":true,"
                   "\"direct_bit_identical\":true,\"model_calls\":%zu}\n",
                   profile_name, cap.backend, numeric, i, out.model_calls);
            printf("{\"profile\":\"%s\",\"backend\":\"%s\",\"mode\":%d,\"fixture\":%zu,\"best_index\":%zu,"
                   "\"logits\":[",
                   profile_name, cap.backend, numeric, i, out.best_index);
            for (size_t k = 0; k < out.n_options; ++k)
                printf("%s%.9g", k ? "," : "", (double)out.logits[k]);
            puts("]}");
            if (!i) {
                options[1].id = options[0].id;
                options[1].id_len = options[0].id_len;
                REQUIRE(geistr_decision_score(d, &request, &out) == GEISTR_INVALID);
                REQUIRE(out.best_index == SIZE_MAX && !out.n_options && !out.logits && !out.external_id[0] &&
                        !out.model_calls);
                REQUIRE(geistr_decision_plan_get(d, &plan) == GEISTR_OK && !plan.n_prompt && !plan.prompt);
                options[1].id = f->options[1].id;
                options[1].id_len = strlen(options[1].id);
                REQUIRE(geistr_decision_cancel(d) == GEISTR_OK);
                REQUIRE(geistr_decision_cancel(d) == GEISTR_OK);
                REQUIRE(geistr_decision_score(d, &request, &out) == GEISTR_CANCELLED && !out.n_options &&
                        !out.model_calls);
                REQUIRE(geistr_decision_score(d, &request, &out) == GEISTR_OK);
                REQUIRE(geistr_decision_reset(d) == GEISTR_OK);
            }
        }
        engine_setup_lock(m);
        geist_decision_destroy(direct);
        engine_setup_unlock(m);
        geistr_decision_close(d);
        geistr_decision_resources resources = {.size = sizeof resources};
        REQUIRE(geistr_decision_resources_get(m, &resources) == GEISTR_OK);
        REQUIRE(!resources.live_allocations && !resources.live_bytes);
    }
    geistr_decision_context *kept = nullptr;
    REQUIRE(contexts(m, chat, history, gemma, profile_name, cap.backend, &kept) == 0);
    geistr_piece piece = {.size = sizeof piece};
    REQUIRE(geistr_chat_next(chat, &piece) == GEISTR_OK);
    geistr_model_close(m); /* chat and context retain the model even after caller release */
    if (kept) {
        static const size_t order[] = {0, 1, 2};
        struct batch b;
        geistr_decision_batch_result r = {.size = sizeof r};
        build(&b, gemma ? &gemma4_contexts[0] : &bonsai2_contexts[0], 3, order);
        REQUIRE(geistr_decision_context_score_batch(kept, &b.batch, &r) == GEISTR_OK && same(&r, 0, order));
        geistr_decision_context_close(kept);
    }
    geistr_chat_close(chat);
    return 0;
}
int main(int argc, char **argv) { return argc == 4 ? run(argv[1], argv[2], argv[3]) : 2; }

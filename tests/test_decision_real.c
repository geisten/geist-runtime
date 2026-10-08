/* White-box integration: identical loaded weights/backend for wrapper and
 * direct engine controls. Do not add an engine-handle escape to the public API. */
#include "../src/runtime.c"
#include "native_cases.h"
#include <stdio.h>
#include <string.h>

#define REQUIRE(x)                                                                                           \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (false)

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
                                                        .max_candidates = GEISTR_DECISION_MAX_OPTIONS};
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
    geistr_piece piece = {.size = sizeof piece};
    REQUIRE(geistr_chat_next(chat, &piece) == GEISTR_OK);
    geistr_model_close(m); /* chat retains the model even after caller release */
    geistr_chat_close(chat);
    return 0;
}
int main(int argc, char **argv) { return argc == 4 ? run(argv[1], argv[2], argv[3]) : 2; }

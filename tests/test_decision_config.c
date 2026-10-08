#include "geistr_catalog.h"
#include "geistr_decision.h"
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

static geistr_decision_config *parse(const char *s, geistr_status expected) {
    geistr_decision_config *c = nullptr;
    char error[128] = "sentinel";
    const geistr_status status = geistr_decision_config_parse(strlen(s), sizeof error, s, &c, error);
    CHECK(status == expected);
    CHECK(expected == GEISTR_OK ? c != nullptr && !error[0] : c == nullptr && error[0]);
    return c;
}

static void configuration(void) {
    geistr_decision_config *c = parse("{\"schema\":1,\"models\":[]}", GEISTR_OK);
    CHECK(geistr_decision_config_count(c) == 0);
    CHECK(!geistr_decision_config_find(geistr_decision_profile_sha256(GEISTR_DECISION_BONSAI2_27B_PQ2), c));
    geistr_decision_config_free(c);
    char json[1024];
    int n = snprintf(json, sizeof json,
                     "{\"models\":[{\"sha256\":\"%s\",\"enabled\":true,\"profile\":\"bonsai2-27b-pq2-v1\"},"
                     "{\"sha256\":\"%s\",\"mode\":\"selected_rows\"}],\"schema\":1}",
                     geistr_decision_profile_sha256(GEISTR_DECISION_BONSAI2_27B_PQ2),
                     geistr_decision_profile_sha256(GEISTR_DECISION_GEMMA4_E2B_Q4));
    CHECK(n > 0 && (size_t)n < sizeof json);
    c = parse(json, GEISTR_OK);
    CHECK(geistr_decision_config_count(c) == 2);
    const geistr_decision_policy *p = geistr_decision_config_get(0, c);
    CHECK(p && p->enabled && p->mode == GEISTR_DECISION_DENSE);
    CHECK(p && p->profile == GEISTR_DECISION_BONSAI2_27B_PQ2);
    p = geistr_decision_config_get(1, c);
    CHECK(p && !p->enabled && p->profile == GEISTR_DECISION_PROFILE_NONE);
    CHECK(p && p->mode == GEISTR_DECISION_SELECTED_ROWS);
    CHECK(!geistr_decision_config_get(2, c));
    memset(json, 'x', sizeof json);
    CHECK(!strcmp(geistr_decision_config_get(0, c)->sha256,
                  geistr_decision_profile_sha256(GEISTR_DECISION_BONSAI2_27B_PQ2)));
    geistr_decision_config_free(c);

    const char *bad[] = {"{}",
                         "[]",
                         "{\"schema\":2,\"models\":[]}",
                         "{\"schema\":1.0,\"models\":[]}",
                         "{\"schema\":1,\"schema\":1,\"models\":[]}",
                         "{\"schema\":1 \"models\":[]}",
                         "{\"schema\":1,\"models\":[],}",
                         "{\"schema\":1,\"models\":[]}{}",
                         "{\"schema\":1,\"models\":[],\"thinking\":false}",
                         "{\"schema\":1,\"models\":[{}]}",
                         "{\"schema\":1,\"models\":[null]}",
                         "{\"schema\":1,\"models\":[,]}",
                         "{\"schema\":1,\"models\":[{\"sha256\":\"short\"}]}",
                         "{\"sche\\u006da\":1,\"models\":[]}",
                         "{\"schema\":1,\"models\":false}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i)
        geistr_decision_config_free(parse(bad[i], GEISTR_FORMAT));
    const char *variants[] = {",\"profile\":\"gemma4-e2b-q4-v1\",\"enabled\":true",
                              ",\"enabled\":true",
                              ",\"enabled\":truefalse",
                              ",\"enabled\":1",
                              ",\"mode\":\"auto\"",
                              ",\"profile\":\"unknown\"",
                              ",\"enabled\":false,\"enabled\":true"};
    for (size_t i = 0; i < sizeof variants / sizeof variants[0]; ++i) {
        n = snprintf(json, sizeof json, "{\"schema\":1,\"models\":[{\"sha256\":\"%s\"%s}]}",
                     geistr_decision_profile_sha256(GEISTR_DECISION_BONSAI2_27B_PQ2), variants[i]);
        CHECK(n > 0 && (size_t)n < sizeof json);
        geistr_decision_config_free(parse(json, GEISTR_FORMAT));
    }
    n = snprintf(json, sizeof json, "{\"schema\":1,\"models\":[{\"sha256\":\"%s\"},{\"sha256\":\"%s\"}]}",
                 geistr_decision_profile_sha256(GEISTR_DECISION_BONSAI2_27B_PQ2),
                 geistr_decision_profile_sha256(GEISTR_DECISION_BONSAI2_27B_PQ2));
    CHECK(n > 0 && (size_t)n < sizeof json);
    geistr_decision_config_free(parse(json, GEISTR_FORMAT));
    const char *valid = "{\"schema\":1,\"models\":[]}";
    for (size_t i = 0; i < strlen(valid); ++i) {
        c = nullptr;
        CHECK(geistr_decision_config_parse(i, 0, valid, &c, nullptr) != GEISTR_OK && !c);
    }
    c = nullptr;
    CHECK(geistr_decision_config_parse(SIZE_MAX, 0, valid, &c, nullptr) == GEISTR_INVALID && !c);
    CHECK(geistr_decision_config_parse(strlen(valid), 0, valid, nullptr, nullptr) == GEISTR_INVALID);
    CHECK(geistr_decision_config_count(nullptr) == 0);
    geistr_decision_config_free(nullptr);
    char *bounded = malloc(GEISTR_DECISION_MAX_CONFIG + 1);
    CHECK(bounded != nullptr);
    if (bounded) {
        memcpy(bounded, valid, strlen(valid));
        memset(bounded + strlen(valid), ' ', GEISTR_DECISION_MAX_CONFIG + 1 - strlen(valid));
        for (size_t k = GEISTR_DECISION_MAX_CONFIG - 1; k <= GEISTR_DECISION_MAX_CONFIG + 1; ++k) {
            c = nullptr;
            CHECK(geistr_decision_config_parse(k, 0, bounded, &c, nullptr) ==
                  (k > GEISTR_DECISION_MAX_CONFIG ? GEISTR_INVALID : GEISTR_OK));
            geistr_decision_config_free(c);
        }
        free(bounded);
    }
}

static void requests(void) {
    geistr_decision_option options[] = {{1, 3, "x", "yes"}, {1, 2, "y", "no"}};
    geistr_decision_request r = {.size = sizeof r,
                                 .operation = GEISTR_OPERATION_DECISION,
                                 .question_len = 2,
                                 .n_options = 2,
                                 .question = "Q?",
                                 .options = options};
    CHECK(geistr_decision_request_validate(0, &r, nullptr) == GEISTR_OK);
    r.operation = GEISTR_OPERATION_CHAT;
    CHECK(geistr_decision_request_validate(0, &r, nullptr) == GEISTR_INVALID);
    r.operation = GEISTR_OPERATION_DECISION;
    options[1].id = "x";
    CHECK(geistr_decision_request_validate(0, &r, nullptr) == GEISTR_INVALID);
    options[1].id = "y";
    const char *bad[] = {"\n",           "\r",           "\x1b",     "\x7f",         "\xc2\x85",
                         "\xe2\x80\xa8", "\xe2\x80\xa9", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
                         "\xe2\x80"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        options[0].id = bad[i];
        options[0].id_len = strlen(bad[i]);
        CHECK(geistr_decision_request_validate(0, &r, nullptr) == GEISTR_INVALID);
    }
    const char nul[] = {'x', 0, 'y'};
    options[0].id = nul;
    options[0].id_len = sizeof nul;
    CHECK(geistr_decision_request_validate(0, &r, nullptr) == GEISTR_INVALID);
    options[0].id = "ä😀";
    options[0].id_len = strlen(options[0].id);
    CHECK(geistr_decision_request_validate(0, &r, nullptr) == GEISTR_OK);
    options[0].id = "x";
    options[0].id_len = 1;
    r.context_len = 1;
    r.context = nullptr;
    CHECK(geistr_decision_request_validate(0, &r, nullptr) == GEISTR_INVALID);
    r.context_len = 0;
    r.question_len = SIZE_MAX;
    CHECK(geistr_decision_request_validate(0, &r, nullptr) == GEISTR_INVALID);
    char *large = malloc(GEISTR_DECISION_MAX_QUESTION + 1);
    CHECK(large != nullptr);
    if (!large)
        return;
    memset(large, 'q', GEISTR_DECISION_MAX_QUESTION + 1);
    r.question = large;
    for (size_t n = GEISTR_DECISION_MAX_QUESTION - 1; n <= GEISTR_DECISION_MAX_QUESTION + 1; ++n) {
        r.question_len = n;
        CHECK(geistr_decision_request_validate(0, &r, nullptr) ==
              (n > GEISTR_DECISION_MAX_QUESTION ? GEISTR_INVALID : GEISTR_OK));
    }
    r.question = "Q?";
    r.question_len = 2;
    options[0].id = large;
    for (size_t n = GEISTR_DECISION_MAX_ID - 1; n <= GEISTR_DECISION_MAX_ID + 1; ++n) {
        options[0].id_len = n;
        CHECK(geistr_decision_request_validate(0, &r, nullptr) ==
              (n > GEISTR_DECISION_MAX_ID ? GEISTR_INVALID : GEISTR_OK));
    }
    options[0].id = "x";
    options[0].id_len = 1;
    options[0].description = large;
    for (size_t n = GEISTR_DECISION_MAX_DESCRIPTION - 1; n <= GEISTR_DECISION_MAX_DESCRIPTION + 1; ++n) {
        options[0].description_len = n;
        CHECK(geistr_decision_request_validate(0, &r, nullptr) ==
              (n > GEISTR_DECISION_MAX_DESCRIPTION ? GEISTR_INVALID : GEISTR_OK));
    }
    options[0].description = "yes";
    options[0].description_len = 3;
    r.n_options = SIZE_MAX;
    CHECK(geistr_decision_request_validate(0, &r, nullptr) == GEISTR_INVALID);
    geistr_decision_option many[GEISTR_DECISION_MAX_OPTIONS];
    char ids[GEISTR_DECISION_MAX_OPTIONS];
    for (size_t i = 0; i < GEISTR_DECISION_MAX_OPTIONS; ++i) {
        ids[i] = (char)('A' + i);
        many[i] = (geistr_decision_option){1, 1, &ids[i], "a"};
    }
    r.options = many;
    for (size_t n = GEISTR_DECISION_MAX_OPTIONS - 1; n <= GEISTR_DECISION_MAX_OPTIONS + 1; ++n) {
        r.n_options = n;
        CHECK(geistr_decision_request_validate(0, &r, nullptr) ==
              (n > GEISTR_DECISION_MAX_OPTIONS ? GEISTR_INVALID : GEISTR_OK));
    }
    r.n_options = 8;
    r.question = r.context = large;
    r.question_len = r.context_len = GEISTR_DECISION_MAX_CONTEXT;
    for (size_t i = 0; i < 8; ++i) {
        many[i].description = large;
        many[i].description_len = GEISTR_DECISION_MAX_DESCRIPTION;
    }
    many[7].description_len -= 8;
    CHECK(geistr_decision_request_validate(0, &r, nullptr) == GEISTR_OK);
    ++many[7].description_len;
    CHECK(geistr_decision_request_validate(0, &r, nullptr) == GEISTR_INVALID);
    free(large);
}

static void abi_and_hash(void) {
    char sum[65] = "sentinel";
    CHECK(geistr_sha256_memory(3, "abc", sum) == GEISTR_OK);
    CHECK(!strcmp(sum, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(geistr_sha256_memory(0, nullptr, sum) == GEISTR_OK);
    CHECK(!strcmp(sum, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(geistr_sha256_memory(1, nullptr, sum) == GEISTR_INVALID && !sum[0]);
    geistr_model_opts opts = GEISTR_MODEL_OPTS_INIT;
    opts.size = offsetof(geistr_model_opts, decision);
    geistr_model *model = nullptr;
    CHECK(geistr_model_open("stub:echo", &opts, &model, nullptr, 0) == GEISTR_OK && model);
    geistr_model_close(model);
    opts.size = sizeof opts - 1;
    CHECK(geistr_model_open("stub:echo", &opts, &model, nullptr, 0) == GEISTR_INVALID && !model);
    geistr_decision_policy policy = {.mode = GEISTR_DECISION_DENSE};
    memcpy(policy.sha256, geistr_decision_profile_sha256(GEISTR_DECISION_BONSAI2_27B_PQ2), 65);
    opts = (geistr_model_opts)GEISTR_MODEL_OPTS_INIT;
    opts.decision = &policy;
    CHECK(geistr_model_open("stub:echo", &opts, &model, nullptr, 0) == GEISTR_OK && model);
    policy.enabled = true;
    geistr_model_info info = {.size = sizeof info};
    CHECK(geistr_model_info_get(model, &info) == GEISTR_OK && !strcmp(info.arch, "stub"));
    geistr_decision_capability capability = {.size = sizeof capability};
    CHECK(geistr_decision_capability_get(model, &capability) == GEISTR_OK && !capability.configured &&
          !capability.available);
    CHECK(!geistr_decision_available());
    geistr_decision *decision = nullptr;
    CHECK(geistr_decision_open(model, 0, nullptr, &decision, nullptr) == GEISTR_FORMAT && !decision);
    geistr_decision_result result = {.size = sizeof result};
    CHECK(geistr_decision_score(nullptr, nullptr, &result) == GEISTR_INVALID);
    CHECK(result.best_index == SIZE_MAX && !result.n_options && !result.logits && !result.external_id[0]);
    geistr_model_close(model);
    char error[128];
    CHECK(geistr_model_open("stub:echo", &opts, &model, error, sizeof error) == GEISTR_INVALID && !model);
    policy.profile = GEISTR_DECISION_BONSAI2_27B_PQ2;
    CHECK(geistr_model_open("stub:echo", &opts, &model, error, sizeof error) == GEISTR_FORMAT && !model);
}

int main(void) {
    configuration();
    requests();
    abi_and_hash();
    if (failures)
        return 1;
    puts("decision contract: strict configuration, default-off, artifact binding and bounded UTF-8 requests "
         "passed");
    return 0;
}

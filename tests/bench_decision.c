/* Decision timing and numeric controls for the #17 evidence plan
 * (docs/DECISION_EVIDENCE_PLAN_17.json). White-box like test_decision_real.c:
 * the runtime wrapper and the direct engine share one loaded model.
 *
 *   bench_decision overhead MODEL PROFILE cpu|gpu      runtime (A) vs direct engine (B), AB/BA pairs
 *   bench_decision direct   MODEL PROFILE cpu|gpu N    N direct DENSE scorings after the warm-up
 *   bench_decision kvpair   MODEL PROFILE cpu|gpu      direct, kv AUTO (A) vs FP32 (B), AB/BA pairs
 *   bench_decision scalar   MODEL PROFILE              cpu_scalar + FP32 KV logits per fixture
 *
 * One JSON object per line on stdout; times in milliseconds. */
#include "../src/runtime.c"
#include "native_cases.h"
#include <stdio.h>
#include <string.h>

enum { WARMUP = 5, PAIRS = 30, CASES = 3 };

#define REQUIRE(x)                                                                                           \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (false)

static const struct native_case *cases_of(const char *profile) {
    return !strcmp(profile, "gemma4") ? gemma4_cases : bonsai2_cases;
}

static geistr_model *open_model(const char *path, const char *profile, bool gpu, double *ms) {
    const bool gemma = !strcmp(profile, "gemma4");
    REQUIRE(gemma || !strcmp(profile, "bonsai2"));
    geistr_decision_policy p = {.enabled = true,
                                .mode    = GEISTR_DECISION_DENSE,
                                .profile = gemma ? GEISTR_DECISION_GEMMA4_E2B_Q4 : GEISTR_DECISION_BONSAI2_27B_PQ2};
    memcpy(p.sha256, geistr_decision_profile_sha256(p.profile), sizeof p.sha256);
    geistr_model_opts mo = GEISTR_MODEL_OPTS_INIT;
    mo.decision          = &p;
    mo.processor         = gpu ? GEISTR_PROCESSOR_GPU : GEISTR_PROCESSOR_CPU;
    mo.context           = 512;
    mo.threads           = 6;
    geistr_model *m      = nullptr;
    char          message[256];
    const double  start = now_ms();
    if (geistr_model_open(path, &mo, &m, message, sizeof message) != GEISTR_OK) {
        fprintf(stderr, "open: %s\n", message);
        exit(1);
    }
    *ms = now_ms() - start;
    return m;
}

static struct geist_decision *direct_open(geistr_model *m, enum geist_kv_mode kv) {
    const struct geist_decision_opts o = {.mode              = GEIST_DECISION_DENSE,
                                          .max_prompt_tokens = 512,
                                          .max_candidates    = GEISTR_DECISION_MAX_OPTIONS,
                                          .kv_mode           = kv};
    struct geist_decision *d = nullptr;
    engine_setup_lock(m);
    const enum geist_status s = geist_decision_create(m->m, m->be, &o, &d);
    engine_setup_unlock(m);
    REQUIRE(s == GEIST_OK && d);
    return d;
}

/* B's phase, bounded as the runtime's scoring_ms: lock, score, copy, unlock. */
static double direct_score(geistr_model *m, struct geist_decision *d, const struct native_case *f, float *logits) {
    struct geist_decision_result r     = {};
    const double                 start = now_ms();
    engine_lock(m);
    const enum geist_status s = geist_decision_score(d, f->n_ids, f->n_options, f->ids, f->candidates, &r);
    for (size_t i = 0; s == GEIST_OK && i < f->n_options; i++)
        logits[i] = r.logits[i];
    engine_unlock(m);
    const double ms = now_ms() - start;
    REQUIRE(s == GEIST_OK && r.n_candidates == f->n_options);
    return ms;
}

static double wrapper_score(geistr_decision *d, const struct native_case *f, double *preparation, double *total) {
    geistr_decision_option options[4];
    for (size_t k = 0; k < f->n_options; ++k)
        options[k] = (geistr_decision_option) {strlen(f->options[k].id), strlen(f->options[k].description),
                                               f->options[k].id, f->options[k].description};
    geistr_decision_request request = {.size         = sizeof request,
                                       .operation    = GEISTR_OPERATION_DECISION,
                                       .question_len = strlen(f->question),
                                       .context_len  = strlen(f->context),
                                       .n_options    = f->n_options,
                                       .question     = f->question,
                                       .context      = f->context,
                                       .options      = options};
    geistr_decision_result out   = {.size = sizeof out};
    const double           start = now_ms();
    REQUIRE(geistr_decision_score(d, &request, &out) == GEISTR_OK && out.n_options == f->n_options);
    *total       = now_ms() - start;
    *preparation = out.preparation_ms;
    return out.scoring_ms;
}

static void header(const char *task, const char *profile, geistr_model *m, double open_ms) {
    geistr_decision_capability cap = {.size = sizeof cap};
    REQUIRE(geistr_decision_capability_get(m, &cap) == GEISTR_OK && cap.available && cap.dense);
    double l[3] = {};
    getloadavg(l, 3);
    printf("{\"task\":\"%s\",\"profile\":\"%s\",\"backend\":\"%s\",\"engine\":\"%s\",\"model_open_ms\":%.3f,"
           "\"load1\":%.2f}\n",
           task, profile, cap.backend, GEISTR_ENGINE_REVISION, open_ms, l[0]);
}

static int overhead(const char *path, const char *profile, bool gpu) {
    double        open_ms;
    geistr_model *m = open_model(path, profile, gpu, &open_ms);
    header("overhead", profile, m, open_ms);
    geistr_decision_opts opts = GEISTR_DECISION_OPTS_INIT;
    opts.mode                 = GEISTR_DECISION_DENSE;
    geistr_decision *w        = nullptr;
    char             message[256];
    double           start = now_ms();
    REQUIRE(geistr_decision_open(m, sizeof message, &opts, &w, message) == GEISTR_OK);
    const double           wrapper_open = now_ms() - start;
    start                               = now_ms();
    struct geist_decision *direct       = direct_open(m, GEIST_KV_FP32); /* as the runtime (#17 p2) */
    const double           direct_open_ms = now_ms() - start;
    printf("{\"setup\":{\"wrapper_open_ms\":%.3f,\"direct_open_ms\":%.3f}}\n", wrapper_open, direct_open_ms);
    opts.mode               = GEISTR_DECISION_SELECTED_ROWS;
    geistr_decision *rows   = nullptr;
    const geistr_status sel = geistr_decision_open(m, sizeof message, &opts, &rows, message);
    printf("{\"selected_rows\":\"%s\"}\n", sel == GEISTR_OK ? "supported" : "not_applicable_unsupported");
    geistr_decision_close(rows);
    const struct native_case *f = cases_of(profile);
    float                     logits[4];
    for (int i = 0; i < WARMUP + PAIRS; i++) {
        const struct native_case *c = &f[i % CASES];
        const bool                ab = i % 2 == 0;
        double                    a, b, prep, total;
        if (ab)
            a = wrapper_score(w, c, &prep, &total), b = direct_score(m, direct, c, logits);
        else
            b = direct_score(m, direct, c, logits), a = wrapper_score(w, c, &prep, &total);
        printf("{\"pair\":%d,\"warmup\":%s,\"case\":%d,\"order\":\"%s\",\"a_scoring_ms\":%.6f,\"b_scoring_ms\":%.6f,"
               "\"a_preparation_ms\":%.6f,\"a_total_ms\":%.6f}\n",
               i, i < WARMUP ? "true" : "false", i % CASES, ab ? "AB" : "BA", a, b, prep, total);
    }
    engine_setup_lock(m);
    geist_decision_destroy(direct);
    engine_setup_unlock(m);
    geistr_decision_close(w);
    geistr_model_close(m);
    return 0;
}

static int direct_block(const char *path, const char *profile, bool gpu, int n) {
    double        open_ms;
    geistr_model *m = open_model(path, profile, gpu, &open_ms);
    header("direct", profile, m, open_ms);
    struct geist_decision    *d = direct_open(m, GEIST_KV_AUTO);
    const struct native_case *f = cases_of(profile);
    float                     logits[4];
    for (int i = 0; i < WARMUP + n; i++) {
        const double ms = direct_score(m, d, &f[i % CASES], logits);
        printf("{\"i\":%d,\"warmup\":%s,\"case\":%d,\"scoring_ms\":%.6f}\n", i, i < WARMUP ? "true" : "false",
               i % CASES, ms);
    }
    engine_setup_lock(m);
    geist_decision_destroy(d);
    engine_setup_unlock(m);
    geistr_model_close(m);
    return 0;
}

static int kvpair(const char *path, const char *profile, bool gpu) {
    double        open_ms;
    geistr_model *m = open_model(path, profile, gpu, &open_ms);
    header("kvpair", profile, m, open_ms);
    struct geist_decision    *autod = direct_open(m, GEIST_KV_AUTO), *fp32 = direct_open(m, GEIST_KV_FP32);
    const struct native_case *f     = cases_of(profile);
    float                     la[4], lb[4];
    for (int i = 0; i < WARMUP + PAIRS; i++) {
        const struct native_case *c  = &f[i % CASES];
        const bool                ab = i % 2 == 0;
        double                    a, b;
        if (ab)
            a = direct_score(m, autod, c, la), b = direct_score(m, fp32, c, lb);
        else
            b = direct_score(m, fp32, c, lb), a = direct_score(m, autod, c, la);
        printf("{\"pair\":%d,\"warmup\":%s,\"case\":%d,\"order\":\"%s\",\"a_scoring_ms\":%.6f,\"b_scoring_ms\":%.6f}\n",
               i, i < WARMUP ? "true" : "false", i % CASES, ab ? "AB" : "BA", a, b);
    }
    engine_setup_lock(m);
    geist_decision_destroy(autod);
    geist_decision_destroy(fp32);
    engine_setup_unlock(m);
    geistr_model_close(m);
    return 0;
}

/* Full-precision control: the engine alone on cpu_scalar with FP32 KV. */
static int scalar(const char *path, const char *profile) {
    struct geist_backend           *be = nullptr;
    struct geist_model             *gm = nullptr;
    const struct geist_backend_opts bo = {.max_threads = 6};
    REQUIRE(geist_backend_create("cpu_scalar", &bo, nullptr, &be) == GEIST_OK && be);
    const struct geist_session_opts lo = {.max_seq_len = 512, .top_p = 1.0f};
    REQUIRE(geist_model_load_with_opts(path, be, &lo, &gm) == GEIST_OK && gm);
    const struct geist_decision_opts o = {.mode              = GEIST_DECISION_DENSE,
                                          .max_prompt_tokens = 512,
                                          .max_candidates    = GEISTR_DECISION_MAX_OPTIONS,
                                          .kv_mode           = GEIST_KV_FP32};
    struct geist_decision *d = nullptr;
    REQUIRE(geist_decision_create(gm, be, &o, &d) == GEIST_OK && d);
    const struct native_case *f = cases_of(profile);
    for (int i = 0; i < CASES; i++) {
        struct geist_decision_result r = {};
        REQUIRE(geist_decision_score(d, f[i].n_ids, f[i].n_options, f[i].ids, f[i].candidates, &r) == GEIST_OK);
        printf("{\"task\":\"scalar\",\"profile\":\"%s\",\"backend\":\"cpu_scalar\",\"kv\":\"fp32\",\"engine\":\"%s\","
               "\"fixture\":%d,\"best_index\":%zu,\"logits\":[",
               profile, GEISTR_ENGINE_REVISION, i, r.best_index);
        for (size_t k = 0; k < r.n_candidates; k++)
            printf("%s%.9g", k ? "," : "", (double) r.logits[k]);
        puts("]}");
    }
    geist_decision_destroy(d);
    geist_model_destroy(gm);
    geist_backend_destroy(be);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "scalar"))
        return scalar(argv[2], argv[3]);
    if (argc < 5)
        return 2;
    const bool gpu = !strcmp(argv[4], "gpu");
    if (!strcmp(argv[1], "overhead"))
        return overhead(argv[2], argv[3], gpu);
    if (!strcmp(argv[1], "kvpair"))
        return kvpair(argv[2], argv[3], gpu);
    if (!strcmp(argv[1], "direct") && argc == 6)
        return direct_block(argv[2], argv[3], gpu, atoi(argv[5]));
    return 2;
}

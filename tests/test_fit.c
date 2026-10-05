/* test_fit.c — device probe, resource fit, verdicts and ranking (#6).
 * The fixtures are geist-serve's (tests/app/core_test.c, tasks_test.c);
 * tools/parity/fit.c compares against its code on random scenarios. */
#include "geistr_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

#define GIB UINT64_C(1073741824)

static geistr_catalog *load(const char *file) {
    FILE *f = fopen(file, "r");
    if (!f)
        return nullptr;
    static char text[1 << 16];
    size_t      n = fread(text, 1, sizeof text, f);
    fclose(f);
    geistr_catalog *c = nullptr;
    CHECK(geistr_catalog_parse(text, n, &c, nullptr, 0) == GEISTR_OK);
    return c;
}

/* A catalog of models "m0".."m<n-1>" of the given sizes and pass counts
 * (total 0: no evidence; totals even, split over DE and EN). */
static geistr_catalog *synthetic(size_t n, const uint64_t *bytes, const unsigned *passed, const unsigned *total) {
    static char json[16384];
    int         at = snprintf(json, sizeof json, "{\"schema\":2,\"revision\":1,\"models\":[");
    for (size_t i = 0; i < n; i++) {
        char quality[256] = "";
        if (total[i])
            snprintf(quality, sizeof quality,
                     ",\"quality\":{\"suite\":\"0123456789ab\",\"date\":\"2026-10-03\",\"engine\":\"e\",\"evidence\":"
                     "\"%064d\",\"tasks\":{\"classify\":{\"de\":[%u,%u],\"en\":[%u,%u]}}}",
                     0, (passed[i] + 1) / 2, total[i] / 2, passed[i] / 2, total[i] / 2);
        at += snprintf(json + at, sizeof json - (size_t) at,
                       "%s{\"id\":\"m%zu\",\"name\":\"M%zu\",\"file\":\"m%zu.gguf\",\"url\":\"https://huggingface.co/a/"
                       "b/resolve/main/m%zu.gguf\",\"sha256\":\"%064zu\",\"bytes\":%llu,\"working_mib\":100,"
                       "\"recommended_ram_gib\":1,\"backends\":[\"cpu\",\"metal\"],\"group_id\":\"g%zu\","
                       "\"group_name\":\"G%zu\",\"quantization\":\"Q4_0\"%s}",
                       i ? "," : "", i, i, i, i, i, (unsigned long long) bytes[i], i, i, quality);
    }
    snprintf(json + at, sizeof json - (size_t) at, "]}");
    geistr_catalog *c = nullptr;
    char            error[128];
    if (geistr_catalog_parse(json, strlen(json), &c, error, sizeof error) != GEISTR_OK)
        fprintf(stderr, "synthetic: %s\n", error);
    return c;
}

static const geistr_fit *fit_of(const geistr_ranking *r, const char *id) {
    for (size_t i = 0; i < geistr_ranking_count(r); i++)
        if (!strcmp(geistr_ranking_get(r, i)->entry->id, id))
            return geistr_ranking_get(r, i);
    return nullptr;
}

/* Resource fit of one model, nothing installed or measured unless given. */
static geistr_resource resource(const geistr_catalog *c, const geistr_device *d, const char *id, bool installed) {
    size_t       n = geistr_catalog_count(c);
    geistr_local local[16];
    for (size_t i = 0; i < n; i++)
        local[i] = (geistr_local) {.size = sizeof *local, .installed = installed};
    geistr_ranking *r = nullptr;
    CHECK(geistr_rank(c, d, local, nullptr, &r) == GEISTR_OK);
    geistr_resource out = fit_of(r, id)->resource;
    geistr_ranking_free(r);
    return out;
}

static void assess(const geistr_catalog *c) {
    geistr_device h = {.size = sizeof h, .supported = true, .ram = 4 * GIB, .available = 3 * GIB,
                       .available_known = true, .disk_known = true, .disk = 20 * GIB, .cores = 4,
                       .kind = GEISTR_DEVICE_PI5};
    CHECK(resource(c, &h, "bitnet-2b", false) == GEISTR_RESOURCE_FITS);
    CHECK(resource(c, &h, "gemma4-e2b", false) == GEISTR_RESOURCE_LIMITED);
    h.available = GIB;
    CHECK(resource(c, &h, "bitnet-2b", false) == GEISTR_RESOURCE_LIMITED);
    h.disk = 10;
    CHECK(resource(c, &h, "bitnet-2b", false) == GEISTR_RESOURCE_UNAVAILABLE);
    CHECK(resource(c, &h, "bitnet-2b", true) == GEISTR_RESOURCE_LIMITED);
    h.disk_known = h.available_known = false;
    h.kind                           = GEISTR_DEVICE_OTHER;
    /* Unmeasured is not limited: a fast, reliable model on Linux is good. */
    CHECK(resource(c, &h, "bitnet-2b", false) == GEISTR_RESOURCE_FITS);
    h.ram = GIB;
    CHECK(resource(c, &h, "bitnet-2b", false) == GEISTR_RESOURCE_UNAVAILABLE);
    h.ram  = 16 * GIB;
    h.kind = GEISTR_DEVICE_APPLE_SILICON;
    CHECK(resource(c, &h, "gemma4-e2b", false) == GEISTR_RESOURCE_FITS);
    h.supported = false;
    CHECK(resource(c, &h, "gemma4-e2b", true) == GEISTR_RESOURCE_UNAVAILABLE);
}

/* One model with the given resource, seconds per answer (< 0 unmeasured)
 * and pass counts: its verdict and reason. */
static geistr_verdict verdict(geistr_resource res, double seconds, unsigned passed, unsigned total,
                              const char **reason) {
    uint64_t        bytes = GIB;
    geistr_catalog *c     = synthetic(1, &bytes, &passed, &total);
    geistr_device   d     = {.size = sizeof d, .supported = true, .ram = 64 * GIB, .cores = 8,
                             .available_known = res == GEISTR_RESOURCE_LIMITED, .available = 1};
    if (res == GEISTR_RESOURCE_UNAVAILABLE)
        d.supported = false;
    geistr_local local = {.size = sizeof local, .installed = true, .cpu = {seconds > 0 ? 200 / seconds : 0, 0}};
    geistr_ranking *r  = nullptr;
    CHECK(geistr_rank(c, &d, &local, nullptr, &r) == GEISTR_OK);
    const geistr_fit *f   = geistr_ranking_get(r, 0);
    geistr_verdict    out = f->verdict;
    *reason               = f->reason;
    CHECK(f->resource == res);
    geistr_ranking_free(r);
    geistr_catalog_free(c);
    return out;
}

static void verdicts(void) {
    struct {
        geistr_resource res;
        double          seconds;
        unsigned        passed, total;
        geistr_verdict  verdict;
        const char     *reason;
    } cases[] = {
            {GEISTR_RESOURCE_FITS, 3, 146, 160, GEISTR_VERDICT_GOOD, "good"},
            {GEISTR_RESOURCE_FITS, 10, 144, 160, GEISTR_VERDICT_GOOD, "good"}, /* both limits inclusive */
            {GEISTR_RESOURCE_FITS, 3, 143, 160, GEISTR_VERDICT_NOT_RECOMMENDED, "unreliable"},
            {GEISTR_RESOURCE_FITS, 31, 160, 160, GEISTR_VERDICT_NOT_RECOMMENDED, "too_slow"},
            {GEISTR_RESOURCE_FITS, 30, 160, 160, GEISTR_VERDICT_USABLE, "slow"},
            {GEISTR_RESOURCE_LIMITED, 3, 160, 160, GEISTR_VERDICT_USABLE, "tight_memory"},
            {GEISTR_RESOURCE_UNAVAILABLE, 3, 160, 160, GEISTR_VERDICT_NOT_RECOMMENDED, "unavailable"},
            {GEISTR_RESOURCE_FITS, -1, 160, 160, GEISTR_VERDICT_UNKNOWN, "speed_unknown"},
            {GEISTR_RESOURCE_FITS, 3, 0, 0, GEISTR_VERDICT_UNKNOWN, "quality_unknown"},
            {GEISTR_RESOURCE_FITS, -1, 31, 160, GEISTR_VERDICT_NOT_RECOMMENDED, "unreliable"},
            {GEISTR_RESOURCE_FITS, 99, 0, 0, GEISTR_VERDICT_NOT_RECOMMENDED, "too_slow"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const char *reason = "";
        CHECK(verdict(cases[i].res, cases[i].seconds, cases[i].passed, cases[i].total, &reason) == cases[i].verdict);
        if (strcmp(reason, cases[i].reason))
            fprintf(stderr, "case %zu: %s, expected %s\n", i, reason, cases[i].reason), failures++;
    }
}

/* Estimates scale the measured throughput by file size, and an estimate
 * alone never rules a model out (#133). */
static void estimates(void) {
    uint64_t        bytes[]  = {4000000000, 1000000000, 2000000000, 2000000000};
    unsigned        passed[] = {146, 146, 146, 146}, total[] = {160, 160, 160, 160};
    geistr_catalog *c        = synthetic(4, bytes, passed, total);
    geistr_device   d = {.size = sizeof d, .supported = true, .ram = 64 * GIB, .cores = 8, .kind = GEISTR_DEVICE_APPLE_SILICON};
    geistr_local    local[4] = {{.size = sizeof *local, .cpu = {20, .5}},
                                {.size = sizeof *local, .cpu = {40, .3}},
                                {.size = sizeof *local, .cpu = {0, 9}},
                                {.size = sizeof *local}};
    geistr_ranking *r        = nullptr;
    CHECK(geistr_rank(c, &d, local, nullptr, &r) == GEISTR_OK);
    /* median throughput (80e9 + 40e9) / 2 = 60e9 B/s → 30 tokens/s for 2 GB; first (.3 + .5) / 2 */
    const geistr_fit *e = fit_of(r, "m3");
    CHECK(e->basis == GEISTR_BASIS_ESTIMATED && e->estimated_from == 2 && e->processor == GEISTR_PROCESSOR_CPU);
    CHECK(e->seconds_cpu > .4 + 200 / 30. - 1e-9 && e->seconds_cpu < .4 + 200 / 30. + 1e-9 && e->seconds_gpu < 0);
    CHECK(fit_of(r, "m0")->basis == GEISTR_BASIS_MEASURED && fit_of(r, "m0")->seconds_cpu == 10.5);
    geistr_ranking_free(r);
    /* Too slow by estimate: "probably", unless quality already rules it out. */
    local[0].cpu = (geistr_speed) {4, 0}, local[1].cpu = (geistr_speed) {1, 0};
    CHECK(geistr_rank(c, &d, local, nullptr, &r) == GEISTR_OK);
    CHECK(fit_of(r, "m3")->verdict == GEISTR_VERDICT_UNKNOWN && !strcmp(fit_of(r, "m3")->reason, "probably_too_slow"));
    geistr_ranking_free(r);
    geistr_catalog_free(c);
    unsigned bad[] = {146, 146, 146, 31};
    c              = synthetic(4, bytes, bad, total);
    CHECK(geistr_rank(c, &d, local, nullptr, &r) == GEISTR_OK);
    CHECK(fit_of(r, "m3")->verdict == GEISTR_VERDICT_NOT_RECOMMENDED && !strcmp(fit_of(r, "m3")->reason, "unreliable"));
    geistr_ranking_free(r);
    geistr_catalog_free(c);
}

/* #122/#133: one recommendation and the suitability order. */
static void ranking(void) {
    geistr_device d = {.size = sizeof d, .supported = true, .ram = 64 * GIB, .cores = 8, .kind = GEISTR_DEVICE_APPLE_SILICON};
    uint64_t      bytes[] = {GIB, GIB, GIB, GIB};
    /* Gemma 4 E4B 147/160 at 29 s vs E2B 146/160 at 18 s: one answer is noise, speed decides;
     * 152/160 at 29 s is five points better and wins; m3 is unreliable. */
    unsigned        passed[] = {147, 146, 152, 100}, total[] = {160, 160, 160, 160};
    geistr_catalog *c        = synthetic(4, bytes, passed, total);
    geistr_local    local[4] = {{.size = sizeof *local, .cpu = {200 / 29., 0}},
                                {.size = sizeof *local, .cpu = {200 / 18., 0}},
                                {.size = sizeof *local, .cpu = {200 / 29., 0}},
                                {.size = sizeof *local, .installed = true, .cpu = {100, 0}}};
    geistr_ranking *r        = nullptr;
    CHECK(geistr_rank(c, &d, local, nullptr, &r) == GEISTR_OK);
    CHECK(!strcmp(geistr_ranking_get(r, 0)->entry->id, "m2") && !strcmp(geistr_ranking_get(r, 1)->entry->id, "m1") &&
          !strcmp(geistr_ranking_get(r, 2)->entry->id, "m0") && !strcmp(geistr_ranking_get(r, 3)->entry->id, "m3"));
    CHECK(geistr_ranking_best(r)->entry == geistr_ranking_get(r, 0)->entry);
    geistr_ranking_free(r);
    /* At the same verdict an installed model is the recommendation, but the order stays. */
    local[1].installed = true;
    CHECK(geistr_rank(c, &d, local, nullptr, &r) == GEISTR_OK);
    CHECK(!strcmp(geistr_ranking_best(r)->entry->id, "m1") && !strcmp(geistr_ranking_get(r, 0)->entry->id, "m2"));
    geistr_ranking_free(r);
    /* Only not-recommended models: no recommendation. */
    d.supported = false;
    CHECK(geistr_rank(c, &d, local, nullptr, &r) == GEISTR_OK && !geistr_ranking_best(r));
    geistr_ranking_free(r);
    geistr_catalog_free(c);
}

static void api(const geistr_catalog *c) {
    geistr_device   d = {};
    geistr_ranking *r = nullptr;
    CHECK(geistr_device_probe(".", &d) == GEISTR_OK && d.size == sizeof d && d.ram > 0 && d.cores > 0 && d.disk_known);
#if defined(__APPLE__) && defined(__aarch64__)
    CHECK(d.kind == GEISTR_DEVICE_APPLE_SILICON && d.gpu == GEISTR_BACKEND_METAL && d.supported);
#endif
    printf("device: %s, %s, %s, %.1f GiB RAM, %u cores%s\n", d.name, d.arch, d.os, (double) d.ram / GIB, d.cores,
           d.supported ? "" : ", engine not supported");
    /* nullptr local: nothing installed or measured. */
    CHECK(geistr_rank(c, &d, nullptr, nullptr, &r) == GEISTR_OK);
    CHECK(geistr_ranking_count(r) == geistr_catalog_count(c));
    for (size_t i = 0; i < geistr_ranking_count(r); i++)
        CHECK(geistr_ranking_get(r, i)->basis == GEISTR_BASIS_NONE && !geistr_ranking_get(r, i)->installed);
    geistr_ranking_free(r);
    /* An older, shorter geistr_local: the stride is its size, missing speeds are 0. */
    struct {
        size_t   size;
        bool     installed;
        uint64_t partial;
    } old[16];
    for (size_t i = 0; i < geistr_catalog_count(c); i++)
        old[i] = (typeof(old[0])) {.size = sizeof old[0], .installed = i == 1};
    CHECK(geistr_rank(c, &d, (const geistr_local *) old, nullptr, &r) == GEISTR_OK);
    CHECK(!geistr_ranking_get(r, 0)->installed || geistr_ranking_get(r, 0)->entry == geistr_catalog_get(c, 1));
    size_t installed = 0;
    for (size_t i = 0; i < geistr_ranking_count(r); i++)
        installed += geistr_ranking_get(r, i)->installed;
    CHECK(installed == 1);
    geistr_ranking_free(r);
    /* Options: validated; a task selects its own quality. */
    geistr_rank_opts bad = {.size = sizeof bad, .fast_s = 20, .usable_s = 10, .reliable = .9};
    CHECK(geistr_rank(c, &d, nullptr, &bad, &r) == GEISTR_INVALID && !r);
    bad = (geistr_rank_opts) {.size = sizeof bad + 8};
    CHECK(geistr_rank(c, &d, nullptr, &bad, &r) == GEISTR_INVALID);
    geistr_rank_opts task = {.size = sizeof task, .fast_s = 10, .usable_s = 30, .reliable = .9, .task = "classify"};
    CHECK(geistr_rank(c, &d, nullptr, &task, &r) == GEISTR_OK);
    uint32_t p, t, all_p, all_t;
    geistr_catalog_quality(c, fit_of(r, "gemma4-e2b")->entry, "classify", &p, &t);
    geistr_catalog_quality(c, fit_of(r, "gemma4-e2b")->entry, nullptr, &all_p, &all_t);
    CHECK(t && t < all_t && fit_of(r, "gemma4-e2b")->total == t && fit_of(r, "gemma4-e2b")->passed == p);
    geistr_catalog_quality(c, fit_of(r, "gemma4-e2b")->entry, "poems", &p, &t);
    CHECK(p == 0 && t == 0);
    geistr_ranking_free(r);
    CHECK(geistr_rank(nullptr, &d, nullptr, nullptr, &r) == GEISTR_INVALID);
    CHECK(!geistr_ranking_best(nullptr) && !geistr_ranking_count(nullptr) && !geistr_ranking_get(nullptr, 0));
}

int main(int argc, char **argv) {
    geistr_catalog *c = load(argc > 1 ? argv[1] : "models/catalog.json");
    CHECK(c);
    if (!c)
        return 1;
    assess(c);
    verdicts();
    estimates();
    ranking();
    api(c);
    geistr_catalog_free(c);
    if (failures)
        fprintf(stderr, "test_fit: %d failures\n", failures);
    else
        puts("fit: device, resource fit, verdicts, estimates, ranking, recommendation, options passed");
    return failures != 0;
}

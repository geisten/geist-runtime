/* Fit and ranking parity with geist-serve (#6): random devices, catalogs and
 * measurements, ranked by geistr_rank and by geist-serve's own app_assess,
 * app_judge, app_estimate_seconds and app_candidate_better in the loop of
 * src/app/status.c. Built by make parity against SERVE_DIR. */
#include "core.h"
#include "tasks.h"
#include "geistr_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* core.c links against the app's model table; the harness never uses it. */
struct app_model app_models[APP_MODEL_COUNT];
size_t           app_model_count;

static unsigned long long seed = 1;
static unsigned           rnd(unsigned n) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned) (seed >> 33) % n;
}
static bool chance(unsigned percent) {
    return rnd(100) < percent;
}

/* geist-serve's assessment sentences → the runtime's codes. */
static const char *code(const char *reason) {
    static const char *const map[][2] = {
            {"PQ2_0", "unsupported_format"},  {"instruction set", "platform"},
            {"disk space", "disk"},           {"smaller than the model", "ram"},
            {"Below the RAM", "ram_recommended"}, {"Available RAM is tight", "available_ram"},
            {"Fits", "fits"}, {"not measured yet.", "not_measured"}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (strstr(reason, map[i][0]))
            return map[i][1];
    return reason;
}

static int failures;
#define SAME(cond, what)                                                         \
    do {                                                                         \
        if (!(cond) && failures++ < 10)                                          \
            fprintf(stderr, "scenario %d model %zu: %s differs\n", s, i, what); \
    } while (0)

int main(void) {
    const unsigned long long sizes[] = {300000000ull, 1200000000ull, 2500000000ull, 5000000000ull, 17000000000ull};
    int                      scenarios = 20000, models_total = 0;
    for (int s = 0; s < scenarios; s++) {
        /* ---- a random catalog ---- */
        size_t n = 1 + rnd(8);
        char   json[16384];
        int    at = snprintf(json, sizeof json, "{\"schema\":2,\"revision\":1,\"models\":[");
        for (size_t i = 0; i < n; i++) {
            bool unsupported = chance(10);
            char quality[256] = "";
            if (chance(70)) {
                unsigned total = 1 + rnd(80), passed = rnd(total + 1), t2 = 1 + rnd(80), p2 = rnd(t2 + 1);
                snprintf(quality, sizeof quality,
                         ",\"quality\":{\"suite\":\"0123456789ab\",\"date\":\"2026-10-03\",\"engine\":\"e\","
                         "\"evidence\":\"%064d\",\"tasks\":{\"classify\":{\"de\":[%u,%u],\"en\":[%u,%u]}}}",
                         0, passed, total, p2, t2);
            }
            char id[16];
            if (i == 0 && chance(30))
                snprintf(id, sizeof id, "bitnet-2b");
            else
                snprintf(id, sizeof id, "m%zu", i);
            at += snprintf(json + at, sizeof json - (size_t) at,
                           "%s{\"id\":\"%s\",\"name\":\"M%zu\",\"file\":\"m%zu.gguf\","
                           "\"url\":\"https://huggingface.co/a/b/resolve/main/m%zu.gguf\",\"sha256\":\"%064zu\","
                           "\"bytes\":%llu,\"working_mib\":%u,\"recommended_ram_gib\":%u,\"backends\":%s,"
                           "\"group_id\":\"g%zu\",\"group_name\":\"G%zu\",\"quantization\":\"Q4_0\"%s%s}",
                           i ? "," : "", id, i, i, i, i,
                           sizes[rnd(5)] + rnd(1000), 100 + rnd(20000), 1 + rnd(64),
                           unsupported ? "[]" : chance(50) ? "[\"cpu\",\"metal\"]" : "[\"cpu\"]", i, i,
                           unsupported ? ",\"unsupported_format\":\"pq2_0\"" : "", quality);
        }
        snprintf(json + at, sizeof json - (size_t) at, "]}");
        geistr_catalog *catalog;
        char            error[128];
        if (geistr_catalog_parse(json, strlen(json), &catalog, error, sizeof error) != GEISTR_OK) {
            fprintf(stderr, "scenario %d: %s\n%s\n", s, error, json);
            return 1;
        }

        /* ---- a random device and local state ---- */
        geistr_device d = {.size = sizeof d};
        d.kind      = (geistr_device_kind) rnd(3);
        d.supported = chance(90);
        d.ram       = (uint64_t) (1 + rnd(64)) * APP_GIB - rnd(3) * 100000000ull;
        d.available_known = chance(80);
        d.available       = d.available_known ? (uint64_t) rnd(64) * APP_GIB : 0;
        d.disk_known      = chance(80);
        d.disk            = d.disk_known ? (uint64_t) rnd(40) * APP_GIB : 0;
        d.cores           = 2 + rnd(10);
        d.gpu             = chance(60) ? GEISTR_BACKEND_METAL : 0;
        geistr_local local[8];
        for (size_t i = 0; i < n; i++) {
            local[i] = (geistr_local) {.size = sizeof *local, .installed = chance(40)};
            local[i].partial  = chance(20) ? rnd(3000) * 1000000ull : 0;
            local[i].cpu.rate = chance(40) ? 1 + rnd(80) : 0;
            local[i].cpu.first = chance(80) ? rnd(30) / 10.0 : 0;
            local[i].gpu.rate = chance(40) ? 1 + rnd(120) : 0;
            local[i].gpu.first = chance(80) ? rnd(30) / 10.0 : 0;
        }
        geistr_rank_opts opts = {.size = sizeof opts, .fast_s = 5 + rnd(10), .usable_s = 15 + rnd(30),
                                 .reliable = chance(50) ? .9 : rnd(100) / 100.0};
        geistr_ranking  *ranking;
        if (geistr_rank(catalog, &d, local, &opts, &ranking) != GEISTR_OK) {
            fprintf(stderr, "scenario %d: geistr_rank failed\n", s);
            return 1;
        }

        /* ---- geist-serve: the loop of src/app/status.c ---- */
        struct app_model    models[8] = {};
        struct app_hardware h = {.device = d.kind == GEISTR_DEVICE_APPLE_SILICON ? APP_APPLE_SILICON
                                           : d.kind == GEISTR_DEVICE_PI5       ? APP_PI5
                                                                                : APP_UNKNOWN,
                                 .ram = d.ram, .available = d.available, .disk = d.disk, .cores = d.cores,
                                 .supported = d.supported, .available_known = d.available_known,
                                 .disk_known = d.disk_known};
        struct app_limits limits = {opts.fast_s, opts.usable_s, opts.reliable};
        double rates[2][8], msizes[2][8], firsts[2][8];
        unsigned measured[2] = {};
        for (size_t i = 0; i < n; i++) {
            const geistr_catalog_entry *e = geistr_catalog_get(catalog, i);
            models[i] = (struct app_model) {.id = e->id, .name = e->name, .file = e->file, .url = e->url,
                                            .sha256 = e->sha256, .bytes = e->bytes, .working_mib = e->working_mib,
                                            .recommended_ram_gib = e->recommended_ram_gib, .backends = e->backends,
                                            .unsupported_format = e->unsupported_format,
                                            .quality_passed = e->quality_passed, .quality_total = e->quality_total};
            rates[0][i] = local[i].cpu.rate, firsts[0][i] = local[i].cpu.first;
            rates[1][i] = local[i].gpu.rate, firsts[1][i] = local[i].gpu.first;
            msizes[0][i] = msizes[1][i] = (double) e->bytes;
            measured[0] += rates[0][i] > 0, measured[1] += rates[1][i] > 0;
        }
        struct app_candidate best_candidate = {}, ranked[8];
        size_t               order[8], ranked_count = 0;
        const struct app_model *best = nullptr;
        for (size_t i = 0; i < n; i++) {
            const struct app_model *m        = &models[i];
            struct app_hardware     adjusted = h;
            if (local[i].partial <= m->bytes && h.disk_known && UINT64_MAX - adjusted.disk > local[i].partial)
                adjusted.disk += local[i].partial;
            bool   gpu        = d.gpu && (m->backends & d.gpu);
            double seconds[2] = {app_answer_seconds(local[i].cpu.rate, local[i].cpu.first),
                                 gpu ? app_answer_seconds(local[i].gpu.rate, local[i].gpu.first) : -1};
            bool estimated = seconds[0] < 0 && seconds[1] < 0;
            if (estimated) {
                seconds[0] = app_estimate_seconds(m->bytes, rates[0], msizes[0], firsts[0], n);
                seconds[1] = gpu ? app_estimate_seconds(m->bytes, rates[1], msizes[1], firsts[1], n) : -1;
            }
            int fastest = seconds[1] >= 0 && (seconds[0] < 0 || seconds[1] < seconds[0]) ? 1
                          : seconds[0] >= 0                                               ? 0
                                                                                          : -1;
            struct app_assessment a = app_assess(&adjusted, m, local[i].installed);
            struct app_judgement  j = app_judge(a.fit, fastest < 0 ? -1 : seconds[fastest], estimated,
                                                m->quality_passed, m->quality_total, limits);
            double rate = m->quality_total ? (double) m->quality_passed / m->quality_total : -1;
            double time = fastest < 0 ? -1 : seconds[fastest];
            struct app_candidate candidate = {j.verdict, rate, time, local[i].installed};
            struct app_candidate fit       = {j.verdict, rate, time, false};
            size_t               k         = ranked_count;
            while (k > 0 && app_candidate_better(fit, ranked[k - 1]))
                ranked[k] = ranked[k - 1], order[k] = order[k - 1], --k;
            ranked[k] = fit, order[k] = i, ++ranked_count;
            if (j.verdict != APP_VERDICT_NOT_RECOMMENDED && (!best || app_candidate_better(candidate, best_candidate)))
                best = m, best_candidate = candidate;

            /* ---- compare ---- */
            const geistr_fit *f = nullptr;
            for (size_t r = 0; r < n; r++)
                if (geistr_ranking_get(ranking, r)->entry == geistr_catalog_get(catalog, i))
                    f = geistr_ranking_get(ranking, r);
            SAME((int) f->resource == (int) a.fit, "resource");
            SAME(!strcmp(f->resource_reason, code(a.reason)), "resource reason");
            SAME((int) f->verdict == (int) j.verdict, "verdict");
            SAME(!strcmp(f->reason, j.reason), "reason");
            SAME(f->seconds_cpu == seconds[0] && f->seconds_gpu == seconds[1], "seconds");
            SAME(f->basis == (fastest < 0 ? GEISTR_BASIS_NONE : estimated ? GEISTR_BASIS_ESTIMATED : GEISTR_BASIS_MEASURED),
                 "basis");
            SAME(f->processor == (fastest < 0 ? GEISTR_PROCESSOR_AUTO : fastest ? GEISTR_PROCESSOR_GPU : GEISTR_PROCESSOR_CPU),
                 "processor");
            SAME(f->estimated_from == (estimated ? measured[fastest > 0 ? 1 : 0] : 0), "estimated_from");
        }
        for (size_t i = 0; i < n; i++)
            SAME(geistr_ranking_get(ranking, i)->entry == geistr_catalog_get(catalog, order[i]), "ranking");
        size_t i = 0;
        SAME(best ? geistr_ranking_best(ranking) && !strcmp(geistr_ranking_best(ranking)->entry->id, best->id)
                  : !geistr_ranking_best(ranking),
             "best choice");
        models_total += (int) n;
        geistr_ranking_free(ranking);
        geistr_catalog_free(catalog);
    }
    if (failures) {
        fprintf(stderr, "fit parity: %d differences\n", failures);
        return 1;
    }
    printf("fit parity with geist-serve: %d scenarios, %d models identical\n", scenarios, models_total);
    return 0;
}

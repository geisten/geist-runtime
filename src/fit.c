/* fit.c — device probe, resource fit, verdict and ranking (#6).
 *
 * Moved unchanged in logic from geist-serve: app_hardware_read
 * (src/app/platform.c), app_assess (src/app/core.c), app_judge,
 * app_estimate_seconds, app_candidate_better (src/app/tasks.c) and the
 * ranking loop of src/app/status.c. Its fixtures are ported in
 * tests/test_fit.c. Reasons are codes; the caller owns the wording. */
#include "geistr_catalog.h"
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(__aarch64__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

/* Names and OS strings are display text: truncation is fine. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#define GIB UINT64_C(1073741824)
#define MIB UINT64_C(1048576)
#define TYPICAL_ANSWER_TOKENS 200 /* about 150 words */
#define RATE_TIE .02              /* pass rates this close count as equal; speed decides */

/* ---- device ------------------------------------------------------------- */

#ifndef __APPLE__
/* Match the engine's generic Linux compilation baseline. */
static bool cpu_supported(void) {
#if defined(__x86_64__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("x86-64-v3") != 0;
#elif defined(__aarch64__)
    const unsigned long required = HWCAP_ATOMICS | HWCAP_FPHP | HWCAP_ASIMDHP | HWCAP_ASIMDDP;
    return (getauxval(AT_HWCAP) & required) == required;
#else
    return false;
#endif
}
#endif

#ifndef __APPLE__
/* Physical cores: the CPUs that are the first thread of their core (SMT
 * siblings share one); the logical count where the topology is unreadable. */
static uint32_t physical_cores(uint32_t logical) {
    uint32_t n = 0;
    for (uint32_t cpu = 0; cpu < logical; cpu++) {
        char path[96], list[64] = "";
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", cpu);
        FILE *f  = fopen(path, "r");
        bool  ok = f && fgets(list, sizeof list, f);
        if (f)
            fclose(f);
        if (!ok)
            return logical;
        n += strtoul(list, nullptr, 10) == cpu;
    }
    return n ? n : logical;
}
#endif

geistr_status geistr_device_probe(const char *models_dir, geistr_device *out) {
    if (!out)
        return GEISTR_INVALID;
    geistr_device *h = out;
    *h               = (geistr_device) {.size = sizeof *h};
    struct utsname u;
    if (uname(&u) != 0)
        return GEISTR_IO;
    snprintf(h->os, sizeof h->os, "%s %s", u.sysname, u.release);
    snprintf(h->arch, sizeof h->arch, "%s", u.machine);
    snprintf(h->name, sizeof h->name, "%s %s", u.sysname, u.machine);
    long cores      = sysconf(_SC_NPROCESSORS_ONLN);
    h->cores        = cores > 0 ? (uint32_t) cores : 1;
    h->logical_cpus = cores > 0 ? (uint32_t) cores : 0;
    struct statvfs disk;
    if (models_dir && statvfs(models_dir, &disk) == 0)
        h->disk_known = !ckd_mul(&h->disk, (uint64_t) disk.f_bavail, (uint64_t) disk.f_frsize);
#ifdef __APPLE__
    char   version[64] = "";
    size_t n           = sizeof version;
    if (sysctlbyname("kern.osproductversion", version, &n, nullptr, 0) == 0)
        snprintf(h->os, sizeof h->os, "macOS %s", version);
    n = sizeof h->ram;
    if (sysctlbyname("hw.memsize", &h->ram, &n, nullptr, 0) != 0)
        return GEISTR_IO;
    n = sizeof h->name;
    if (sysctlbyname("machdep.cpu.brand_string", h->name, &n, nullptr, 0) != 0)
        snprintf(h->name, sizeof h->name, "Mac (%s)", h->arch);
    h->supported = strcmp(u.machine, "arm64") == 0;
    if (h->supported) {
        h->kind = GEISTR_DEVICE_APPLE_SILICON;
        h->gpu  = GEISTR_BACKEND_METAL;
    }
    int pcores = 0;
    n          = sizeof pcores;
    if (sysctlbyname("hw.perflevel0.physicalcpu", &pcores, &n, nullptr, 0) == 0 && pcores > 0)
        h->cores = (uint32_t) pcores;
    /* Free + inactive is a conservative snapshot, not an allocation guarantee. */
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    mach_port_t            host  = mach_host_self();
    vm_size_t              page  = 0;
    if (host_page_size(host, &page) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, (host_info64_t) &vm, &count) == KERN_SUCCESS) {
        h->available       = ((uint64_t) vm.free_count + vm.inactive_count) * page;
        h->available_known = true;
    }
    mach_port_deallocate(mach_task_self(), host);
#else
    h->supported = cpu_supported();
    h->cores     = physical_cores(h->logical_cpus);
    FILE *f      = fopen("/proc/meminfo", "r");
    if (f) {
        char               line[256];
        unsigned long long kb;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %llu kB", &kb) == 1)
                h->ram = kb * 1024;
            if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
                h->available       = kb * 1024;
                h->available_known = true;
            }
        }
        fclose(f);
    }
    f = fopen("/etc/os-release", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "PRETTY_NAME=", 12))
                continue;
            char *name                  = line + 12;
            name[strcspn(name, "\r\n")] = 0;
            size_t length               = strlen(name);
            if (length >= 2 && (name[0] == '"' || name[0] == '\'') && name[length - 1] == name[0]) {
                name[length - 1] = 0;
                ++name;
            }
            snprintf(h->os, sizeof h->os, "%s", name);
            break;
        }
        fclose(f);
    }
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "model name", 10))
                continue;
            char *name = strchr(line, ':');
            if (!name)
                continue;
            ++name;
            while (*name == ' ' || *name == '\t')
                ++name;
            name[strcspn(name, "\r\n")] = 0;
            snprintf(h->name, sizeof h->name, "%s", name);
            break;
        }
        fclose(f);
    }
    f = fopen("/proc/device-tree/model", "r");
    if (f) {
        size_t got   = fread(h->name, 1, sizeof h->name - 1, f);
        h->name[got] = 0;
        fclose(f);
        if (strstr(h->name, "Raspberry Pi 5"))
            h->kind = GEISTR_DEVICE_PI5;
    }
    if (!h->ram)
        return GEISTR_IO;
#endif
    return GEISTR_OK;
}

/* ---- one model ---------------------------------------------------------- */

/* app_assess: what the hardware allows, before any speed. */
static geistr_resource assess(const geistr_device *h, const geistr_catalog_entry *m, bool installed,
                              const char **reason) {
    *reason = "not_measured"; /* fits, no device profile */
    if (m->unsupported_format || !m->backends)
        return *reason = "unsupported_format", GEISTR_RESOURCE_UNAVAILABLE;
    if (!h->supported)
        return *reason = "platform", GEISTR_RESOURCE_UNAVAILABLE;
    if (!installed && h->disk_known && (h->disk < m->bytes || h->disk - m->bytes < 256 * MIB))
        return *reason = "disk", GEISTR_RESOURCE_UNAVAILABLE;
    if (h->ram && h->ram < m->bytes)
        return *reason = "ram", GEISTR_RESOURCE_UNAVAILABLE;
    if (h->ram < (uint64_t) m->recommended_ram_gib * GIB * 95 / 100)
        return *reason = "ram_recommended", GEISTR_RESOURCE_LIMITED;
    if (h->available_known && h->available < (uint64_t) m->working_mib * MIB)
        return *reason = "available_ram", GEISTR_RESOURCE_LIMITED;
    if ((h->kind == GEISTR_DEVICE_PI5 && !strcmp(m->id, "bitnet-2b")) ||
        (h->kind == GEISTR_DEVICE_APPLE_SILICON && h->cores >= 4))
        *reason = "fits";
    /* Unknown speed is not a resource limit; the verdict weighs speed. */
    return GEISTR_RESOURCE_FITS;
}

static double answer_seconds(double rate, double first) {
    return rate > 0 ? (first > 0 ? first : 0) + TYPICAL_ANSWER_TOKENS / rate : -1;
}

static double median(double *v, size_t n) {
    for (size_t i = 1; i < n; i++) /* insertion sort: n is the catalog size */
        for (size_t k = i; k && v[k - 1] > v[k]; k--) {
            double t = v[k];
            v[k] = v[k - 1], v[k - 1] = t;
        }
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

/* Seconds per typical answer for a model not measured here, from the models
 * measured on the same processor: decoding is mostly memory bound, so the
 * median throughput (rate × file bytes) carries over. ponytail: one bandwidth
 * figure per processor; ternary kernels decode faster per byte, so their
 * estimates are conservative. */
static double estimate(uint64_t bytes, size_t n, const double *rate, const double *size, const double *first,
                       double *scratch) {
    double *throughput = scratch, *firsts = scratch + n;
    size_t  k          = 0;
    for (size_t i = 0; i < n; i++)
        if (rate[i] > 0 && size[i] > 0)
            throughput[k] = rate[i] * size[i], firsts[k++] = first[i] > 0 ? first[i] : 0;
    if (!k || !bytes)
        return -1;
    return answer_seconds(median(throughput, k) / (double) bytes, median(firsts, k));
}

/* app_judge: known problems first, so a missing figure never hides a
 * measured one; an estimate alone never rules a model out (#133). */
static geistr_verdict judge(geistr_resource resource, double seconds, bool estimated, uint32_t passed,
                            uint32_t total, const geistr_rank_opts *l, const char **reason) {
    bool known_quality = total > 0, known_speed = seconds >= 0;
    if (resource == GEISTR_RESOURCE_UNAVAILABLE)
        return *reason = "unavailable", GEISTR_VERDICT_NOT_RECOMMENDED;
    if (known_quality && passed < l->reliable * total)
        return *reason = "unreliable", GEISTR_VERDICT_NOT_RECOMMENDED;
    if (known_speed && seconds > l->usable_s)
        return estimated ? (*reason = "probably_too_slow", GEISTR_VERDICT_UNKNOWN)
                         : (*reason = "too_slow", GEISTR_VERDICT_NOT_RECOMMENDED);
    if (!known_quality)
        return *reason = "quality_unknown", GEISTR_VERDICT_UNKNOWN;
    if (!known_speed)
        return *reason = "speed_unknown", GEISTR_VERDICT_UNKNOWN;
    if (seconds > l->fast_s)
        return *reason = "slow", GEISTR_VERDICT_USABLE;
    if (resource == GEISTR_RESOURCE_LIMITED)
        return *reason = "tight_memory", GEISTR_VERDICT_USABLE;
    return *reason = "good", GEISTR_VERDICT_GOOD;
}

geistr_resource geistr_assess(const geistr_catalog_entry *m, const geistr_device *device, bool installed,
                              const char **reason) {
    const char   *ignored;
    geistr_device d = {};
    if (!reason)
        reason = &ignored;
    if (!m || !device || device->size < sizeof(size_t) || device->size > sizeof d) {
        *reason = "invalid";
        return GEISTR_RESOURCE_UNAVAILABLE;
    }
    memcpy(&d, device, device->size);
    return assess(&d, m, installed, reason);
}

/* ---- ranking ------------------------------------------------------------ */

struct candidate {
    geistr_verdict verdict;
    double         rate;    /* pass rate 0..1, < 0 without a reference test */
    double         seconds; /* per typical answer; < 0 unknown */
    bool           installed;
};

static int verdict_rank(geistr_verdict v) {
    return v == GEISTR_VERDICT_GOOD ? 0 : v == GEISTR_VERDICT_USABLE ? 1 : v == GEISTR_VERDICT_UNKNOWN ? 2 : 3;
}

/* app_candidate_better: verdict, known quality, installed, pass rate (within
 * RATE_TIE equal), then speed. */
static bool better(struct candidate a, struct candidate b) {
    if (verdict_rank(a.verdict) != verdict_rank(b.verdict))
        return verdict_rank(a.verdict) < verdict_rank(b.verdict);
    if ((a.rate >= 0) != (b.rate >= 0))
        return a.rate >= 0;
    if (a.installed != b.installed)
        return a.installed;
    if (a.rate - b.rate > RATE_TIE || b.rate - a.rate > RATE_TIE)
        return a.rate > b.rate;
    if ((a.seconds >= 0) != (b.seconds >= 0))
        return a.seconds >= 0;
    return a.seconds < b.seconds;
}

struct geistr_ranking {
    size_t      count;
    geistr_fit *fits;  /* catalog order */
    size_t     *order; /* suitability order, indexes into fits */
    geistr_fit *best;
};

geistr_status geistr_rank(const geistr_catalog *catalog, const geistr_device *device, const geistr_local *local,
                          const geistr_rank_opts *opts, geistr_ranking **out) {
    if (!out)
        return GEISTR_INVALID;
    *out = nullptr;
    /* Fields beyond the caller's size keep their defaults. */
    geistr_rank_opts l = {.size = sizeof l, .fast_s = 10, .usable_s = 30, .reliable = .9};
    geistr_device    d = {};
    if (!catalog || !device || device->size < sizeof(size_t) || device->size > sizeof d ||
        (local && (local->size < sizeof(size_t) || local->size > sizeof(geistr_local))) ||
        (opts && (opts->size < sizeof(size_t) || opts->size > sizeof l)))
        return GEISTR_INVALID;
    memcpy(&d, device, device->size);
    if (opts)
        memcpy(&l, opts, opts->size);
    if (!(l.fast_s > 0) || !(l.usable_s >= l.fast_s) || !(l.reliable >= 0 && l.reliable <= 1))
        return GEISTR_INVALID;
    device = &d;
    size_t          n = geistr_catalog_count(catalog);
    geistr_ranking *r = calloc(1, sizeof *r);
    geistr_local   *in = calloc(n ? n : 1, sizeof *in);
    double         *buf = calloc(n ? 8 * n : 1, sizeof *buf);
    if (r) {
        r->fits  = calloc(n ? n : 1, sizeof *r->fits);
        r->order = calloc(n ? n : 1, sizeof *r->order);
    }
    if (!r || !in || !buf || !r->fits || !r->order) {
        free(in);
        free(buf);
        geistr_ranking_free(r);
        return GEISTR_NO_MEMORY;
    }
    for (size_t i = 0; local && i < n; i++) /* stride: the caller's struct size */
        memcpy(&in[i], (const char *) local + i * local->size, local->size);
    /* Measurements here, the basis for estimates of models not measured. */
    double  *rates[2] = {buf, buf + n}, *firsts[2] = {buf + 2 * n, buf + 3 * n}, *sizes = buf + 4 * n;
    double  *scratch = buf + 5 * n; /* 2n */
    uint32_t measured[2] = {0, 0};
    for (size_t i = 0; i < n; i++) {
        rates[0][i] = in[i].cpu.rate, firsts[0][i] = in[i].cpu.first;
        rates[1][i] = in[i].gpu.rate, firsts[1][i] = in[i].gpu.first;
        sizes[i]    = (double) geistr_catalog_get(catalog, i)->bytes;
        measured[0] += rates[0][i] > 0;
        measured[1] += rates[1][i] > 0;
    }
    struct candidate best = {}, *ranked = (struct candidate *) calloc(n ? n : 1, sizeof *ranked);
    if (!ranked) {
        free(in);
        free(buf);
        geistr_ranking_free(r);
        return GEISTR_NO_MEMORY;
    }
    for (size_t i = 0; i < n; i++) {
        const geistr_catalog_entry *m   = geistr_catalog_get(catalog, i);
        geistr_fit                 *f   = &r->fits[i];
        geistr_device               adj = *device;
        if (in[i].partial <= m->bytes && adj.disk_known && UINT64_MAX - adj.disk > in[i].partial)
            adj.disk += in[i].partial;
        bool gpu      = device->gpu && (m->backends & device->gpu);
        f->entry      = m;
        f->installed  = in[i].installed;
        f->resource   = assess(&adj, m, in[i].installed, &f->resource_reason);
        f->seconds_cpu = answer_seconds(in[i].cpu.rate, in[i].cpu.first);
        f->seconds_gpu = gpu ? answer_seconds(in[i].gpu.rate, in[i].gpu.first) : -1;
        /* No measurement of its own: an estimate, always labelled. */
        bool estimated = f->seconds_cpu < 0 && f->seconds_gpu < 0;
        if (estimated) {
            f->seconds_cpu = estimate(m->bytes, n, rates[0], sizes, firsts[0], scratch);
            f->seconds_gpu = gpu ? estimate(m->bytes, n, rates[1], sizes, firsts[1], scratch) : -1;
        }
        int fastest = f->seconds_gpu >= 0 && (f->seconds_cpu < 0 || f->seconds_gpu < f->seconds_cpu) ? 1
                      : f->seconds_cpu >= 0                                                         ? 0
                                                                                                    : -1;
        double seconds = fastest < 0 ? -1 : fastest ? f->seconds_gpu : f->seconds_cpu;
        geistr_catalog_quality(catalog, m, l.task, &f->passed, &f->total);
        f->verdict        = judge(f->resource, seconds, estimated, f->passed, f->total, &l, &f->reason);
        f->basis          = fastest < 0 ? GEISTR_BASIS_NONE : estimated ? GEISTR_BASIS_ESTIMATED : GEISTR_BASIS_MEASURED;
        f->processor      = fastest < 0 ? GEISTR_PROCESSOR_AUTO : fastest ? GEISTR_PROCESSOR_GPU : GEISTR_PROCESSOR_CPU;
        f->estimated_from = estimated ? measured[fastest > 0 ? 1 : 0] : 0;

        double           rate      = f->total ? (double) f->passed / f->total : -1;
        struct candidate candidate = {f->verdict, rate, seconds, f->installed};
        /* Suitability order ignores installed: stable insertion. */
        struct candidate fit = {f->verdict, rate, seconds, false};
        size_t           at  = i;
        while (at > 0 && better(fit, ranked[at - 1]))
            ranked[at] = ranked[at - 1], r->order[at] = r->order[at - 1], --at;
        ranked[at] = fit, r->order[at] = i;
        if (f->verdict != GEISTR_VERDICT_NOT_RECOMMENDED && (!r->best || better(candidate, best)))
            r->best = f, best = candidate;
    }
    r->count = n;
    free(ranked);
    free(in);
    free(buf);
    *out = r;
    return GEISTR_OK;
}

void geistr_ranking_free(geistr_ranking *r) {
    if (r) {
        free(r->fits);
        free(r->order);
        free(r);
    }
}

size_t geistr_ranking_count(const geistr_ranking *r) {
    return r ? r->count : 0;
}

const geistr_fit *geistr_ranking_get(const geistr_ranking *r, size_t index) {
    return r && index < r->count ? &r->fits[r->order[index]] : nullptr;
}

const geistr_fit *geistr_ranking_best(const geistr_ranking *r) {
    return r ? r->best : nullptr;
}

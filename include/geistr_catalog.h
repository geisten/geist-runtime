/*
 * geistr_catalog.h — model catalog and file verification (geist-runtime#5).
 *
 * A catalog lists models that can be downloaded: file name, size, SHA-256,
 * source URL, memory needs, backends and quality evidence. It is JSON in the
 * format of geist-serve's models/catalog.json (schema 1 and 2), parsed
 * strictly: unknown or duplicate keys, unsafe file names, URLs outside
 * huggingface.co/…/resolve/, and malformed evidence are refused as a whole.
 *
 * geistr_rank says which model and processor suit this computer (#6).
 *
 * geistr_catalog_check tells whether a model is installed in a folder: the
 * file is there and its SHA-256 matches. A wrong size or hash is MISMATCH;
 * such a file must not be loaded. Hashing a multi-GB file takes seconds, so
 * a match leaves a receipt (<models>/.verified/<sha256>) bound to the file's
 * identity, size and change time; while the file stays untouched, later
 * checks need no rehash.
 *
 * Needs no engine and no network: downloading is a separate, optional module.
 * Family, chat template and context come from the GGUF itself
 * (geistr_model_info), not from the catalog.
 *
 * Conventions as in geistr.h. Stability: EXPERIMENTAL until 1.0.
 */
#pragma once
#include "geistr.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct geistr_catalog geistr_catalog;

enum {
    GEISTR_BACKEND_CPU    = 1,
    GEISTR_BACKEND_METAL  = 2,
    GEISTR_BACKEND_VULKAN = 4,
};

/* One model. Strings are borrowed from the catalog: valid until
 * geistr_catalog_free. Optional strings are nullptr when absent. New fields
 * are only ever appended; the runtime owns the struct. */
typedef struct geistr_catalog_entry {
    const char *id;                 /* unique, [A-Za-z0-9._-] */
    const char *name;               /* display name */
    const char *file;               /* unique file name in the models folder, *.gguf */
    const char *url;                /* https://huggingface.co/…/resolve/… */
    const char *sha256;             /* 64 lowercase hex digits */
    const char *group_id;           /* variants of one model share it (= id in schema 1) */
    const char *group_name;         /* (= name in schema 1) */
    const char *quantization;       /* e.g. "Q4_0"; optional (schema 1: absent) */
    const char *reasoning_format;   /* "none" or "think_tags"; optional */
    const char *unsupported_format; /* format this engine cannot run yet; then backends == 0 */
    const char *quality;            /* reference suite evidence, raw JSON object; optional */
    const char *reference;          /* speed on reference platforms, raw JSON array; optional */
    uint64_t    bytes;              /* exact file size */
    uint32_t    working_mib;        /* memory beyond the file while running */
    uint32_t    recommended_ram_gib;
    uint32_t    backends;       /* GEISTR_BACKEND_* bits */
    uint32_t    quality_passed; /* sums over all quality tasks; 0/0 without evidence */
    uint32_t    quality_total;
} geistr_catalog_entry;

/* Parse len bytes of catalog JSON (at most 1 MiB, at most 1024 models).
 * On failure *out is nullptr, the status is GEISTR_FORMAT (or NO_MEMORY /
 * INVALID), and error (if cap > 0) says why. Thread-safe. */
geistr_status geistr_catalog_parse(const char *json, size_t len, geistr_catalog **out, char *error,
                                   size_t cap);

/* A parsed catalog is immutable: safe to read from any number of threads. */
void                        geistr_catalog_free(geistr_catalog *catalog);
uint32_t                    geistr_catalog_revision(const geistr_catalog *catalog);
size_t                      geistr_catalog_count(const geistr_catalog *catalog);
const geistr_catalog_entry *geistr_catalog_get(const geistr_catalog *catalog, size_t index);
const geistr_catalog_entry *geistr_catalog_find(const geistr_catalog *catalog, const char *id);

typedef enum geistr_install {
    GEISTR_INSTALL_MISSING = 0, /* no file of that name */
    GEISTR_INSTALL_UNVERIFIED,  /* right size, no receipt, not hashed (hash == false) */
    GEISTR_INSTALL_OK,          /* SHA-256 matches: installed */
    GEISTR_INSTALL_MISMATCH,    /* wrong size or hash, or not a regular file: never load it */
} geistr_install;

/* Is entry's file in models_dir installed? With hash, a file without a valid
 * receipt is hashed now (seconds per GB) and a match leaves a receipt;
 * without hash only receipts count, so listing a folder stays instant.
 * A file that changes while it is hashed is MISMATCH.
 * GEISTR_IO when the file exists but cannot be read. Never deletes anything.
 * Blocking; thread-safe for distinct files. */
geistr_status geistr_catalog_check(const geistr_catalog_entry *entry, const char *models_dir,
                                   bool hash, geistr_install *out);

/* The models folder shared with the geisten app: $GEISTEN_HOME/models, else
 * ~/Library/Application Support/geisten/models (macOS) or
 * ${XDG_DATA_HOME:-~/.local/share}/geisten/models. Not created here.
 * GEISTR_INVALID when HOME is unset or out is too small. */
geistr_status geistr_models_dir(char *out, size_t cap);

/* Lowercase hex SHA-256 of a file into out[65]. GEISTR_IO if unreadable. */
geistr_status geistr_sha256_file(const char *path, char out[65]);

/* Reference-suite counts of one task ("classify", "extract", …), or of all
 * tasks with task == nullptr. 0/0 when the entry has no such evidence. */
void geistr_catalog_quality(const geistr_catalog *catalog, const geistr_catalog_entry *entry, const char *task,
                            uint32_t *passed, uint32_t *total);

/* ------------------------------------------------------------------------ */
/* Device fit and ranking (#6): which model, which processor                 */
/* ------------------------------------------------------------------------ */

typedef enum geistr_device_kind {
    GEISTR_DEVICE_OTHER = 0,
    GEISTR_DEVICE_APPLE_SILICON,
    GEISTR_DEVICE_PI5,
} geistr_device_kind;

typedef struct geistr_device {
    size_t             size; /* sizeof(geistr_device) */
    char               name[160], arch[32], os[128];
    geistr_device_kind kind;
    uint64_t           ram, available, disk; /* bytes; disk = free space in the models folder */
    uint32_t           cores;                /* performance cores where known */
    uint32_t           logical_cpus;
    uint32_t           gpu;       /* GEISTR_BACKEND_METAL or _VULKAN the engine can use here, or 0 */
    bool               supported; /* the engine's CPU baseline is met */
    bool               available_known, disk_known;
} geistr_device;

/* Read this computer's memory, free disk in models_dir, cores and platform.
 * gpu is METAL on Apple Silicon; Vulkan is not probed yet (set it yourself).
 * GEISTR_IO when RAM cannot be read. */
geistr_status geistr_device_probe(const char *models_dir, geistr_device *out);

/* Measured here on one processor: tokens/s while answering and seconds to
 * the first token. rate 0 = not measured. */
typedef struct geistr_speed {
    double rate, first;
} geistr_speed;

/* What the caller knows about one model on this computer. */
typedef struct geistr_local {
    size_t       size; /* sizeof(geistr_local) */
    bool         installed;
    uint64_t     partial; /* bytes of an unfinished download */
    geistr_speed cpu, gpu;
} geistr_local;

typedef enum geistr_resource {
    GEISTR_RESOURCE_FITS = 0,
    GEISTR_RESOURCE_LIMITED,     /* may run: tight memory, or nothing known that says it fits well */
    GEISTR_RESOURCE_UNAVAILABLE, /* cannot run here */
} geistr_resource;

typedef enum geistr_verdict {
    GEISTR_VERDICT_GOOD = 0,        /* good enough and fast enough here */
    GEISTR_VERDICT_USABLE,          /* with a limit: slow or tight memory */
    GEISTR_VERDICT_NOT_RECOMMENDED, /* known bad: unavailable, unreliable, too slow */
    GEISTR_VERDICT_UNKNOWN,         /* missing quality or speed */
} geistr_verdict;

typedef enum geistr_basis {
    GEISTR_BASIS_NONE = 0,  /* no speed */
    GEISTR_BASIS_MEASURED,  /* this model was measured here */
    GEISTR_BASIS_ESTIMATED, /* scaled from other models measured here */
} geistr_basis;

typedef struct geistr_rank_opts {
    size_t      size;     /* sizeof(geistr_rank_opts) */
    double      fast_s;   /* seconds per typical answer (200 tokens) that count as fast; default 10 */
    double      usable_s; /* slower than this is too slow; default 30 */
    double      reliable; /* reference pass rate needed, 0..1; default 0.9 */
    const char *task;     /* quality of this task only; default nullptr = all tasks */
} geistr_rank_opts;

/* One model's assessment. Reasons are stable codes for the caller's text:
 *   resource_reason: unsupported_format, platform, disk, ram, ram_recommended,
 *                    available_ram, fits, not_measured
 *   reason:          good, slow, tight_memory, speed_unknown, quality_unknown,
 *                    probably_too_slow, too_slow, unreliable, unavailable
 * Owned by the ranking; appended to only. */
typedef struct geistr_fit {
    const geistr_catalog_entry *entry;
    geistr_resource             resource;
    const char                 *resource_reason;
    geistr_verdict              verdict;
    const char                 *reason;
    geistr_basis                basis;
    geistr_processor            processor; /* the faster one, CPU or GPU; AUTO when no speed */
    double                      seconds_cpu, seconds_gpu; /* per typical answer; < 0 unknown */
    uint32_t                    estimated_from; /* measured models behind an estimate */
    uint32_t                    passed, total;  /* reference suite; total 0 = no evidence */
    bool                        installed;
} geistr_fit;

typedef struct geistr_ranking geistr_ranking;

/* Rank every catalog model for this device. local is indexed like the
 * catalog (geistr_catalog_get) and may be nullptr (nothing installed or
 * measured); each element's size is local[0].size. The catalog must outlive
 * the ranking. */
geistr_status geistr_rank(const geistr_catalog *catalog, const geistr_device *device, const geistr_local *local,
                          const geistr_rank_opts *opts, geistr_ranking **out);
void          geistr_ranking_free(geistr_ranking *ranking);
/* In suitability order: verdict (good, usable, unknown, not recommended),
 * known quality, pass rate (within 2 points equal), speed. Installed or not
 * does not count, so a row never moves when a download completes. */
size_t            geistr_ranking_count(const geistr_ranking *ranking);
const geistr_fit *geistr_ranking_get(const geistr_ranking *ranking, size_t index);
/* The one recommendation: as above, but installed beats not installed at
 * the same verdict and known quality. nullptr when every model is not
 * recommended. */
const geistr_fit *geistr_ranking_best(const geistr_ranking *ranking);

#ifdef __cplusplus
}
#endif

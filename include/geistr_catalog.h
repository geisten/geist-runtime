/*
 * geistr_catalog.h — model catalog and file verification (geist-runtime#5).
 *
 * A catalog lists models that can be downloaded: file name, size, SHA-256,
 * source URL, memory needs, backends and quality evidence. It is JSON in the
 * format of geist-serve's models/catalog.json (schema 1 and 2), parsed
 * strictly: unknown or duplicate keys, unsafe file names, URLs outside
 * huggingface.co/…/resolve/, and malformed evidence are refused as a whole.
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

#ifdef __cplusplus
}
#endif

/* catalog.c — model catalog, SHA-256 verification, receipts (#5).
 *
 * The parser's rules are moved from geist-serve (src/app/catalog.c); its
 * invalid-input cases are ported in tests/test_catalog.py. The receipt stamp
 * is geist-serve's model_stamp (src/app/jobs.c). */
#include "geistr_catalog.h"
#include "stream.h"
#define JSMN_STATIC
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "jsmn.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__) && !defined(GEISTR_PORTABLE_SHA256)
#include <CommonCrypto/CommonDigest.h>
#endif

#define MAX_BYTES  (1u << 20)
#define MAX_MODELS 1024
#define GIB        UINT64_C(1073741824)

struct quality_tasks {
    char     name[8][33];
    uint32_t passed[8], total[8];
    unsigned n;
};

struct geistr_catalog {
    geistr_catalog_entry *models;
    struct quality_tasks *tasks; /* parallel to models */
    size_t                count;
    uint32_t              revision;
    char                 *strings; /* every entry string, NUL-separated */
    size_t                used, cap;
};

struct json {
    const char *src;
    jsmntok_t  *tok;
    int         n;
};

/* Direct child `key` of object `obj`; -1 when absent. */
static int get(const struct json *j, int obj, const char *key) {
    if (obj < 0 || j->tok[obj].type != JSMN_OBJECT)
        return -1;
    size_t kl = strlen(key);
    for (int i = obj + 1; i + 1 < j->n && j->tok[i].start < j->tok[obj].end; i++)
        if (j->tok[i].parent == obj && j->tok[i].type == JSMN_STRING &&
            (size_t) (j->tok[i].end - j->tok[i].start) == kl && !memcmp(j->src + j->tok[i].start, key, kl))
            return i + 1;
    return -1;
}

/* No unknown or duplicate keys, and no escaped keys masquerading as known names. */
static bool keys(const struct json *j, int object, const char *const *allowed) {
    if (object < 0 || j->tok[object].type != JSMN_OBJECT)
        return false;
    unsigned seen = 0;
    for (int i = object + 1; i < j->n && j->tok[i].start < j->tok[object].end; ++i) {
        if (j->tok[i].parent != object)
            continue;
        if (j->tok[i].type != JSMN_STRING)
            return false;
        unsigned k = 0;
        for (; allowed[k]; ++k)
            if ((size_t) (j->tok[i].end - j->tok[i].start) == strlen(allowed[k]) &&
                !memcmp(j->src + j->tok[i].start, allowed[k], strlen(allowed[k])))
                break;
        if (!allowed[k] || (seen & (1u << k)))
            return false;
        seen |= 1u << k;
    }
    return true;
}

static bool number(const struct json *j, int object, const char *key, uint64_t max, uint64_t *out) {
    int t = get(j, object, key);
    if (t < 0 || j->tok[t].type != JSMN_PRIMITIVE)
        return false;
    size_t      n = (size_t) (j->tok[t].end - j->tok[t].start);
    const char *s = j->src + j->tok[t].start;
    if (!n || n > 20 || (n > 1 && s[0] == '0'))
        return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9' || v > (max - (unsigned) (s[i] - '0')) / 10)
            return false;
        v = v * 10 + (unsigned) (s[i] - '0');
    }
    if (!v || v > max)
        return false;
    *out = v;
    return true;
}

/* Copy n raw bytes into the string pool. */
static const char *keep(geistr_catalog *c, const char *s, size_t n) {
    if (n + 1 > c->cap - c->used)
        return nullptr;
    char *out = c->strings + c->used;
    memcpy(out, s, n);
    out[n] = 0;
    c->used += n + 1;
    return out;
}

/* Plain UTF-8 strings only: no escapes, no control, quote or path bytes. */
static const char *string(geistr_catalog *c, const struct json *j, int object, const char *key, size_t max) {
    int t = get(j, object, key);
    if (t < 0 || j->tok[t].type != JSMN_STRING)
        return nullptr;
    size_t      n = (size_t) (j->tok[t].end - j->tok[t].start);
    const char *s = j->src + j->tok[t].start;
    if (!n || n > max)
        return nullptr;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char) s[i] < 32 || s[i] == '\\' || s[i] == '"' || (unsigned char) s[i] == 127)
            return nullptr;
    const char *out = keep(c, s, n);
    char            validated[1025];
    struct str_utf8 utf = {0};
    if (!out || !str_utf8_feed(&utf, out, validated, sizeof validated) || utf.used)
        return nullptr;
    return out;
}

static bool component(const char *s, bool file) {
    if (!s || !isalnum((unsigned char) *s) || strstr(s, ".."))
        return false;
    for (const char *p = s; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
              *p == '_' || *p == '.'))
            return false;
    size_t n = strlen(s);
    return !file || (n > 5 && !strcmp(s + n - 5, ".gguf"));
}

static bool hex(const char *s, size_t n) {
    if (!s || strlen(s) != n)
        return false;
    for (; *s; ++s)
        if (!(*s >= '0' && *s <= '9') && !(*s >= 'a' && *s <= 'f'))
            return false;
    return true;
}

static bool iso_date(const char *date) {
    if (!date || strlen(date) != 10)
        return false;
    for (int i = 0; i < 10; ++i)
        if (i == 4 || i == 7 ? date[i] != '-' : !isdigit((unsigned char) date[i]))
            return false;
    return true;
}

/* [passed, total], 0 <= passed <= total, added to sum. */
static bool counts(const struct json *j, int object, const char *key, uint32_t sum[2]) {
    int      t = get(j, object, key);
    uint64_t v[2];
    if (t < 0 || j->tok[t].type != JSMN_ARRAY || j->tok[t].size != 2)
        return false;
    for (int k = 0; k < 2; ++k) {
        const jsmntok_t *e = &j->tok[t + 1 + k];
        if (e->type != JSMN_PRIMITIVE || e->parent != t)
            return false;
        const char *s   = j->src + e->start;
        int         len = e->end - e->start;
        if (len < 1 || len > 6 || (len > 1 && s[0] == '0'))
            return false;
        v[k] = 0;
        for (int i = 0; i < len; ++i) {
            if (s[i] < '0' || s[i] > '9')
                return false;
            v[k] = v[k] * 10 + (unsigned) (s[i] - '0');
        }
    }
    sum[0] += (uint32_t) v[0];
    sum[1] += (uint32_t) v[1];
    return v[1] && v[0] <= v[1];
}

/* Reference benchmark evidence, validated, then kept as its raw JSON text. */
static const char *quality(geistr_catalog *c, const struct json *j, int object, geistr_catalog_entry *m) {
    static const char *const quality_keys[]  = {"suite", "date", "engine", "evidence", "tasks", nullptr};
    static const char *const language_keys[] = {"de", "en", nullptr};
    if (!keys(j, object, quality_keys) || !hex(string(c, j, object, "suite", 12), 12) ||
        !hex(string(c, j, object, "evidence", 64), 64) || !component(string(c, j, object, "engine", 48), false) ||
        !iso_date(string(c, j, object, "date", 10)))
        return nullptr;
    int tasks = get(j, object, "tasks");
    if (tasks < 0 || j->tok[tasks].type != JSMN_OBJECT || j->tok[tasks].size < 1 || j->tok[tasks].size > 8)
        return nullptr;
    for (int i = tasks + 1; i < j->n && j->tok[i].start < j->tok[tasks].end; ++i) {
        if (j->tok[i].parent != tasks)
            continue;
        char   name[33];
        size_t n = (size_t) (j->tok[i].end - j->tok[i].start);
        if (j->tok[i].type != JSMN_STRING || !n || n >= sizeof name)
            return nullptr;
        memcpy(name, j->src + j->tok[i].start, n);
        name[n]                 = 0;
        struct quality_tasks *q = &c->tasks[m - c->models];
        uint32_t sum[2]         = {};
        if (!component(name, false) || !keys(j, i + 1, language_keys) || !counts(j, i + 1, "de", sum) ||
            !counts(j, i + 1, "en", sum) || q->n == 8)
            return nullptr;
        memcpy(q->name[q->n], name, n + 1);
        q->passed[q->n]  = sum[0];
        q->total[q->n++] = sum[1];
        m->quality_passed += sum[0];
        m->quality_total += sum[1];
    }
    return keep(c, j->src + j->tok[object].start, (size_t) (j->tok[object].end - j->tok[object].start));
}

/* Speed on reference platforms: 1..4 entries of integers and plain strings. */
static const char *reference(geistr_catalog *c, const struct json *j, int list) {
    static const char *const entry_keys[] = {"platform", "backend", "answer_ms", "tokens_per_s",
                                             "memory_mib", "date", "engine", nullptr};
    if (list < 0 || j->tok[list].type != JSMN_ARRAY || j->tok[list].size < 1 || j->tok[list].size > 4)
        return nullptr;
    for (int i = list + 1; i < j->n && j->tok[i].start < j->tok[list].end; ++i) {
        if (j->tok[i].parent != list)
            continue;
        uint64_t    v;
        const char *backend = keys(j, i, entry_keys) ? string(c, j, i, "backend", 3) : nullptr;
        if (!backend || (strcmp(backend, "cpu") && strcmp(backend, "gpu")) || !string(c, j, i, "platform", 64) ||
            !number(j, i, "answer_ms", 3600000, &v) || !number(j, i, "tokens_per_s", 100000, &v) ||
            !number(j, i, "memory_mib", 1048576, &v) || !iso_date(string(c, j, i, "date", 10)) ||
            !component(string(c, j, i, "engine", 48), false))
            return nullptr;
    }
    return keep(c, j->src + j->tok[list].start, (size_t) (j->tok[list].end - j->tok[list].start));
}

/* The vision tower: {url, sha256, bytes}; url a safetensors checkpoint at a
 * pinned revision (40 hex digits), so the extraction is reproducible. */
static bool vision(geistr_catalog *c, const struct json *j, int object, geistr_catalog_entry *m) {
    static const char *const vision_keys[] = {"url", "sha256", "bytes", nullptr};
    if (j->tok[object].type != JSMN_OBJECT || !keys(j, object, vision_keys))
        return false;
    m->vision_url    = string(c, j, object, "url", 1024);
    m->vision_sha256 = string(c, j, object, "sha256", 64);
    const char *rev  = m->vision_url ? strstr(m->vision_url, "/resolve/") : nullptr;
    size_t      n    = m->vision_url ? strlen(m->vision_url) : 0;
    if (!rev || strncmp(m->vision_url, "https://huggingface.co/", 23) || strpbrk(m->vision_url, "@?#% ") ||
        strstr(m->vision_url, "..") || n < 12 || strcmp(m->vision_url + n - 12, ".safetensors"))
        return false;
    rev += strlen("/resolve/");
    for (int k = 0; k < 40; k++)
        if (!(rev[k] >= '0' && rev[k] <= '9') && !(rev[k] >= 'a' && rev[k] <= 'f'))
            return false;
    return rev[40] == '/' && hex(m->vision_sha256, 64) && number(j, object, "bytes", 4 * GIB, &m->vision_bytes) &&
           m->vision_bytes > 0;
}

/* Validate model i of the list into m; nullptr on success, else why not. */
static const char *entry(geistr_catalog *c, const struct json *j, int i, uint64_t schema, geistr_catalog_entry *m) {
    static const char *const model_keys[] = {"id",         "name",         "file",
                                             "url",        "sha256",       "bytes",
                                             "working_mib", "recommended_ram_gib", "backends",
                                             "unsupported_format", "group_id", "group_name",
                                             "quantization", "reasoning_format", "quality",
                                             "reference",  "vision",       nullptr};
    uint64_t                 v;
    if (!keys(j, i, model_keys))
        return "unknown or duplicate key";
    m->id     = string(c, j, i, "id", 63);
    m->name   = string(c, j, i, "name", 100);
    m->file   = string(c, j, i, "file", 180);
    m->url    = string(c, j, i, "url", 1024);
    m->sha256 = string(c, j, i, "sha256", 64);
    if (!component(m->id, false) || !strcmp(m->id, "custom"))
        return "bad id";
    if (!m->name)
        return "bad name";
    if (!component(m->file, true))
        return "bad file name";
    if (!hex(m->sha256, 64))
        return "bad sha256";
    if (!m->url || strncmp(m->url, "https://huggingface.co/", 23) || !strstr(m->url, "/resolve/") ||
        strpbrk(m->url, "@?#% ") || strstr(m->url, ".."))
        return "bad url";
    if (get(j, i, "reasoning_format") >= 0) {
        m->reasoning_format = string(c, j, i, "reasoning_format", 24);
        if (!m->reasoning_format || (strcmp(m->reasoning_format, "none") && strcmp(m->reasoning_format, "think_tags")))
            return "bad reasoning_format";
    }
    if (get(j, i, "quality") >= 0 && !(m->quality = quality(c, j, get(j, i, "quality"), m)))
        return "bad quality";
    if (get(j, i, "reference") >= 0 && !(m->reference = reference(c, j, get(j, i, "reference"))))
        return "bad reference";
    if (get(j, i, "vision") >= 0 && !vision(c, j, get(j, i, "vision"), m))
        return "bad vision";
    if (schema == 2) {
        m->group_id     = string(c, j, i, "group_id", 63);
        m->group_name   = string(c, j, i, "group_name", 100);
        m->quantization = string(c, j, i, "quantization", 32);
        if (!component(m->group_id, false) || !strcmp(m->group_id, "custom") || !m->group_name ||
            !component(m->quantization, false))
            return "bad group";
    } else {
        /* Legacy catalogs have no trusted grouping metadata. Never guess from names. */
        if (get(j, i, "group_id") >= 0 || get(j, i, "group_name") >= 0 || get(j, i, "quantization") >= 0)
            return "group fields need schema 2";
        m->group_id   = m->id;
        m->group_name = m->name;
    }
    if (!number(j, i, "bytes", 256 * GIB, &m->bytes))
        return "bad bytes";
    if (!number(j, i, "working_mib", 1048576, &v))
        return "bad working_mib";
    m->working_mib = (uint32_t) v;
    if (!number(j, i, "recommended_ram_gib", 4096, &v))
        return "bad recommended_ram_gib";
    m->recommended_ram_gib = (uint32_t) v;
    int backends           = get(j, i, "backends");
    if (backends < 0 || j->tok[backends].type != JSMN_ARRAY)
        return "bad backends";
    for (int k = backends + 1; k < j->n && j->tok[k].start < j->tok[backends].end; ++k) {
        if (j->tok[k].parent != backends)
            continue;
        /* Compare complete raw tokens: an escaped NUL must not truncate an
         * unknown name into a supported backend. */
        if (j->tok[k].type != JSMN_STRING)
            return "bad backends";
        const char *name   = j->src + j->tok[k].start;
        size_t      length = (size_t) (j->tok[k].end - j->tok[k].start);
        uint32_t    bit    = length == 3 && !memcmp(name, "cpu", 3)      ? GEISTR_BACKEND_CPU
                             : length == 5 && !memcmp(name, "metal", 5)  ? GEISTR_BACKEND_METAL
                             : length == 6 && !memcmp(name, "vulkan", 6) ? GEISTR_BACKEND_VULKAN
                                                                         : 0;
        if (!bit || (m->backends & bit))
            return "bad backends";
        m->backends |= bit;
    }
    if (get(j, i, "unsupported_format") >= 0) {
        m->unsupported_format = string(c, j, i, "unsupported_format", 32);
        if (!m->unsupported_format || strcmp(m->unsupported_format, "pq2_0") || m->backends)
            return "bad unsupported_format";
    }
    if (!(m->backends & GEISTR_BACKEND_CPU) && !m->unsupported_format)
        return "every runnable model needs the cpu backend";
    for (size_t k = 0; k < c->count; ++k) {
        const geistr_catalog_entry *p = &c->models[k];
        if (!strcmp(m->id, p->id) || !strcmp(m->file, p->file))
            return "duplicate id or file";
        if (!strcmp(m->group_id, p->group_id) &&
            (strcmp(m->group_name, p->group_name) || (m->quantization && !strcmp(m->quantization, p->quantization))))
            return "conflicting group";
    }
    return nullptr;
}

geistr_status geistr_catalog_parse(const char *text, size_t len, geistr_catalog **out, char *error, size_t cap) {
    if (error && cap)
        error[0] = 0;
    if (!out || (!text && len))
        return GEISTR_INVALID;
    *out = nullptr;
    const char    *why    = "not a catalog object";
    geistr_status  status = GEISTR_FORMAT;
    struct json    j      = {.src = text};
    geistr_catalog *c     = nullptr;
    jsmn_parser    p;
    if (len > MAX_BYTES || memchr(text, 0, len)) {
        why = len > MAX_BYTES ? "larger than 1 MiB" : "contains NUL";
        goto bad;
    }
    jsmn_init(&p);
    int n = jsmn_parse(&p, text, len, nullptr, 0);
    if (n < 1) {
        why = "not valid JSON";
        goto bad;
    }
    c   = calloc(1, sizeof *c);
    j.tok = malloc((size_t) n * sizeof *j.tok);
    if (c)
        c->strings = malloc(c->cap = 2 * len + 2); /* raw evidence repeats its strings */
    if (!c || !j.tok || !c->strings) {
        status = GEISTR_NO_MEMORY;
        why    = "out of memory";
        goto bad;
    }
    jsmn_init(&p);
    j.n = jsmn_parse(&p, text, len, j.tok, (unsigned) n);
    if (j.n != n || j.tok[0].type != JSMN_OBJECT)
        goto bad;
    for (int i = 1; i < j.n; i++)
        if (j.tok[i].parent < 0)
            goto bad; /* something after the root object */
    static const char *const root_keys[] = {"schema", "revision", "models", nullptr};
    uint64_t                 schema, revision;
    if (!keys(&j, 0, root_keys) || !number(&j, 0, "schema", 2, &schema) ||
        !number(&j, 0, "revision", 1000000000, &revision)) {
        why = "bad schema, revision or root key";
        goto bad;
    }
    c->revision = (uint32_t) revision;
    int list    = get(&j, 0, "models");
    if (list < 0 || j.tok[list].type != JSMN_ARRAY || j.tok[list].size < 1 || j.tok[list].size > MAX_MODELS) {
        why = "models must list 1 to 1024 entries";
        goto bad;
    }
    c->models = calloc((size_t) j.tok[list].size, sizeof *c->models);
    c->tasks  = calloc((size_t) j.tok[list].size, sizeof *c->tasks);
    if (!c->models || !c->tasks) {
        status = GEISTR_NO_MEMORY;
        why    = "out of memory";
        goto bad;
    }
    for (int i = list + 1; i < j.n && j.tok[i].start < j.tok[list].end; ++i) {
        if (j.tok[i].parent != list)
            continue;
        if ((why = entry(c, &j, i, schema, &c->models[c->count]))) {
            if (error && cap)
                snprintf(error, cap, "model %zu: %s", c->count + 1, why);
            why = nullptr;
            goto bad;
        }
        ++c->count;
    }
    free(j.tok);
    *out = c;
    return GEISTR_OK;
bad:
    if (why && error && cap)
        snprintf(error, cap, "%s", why);
    free(j.tok);
    geistr_catalog_free(c);
    return status;
}

void geistr_catalog_free(geistr_catalog *c) {
    if (c) {
        free(c->models);
        free(c->tasks);
        free(c->strings);
        free(c);
    }
}

uint32_t geistr_catalog_revision(const geistr_catalog *c) {
    return c ? c->revision : 0;
}

size_t geistr_catalog_count(const geistr_catalog *c) {
    return c ? c->count : 0;
}

const geistr_catalog_entry *geistr_catalog_get(const geistr_catalog *c, size_t index) {
    return c && index < c->count ? &c->models[index] : nullptr;
}

const geistr_catalog_entry *geistr_catalog_find(const geistr_catalog *c, const char *id) {
    for (size_t i = 0; c && id && i < c->count; ++i)
        if (!strcmp(id, c->models[i].id))
            return &c->models[i];
    return nullptr;
}

void geistr_catalog_quality(const geistr_catalog *c, const geistr_catalog_entry *m, const char *task,
                            uint32_t *passed, uint32_t *total) {
    uint32_t p = 0, t = 0;
    if (c && m >= c->models && m < c->models + c->count) {
        const struct quality_tasks *q = &c->tasks[m - c->models];
        p = task ? 0 : m->quality_passed;
        t = task ? 0 : m->quality_total;
        for (unsigned k = 0; task && k < q->n; k++)
            if (!strcmp(task, q->name[k]))
                p = q->passed[k], t = q->total[k];
    }
    if (passed)
        *passed = p;
    if (total)
        *total = t;
}

/* ---- SHA-256 ------------------------------------------------------------ */

#if defined(__APPLE__) && !defined(GEISTR_PORTABLE_SHA256)
typedef CC_SHA256_CTX sha256_ctx;
static void sha256_init(sha256_ctx *s) {
    CC_SHA256_Init(s);
}
static void sha256_update(sha256_ctx *s, const unsigned char *p, size_t n) {
    CC_SHA256_Update(s, p, (CC_LONG) n);
}
static void sha256_final(sha256_ctx *s, unsigned char digest[32]) {
    CC_SHA256_Final(digest, s);
}
#else
/* ponytail: portable FIPS 180-4, ~140 MB/s (M1; CommonCrypto 790 MB/s);
 * receipts make rehashing rare.
 * Use the ARMv8/SHA-NI instructions if first-time verification gets slow. */
typedef struct {
    uint32_t      h[8];
    uint64_t      bits;
    unsigned char block[64];
    size_t        fill;
} sha256_ctx;
static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static uint32_t ror(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}
static void sha256_block(sha256_ctx *s, const unsigned char *p) {
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t) p[4 * i] << 24 | (uint32_t) p[4 * i + 1] << 16 | (uint32_t) p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = a[7] + (ror(a[4], 6) ^ ror(a[4], 11) ^ ror(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
        uint32_t t2 = (ror(a[0], 2) ^ ror(a[0], 13) ^ ror(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof *a);
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++)
        s->h[i] += a[i];
}
static void sha256_init(sha256_ctx *s) {
    *s = (sha256_ctx) {.h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}};
}
static void sha256_update(sha256_ctx *s, const unsigned char *p, size_t n) {
    s->bits += (uint64_t) n * 8;
    while (n) {
        if (!s->fill && n >= 64) {
            sha256_block(s, p);
            p += 64;
            n -= 64;
            continue;
        }
        size_t take = 64 - s->fill < n ? 64 - s->fill : n;
        memcpy(s->block + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
        if (s->fill == 64) {
            sha256_block(s, s->block);
            s->fill = 0;
        }
    }
}
static void sha256_final(sha256_ctx *s, unsigned char digest[32]) {
    uint64_t      bits = s->bits;
    unsigned char pad[72] = {0x80};
    size_t        n       = (s->fill < 56 ? 56 : 120) - s->fill;
    for (int i = 0; i < 8; i++)
        pad[n + i] = (unsigned char) (bits >> (56 - 8 * i));
    sha256_update(s, pad, n + 8);
    for (int i = 0; i < 32; i++)
        digest[i] = (unsigned char) (s->h[i / 4] >> (24 - 8 * (i % 4)));
}
#endif

geistr_status geistr_sha256_memory(size_t len, const void *data, char *out) {
    if (out) out[0] = 0;
    if (!out || (!data && len) || len > UINT64_MAX / 8)
        return GEISTR_INVALID;
    sha256_ctx s;
    sha256_init(&s);
    const unsigned char *p = data;
    while (len) {
        const size_t chunk = len < (1u << 20) ? len : (1u << 20);
        sha256_update(&s, p, chunk);
        p += chunk;
        len -= chunk;
    }
    unsigned char digest[32];
    sha256_final(&s, digest);
    for (size_t i = 0; i < 32; ++i)
        snprintf(out + 2 * i, 3, "%02x", digest[i]);
    return GEISTR_OK;
}

geistr_status geistr_sha256_file(const char *path, char out[65]) {
    if (!path || !out)
        return GEISTR_INVALID;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return GEISTR_IO;
    enum { block = 1 << 20 };
    unsigned char *buf = malloc(block), digest[32];
    if (!buf) {
        close(fd);
        return GEISTR_NO_MEMORY;
    }
    sha256_ctx s;
    sha256_init(&s);
    ssize_t n;
    while ((n = read(fd, buf, block)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0)
            sha256_update(&s, buf, (size_t) n);
    free(buf);
    close(fd);
    if (n < 0)
        return GEISTR_IO;
    sha256_final(&s, digest);
    for (int i = 0; i < 32; i++)
        snprintf(out + 2 * i, 3, "%02x", digest[i]);
    return GEISTR_OK;
}

/* ---- installed? --------------------------------------------------------- */

/* The file's identity; any write, rename or replace changes it (ctime). */
static bool stamp(const char *path, const geistr_catalog_entry *m, char out[256]) {
    struct stat s;
    if (lstat(path, &s) != 0 || !S_ISREG(s.st_mode) || (uint64_t) s.st_size != m->bytes)
        return false;
#ifdef __APPLE__
    struct timespec modified = s.st_mtimespec, changed = s.st_ctimespec;
#else
    struct timespec modified = s.st_mtim, changed = s.st_ctim;
#endif
    int n = snprintf(out, 256, "v1 %s %ju %ju %ju %ju %ju %ju %ju %jd %ld %jd %ld\n", m->sha256,
                     (uintmax_t) s.st_dev, (uintmax_t) s.st_ino, (uintmax_t) s.st_size, (uintmax_t) s.st_mode,
                     (uintmax_t) s.st_uid, (uintmax_t) s.st_gid, (uintmax_t) s.st_nlink, (intmax_t) modified.tv_sec,
                     modified.tv_nsec, (intmax_t) changed.tv_sec, changed.tv_nsec);
    return n > 0 && n < 256;
}

static bool join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    return n > 0 && (size_t) n < cap;
}

geistr_status geistr_catalog_check(const geistr_catalog_entry *m, const char *dir, bool hash, geistr_install *out) {
    const size_t flen = m && m->file ? strlen(m->file) : 0; /* a model, or its vision tower (#92) */
    if (!m || !dir || !out || !hex(m->sha256, 64) ||
        !(component(m->file, true) ||
          (component(m->file, false) && flen > 12 && !strcmp(m->file + flen - 12, ".safetensors"))))
        return GEISTR_INVALID;
    *out = GEISTR_INSTALL_MISSING;
    char path[4096], receipts[4096], receipt[4096], before[256], after[256], seen[256] = "", sum[65];
    if (!join(path, sizeof path, dir, m->file) || !join(receipts, sizeof receipts, dir, ".verified") ||
        !join(receipt, sizeof receipt, receipts, m->sha256))
        return GEISTR_INVALID;
    struct stat s;
    if (lstat(path, &s) != 0)
        return errno == ENOENT ? GEISTR_OK : GEISTR_IO;
    if (!stamp(path, m, before)) {
        *out = GEISTR_INSTALL_MISMATCH;
        return GEISTR_OK;
    }
    FILE *f = fopen(receipt, "r");
    if (f) {
        size_t n = fread(seen, 1, sizeof seen - 1, f);
        seen[n]  = 0;
        fclose(f);
    }
    if (!strcmp(seen, before)) {
        *out = GEISTR_INSTALL_OK;
        return GEISTR_OK;
    }
    if (!hash) {
        *out = GEISTR_INSTALL_UNVERIFIED;
        return GEISTR_OK;
    }
    geistr_status status = geistr_sha256_file(path, sum);
    if (status != GEISTR_OK)
        return status;
    if (strcmp(sum, m->sha256) || !stamp(path, m, after) || strcmp(before, after)) {
        *out = GEISTR_INSTALL_MISMATCH;
        return GEISTR_OK;
    }
    *out = GEISTR_INSTALL_OK;
    /* The receipt is a cache: failing to write it costs only a later rehash. */
    char part[4200];
    snprintf(part, sizeof part, "%s.%ld", receipt, (long) getpid());
    if ((mkdir(receipts, 0700) == 0 || errno == EEXIST) && (f = fopen(part, "w"))) {
        bool ok = fputs(before, f) >= 0;
        ok      = fclose(f) == 0 && ok;
        if (!ok || rename(part, receipt) != 0)
            unlink(part);
    }
    return GEISTR_OK;
}

geistr_status geistr_models_dir(char *out, size_t cap) {
    const char *home = getenv("GEISTEN_HOME"), *user = getenv("HOME");
    if (!home)
        home = getenv("GEIST_HOME");
    int n;
    if (!out || !cap)
        return GEISTR_INVALID;
    if (home && *home)
        n = snprintf(out, cap, "%s/models", home);
    else if (!user || !*user)
        return GEISTR_INVALID;
    else {
#ifdef __APPLE__
        n = snprintf(out, cap, "%s/Library/Application Support/geisten/models", user);
#else
        const char *data = getenv("XDG_DATA_HOME");
        n = data && *data ? snprintf(out, cap, "%s/geisten/models", data)
                          : snprintf(out, cap, "%s/.local/share/geisten/models", user);
#endif
    }
    return n > 0 && (size_t) n < cap ? GEISTR_OK : GEISTR_INVALID;
}

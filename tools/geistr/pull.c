/* pull.c — geistr's download module (#11): libcurl, https only, resumable,
 * verified before the file takes its final name. Moved from geist-serve's
 * download_model (src/app/jobs.c). */
#include "pull.h"
#include "cli.h"
#include "json.h"
#include <curl/curl.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_pull_interrupt(int signal) {
    (void) signal;
    stop = 1;
}

struct sink {
    FILE    *file;
    uint64_t offset, bytes, limit;
    bool     over; /* the file is larger than the catalog says */
};

static size_t on_data(char *data, size_t size, size_t count, void *context) {
    struct sink *s = context;
    size_t       n = size * count;
    if ((s->over = n > s->limit - s->bytes) || fwrite(data, 1, n, s->file) != n)
        return 0;
    s->bytes += n;
    return n;
}

static int on_progress(void *context, curl_off_t total, curl_off_t now, curl_off_t up, curl_off_t sent) {
    (void) total, (void) now, (void) up, (void) sent;
    const struct sink *s = context;
    static int         shown = -1;
    int                pct   = (int) (s->bytes * 100 / s->limit);
    if (pct != shown && isatty(2))
        fprintf(stderr, "\r%3d%% of %.1f GB", pct, (double) s->limit / 1e9), shown = pct;
    return stop;
}

/* The options every geistr download has: https only (http too in the test
 * build), redirects, CA bundle, timeouts, failure on HTTP errors. */
static void easy_setup(CURL *curl, const char *url) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
#ifndef __APPLE__
    if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0)
        curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    else if (access("/etc/ssl/cert.pem", R_OK) == 0)
        curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/cert.pem");
#endif
#ifdef GEISTR_TESTING
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 128L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
}

/* A catalog URL, or in the test build under GEISTR_TEST_URL_BASE. */
static void source_url(const char *catalog_url, char *url, size_t cap) {
    snprintf(url, cap, "%s", catalog_url);
#ifdef GEISTR_TESTING
    const char *base = getenv("GEISTR_TEST_URL_BASE"); /* replaces https://huggingface.co */
    if (base)
        snprintf(url, cap, "%s%s", base, catalog_url + strlen("https://huggingface.co"));
#endif
}

int geistr_pull(const geistr_catalog_entry *m, const char *dir) {
    char target[4200], part[4300], url[2048];
    snprintf(target, sizeof target, "%s/%s", dir, m->file);
    snprintf(part, sizeof part, "%s.part", target);
    source_url(m->url, url, sizeof url);
    if (!make_dirs(dir, 0755)) {
        fprintf(stderr, "geistr: cannot create %s: %s\n", dir, strerror(errno));
        return 1;
    }
    FILE       *file = fopen(part, "ab+");
    struct stat st;
    if (!file || fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t) st.st_size > m->bytes) {
        fprintf(stderr, "geistr: invalid partial download %s; remove it and retry\n", part);
        if (file)
            fclose(file);
        return 1;
    }
    struct sink s = {.file = file, .offset = (uint64_t) st.st_size, .bytes = (uint64_t) st.st_size, .limit = m->bytes};
    struct sigaction sa = {.sa_handler = on_pull_interrupt};
    sigaction(SIGINT, &sa, nullptr);
    bool ok = s.bytes == m->bytes;
    if (!ok) {
        fprintf(stderr, "%s %s → %s\n", s.offset ? "resuming" : "downloading", m->id, dir);
        curl_global_init(CURL_GLOBAL_DEFAULT);
        CURL *curl = curl_easy_init();
        if (!curl) {
            fclose(file);
            return 1;
        }
        easy_setup(curl, url);
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t) s.offset);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &s);
        CURLcode rc = curl_easy_perform(curl);
        if (rc == CURLE_RANGE_ERROR && !stop && fflush(file) == 0 && ftruncate(fileno(file), 0) == 0 &&
            fseeko(file, 0, SEEK_SET) == 0) {
            /* The server cannot resume: start again from 0. */
            s.offset = s.bytes = 0;
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t) 0);
            rc = curl_easy_perform(curl);
        }
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        ok = rc == CURLE_OK && (status == 200 || status == 206) && s.bytes == m->bytes;
        if (isatty(2))
            fputc('\n', stderr);
        /* Complete but of another size (the catalog is behind the file):
         * resuming cannot help, so the part goes. */
        const bool sized = !stop && ((rc == CURLE_OK && s.bytes != m->bytes) || s.over);
        if (sized) {
            fprintf(stderr, "geistr: %s is %s %ju bytes the catalog gives; removed, the catalog may be out of date (geistr pull)\n",
                    m->id, s.over ? "larger than the" : "smaller than the", (uintmax_t) m->bytes);
            unlink(part);
        } else if (!ok)
            fprintf(stderr, "geistr: download %s: %s; run geistr pull %s again to resume\n",
                    stop ? "stopped" : "failed", stop ? "Ctrl-C" : curl_easy_strerror(rc), m->id);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0)
        ok = false;
    if (fclose(file) != 0)
        ok = false;
    if (!ok)
        return stop ? 130 : 1;
    /* Verified under its final name: one hash, which also leaves the receipt.
     * Anything but a match is removed; installed means verified. An older
     * file of that name waits aside and comes back if the new one fails. */
    fprintf(stderr, "verifying %s …\n", m->id);
    geistr_install state = GEISTR_INSTALL_MISMATCH;
    char           old[4300];
    snprintf(old, sizeof old, "%s.old", target);
    const bool aside = rename(target, old) == 0;
    if (!aside && errno != ENOENT) {
        fprintf(stderr, "geistr: cannot replace %s: %s\n", target, strerror(errno));
        return 1;
    }
    if (rename(part, target) != 0) {
        fprintf(stderr, "geistr: cannot install %s: %s\n", target, strerror(errno));
        if (aside)
            rename(old, target);
        return 1;
    }
    if (geistr_catalog_check(m, dir, true, &state) != GEISTR_OK || state != GEISTR_INSTALL_OK) {
        unlink(target);
        if (aside)
            rename(old, target);
        fprintf(stderr, "geistr: %s does not match its SHA-256; removed%s, try again\n", m->id,
                aside ? " (the previous file stays)" : "");
        return 1;
    }
    if (aside)
        unlink(old);
    printf("✓ %s installed\n", m->id);
    return 0;
}

/* ---- the vision tower (#92) --------------------------------------------------
 * The vision tensors of a safetensors checkpoint, fetched by HTTP ranges (a
 * few hundred MB of a ~10 GB file) into vision_tower.safetensors next to the
 * model, then verified against the catalog's SHA-256. */

#define VISION_HEADER_MAX (64u << 20)

struct range_sink {
    FILE    *file; /* or, without a file, into data */
    char    *data;
    uint64_t bytes, limit, total, done; /* total, done: for the progress line */
};

static size_t on_range(char *data, size_t size, size_t count, void *context) {
    struct range_sink *s = context;
    size_t             n = size * count;
    if (n > s->limit - s->bytes)
        return 0; /* more than asked for: a server that ignored the range */
    if (s->file ? fwrite(data, 1, n, s->file) != n : (memcpy(s->data + s->bytes, data, n), false))
        return 0;
    s->bytes += n;
    if (s->total && isatty(2))
        fprintf(stderr, "\r%3d%% of %.0f MB", (int) ((s->done + s->bytes) * 100 / s->total), (double) s->total / 1e6);
    return n;
}

/* Bytes [from, to) of url into s (a file, or s->data of to - from bytes). */
static bool fetch_range(CURL *curl, uint64_t from, uint64_t to, struct range_sink *s) {
    char range[64];
    snprintf(range, sizeof range, "%ju-%ju", (uintmax_t) from, (uintmax_t) (to - 1));
    s->bytes = 0, s->limit = to - from;
    curl_easy_setopt(curl, CURLOPT_RANGE, range);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_range);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, s);
    long status = 0;
    bool ok     = curl_easy_perform(curl) == CURLE_OK;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    return ok && status == 206 && s->bytes == to - from && !stop;
}

struct tensor {
    const char *name, *dtype, *shape; /* raw JSON text in the header */
    size_t      n_name, n_dtype, n_shape;
    uint64_t    from, to;             /* in the source's data section */
};

static int by_offset(const void *a, const void *b) {
    const struct tensor *x = a, *y = b;
    return x->from < y->from ? -1 : x->from > y->from;
}

static bool vision_tensor(const char *name, size_t n) {
    static const char *const prefixes[] = {"model.vision_tower.", "model.embed_vision."};
    for (size_t i = 0; i < 2; i++)
        if (n > strlen(prefixes[i]) && !strncmp(name, prefixes[i], strlen(prefixes[i])))
            return true;
    return false;
}

int geistr_pull_vision(const geistr_catalog_entry *m, const char *dir) {
    if (!m->vision_url)
        return 0;
    /* ponytail: the engine's one name, next to the GGUF: one tower per models
     * folder (an E4B beside it refuses the E2B tower by its width); a name
     * per model once geistlib takes a tower path. */
    const char *name = "vision_tower.safetensors";
    char        target[4600], part[4700], url[2048];
    snprintf(target, sizeof target, "%s/%s", dir, name);
    snprintf(part, sizeof part, "%s.part", target);
    source_url(m->vision_url, url, sizeof url);
    geistr_catalog_entry tower = {.id = m->id, .name = m->name, .file = name, .url = m->vision_url,
                                  .sha256 = m->vision_sha256, .bytes = m->vision_bytes};
    geistr_install       state = GEISTR_INSTALL_MISSING;
    if (geistr_catalog_check(&tower, dir, false, &state) == GEISTR_OK &&
        (state == GEISTR_INSTALL_OK ||
         (state == GEISTR_INSTALL_UNVERIFIED && geistr_catalog_check(&tower, dir, true, &state) == GEISTR_OK &&
          state == GEISTR_INSTALL_OK)))
        return 0; /* there and verified (the receipt, else one hash) */
    fprintf(stderr, "vision tower for %s → %s\n", m->id, dir);
    struct sigaction sa = {.sa_handler = on_pull_interrupt};
    sigaction(SIGINT, &sa, nullptr);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL          *curl   = curl_easy_init();
    unsigned char  len8[8];
    char          *header = nullptr;
    struct tensor *t      = nullptr;
    struct json    j      = {};
    FILE          *out    = nullptr;
    int            rc     = 1;
    size_t         count  = 0;
    uint64_t       n = 0, data_at = 0, total = 0;
    struct range_sink s = {.data = (char *) len8};
    if (!curl)
        goto done;
    easy_setup(curl, url);
    if (!fetch_range(curl, 0, 8, &s)) {
        fprintf(stderr, "geistr: vision tower: cannot read %s\n", m->vision_url);
        goto done;
    }
    for (int k = 7; k >= 0; k--)
        n = n << 8 | len8[k];
    if (n < 2 || n > VISION_HEADER_MAX || !(header = malloc(n + 1))) {
        fprintf(stderr, "geistr: vision tower: not a safetensors header\n");
        goto done;
    }
    s = (struct range_sink) {.data = header};
    if (!fetch_range(curl, 8, 8 + n, &s))
        goto done;
    header[n] = 0;
    data_at   = 8 + n;
    if (!json_parse(&j, header, n) || !(t = calloc((size_t) j.n, sizeof *t)))
        goto done;
    /* top-level keys are the tensors (parent 0), each followed by its object */
    for (int i = 1; i + 1 < j.n; i++) {
        size_t      kn  = 0;
        const char *key = json_raw(&j, i, &kn);
        if (json_parent(&j, i) != 0 || !vision_tensor(key, kn))
            continue;
        int    v       = i + 1, offsets = json_field(&j, v, "data_offsets");
        int    a       = json_next(&j, offsets, -1), b = json_next(&j, offsets, a);
        struct tensor x = {.name = key, .n_name = kn,
                           .from = (uint64_t) json_number(&j, a, -1), .to = (uint64_t) json_number(&j, b, -1)};
        x.dtype = json_raw(&j, json_field(&j, v, "dtype"), &x.n_dtype);
        x.shape = json_raw(&j, json_field(&j, v, "shape"), &x.n_shape);
        if (!x.dtype || !x.shape || a < 0 || b < 0 || x.to < x.from)
            goto done;
        t[count++] = x, total += x.to - x.from;
    }
    if (!count) {
        fprintf(stderr, "geistr: vision tower: no vision tensors in %s\n", m->vision_url);
        goto done;
    }
    qsort(t, count, sizeof *t, by_offset);
    /* the new header: the tensors in the source's order of data, offsets from 0 */
    char  *head = nullptr;
    size_t head_n = 0;
    FILE  *h      = open_memstream(&head, &head_n);
    if (!h)
        goto done;
    fputc('{', h);
    for (size_t k = 0, at = 0; k < count; k++) {
        fprintf(h, "%s\"%.*s\":{\"dtype\":\"%.*s\",\"shape\":%.*s,\"data_offsets\":[%zu,%zu]}", k ? "," : "",
                (int) t[k].n_name, t[k].name, (int) t[k].n_dtype, t[k].dtype, (int) t[k].n_shape, t[k].shape, at,
                at + (size_t) (t[k].to - t[k].from));
        at += (size_t) (t[k].to - t[k].from);
    }
    fputc('}', h);
    while (ftell(h) % 8) /* the data section starts aligned */
        fputc(' ', h);
    fclose(h);
    unsigned char hlen[8];
    for (int k = 0; k < 8; k++)
        hlen[k] = (unsigned char) ((uint64_t) head_n >> (8 * k));
    if (8 + head_n + total != m->vision_bytes) { /* known before the download */
        fprintf(stderr, "geistr: vision tower for %s: %ju bytes, the catalog says %ju; the catalog may be out of date\n",
                m->id, (uintmax_t) (8 + head_n + total), (uintmax_t) m->vision_bytes);
        free(head);
        goto done;
    }
    out = make_dirs(dir, 0755) ? fopen(part, "wb") : nullptr;
    bool ok = out && head && fwrite(hlen, 1, 8, out) == 8 && fwrite(head, 1, head_n, out) == head_n;
    free(head);
    if (!ok)
        goto done;
    /* the data: neighbouring tensors in one range each */
    s = (struct range_sink) {.file = out, .total = total};
    for (size_t k = 0; k < count && ok;) {
        size_t e = k;
        while (e + 1 < count && t[e + 1].from == t[e].to)
            e++;
        ok = fetch_range(curl, data_at + t[k].from, data_at + t[e].to, &s);
        s.done += t[e].to - t[k].from;
        k = e + 1;
    }
    if (isatty(2))
        fputc('\n', stderr);
    if (fclose(out) != 0)
        ok = false;
    out = nullptr;
    if (!ok) {
        fprintf(stderr, "geistr: vision tower: download %s\n", stop ? "stopped (Ctrl-C)" : "failed; run geistr pull again");
        unlink(part);
        rc = stop ? 130 : 1;
        goto done;
    }
    geistr_status checked = GEISTR_IO;
    if (rename(part, target) != 0 || (checked = geistr_catalog_check(&tower, dir, true, &state)) != GEISTR_OK ||
        state != GEISTR_INSTALL_OK) {
        if (checked == GEISTR_OK) /* only a file known to be wrong goes */
            unlink(target);
        fprintf(stderr, "geistr: vision tower for %s does not match its SHA-256; removed\n", m->id);
        goto done;
    }
    printf("✓ vision tower for %s installed\n", m->id);
    rc = 0;
done:
    if (out)
        fclose(out), unlink(part);
    json_free(&j);
    free(t);
    free(header);
    if (curl)
        curl_easy_cleanup(curl);
    curl_global_cleanup();
    return rc;
}

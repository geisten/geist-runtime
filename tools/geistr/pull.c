/* pull.c — geistr's download module (#11): libcurl, https only, resumable,
 * verified before the file takes its final name. Moved from geist-serve's
 * download_model (src/app/jobs.c). */
#include "pull.h"
#include "cli.h"
#include <curl/curl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_interrupt(int signal) {
    (void) signal;
    stop = 1;
}

struct sink {
    FILE    *file;
    uint64_t offset, bytes, limit;
};

static size_t on_data(char *data, size_t size, size_t count, void *context) {
    struct sink *s = context;
    size_t       n = size * count;
    if (n > s->limit - s->bytes || fwrite(data, 1, n, s->file) != n)
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

int geistr_pull(const geistr_catalog_entry *m, const char *dir) {
    char target[4200], part[4300], url[2048];
    snprintf(target, sizeof target, "%s/%s", dir, m->file);
    snprintf(part, sizeof part, "%s.part", target);
    snprintf(url, sizeof url, "%s", m->url);
#ifdef GEISTR_TESTING
    const char *base = getenv("GEISTR_TEST_URL_BASE"); /* replaces https://huggingface.co */
    if (base)
        snprintf(url, sizeof url, "%s%s", base, m->url + strlen("https://huggingface.co"));
#endif
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
    struct sigaction sa = {.sa_handler = on_interrupt};
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
        if (!ok)
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
     * Anything but a match is removed; installed means verified. */
    fprintf(stderr, "verifying %s …\n", m->id);
    geistr_install state = GEISTR_INSTALL_MISMATCH;
    if (rename(part, target) != 0) {
        fprintf(stderr, "geistr: cannot install %s: %s\n", target, strerror(errno));
        return 1;
    }
    if (geistr_catalog_check(m, dir, true, &state) != GEISTR_OK || state != GEISTR_INSTALL_OK) {
        unlink(target);
        fprintf(stderr, "geistr: %s does not match its SHA-256; removed, try again\n", m->id);
        return 1;
    }
    printf("✓ %s installed\n", m->id);
    return 0;
}

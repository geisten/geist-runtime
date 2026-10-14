/* files.c — your own files in the chat (#91): geistr index DIR and
 * chat --files DIR. A folder's text (.txt, .md, …; PDF through pdftotext when
 * it is installed) in chunks of about a paragraph, each embedded by the
 * catalog's embedding model; the index lives in the data folder (0600) and is
 * brought up to date by file hash. A question gets the nearest chunks before
 * it, with their files. Nothing leaves the computer. */
#define _XOPEN_SOURCE 700 /* realpath (musl declares it only for XSI) */
#include "files.h"
#include "cli.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define CHUNK    1200     /* bytes a chunk aims at: a few paragraphs, ~300 tokens */
#define TOP      4        /* chunks put before a question */
#define NEAREST  8        /* with a reranker: the nearest chunks it judges */
#define FILE_MAX (8 << 20) /* larger files are left out */
#define DIMS_MAX 8192

/* The index file's first line. A new one when FILES_PASSAGE_PREFIX or how
 * chunks are embedded changes: an index of another version is made anew. */
#define INDEX_VERSION "geistr-index 1\n"

struct chunk {
    char  *path; /* relative to the folder */
    char   sha[65];
    char  *text;
    float *vec;
};

struct files {
    geistr_model *model;
    geistr_model *rerank; /* the catalog's reranker when installed (#150), else nullptr */
    bool          rerank_tried; /* opened at the first question, not for geistr index */
    char          dir[4096], index[4400], model_file[256];
    size_t        dims;
    struct chunk *c;
    size_t        n, cap;
    float         query[DIMS_MAX];
};

static bool push(struct files *f, struct chunk c) {
    if (f->n == f->cap) {
        size_t        cap  = f->cap ? f->cap * 2 : 256;
        struct chunk *grow = realloc(f->c, cap * sizeof *grow);
        if (!grow)
            return false;
        f->c = grow, f->cap = cap;
    }
    f->c[f->n++] = c;
    return true;
}

static void chunks_free(struct chunk *c, size_t n) {
    for (size_t i = 0; i < n; i++)
        free(c[i].path), free(c[i].text), free(c[i].vec);
    free(c);
}

/* prefix and text embedded into vec */
static geistr_status embed(struct files *f, const char *prefix, const char *text, float *vec, size_t cap,
                           size_t *dims) {
    if (!*prefix)
        return geistr_embed(f->model, text, vec, cap, dims, nullptr);
    size_t n = strlen(prefix) + strlen(text) + 1;
    char  *s = malloc(n);
    if (!s)
        return GEISTR_NO_MEMORY;
    snprintf(s, n, "%s%s", prefix, text);
    geistr_status status = geistr_embed(f->model, s, vec, cap, dims, nullptr);
    free(s);
    return status;
}

/* ---- the folder -------------------------------------------------------------- */

static bool wanted(const char *name) {
    static const char *const kinds[] = {".txt", ".md", ".markdown", ".rst", ".org", ".text", ".pdf", nullptr};
    const char              *dot     = strrchr(name, '.');
    for (size_t i = 0; dot && kinds[i]; i++)
        if (!strcasecmp(dot, kinds[i]))
            return true;
    return false;
}

struct paths {
    char **v;
    size_t n, cap;
};

/* The wanted files below dir/rel, hidden ones left out, symlinks not followed. */
static void walk(const char *dir, const char *rel, struct paths *p, int depth) {
    char path[8300];
    snprintf(path, sizeof path, "%s%s%s", dir, *rel ? "/" : "", rel);
    DIR *d = depth < 32 ? opendir(path) : nullptr;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.')
            continue;
        char sub[4300], full[8300];
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", dir, sub);
        struct stat st;
        if (lstat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            walk(dir, sub, p, depth + 1);
        else if (S_ISREG(st.st_mode) && st.st_size <= FILE_MAX && wanted(e->d_name)) {
            if (p->n == p->cap) {
                size_t cap  = p->cap ? p->cap * 2 : 64;
                char **grow = realloc(p->v, cap * sizeof *grow);
                if (!grow)
                    break;
                p->v = grow, p->cap = cap;
            }
            if (!(p->v[p->n] = strdup(sub)))
                break;
            p->n++;
        }
    }
    if (d)
        closedir(d);
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(char *const *) a, *(char *const *) b);
}

/* A file's text (malloc'd); a PDF's through pdftotext, nullptr without it. */
static char *read_text(const char *path) {
    const char *dot = strrchr(path, '.');
    FILE       *in  = nullptr;
    pid_t       pid = 0;
    if (dot && !strcasecmp(dot, ".pdf")) {
        int fd[2];
        if (pipe(fd) != 0)
            return nullptr;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
        posix_spawn_file_actions_addclose(&fa, fd[0]);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        char *argv[] = {"pdftotext", "-q", "-enc", "UTF-8", (char *) path, "-", nullptr};
        int   rc     = posix_spawnp(&pid, "pdftotext", &fa, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        close(fd[1]);
        if (rc != 0) {
            close(fd[0]);
            return nullptr;
        }
        in = fdopen(fd[0], "r");
    } else
        in = fopen(path, "r");
    char  *text = nullptr;
    size_t len = 0, cap = 0, got;
    char   block[65536];
    while (in && (got = fread(block, 1, sizeof block, in)) > 0 && len + got <= FILE_MAX) {
        if (len + got + 1 > cap) {
            char *grow = realloc(text, cap = (len + got + 1) * 2);
            if (!grow)
                break;
            text = grow;
        }
        memcpy(text + len, block, got), len += got;
    }
    if (in)
        fclose(in);
    int status = 0;
    if (pid > 0 && (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0))
        free(text), text = nullptr;
    if (text)
        text[len] = 0;
    else if (!pid)
        text = in ? strdup("") : nullptr;
    return text;
}

/* Paragraphs gathered into chunks of about CHUNK bytes; a longer paragraph
 * is cut at a space. ponytail: bytes, not tokens; a cut never splits a
 * UTF-8 character but may split a sentence. */
static size_t next_chunk(const char *s, size_t len, size_t at, size_t *end) {
    while (at < len && strchr(" \t\r\n", s[at]))
        at++;
    if (at >= len)
        return *end = len, len;
    size_t e = at;
    for (;;) {
        const char *gap  = strstr(s + e, "\n\n");
        size_t      next = gap ? (size_t) (gap - s) + 2 : len;
        if (next - at > CHUNK && e > at)
            break; /* the next paragraph would make it too long */
        e = next;
        if (e - at >= CHUNK || e >= len)
            break;
    }
    if (e - at > 2 * CHUNK) { /* one long paragraph: cut at a space */
        e = at + CHUNK;
        while (e > at + CHUNK / 2 && s[e] != ' ' && s[e] != '\n')
            e--;
        while (e < len && ((unsigned char) s[e] & 0xC0) == 0x80)
            e++;
    }
    *end = e;
    return at;
}

/* ---- the index file ---------------------------------------------------------- */

static bool load(struct files *f, struct chunk **old, size_t *n_old) {
    *old = nullptr, *n_old = 0;
    FILE *in = fopen(f->index, "rb");
    if (!in)
        return errno == ENOENT;
    char   line[4400], dir[4096] = "", model[256] = "";
    size_t dims = 0;
    bool   ok   = fgets(line, sizeof line, in) && !strcmp(line, INDEX_VERSION) && fgets(dir, sizeof dir, in) &&
                fgets(line, sizeof line, in) && sscanf(line, "%255s %zu", model, &dims) == 2;
    dir[strcspn(dir, "\n")] = 0;
    /* another model's vectors do not compare: the folder is indexed anew */
    ok = ok && !strcmp(dir, f->dir) && !strcmp(model, f->model_file) && dims == f->dims;
    struct files was = {.dims = dims};
    while (ok && fgets(line, sizeof line, in)) {
        struct chunk c = {};
        size_t       path_len = 0, text_len = 0;
        ok = sscanf(line, "%64s %zu %zu", c.sha, &path_len, &text_len) == 3 && path_len < 4096 &&
             text_len <= FILE_MAX && (c.path = calloc(path_len + 1, 1)) && (c.text = calloc(text_len + 1, 1)) &&
             (c.vec = malloc(dims * sizeof *c.vec)) && fread(c.path, 1, path_len, in) == path_len &&
             fread(c.text, 1, text_len, in) == text_len && fread(c.vec, sizeof *c.vec, dims, in) == dims &&
             push(&was, c);
        if (!ok)
            free(c.path), free(c.text), free(c.vec);
    }
    fclose(in);
    if (!ok) /* damaged or of another model: indexed anew */
        chunks_free(was.c, was.n), was.c = nullptr, was.n = 0;
    *old = was.c, *n_old = was.n;
    return true;
}

static bool save(const struct files *f) {
    char part[4500];
    snprintf(part, sizeof part, "%s.%ld", f->index, (long) getpid());
    int   fd  = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0600); /* private: your files' text */
    FILE *out = fd >= 0 ? fdopen(fd, "wb") : nullptr;
    bool  ok  = out && fprintf(out, INDEX_VERSION "%s\n%s %zu\n", f->dir, f->model_file, f->dims) > 0;
    for (size_t i = 0; ok && i < f->n; i++) {
        const struct chunk *c = &f->c[i];
        ok = fprintf(out, "%s %zu %zu\n", c->sha, strlen(c->path), strlen(c->text)) > 0 &&
             fwrite(c->path, 1, strlen(c->path), out) == strlen(c->path) &&
             fwrite(c->text, 1, strlen(c->text), out) == strlen(c->text) &&
             fwrite(c->vec, sizeof *c->vec, f->dims, out) == f->dims;
    }
    if (out)
        ok = fclose(out) == 0 && ok;
    else if (fd >= 0)
        close(fd);
    ok = ok && rename(part, f->index) == 0;
    if (!ok)
        unlink(part);
    return ok;
}

/* ---- open: the model, the index brought up to date -------------------------- */

static uint64_t fnv(const char *s) {
    uint64_t h = 14695981039346656037ull;
    while (*s)
        h = (h ^ (unsigned char) *s++) * 1099511628211ull;
    return h;
}

/* The catalog's first installed embedding model. */
static bool embedding_model(char *path, size_t cap, char *file, size_t file_cap) {
    geistr_catalog *c  = load_catalog();
    const char     *id = nullptr;
    for (size_t i = 0; c && i < geistr_catalog_count(c) && !id; i++)
        if (geistr_catalog_get(c, i)->embedding == 1)
            id = geistr_catalog_get(c, i)->id;
    char             wanted_id[64] = "";
    geistr_reasoning reasoning;
    if (id) {
        snprintf(wanted_id, sizeof wanted_id, "%s", id);
        snprintf(file, file_cap, "%s", geistr_catalog_find(c, id)->file);
    }
    geistr_catalog_free(c);
    if (!id) {
        fputs("geistr: the catalog has no embedding model\n", stderr);
        return false;
    }
    path[0] = 0;
    return resolve(wanted_id, path, cap, &reasoning, nullptr) == OK; /* says: geistr pull … */
}

/* The catalog's reranker (#150) when it is installed; else nullptr and a
 * hint how to get it. Never a download prompt: --files works without it. */
static geistr_model *reranker_open(void) {
    geistr_catalog             *c = load_catalog();
    const geistr_catalog_entry *m = nullptr;
    for (size_t i = 0; c && i < geistr_catalog_count(c) && !m; i++)
        if (geistr_catalog_get(c, i)->embedding == 2)
            m = geistr_catalog_get(c, i);
    geistr_install state = GEISTR_INSTALL_MISSING;
    char           path[4400];
    if (m && (geistr_catalog_check(m, models_dir, true, &state) != GEISTR_OK || state != GEISTR_INSTALL_OK))
        fprintf(stderr, "geistr: sources name all files searched; for only the ones that answer: geistr pull %s\n",
                m->id);
    geistr_model *model = nullptr;
    if (m && state == GEISTR_INSTALL_OK) {
        snprintf(path, sizeof path, "%s/%s", models_dir, m->file);
        char              error[256];
        geistr_model_opts mo = GEISTR_MODEL_OPTS_INIT;
        mo.processor         = GEISTR_PROCESSOR_GPU; /* 0.63 s a question on an M1 Max's GPU, 6.3 s on its CPU */
        mo.context           = 2048;                 /* a question and a piece in the reranker's prompt */
        mo.threads           = engine_threads();
        if (geistr_model_open(path, &mo, &model, error, sizeof error) != GEISTR_OK)
            mo.processor = GEISTR_PROCESSOR_CPU, model = nullptr;
        if (!model && geistr_model_open(path, &mo, &model, error, sizeof error) != GEISTR_OK)
            fprintf(stderr, "geistr: cannot open the reranker (%s): sources name all files searched\n", error);
    }
    geistr_catalog_free(c);
    return model;
}

struct files *files_open(const char *dir, bool quiet) {
    struct files *f = calloc(1, sizeof *f);
    char          model_path[4200];
    if (!f)
        return nullptr;
    if (!realpath(dir, f->dir)) {
        fprintf(stderr, "geistr: %s: %s\n", dir, strerror(errno));
        return files_close(f), nullptr;
    }
    char folder[4200];
    snprintf(folder, sizeof folder, "%s/index", data_dir);
    if (!data_dir[0] || !make_dirs(folder, 0700)) {
        fputs("geistr: no data folder for the index (set HOME or GEISTEN_HOME)\n", stderr);
        return files_close(f), nullptr;
    }
    snprintf(f->index, sizeof f->index, "%s/%016llx.idx", folder, (unsigned long long) fnv(f->dir));
    if (!embedding_model(model_path, sizeof model_path, f->model_file, sizeof f->model_file))
        return files_close(f), nullptr;
    char              error[256];
    geistr_model_opts mo = GEISTR_MODEL_OPTS_INIT;
    mo.processor         = GEISTR_PROCESSOR_CPU;
    mo.context           = 2048; /* a chunk or a question */
    mo.threads           = engine_threads();
    if (geistr_model_open(model_path, &mo, &f->model, error, sizeof error) != GEISTR_OK) {
        fprintf(stderr, "geistr: cannot open the embedding model: %s\n", error);
        return files_close(f), nullptr;
    }
    size_t dims = 0;
    (void) geistr_embed(f->model, "dimension", nullptr, 0, &dims, nullptr); /* no room: tells the size */
    if (!dims || dims > DIMS_MAX) {
        fprintf(stderr, "geistr: the embedding model does not embed: %s\n", geistr_model_error(f->model));
        return files_close(f), nullptr;
    }
    f->dims = dims;

    struct chunk *old   = nullptr;
    size_t        n_old = 0;
    if (!load(f, &old, &n_old)) {
        fprintf(stderr, "geistr: cannot read the index %s\n", f->index);
        return files_close(f), nullptr;
    }
    struct paths p = {};
    walk(f->dir, "", &p, 0);
    qsort(p.v, p.n, sizeof *p.v, by_name);
    size_t files = 0, fresh = 0;
    bool   ok = true;
    for (size_t i = 0; i < p.n && ok && !interrupted; i++) {
        char full[8300], sha[65];
        snprintf(full, sizeof full, "%s/%s", f->dir, p.v[i]);
        if (geistr_sha256_file(full, sha) != GEISTR_OK)
            continue;
        size_t was = 0; /* unchanged: its chunks as they were */
        for (size_t k = 0; k < n_old; k++)
            if (old[k].path && !strcmp(old[k].path, p.v[i]) && !strcmp(old[k].sha, sha)) {
                ok  = push(f, old[k]);
                old[k] = (struct chunk) {}, was++;
            }
        if (was) {
            files++;
            continue;
        }
        char       *text = read_text(full);
        const char *dot  = strrchr(p.v[i], '.');
        const bool  pdf  = dot && !strcasecmp(dot, ".pdf");
        if (!text || (pdf && !text[strspn(text, " \t\r\n\f")])) { /* left out, and said why (#173) */
            if (!quiet)
                fprintf(stderr, "%sleft out %s: %s\n", isatty(2) ? "\r\033[K" : "", p.v[i],
                        !pdf    ? "cannot read it"
                        : text  ? "no text in it (a scanned PDF?)"
                                : "pdftotext could not read it, or is missing (macOS: brew install poppler; Linux: poppler-utils)");
            free(text);
            continue;
        }
        if (!quiet && isatty(2))
            fprintf(stderr, "\rindexing %zu/%zu: %.60s\033[K", i + 1, p.n, p.v[i]);
        size_t len = strlen(text), end = 0;
        for (size_t at = next_chunk(text, len, 0, &end); ok && at < len; at = next_chunk(text, len, end, &end)) {
            struct chunk c = {.path = strdup(p.v[i]), .text = strndup(text + at, end - at),
                              .vec = malloc(f->dims * sizeof *c.vec)};
            size_t got = 0;
            memcpy(c.sha, sha, sizeof c.sha);
            ok = c.path && c.text && c.vec;
            geistr_status s = ok ? embed(f, FILES_PASSAGE_PREFIX, c.text, c.vec, f->dims, &got) : GEISTR_NO_MEMORY;
            if (s != GEISTR_OK || !push(f, c)) {
                free(c.path), free(c.text), free(c.vec);
                ok = s == GEISTR_CONTEXT; /* ponytail: an overlong chunk (dense non-Latin text) is left out */
                continue;
            }
            fresh++;
        }
        files++;
        free(text);
    }
    if (!quiet && isatty(2))
        fputs("\r\033[K", stderr);
    for (size_t i = 0; i < p.n; i++)
        free(p.v[i]);
    free(p.v);
    chunks_free(old, n_old); /* what went: removed or changed files */
    if (!ok || interrupted || !save(f)) {
        fprintf(stderr, "geistr: indexing %s %s\n", f->dir, interrupted ? "stopped" : "failed");
        return files_close(f), nullptr;
    }
    if (!quiet)
        fprintf(stderr, "%s: %zu file%s, %zu chunks (%zu new)\n", f->dir, files, files == 1 ? "" : "s", f->n, fresh);
    return f;
}

void files_close(struct files *f) {
    if (!f)
        return;
    chunks_free(f->c, f->n);
    geistr_model_close(f->model);
    geistr_model_close(f->rerank);
    free(f);
}

const char *files_dir(const struct files *f) {
    return f->dir;
}

/* ---- a question -------------------------------------------------------------- */

/* The reranker (Qwen3-Reranker-0.6B, #150) judges a question and a piece of
 * a chunk (a paragraph) in the prompt of its model card; a chunk answers when
 * P("yes") over "yes" and "no" reaches RERANK_MIN for one of its pieces.
 * Measured with scripts/retrieval-bench.py --sources --rerank-gguf --pieces
 * (55 questions, 16 off-topic): the 8 nearest chunks, this instruction (in
 * English, as Qwen recommends) and 0.2 named the right files with precision
 * 0.95, recall 0.90, and nothing for 88 % of the off-topic questions; whole
 * chunks reached a recall of 0.74 at most. */
#define RERANK_INSTRUCT \
    "Given a question, judge whether the passage from the user's notes contains the information that answers it"
#define RERANK_MIN 0.2f
#define PIECE      400 /* bytes a piece aims at at most: a paragraph, a long one cut after a sentence */
#define PIECE_MIN  80  /* a shorter paragraph (a heading) goes with the next */

/* P(yes) for text[0..len), or -1 when the reranker failed. */
static float relevance(geistr_model *r, const char *question, const char *text, size_t len) {
    static const char form[] =
            "<|im_start|>system\nJudge whether the Document meets the requirements based on the Query and the "
            "Instruct provided. Note that the answer can only be \"yes\" or \"no\".<|im_end|>\n<|im_start|>user\n"
            "<Instruct>: %s\n<Query>: %s\n<Document>: %.*s<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
    size_t n      = sizeof form + strlen(RERANK_INSTRUCT) + strlen(question) + len;
    char  *prompt = malloc(n);
    if (!prompt || len > INT_MAX)
        return free(prompt), -1;
    snprintf(prompt, n, form, RERANK_INSTRUCT, question, (int) len, text);
    static const char *const yes_no[] = {"yes", "no"};
    float                    logit[2];
    geistr_status            s = geistr_logits(r, prompt, 2, yes_no, logit);
    free(prompt);
    return s == GEISTR_OK ? 1.0f / (1.0f + expf(logit[1] - logit[0])) : -1;
}

/* Where a piece of at most PIECE bytes from s ends in s[0..len): after the
 * last sentence (". ", "! ", "? ") or line that fits, else at a space. */
static size_t piece_end(const char *s, size_t len) {
    if (len <= PIECE)
        return len;
    size_t space = 0;
    for (size_t i = PIECE; i > 0; i--) {
        if (s[i] == '\n' || ((s[i] == ' ') && strchr(".!?", s[i - 1])))
            return i;
        if (!space && s[i] == ' ')
            space = i;
    }
    if (space)
        return space;
    size_t e = PIECE; /* one long word: never inside a UTF-8 character */
    while (e < len && ((unsigned char) s[e] & 0xC0) == 0x80)
        e++;
    return e;
}

/* The best P(yes) of the chunk's pieces, or -1 when the reranker failed. */
static float chunk_relevance(geistr_model *r, const char *question, const char *text) {
    float  best = 0;
    size_t len = strlen(text), at = 0;
    while (at < len) {
        while (at < len && strchr(" \t\r\n", text[at]))
            at++;
        if (at >= len)
            break;
        size_t      end = at; /* paragraphs until the piece is long enough */
        const char *gap;
        do {
            gap = strstr(text + end, "\n\n");
            end = gap ? (size_t) (gap - text) : len;
        } while (end - at < PIECE_MIN && gap && (end += 2) < len);
        for (size_t from = at; from < end;) { /* a long paragraph in sentences */
            size_t n = piece_end(text + from, end - from);
            float  p = relevance(r, question, text + from, n);
            if (p < 0)
                return -1;
            best = p > best ? p : best;
            from += n;
            while (from < end && strchr(" \t\r\n", text[from]))
                from++;
        }
        at = end;
    }
    return best;
}

char *files_ask(struct files *f, const char *question, char *sources, size_t cap, bool *nothing) {
    sources[0] = 0;
    *nothing   = false;
    if (!f->n)
        return strdup(question);
    size_t        dims = 0;
    geistr_status s    = embed(f, FILES_QUERY_PREFIX, question, f->query, DIMS_MAX, &dims);
    if (s != GEISTR_OK || dims != f->dims)
        return nullptr;
    if (!f->rerank_tried)
        f->rerank = reranker_open(), f->rerank_tried = true;
    const size_t want = f->rerank ? NEAREST : TOP;
    size_t       best[NEAREST];
    float        score[NEAREST];
    size_t       k = 0;
    for (size_t i = 0; i < f->n; i++) { /* the want nearest, nearest first */
        float d = 0;
        for (size_t j = 0; j < dims; j++)
            d += f->query[j] * f->c[i].vec[j];
        size_t at = k < want ? k++ : want;
        while (at > 0 && score[at - 1] < d) {
            if (at < want)
                best[at] = best[at - 1], score[at] = score[at - 1];
            at--;
        }
        if (at < want)
            best[at] = i, score[at] = d;
    }
    if (f->rerank) { /* only the chunks that answer, the most likely first, at most TOP */
        size_t pick[TOP], kept = 0;
        float  prob[TOP];
        bool   failed = false;
        for (size_t i = 0; i < k && !failed; i++) {
            float p = chunk_relevance(f->rerank, question, f->c[best[i]].text);
            if ((failed = p < 0)) { /* the nearest, as without a reranker */
                fprintf(stderr, "geistr: the reranker failed (%s): sources name all files searched\n",
                        geistr_model_error(f->rerank));
                geistr_model_close(f->rerank), f->rerank = nullptr;
            }
            if (failed || p < RERANK_MIN)
                continue;
            size_t at = kept < TOP ? kept++ : TOP;
            while (at > 0 && prob[at - 1] < p) {
                if (at < TOP)
                    pick[at] = pick[at - 1], prob[at] = prob[at - 1];
                at--;
            }
            if (at < TOP)
                pick[at] = best[i], prob[at] = p;
        }
        if (!failed) {
            memcpy(best, pick, kept * sizeof *pick), k = kept;
            if (!k) { /* nothing relevant: the question alone */
                *nothing = true;
                return strdup(question);
            }
        }
    }
    if (k > TOP)
        k = TOP;
    char  *out = nullptr;
    size_t len = 0;
    FILE  *m   = open_memstream(&out, &len);
    if (!m)
        return nullptr;
    fputs("Excerpts from my files (use them if they help, and say which file you used):\n\n", m);
    for (size_t i = 0; i < k; i++) {
        const struct chunk *c = &f->c[best[i]];
        fprintf(m, "[%s]\n%s\n\n", c->path, c->text);
        bool named = false; /* sources: each file once, nearest first */
        for (size_t j = 0; j < i; j++)
            named |= !strcmp(f->c[best[j]].path, c->path);
        size_t at = strlen(sources);
        if (!named && at + strlen(c->path) + 3 < cap)
            snprintf(sources + at, cap - at, "%s%s", at ? ", " : "", c->path);
    }
    fprintf(m, "My question: %s", question);
    fclose(m);
    return out;
}

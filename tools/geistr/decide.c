#include "decide.h"
#include "geistr_catalog.h"
#include "geistr_decision.h"
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double milliseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1000000.0;
}
static const char *command = "decide"; /* decide or decide-batch, for the messages */
static int failure(int code, const char *message) {
    fprintf(stderr, "geistr %s: %s\n", command, message);
    return code;
}
static geistr_status read_bounded(size_t *length, size_t limit, const char *path, char **out) {
    *out = nullptr;
    *length = 0;
    size_t capacity;
    if (ckd_add(&capacity, limit, 1))
        return GEISTR_INVALID;
    FILE *in = !strcmp(path, "-") ? stdin : fopen(path, "rb");
    if (!in)
        return GEISTR_IO;
    char *buf = malloc(capacity);
    if (!buf) {
        if (in != stdin)
            fclose(in);
        return GEISTR_NO_MEMORY;
    }
    size_t n = fread(buf, 1, capacity, in);
    bool bad = ferror(in) || n > limit;
    if (in != stdin)
        fclose(in);
    if (bad) {
        free(buf);
        return n > limit ? GEISTR_INVALID : GEISTR_IO;
    }
    /* The extra byte is available for the terminator after bounded reading. */
    buf[n] = 0;
    *length = n;
    *out = buf;
    return GEISTR_OK;
}
geistr_status geistr_decide_config_read(size_t error_cap, const char *path, geistr_decision_config **out,
                                        char *error) {
    if (!out)
        return GEISTR_INVALID;
    *out = nullptr;
    char *json = nullptr;
    size_t n = 0;
    geistr_status status = path ? read_bounded(&n, GEISTR_DECISION_MAX_CONFIG, path, &json) : GEISTR_INVALID;
    if (status == GEISTR_OK)
        status = geistr_decision_config_parse(n, error_cap, json, out, error);
    else if (error && error_cap)
        snprintf(error, error_cap, "configuration unreadable or too large");
    free(json);
    return status;
}
/* Without --config: every verified profile of the registry (#162), enabled
 * for its exact artifact. A --config file replaces this list, so it can still
 * withhold or disable a model. */
static geistr_status builtin_config(size_t error_cap, geistr_decision_config **out, char *error) {
    char json[4096];
    size_t used = (size_t)snprintf(json, sizeof json, "{\"schema\":1,\"models\":[");
    for (int p = GEISTR_DECISION_PROFILE_NONE + 1; p < GEISTR_DECISION_MAX_POLICIES; ++p) {
        const char *sha = geistr_decision_profile_sha256((geistr_decision_profile)p);
        if (!*sha)
            break;
        int n = snprintf(json + used, sizeof json - used, "%s{\"sha256\":\"%s\",\"enabled\":true,\"profile\":\"%s\"}",
                         p > GEISTR_DECISION_PROFILE_NONE + 1 ? "," : "", sha,
                         geistr_decision_profile_name((geistr_decision_profile)p));
        if (n < 0 || (size_t)n >= sizeof json - used)
            return GEISTR_INVALID;
        used += (size_t)n;
    }
    int n = snprintf(json + used, sizeof json - used, "]}");
    if (n < 0 || (size_t)n >= sizeof json - used)
        return GEISTR_INVALID;
    return geistr_decision_config_parse(used + (size_t)n, error_cap, json, out, error);
}
static bool argument(size_t argc, size_t *index, const char *const *argv, const char **value) {
    if (*index + 1 >= argc || !argv[*index + 1])
        return false;
    *value = argv[++*index];
    return true;
}

/* Configuration, catalog/path lookup with SHA, permission and model load,
 * shared by decide and decide-batch. Returns 0 or the exit code; unload frees
 * whatever was set, also after a failure. */
struct loaded {
    char *catalog_json;
    geistr_decision_config *config;
    geistr_catalog *catalog;
    geistr_model *model;
    const geistr_decision_policy *policy;
    char sha[65];
};
static void unload(struct loaded *l) {
    geistr_model_close(l->model);
    geistr_catalog_free(l->catalog);
    geistr_decision_config_free(l->config);
    free(l->catalog_json);
    *l = (struct loaded){};
}
static int load(struct loaded *l, const char *model, const char *config_file, const char *profile,
                const char *models_dir, const char *catalog_file, const geistr_decide_host *host,
                geistr_processor processor, uint32_t context) {
    char error[256];
    size_t n = 0;
    geistr_status status = config_file ? geistr_decide_config_read(sizeof error, config_file, &l->config, error)
                                       : builtin_config(sizeof error, &l->config, error);
    if (status != GEISTR_OK)
        return failure(1, error);
    char path[4096], directory[4096];
    const char *selected_path = model;
    if (!strchr(model, '/') && !strstr(model, ".gguf")) {
        if (host && host->catalog)
            l->catalog = host->catalog(models_dir, catalog_file);
        else if (catalog_file && read_bounded(&n, 1u << 20, catalog_file, &l->catalog_json) == GEISTR_OK)
            status = geistr_catalog_parse(l->catalog_json, n, &l->catalog, error, sizeof error);
        if (!l->catalog)
            return failure(1, "catalog unreadable or invalid");
        const geistr_catalog_entry *entry = geistr_catalog_find(l->catalog, model);
        if (!entry)
            return failure(1, "unknown catalog model ID");
        if (!models_dir) {
            if (geistr_models_dir(directory, sizeof directory) != GEISTR_OK)
                return failure(1, "models directory unavailable");
            models_dir = directory;
        }
        geistr_install install;
        if (geistr_catalog_check(entry, models_dir, true, &install) != GEISTR_OK ||
            install != GEISTR_INSTALL_OK)
            return failure(1, "model not installed and SHA-verified; no download");
        const int written = snprintf(path, sizeof path, "%s/%s", models_dir, entry->file);
        if (written < 0 || (size_t)written >= sizeof path)
            return failure(1, "model path too long");
        selected_path = path;
        memcpy(l->sha, entry->sha256, sizeof l->sha);
    } else if (geistr_sha256_file(selected_path, l->sha) != GEISTR_OK)
        return failure(1, "model artifact unreadable");
    l->policy = geistr_decision_config_find(l->sha, l->config);
    if (!l->policy || !l->policy->enabled)
        return failure(1, "decision permission absent or disabled for this artifact");
    if (profile && strcmp(profile, geistr_decision_profile_name(l->policy->profile)))
        return failure(1, "profile override does not match the immutable artifact policy");
    geistr_model_opts mo = GEISTR_MODEL_OPTS_INIT;
    mo.decision = l->policy;
    mo.processor = processor;
    mo.context = context;
    status = geistr_model_open(selected_path, &mo, &l->model, error, sizeof error);
    if (status != GEISTR_OK)
        return failure(1, error);
    return 0;
}

int geistr_decide_command(size_t argc, const char *const *argv, const geistr_decide_host *host) {
    if (!argc || argc > 128 || !argv || !argv[0])
        return failure(2, "model and options required; see geistr help");
    const double start = milliseconds();
    const char *question = nullptr, *question_file = nullptr, *context = nullptr;
    const char *config_file = nullptr, *catalog_file = host ? host->catalog_file : nullptr,
               *models_dir = host ? host->models_dir : nullptr, *profile = nullptr;
    geistr_processor processor = GEISTR_PROCESSOR_AUTO;
    geistr_decision_mode mode = GEISTR_DECISION_DEFAULT;
    geistr_decision_option options[GEISTR_DECISION_MAX_OPTIONS];
    size_t count = 0, serialized = 0;
    unsigned seen = 0;
    for (size_t i = 0; i < argc; ++i) {
        if (!argv[i])
            return failure(2, "null argument");
        size_t n = strnlen(argv[i], GEISTR_DECISION_MAX_SERIALIZED + 1);
        if (n > GEISTR_DECISION_MAX_SERIALIZED - serialized)
            return failure(2, "serialized request too large");
        serialized += n;
    }
    for (size_t i = 1; i < argc; ++i) {
        const char *value = nullptr;
        unsigned bit = 0;
        if (strncmp(argv[i], "--", 2)) { /* short form: the question, then option IDs */
            if (!(seen & 1)) {
                seen |= 1;
                question = argv[i];
            } else if (count == GEISTR_DECISION_MAX_OPTIONS)
                return failure(2, "too many options");
            else {
                const size_t n = strnlen(argv[i], GEISTR_DECISION_MAX_ID + 1);
                options[count++] = (geistr_decision_option){n, n, argv[i], argv[i]};
            }
            continue;
        }
        if (!strcmp(argv[i], "--option")) {
            if (count == GEISTR_DECISION_MAX_OPTIONS || !argument(argc, &i, argv, &value))
                return failure(2, "--option requires a bounded ID and description");
            const char *description = nullptr;
            if (!argument(argc, &i, argv, &description))
                return failure(2, "missing option description");
            options[count++] = (geistr_decision_option){
                strnlen(value, GEISTR_DECISION_MAX_ID + 1),
                strnlen(description, GEISTR_DECISION_MAX_DESCRIPTION + 1), value, description};
            continue;
        }
        if (!argument(argc, &i, argv, &value))
            return failure(2, "missing flag value");
        const char *flag = argv[i - 1];
        if (!strcmp(flag, "--question")) {
            bit = 1;
            question = value;
        } else if (!strcmp(flag, "--question-file")) {
            bit = 1;
            question_file = value;
        } else if (!strcmp(flag, "--context")) {
            bit = 2;
            context = value;
        } else if (!strcmp(flag, "--config")) {
            bit = 4;
            config_file = value;
        } else if (!strcmp(flag, "--catalog")) {
            bit = 8;
            catalog_file = value;
        } else if (!strcmp(flag, "--models")) {
            bit = 16;
            models_dir = value;
        } else if (!strcmp(flag, "--profile")) {
            bit = 32;
            profile = value;
        } else if (!strcmp(flag, "--processor")) {
            bit = 64;
            if (!strcmp(value, "cpu"))
                processor = GEISTR_PROCESSOR_CPU;
            else if (!strcmp(value, "gpu"))
                processor = GEISTR_PROCESSOR_GPU;
            else if (!strcmp(value, "auto"))
                processor = GEISTR_PROCESSOR_AUTO;
            else
                return failure(2, "unknown processor");
        } else if (!strcmp(flag, "--mode")) {
            bit = 128;
            if (!strcmp(value, "dense"))
                mode = GEISTR_DECISION_DENSE;
            else if (!strcmp(value, "selected_rows"))
                mode = GEISTR_DECISION_SELECTED_ROWS;
            else
                return failure(2, "unknown numeric mode");
        } else
            return failure(2, "unknown flag");
        if (seen & bit)
            return failure(2, "duplicate/conflicting flag");
        seen |= bit;
    }
    if (!question && !question_file)
        return failure(2, "--question TEXT (or --question-file) required");
    if (!count)
        return failure(2, "at least one --option ID DESC required");
    char *owned_question = nullptr;
    size_t question_len = question ? strnlen(question, GEISTR_DECISION_MAX_QUESTION + 1) : 0;
    if (question_file && read_bounded(&question_len, GEISTR_DECISION_MAX_QUESTION, question_file,
                                      &owned_question) != GEISTR_OK)
        return failure(2, "question file/stdin unreadable or too large");
    if (owned_question)
        question = owned_question;
    geistr_decision_request request = {.size = sizeof request,
                                       .operation = GEISTR_OPERATION_DECISION,
                                       .question_len = question_len,
                                       .context_len =
                                           context ? strnlen(context, GEISTR_DECISION_MAX_CONTEXT + 1) : 0,
                                       .n_options = count,
                                       .question = question,
                                       .context = context,
                                       .options = options};
    char error[256];
    int code = 1;
    struct loaded loaded = {};
    geistr_decision *decision = nullptr;
    geistr_status status = geistr_decision_request_validate(sizeof error, &request, error);
    if (status != GEISTR_OK) {
        code = failure(2, error);
        goto done;
    }
    if (!geistr_decision_available()) {
        code = failure(1, "linked decision engine is disabled; no fallback");
        goto done;
    }
    code = load(&loaded, argv[0], config_file, profile, models_dir, catalog_file, host, processor, 512);
    if (code)
        goto done;
    code = 1;
    const geistr_decision_policy *policy = loaded.policy;
    geistr_model *model = loaded.model;
    const char *sha = loaded.sha;
    geistr_decision_opts opts = GEISTR_DECISION_OPTS_INIT;
    opts.mode = mode;
    status = geistr_decision_open(model, sizeof error, &opts, &decision, error);
    if (status != GEISTR_OK) {
        code = failure(1, error);
        goto done;
    }
    const double ready = milliseconds();
    geistr_decision_result result = {.size = sizeof result};
    if (host && host->active)
        host->active(decision, host->context);
    status = geistr_decision_score(decision, &request, &result);
    if (status != GEISTR_OK) {
        code = failure(status == GEISTR_CANCELLED ? 130 : 1, geistr_decision_error(decision));
        goto done;
    }
    geistr_decision_capability capability = {.size = sizeof capability};
    geistr_decision_plan plan = {.size = sizeof plan};
    if (geistr_decision_capability_get(model, &capability) != GEISTR_OK ||
        geistr_decision_plan_get(decision, &plan) != GEISTR_OK) {
        code = failure(1, "diagnostics unavailable");
        goto done;
    }
    if (!isatty(STDERR_FILENO)) /* the diagnostics are for programs, not for a terminal */
        fprintf(stderr,
            "{\"schema\":1,\"profile\":\"%s\",\"artifact_sha256\":\"%s\",\"template_sha256\":\"%s\","
            "\"runtime\":\"%s\","
            "\"engine_revision\":\"%s\",\"engine\":\"%s\",\"operation\":\"decision\",\"backend\":\"%s\","
            "\"requested_mode\":%d,\"resolved_mode\":%d,\"prompt_tokens\":%zu,\"model_"
            "calls\":%zu,\"setup_ms\":%.3f,\"preparation_ms\":%.3f,\"scoring_ms\":%.3f,\"until_result_ms\":%."
            "3f}\n",
            geistr_decision_profile_name(policy->profile), sha, plan.template_sha256, geistr_version(),
            capability.engine_revision, capability.engine_version, capability.backend, mode, result.mode,
            result.prompt_tokens, result.model_calls, ready - start, result.preparation_ms, result.scoring_ms,
            milliseconds() - start);
    if (printf("%s\n", result.external_id) < 0) {
        code = failure(1, "selection output failed");
        goto done;
    }
    if (isatty(STDOUT_FILENO)) { /* a person reads it: each option's probability too */
        int width = 0;
        for (size_t i = 0; i < result.n_options; ++i)
            if ((int)options[i].id_len > width)
                width = (int)options[i].id_len;
        for (size_t i = 0; i < result.n_options; ++i)
            printf("  %-*.*s  %.2f\n", width, (int)options[i].id_len, options[i].id,
                   result.selection_probabilities[i]);
    }
    if (fflush(stdout)) {
        code = failure(1, "selection output failed");
        goto done;
    }
    code = 0;
done:
    if (host && host->active && decision)
        host->active(nullptr, host->context);
    geistr_decision_close(decision);
    unload(&loaded);
    free(owned_question);
    return code;
}

/* ---- decide-batch (#164): one shared context, 1..32 questions ------------
 * The request is program input in exactly this shape (any key order,
 * whitespace between tokens, every field required and once):
 *   {"context":"…","questions":[{"id":"q1","question":"…",
 *     "options":[{"id":"yes","description":"Yes"},…]},…]}
 * Strings are decoded into one arena as large as the input: no escape
 * decodes to more bytes than it takes. NUL, lone surrogates and raw controls
 * are refused; UTF-8 and the ID rules are geistr_decision_request_validate's. */
struct cursor {
    const char *p, *end;
    char *arena;
};
struct batch_input {
    const char *context;
    size_t context_len;
    geistr_decision_question questions[GEISTR_DECISION_MAX_BATCH];
    geistr_decision_option options[GEISTR_DECISION_MAX_BATCH][GEISTR_DECISION_MAX_OPTIONS];
    geistr_decision_batch batch;
};
static void blank(struct cursor *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        ++c->p;
}
static bool expect(struct cursor *c, char ch) {
    blank(c);
    if (c->p == c->end || *c->p != ch)
        return false;
    ++c->p;
    return true;
}
static bool unit(struct cursor *c, uint32_t *u) {
    *u = 0;
    for (int i = 0; i < 4; ++i, ++c->p) {
        const char h = c->p < c->end ? *c->p : 0;
        const int v = h >= '0' && h <= '9'   ? h - '0'
                      : h >= 'a' && h <= 'f' ? h - 'a' + 10
                      : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                             : -1;
        if (v < 0)
            return false;
        *u = *u << 4 | (uint32_t)v;
    }
    return true;
}
static bool string(struct cursor *c, const char **out, size_t *len) {
    static const char escapes[] = "\"\"\\\\//b\bf\fn\nr\rt\t"; /* escape letter, byte */
    if (!expect(c, '"'))
        return false;
    char *o = c->arena;
    while (c->p < c->end && *c->p != '"') {
        unsigned char ch = (unsigned char)*c->p++;
        if (ch < 0x20)
            return false;
        if (ch != '\\') {
            *o++ = (char)ch;
            continue;
        }
        if (c->p == c->end)
            return false;
        ch = (unsigned char)*c->p++;
        const char *e = escapes;
        while (*e && *e != (char)ch)
            e += 2;
        if (*e) {
            *o++ = e[1];
            continue;
        }
        uint32_t cp, low;
        if (ch != 'u' || !unit(c, &cp) || !cp || (cp >= 0xdc00 && cp <= 0xdfff))
            return false;
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (c->end - c->p < 2 || c->p[0] != '\\' || c->p[1] != 'u')
                return false;
            c->p += 2;
            if (!unit(c, &low) || low < 0xdc00 || low > 0xdfff)
                return false;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        }
        if (cp < 0x80)
            *o++ = (char)cp;
        else if (cp < 0x800) {
            *o++ = (char)(0xc0 | cp >> 6);
            *o++ = (char)(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            *o++ = (char)(0xe0 | cp >> 12);
            *o++ = (char)(0x80 | (cp >> 6 & 0x3f));
            *o++ = (char)(0x80 | (cp & 0x3f));
        } else {
            *o++ = (char)(0xf0 | cp >> 18);
            *o++ = (char)(0x80 | (cp >> 12 & 0x3f));
            *o++ = (char)(0x80 | (cp >> 6 & 0x3f));
            *o++ = (char)(0x80 | (cp & 0x3f));
        }
    }
    if (c->p == c->end)
        return false;
    ++c->p;
    *out = c->arena;
    *len = (size_t)(o - c->arena);
    *o++ = 0;
    c->arena = o;
    return true;
}
/* "key": with key one of names, each at most once; its index, else -1. */
static int key(struct cursor *c, unsigned *seen, size_t n, const char *const *names) {
    const char *k;
    size_t len;
    if (!string(c, &k, &len) || !expect(c, ':'))
        return -1;
    for (size_t i = 0; i < n; ++i)
        if (len == strlen(names[i]) && !memcmp(k, names[i], len) && !(*seen & 1u << i)) {
            *seen |= 1u << i;
            return (int)i;
        }
    return -1;
}
static bool option(struct cursor *c, geistr_decision_option *o) {
    static const char *const names[] = {"id", "description"};
    unsigned seen = 0;
    if (!expect(c, '{'))
        return false;
    do {
        const int k = key(c, &seen, 2, names);
        if (k < 0 || !(k ? string(c, &o->description, &o->description_len) : string(c, &o->id, &o->id_len)))
            return false;
    } while (expect(c, ','));
    return seen == 3 && expect(c, '}');
}
static bool question(struct cursor *c, geistr_decision_question *q, geistr_decision_option *options) {
    static const char *const names[] = {"id", "question", "options"};
    unsigned seen = 0;
    if (!expect(c, '{'))
        return false;
    do {
        const int k = key(c, &seen, 3, names);
        bool ok = k == 0 ? string(c, &q->id, &q->id_len) : k == 1 && string(c, &q->question, &q->question_len);
        if (k == 2 && expect(c, '[')) {
            do {
                ok = q->n_options < GEISTR_DECISION_MAX_OPTIONS && option(c, &options[q->n_options]);
                q->n_options += ok;
            } while (ok && expect(c, ','));
            ok = ok && expect(c, ']');
        }
        if (!ok)
            return false;
    } while (expect(c, ','));
    q->options = options;
    return seen == 7 && expect(c, '}');
}
static bool request(struct cursor *c, struct batch_input *in) {
    static const char *const names[] = {"context", "questions"};
    geistr_decision_batch *b = &in->batch;
    unsigned seen = 0;
    if (!expect(c, '{'))
        return false;
    do {
        const int k = key(c, &seen, 2, names);
        bool ok = k == 0 && string(c, &in->context, &in->context_len);
        if (k == 1 && expect(c, '[')) {
            do {
                ok = b->n_questions < GEISTR_DECISION_MAX_BATCH &&
                     question(c, &in->questions[b->n_questions], in->options[b->n_questions]);
                b->n_questions += ok;
            } while (ok && expect(c, ','));
            ok = ok && expect(c, ']');
        }
        if (!ok)
            return false;
    } while (expect(c, ','));
    if (seen != 3 || !expect(c, '}'))
        return false;
    blank(c);
    return c->p == c->end;
}

/* Every question under the single-question limits, the ID rules and
 * uniqueness, before anything is loaded; the context API checks it again. */
static int validate(const struct batch_input *in) {
    char error[320], why[160];
    for (size_t i = 0; i < in->batch.n_questions; ++i) {
        const geistr_decision_question *q = &in->questions[i];
        const geistr_decision_option id = {q->id_len, 1, q->id, "x"};
        const geistr_decision_request probe = {.size = sizeof probe,
                                               .operation = GEISTR_OPERATION_DECISION,
                                               .question_len = 1,
                                               .n_options = 1,
                                               .question = "x",
                                               .options = &id};
        const geistr_decision_request r = {.size = sizeof r,
                                           .operation = GEISTR_OPERATION_DECISION,
                                           .question_len = q->question_len,
                                           .context_len = in->context_len,
                                           .n_options = q->n_options,
                                           .question = q->question,
                                           .context = in->context,
                                           .options = q->options};
        const bool id_valid = geistr_decision_request_validate(0, &probe, nullptr) == GEISTR_OK;
        bool duplicate = false;
        for (size_t k = 0; k < i; ++k)
            duplicate |= q->id_len == in->questions[k].id_len && !memcmp(q->id, in->questions[k].id, q->id_len);
        if (!id_valid)
            snprintf(error, sizeof error, "question %zu: invalid question ID", i);
        else if (duplicate)
            snprintf(error, sizeof error, "question %zu (%.*s): duplicate question ID", i, (int)q->id_len, q->id);
        else if (geistr_decision_request_validate(sizeof why, &r, why) != GEISTR_OK)
            snprintf(error, sizeof error, "question %zu (%.*s): %s", i, (int)q->id_len, q->id, why);
        else
            continue;
        return failure(2, error);
    }
    return 0;
}

/* A positive decimal count up to limit; nothing else (no sign, no space). */
static bool count(const char *s, size_t limit, size_t *out) {
    size_t v = 0;
    for (const char *p = s; *p; ++p)
        if (*p < '0' || *p > '9' || ckd_mul(&v, v, 10) || ckd_add(&v, v, (size_t)(*p - '0')) || v > limit)
            return false;
    *out = v;
    return *s && v;
}

static void json_text(FILE *f, const char *s) {
    fputc('"', f);
    for (; s && *s; ++s) {
        const unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            fprintf(f, "\\%c", ch);
        else if (ch < 0x20 || ch == 0x7f)
            fprintf(f, "\\u%04x", ch);
        else
            fputc(ch, f);
    }
    fputc('"', f);
}

int geistr_decide_batch_command(size_t argc, const char *const *argv, const geistr_decide_host *host) {
    command = "decide-batch";
    if (!argc || argc > 128 || !argv || !argv[0])
        return failure(2, "model and options required; see geistr decide-batch --help");
    const double start = milliseconds();
    const char *config_file = nullptr, *request_file = nullptr, *profile = nullptr,
               *catalog_file = host ? host->catalog_file : nullptr, *models_dir = host ? host->models_dir : nullptr;
    geistr_processor processor = GEISTR_PROCESSOR_AUTO;
    size_t tokens = 0, budget_mib = 0;
    unsigned seen = 0;
    for (size_t i = 1; i < argc; ++i) {
        const char *value = nullptr, *flag = argv[i];
        unsigned bit = 1;
        if (!flag)
            return failure(2, "null argument");
        if (strcmp(flag, "--shared-context")) {
            if (!argument(argc, &i, argv, &value))
                return failure(2, "missing flag value");
            if (!strcmp(flag, "--config")) {
                bit = 2;
                config_file = value;
            } else if (!strcmp(flag, "--request-file")) {
                bit = 4;
                request_file = value;
            } else if (!strcmp(flag, "--profile")) {
                bit = 8;
                profile = value;
            } else if (!strcmp(flag, "--models")) {
                bit = 16;
                models_dir = value;
            } else if (!strcmp(flag, "--catalog")) {
                bit = 32;
                catalog_file = value;
            } else if (!strcmp(flag, "--processor")) {
                bit = 64;
                if (!strcmp(value, "cpu"))
                    processor = GEISTR_PROCESSOR_CPU;
                else if (!strcmp(value, "gpu"))
                    processor = GEISTR_PROCESSOR_GPU;
                else if (strcmp(value, "auto"))
                    return failure(2, "unknown processor");
            } else if (!strcmp(flag, "--context-tokens")) {
                bit = 128;
                if (!count(value, UINT32_MAX, &tokens))
                    return failure(2, "--context-tokens needs a count from 1 to 4294967295");
            } else if (!strcmp(flag, "--snapshot-budget-mib")) {
                bit = 256;
                if (!count(value, SIZE_MAX >> 20, &budget_mib))
                    return failure(2, "--snapshot-budget-mib needs a positive count that fits in bytes");
            } else
                return failure(2, "unknown flag");
        }
        if (seen & bit)
            return failure(2, "duplicate/conflicting flag");
        seen |= bit;
    }
    if (!(seen & 1))
        return failure(2, "--shared-context required: V1 has only the explicit shared-context operation");
    if (!request_file)
        return failure(2, "--request-file FILE|- required");
    char *json = nullptr;
    size_t n = 0;
    if (read_bounded(&n, GEISTR_DECISION_MAX_SERIALIZED, request_file, &json) != GEISTR_OK)
        return failure(2, "request file/stdin unreadable or larger than 1 MiB");
    struct batch_input *in = calloc(1, sizeof *in);
    char *arena = malloc(n + 1);
    if (!in || !arena) {
        free(json), free(arena), free(in);
        return failure(1, "out of memory");
    }
    in->batch = (geistr_decision_batch){sizeof in->batch, GEISTR_OPERATION_DECISION, 0, in->questions};
    char error[256];
    int code = 1;
    struct loaded loaded = {};
    geistr_decision_context *context = nullptr;
    geistr_decision_context_info info = {.size = sizeof info};
    geistr_decision_batch_result result = {.size = sizeof result, .failed_index = SIZE_MAX};
    char backend[32] = "", revision[64] = "", version[64] = "", kv_cache[16] = "";
    double loaded_ms = 0, opened_ms = 0, result_ms = 0, output_ms = 0;
    struct cursor cursor = {json, json + n, arena};
    if (!request(&cursor, in)) {
        snprintf(error, sizeof error,
                 "request: invalid JSON or shape near byte %zu (exactly context and questions[1..32] with id, "
                 "question and options[1..26] with id and description)",
                 (size_t)(cursor.p - json));
        code = failure(2, error);
        goto done;
    }
    if ((code = validate(in)))
        goto done;
    code = 1;
    if (!geistr_decision_available() || !geistr_decision_context_available()) {
        failure(1, "linked engine built without DECISION=1 DECISION_CONTEXT=1; no fallback");
        goto done;
    }
    if ((code = load(&loaded, argv[0], config_file, profile, models_dir, catalog_file, host, processor,
                     tokens ? (uint32_t)tokens : 512)))
        goto done;
    code = 1;
    loaded_ms = milliseconds();
    geistr_decision_context_opts opts = GEISTR_DECISION_CONTEXT_OPTS_INIT;
    opts.max_prompt_tokens = tokens; /* 0: the C API's default */
    opts.max_snapshot_bytes = budget_mib << 20;
    const geistr_status opened = geistr_decision_context_open(loaded.model, in->context_len, sizeof error,
                                                              in->context, &opts, &context, error);
    opened_ms = milliseconds();
    if (opened != GEISTR_OK || geistr_decision_context_info_get(context, &info) != GEISTR_OK) {
        failure(1, opened != GEISTR_OK ? error : "context diagnostics unavailable");
        goto done;
    }
    snprintf(backend, sizeof backend, "%s", info.backend ? info.backend : "");
    snprintf(revision, sizeof revision, "%s", info.engine_revision ? info.engine_revision : "");
    snprintf(version, sizeof version, "%s", info.engine_version ? info.engine_version : "");
    snprintf(kv_cache, sizeof kv_cache, "%s", info.kv_cache ? info.kv_cache : "");
    if (host && host->context_active)
        host->context_active(context, host->context);
    const geistr_status status = geistr_decision_context_score_batch(context, &in->batch, &result);
    result_ms = milliseconds();
    if (status != GEISTR_OK) {
        code = failure(status == GEISTR_CANCELLED ? 130 : 1, geistr_decision_context_error(context));
        goto done;
    }
    /* Staged completely, then written once: every selection or a failure. */
    char out[GEISTR_DECISION_MAX_BATCH * (2 * GEISTR_DECISION_MAX_ID + 2) + 1];
    size_t used = 0;
    for (size_t i = 0; i < result.n_answers; ++i)
        used += (size_t)snprintf(out + used, sizeof out - used, "%s\t%s\n", result.answers[i].question_id,
                                 result.answers[i].external_id);
    if (fwrite(out, 1, used, stdout) != used || fflush(stdout)) {
        failure(1, "selection output failed");
        goto done;
    }
    output_ms = milliseconds();
    code = 0;
done:
    if (host && host->context_active && context)
        host->context_active(nullptr, host->context);
    size_t prompt[GEISTR_DECISION_MAX_BATCH], uncached[GEISTR_DECISION_MAX_BATCH];
    for (size_t i = 0; i < result.n_answers; ++i)
        prompt[i] = result.answers[i].prompt_tokens, uncached[i] = result.answers[i].uncached_tokens;
    const bool record = loaded.model != nullptr, opened_context = context != nullptr;
    const char *profile_name = record ? geistr_decision_profile_name(loaded.policy->profile) : "";
    char sha[65];
    memcpy(sha, loaded.sha, sizeof sha);
    const double teardown = milliseconds();
    geistr_decision_context_close(context);
    unload(&loaded);
    const double end = milliseconds();
    /* One record once a model was loaded, written after teardown so that
     * total_ms includes it. Context, question and option text never appear. */
    if (record) {
        FILE *e = stderr;
        fprintf(e, "{\"schema\":1,\"operation\":\"decision_batch\",\"exit\":%d,\"profile\":", code);
        json_text(e, profile_name);
        fprintf(e, ",\"artifact_sha256\":\"%s\",\"runtime\":", sha);
        json_text(e, geistr_version());
        if (opened_context) {
            fprintf(e, ",\"template_sha256\":\"%s\",\"context_sha256\":\"%s\",\"engine_revision\":",
                    info.template_sha256, info.context_sha256);
            json_text(e, revision);
            fputs(",\"engine\":", e);
            json_text(e, version);
            fputs(",\"backend\":", e);
            json_text(e, backend);
            fputs(",\"kv_cache\":", e);
            json_text(e, kv_cache);
            fprintf(e,
                    ",\"activation\":null,\"mode\":\"dense\",\"prefill_chunk\":%zu,\"max_prompt_tokens\":%zu,"
                    "\"cached_tokens\":%zu,\"snapshot_bytes\":%zu,\"max_snapshot_bytes\":%zu,"
                    "\"setup_model_calls\":%zu",
                    info.prefill_chunk, info.max_prompt_tokens, info.cached_tokens, info.snapshot_bytes,
                    info.max_snapshot_bytes, info.setup_model_calls);
        }
        fprintf(e, ",\"questions\":%zu,\"prepared_questions\":%zu,\"restores\":%zu,\"failed_index\":",
                in->batch.n_questions, result.prepared_questions, result.model_calls);
        if (result.failed_index == SIZE_MAX)
            fputs("null,\"failed_id\":null", e);
        else {
            fprintf(e, "%zu,\"failed_id\":", result.failed_index);
            json_text(e, result.failed_id);
        }
        fputs(",\"prompt_tokens\":[", e);
        for (size_t i = 0; i < result.n_answers; ++i)
            fprintf(e, "%s%zu", i ? "," : "", prompt[i]);
        fputs("],\"uncached_tokens\":[", e);
        for (size_t i = 0; i < result.n_answers; ++i)
            fprintf(e, "%s%zu", i ? "," : "", uncached[i]);
        /* Each phase ends at a synchronous return (the engine has finished). */
        fprintf(e,
                "],\"load_ms\":%.3f,\"context_ms\":%.3f,\"preparation_ms\":%.3f,\"scoring_ms\":%.3f,"
                "\"batch_ms\":%.3f,\"until_result_ms\":%.3f,\"output_ms\":%.3f,\"teardown_ms\":%.3f,"
                "\"total_ms\":%.3f}\n",
                loaded_ms - start, opened_ms > 0 ? opened_ms - loaded_ms : 0.0, result.preparation_ms,
                result.scoring_ms, result.batch_ms, result_ms > 0 ? result_ms - start : 0.0,
                output_ms > 0 ? output_ms - result_ms : 0.0, end - teardown, end - start);
    }
    free(in);
    free(arena);
    free(json);
    return code;
}

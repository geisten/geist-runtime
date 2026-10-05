#include "decide.h"
#include "geistr_catalog.h"
#include "geistr_decision.h"
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double milliseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1000000.0;
}
static int failure(int code, const char *message) {
    fprintf(stderr, "geistr decide: %s\n", message);
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
static bool argument(size_t argc, size_t *index, const char *const *argv, const char **value) {
    if (*index + 1 >= argc || !argv[*index + 1])
        return false;
    *value = argv[++*index];
    return true;
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
    if ((!question && !question_file) || !config_file || !count)
        return failure(2, "question, --config and supplied --option values required");
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
    char *catalog_json = nullptr;
    geistr_decision_config *config = nullptr;
    geistr_catalog *catalog = nullptr;
    geistr_model *model = nullptr;
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
    size_t n = 0;
    status = geistr_decide_config_read(sizeof error, config_file, &config, error);
    if (status != GEISTR_OK) {
        code = failure(1, error);
        goto done;
    }
    char path[4096], directory[4096], sha[65];
    const char *selected_path = argv[0];
    if (!strchr(argv[0], '/') && !strstr(argv[0], ".gguf")) {
        if (host && host->catalog)
            catalog = host->catalog(models_dir, catalog_file);
        else if (catalog_file && read_bounded(&n, 1u << 20, catalog_file, &catalog_json) == GEISTR_OK)
            status = geistr_catalog_parse(catalog_json, n, &catalog, error, sizeof error);
        if (!catalog) {
            code = failure(1, "catalog unreadable or invalid");
            goto done;
        }
        const geistr_catalog_entry *entry = geistr_catalog_find(catalog, argv[0]);
        if (!entry) {
            code = failure(1, "unknown catalog model ID");
            goto done;
        }
        if (!models_dir) {
            if (geistr_models_dir(directory, sizeof directory) != GEISTR_OK) {
                code = failure(1, "models directory unavailable");
                goto done;
            }
            models_dir = directory;
        }
        geistr_install install;
        if (geistr_catalog_check(entry, models_dir, true, &install) != GEISTR_OK ||
            install != GEISTR_INSTALL_OK) {
            code = failure(1, "model not installed and SHA-verified; no download");
            goto done;
        }
        const int written = snprintf(path, sizeof path, "%s/%s", models_dir, entry->file);
        if (written < 0 || (size_t)written >= sizeof path) {
            code = failure(1, "model path too long");
            goto done;
        }
        selected_path = path;
        memcpy(sha, entry->sha256, sizeof sha);
    } else if (geistr_sha256_file(selected_path, sha) != GEISTR_OK) {
        code = failure(1, "model artifact unreadable");
        goto done;
    }
    const geistr_decision_policy *policy = geistr_decision_config_find(sha, config);
    if (!policy || !policy->enabled) {
        code = failure(1, "decision permission absent or disabled for this artifact");
        goto done;
    }
    if (profile && strcmp(profile, geistr_decision_profile_name(policy->profile))) {
        code = failure(1, "profile override does not match the immutable artifact policy");
        goto done;
    }
    geistr_model_opts mo = GEISTR_MODEL_OPTS_INIT;
    mo.decision = policy;
    mo.processor = processor;
    mo.context = 512;
    status = geistr_model_open(selected_path, &mo, &model, error, sizeof error);
    if (status != GEISTR_OK) {
        code = failure(1, error);
        goto done;
    }
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
    if (printf("%s\n", result.external_id) < 0 || fflush(stdout)) {
        code = failure(1, "selection output failed");
        goto done;
    }
    code = 0;
done:
    if (host && host->active && decision)
        host->active(nullptr, host->context);
    geistr_decision_close(decision);
    geistr_model_close(model);
    geistr_catalog_free(catalog);
    geistr_decision_config_free(config);
    free(catalog_json);
    free(owned_question);
    return code;
}

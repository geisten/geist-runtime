#pragma once
#include "geistr_catalog.h"
#include "geistr_decision.h"
#include <stddef.h>
#include <stdint.h>

typedef void (*geistr_decide_active_fn)(geistr_decision *decision, void *context);
typedef void (*geistr_decide_context_fn)(geistr_decision_context *context, void *host_context);
typedef geistr_catalog *(*geistr_decide_catalog_fn)(const char *models_dir, const char *catalog_file);
/* The single dispatcher supplies its common catalog policy and signal hook.
 * active receives the live handle before scoring, then nullptr before close;
 * the dispatcher must quiesce cancellation before returning from clear.
 * context_active does the same for decide-batch's context. */
typedef struct geistr_decide_host {
    const char *models_dir, *catalog_file;
    geistr_decide_catalog_fn catalog;
    geistr_decide_active_fn active;
    void *context;
    geistr_decide_context_fn context_active;
} geistr_decide_host;
/* argv starts with <model>, not program/subcommand. No download is attempted. */
int geistr_decide_command(size_t argc, const char *const *argv, const geistr_decide_host *host);
/* geistr decide's options as given (#288). What a profile brings, the threshold
 * and early exit, is applied after the model is loaded: geistr_decide_threshold
 * and geistr_decide_early_exit_default. */
typedef struct geistr_decide_args {
    const char *question;      /* the text; nullptr when it is read from question_file */
    const char *question_file; /* --question @FILE, --question - (stdin) */
    const char *context, *config_file, *catalog_file, *models_dir, *profile;
    const char *escalate; /* "chat" (the default), a model, or nullptr: --no-escalate */
    geistr_processor processor;
    geistr_decision_mode mode;
    double threshold;        /* --threshold P in [0, 1]; < 0: the profile's (geistr_decide_threshold) */
    double temperature;      /* --temperature T */
    uint32_t max_tokens;     /* --max-tokens N: the chat answer's cap, thinking included */
    int early_exit;          /* -1: the profile's default; 0: --no-early-exit; 1: --early-exit[=P] */
    uint32_t probe_interval; /* fixed at the validated 128 (--probe-interval: deprecated alias) */
    double exit_threshold;   /* --early-exit=P (0.8) */
    size_t n_options;
    geistr_decision_option options[GEISTR_DECISION_MAX_OPTIONS];
} geistr_decide_args;
/* argv as for geistr_decide_command. 0, or 2 after a message on stderr. The
 * options #288 removed are still accepted for one release, with a warning on
 * stderr. */
int geistr_decide_parse(size_t argc, const char *const *argv, geistr_decide_args *out);
/* The threshold below which decide escalates (or, with --no-escalate, exits 3)
 * when --threshold is not given: the profile's calibrated one. No profile
 * carries one yet, so every model gets GEISTR_DECIDE_DEFAULT_THRESHOLD (#288). */
double geistr_decide_threshold(geistr_decision_profile profile);
/* decide-batch (#164): argv as above; one shared context, 1..32 questions,
 * stdout question_id TAB option_id LF in input order only on total success. */
int geistr_decide_batch_command(size_t argc, const char *const *argv, const geistr_decide_host *host);
/* Whether --escalate chat exits early by default for a verified decision
 * profile: only where a declared validation established it (geistlib#286). */
bool geistr_decide_early_exit_default(geistr_decision_profile profile);
/* A model given by path (#285): its SHA-256 from the receipt geistr pull or a
 * catalog check left next to it (<dir>/.verified/<sha256>, bound to the
 * file's identity), never by hashing it. false and "" when no receipt matches
 * the file as it is now: decide treats it as unverified. */
bool geistr_decide_receipt_sha(const char *path, char sha[static 65]);
/* decide's SHA-256 for a path: the receipt's; without one, configured (a
 * --config allow-list must match it) hashes the file, else "" (unverified). */
bool geistr_decide_path_sha(const char *path, bool configured, char sha[static 65]);
/* Shared bounded reader for catalog inspection and execution. */
[[nodiscard]] geistr_status geistr_decide_config_read(size_t error_cap, const char *path,
                                                      geistr_decision_config **out, char *error);

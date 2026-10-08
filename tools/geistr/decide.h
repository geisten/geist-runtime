#pragma once
#include "geistr_catalog.h"
#include "geistr_decision.h"
#include <stddef.h>

typedef void (*geistr_decide_active_fn)(geistr_decision *decision, void *context);
typedef geistr_catalog *(*geistr_decide_catalog_fn)(const char *models_dir, const char *catalog_file);
/* The single dispatcher supplies its common catalog policy and signal hook.
 * active receives the live handle before scoring, then nullptr before close;
 * the dispatcher must quiesce cancellation before returning from clear. */
typedef struct geistr_decide_host {
    const char *models_dir, *catalog_file;
    geistr_decide_catalog_fn catalog;
    geistr_decide_active_fn active;
    void *context;
} geistr_decide_host;
/* argv starts with <model>, not program/subcommand. No download is attempted. */
int geistr_decide_command(size_t argc, const char *const *argv, const geistr_decide_host *host);
/* Shared bounded reader for catalog inspection and execution. */
[[nodiscard]] geistr_status geistr_decide_config_read(size_t error_cap, const char *path,
                                                      geistr_decision_config **out, char *error);

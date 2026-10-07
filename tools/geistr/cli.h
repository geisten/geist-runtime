/* cli.h — what the parts of the geistr CLI share. */
#pragma once
#include "geistr_catalog.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef GEISTR_ENGINE
#define GEISTR_ENGINE "unknown" /* the geistlib commit, set by the Makefile */
#endif

enum { OK = 0, ERROR = 1, USAGE = 2, CANCELLED = 130 }; /* exit codes */

/* ---- config.c: settings in geistr.conf, the data folder ------------------ */

struct settings {
    char   model[256], processor[8], system[2048];
    double temperature;
    bool   markdown, stats, intro, resume;
};
extern struct settings cfg;
extern char            config_path[4200], data_dir[4096]; /* "" when unknown */

bool data_folder(void); /* find both; false without HOME or GEISTEN_HOME */
void config_load(void);
bool config_save(void);
int  config(int n, const char **args); /* geistr config [key [value]] */

bool make_dirs(const char *path, unsigned mode); /* mkdir -p */

/* Styled output: a terminal, and NO_COLOR not set. */
static inline bool tty_out(void) {
    return isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
}

/* ---- speed.c: tokens/s measured here ---------------------------------------- */

/* "  42.3 tok/s · 1.8 s" after an answer (unless the stats setting is off). */
void speed_line(unsigned tokens, double generation_ms, double total_ms, FILE *out);
/* A complete answer's speed into speed.tsv; source "bench" or "answer". */
void speed_record(const char *model, const char *backend, unsigned tokens, double generation_ms, double first_ms,
                  const char *source);
/* An answer's speed line from its chat; complete ones are also recorded. */
void speed(geistr_chat *chat, const char *model, const char *backend, bool complete, const char *source, FILE *out);
/* This engine's measured speeds into local[] (indexed like the catalog). */
void   speeds_load(const geistr_catalog *c, geistr_local *local);
double reference_rate(const geistr_catalog_entry *m, const char *proc); /* the catalog's; 0 if none */
void   speed_bar(const char *symbol, double measured, double reference, double max, int width, bool tty);
int    speed_compare(int n, const char **refs); /* geistr bench --compare [A [B]] */

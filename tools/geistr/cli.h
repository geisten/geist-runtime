/* cli.h — what the parts of the geistr CLI share. */
#pragma once
#include "geistr_catalog.h"
#include <signal.h>
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
static inline const char *dim(bool on) { return on ? "\033[2m" : ""; }
static inline const char *normal(bool on) { return on ? "\033[0m" : ""; }

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

/* ---- conversation.c: what was said in a chat --------------------------------- */

struct conversation {
    size_t n, cap;
    char **role, **content;
    char   system[2048]; /* the system prompt for a new conversation */
    bool   carry;        /* the next send carries it all (a new model, prompt or temperature) */
    char   file[4400], resumed_from[4400]; /* "" when not kept */
};

void conv_push(struct conversation *c, const char *role, const char *content);
void conv_clear(struct conversation *c); /* /clear: a new conversation, and a new file */
void conv_free(struct conversation *c);
/* /system: set it, and in the conversation (first, or removed); true when a
 * chat holding the conversation has to read it anew. */
bool conv_system(struct conversation *c, const char *text);
/* The user said text: where the next send starts (0 = all of it; whole for a
 * service, which finds what it holds). */
size_t conv_say(struct conversation *c, const char *text, bool whole);
void   conv_answered(struct conversation *c, const char *answer); /* also stores it */
void   conv_refused(struct conversation *c);                      /* the chat refused the last message */
/* The last question and its first 60 characters' bytes, for "↻ … „…“". */
const char *conv_last_question(const struct conversation *c, int *bytes);
void        conv_file_new(struct conversation *c);
void        conv_store(struct conversation *c);
void        conv_resume(struct conversation *c); /* the newest conversation, if any */

/* ---- chat.c: geistr chat and run --------------------------------------------- */

/* A model with a chat on it, and the choices behind both. Replaced as a whole
 * by a runtime switch (/gpu, /model …); the conversation moves along. */
struct session {
    geistr_model *model;
    geistr_chat  *chat;
    char          name[256], processor[8], backend[16], format[16];
    double           temperature;
    uint32_t         context;
    geistr_reasoning reasoning; /* the catalog's, for each chat on the model */
};

extern geistr_chat *volatile running; /* the answer Ctrl-C stops */
extern volatile sig_atomic_t interrupted;
void on_interrupt(int signal);

int  session_open(struct session *x, const char *name, const char *processor, double temperature, bool interactive);
void session_close(struct session *x);
bool on_gpu(const struct session *x);
int  answer_once(const char *name, const char *prompt, const char *processor); /* geistr run */
int  chat(const char *name, const char *processor, const char *remote, bool fresh); /* remote: a service's socket */

/* ---- geistr.c --------------------------------------------------------------- */

extern const char *models_dir;
geistr_catalog    *load_catalog(void);
int                resolve(const char *model, char *path, size_t cap, geistr_reasoning *reasoning);

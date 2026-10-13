/* config.c — see cli.h. geistr.conf is "key = value" lines; the settings
 * table below is the one place a setting is defined. */
#include "cli.h"
#include "geistr_catalog.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct settings cfg = {.processor = "auto", .markdown = true, .stats = true, .intro = true, .resume = true, .history = true, .thinking = true,
                       .resume_tokens = 2048};
char            config_path[4200], data_dir[4096];
static char     config_dir[4096];

enum kind { TEXT, CHOICE, NUMBER, SWITCH };
static const struct setting {
    const char *key;
    enum kind   kind;
    void       *at;
    size_t      cap;     /* TEXT, CHOICE */
    const char *choices; /* CHOICE: the first is the default */
    double      max;     /* NUMBER: from 0 to this */
} settings[] = {
        {.key = "model", .kind = TEXT, .at = cfg.model, .cap = sizeof cfg.model},
        {.key = "processor", .kind = CHOICE, .at = cfg.processor, .cap = sizeof cfg.processor, .choices = "auto cpu gpu"},
        {.key = "temperature", .kind = NUMBER, .at = &cfg.temperature, .max = 2},
        {.key = "system", .kind = TEXT, .at = cfg.system, .cap = sizeof cfg.system},
        {.key = "markdown", .kind = SWITCH, .at = &cfg.markdown},
        {.key = "stats", .kind = SWITCH, .at = &cfg.stats},
        {.key = "intro", .kind = SWITCH, .at = &cfg.intro},
        {.key = "resume", .kind = SWITCH, .at = &cfg.resume},
        {.key = "history", .kind = SWITCH, .at = &cfg.history},
        {.key = "resume_tokens", .kind = NUMBER, .at = &cfg.resume_tokens, .max = 1e6},
        {.key = "threads", .kind = NUMBER, .at = &cfg.threads, .max = 1024},
        {.key = "thinking", .kind = SWITCH, .at = &cfg.thinking},
};
enum { N_SETTINGS = sizeof settings / sizeof *settings };

static const struct setting *find(const char *key) {
    for (size_t i = 0; i < N_SETTINGS; i++)
        if (!strcmp(settings[i].key, key))
            return &settings[i];
    return nullptr;
}

/* Thinking per model (#212): "<id>.thinking = off"; a model without a line
 * of its own follows the thinking setting. */
static struct {
    char id[128];
    bool on;
} per_model[64];
static size_t     n_per_model;
static const char THINKING[] = ".thinking";

/* A "<id>.thinking" key; the id's length in *n. */
static bool thinking_key(const char *key, size_t *n) {
    size_t len = strlen(key), k = sizeof THINKING - 1;
    *n         = len > k ? len - k : 0;
    return *n && *n < sizeof per_model[0].id && !strcmp(key + *n, THINKING);
}

static ptrdiff_t per_model_find(const char *id, size_t n) {
    for (size_t i = 0; i < n_per_model; i++)
        if (strlen(per_model[i].id) == n && !strncmp(per_model[i].id, id, n))
            return (ptrdiff_t) i;
    return -1;
}

bool thinking_on(const char *id) {
    ptrdiff_t i = per_model_find(id, strlen(id));
    return i >= 0 ? per_model[i].on : cfg.thinking;
}

/* nullptr when the value is valid for key and stored, else why not. */
static const char *set(const char *key, const char *value) {
    static char why[96];
    size_t      id_n;
    if (thinking_key(key, &id_n)) { /* "" removes the model's own value */
        if (*value && strcmp(value, "on") && strcmp(value, "off"))
            return "the value is on or off";
        ptrdiff_t i = per_model_find(key, id_n);
        if (!*value && i >= 0)
            per_model[i] = per_model[--n_per_model];
        else if (*value && i < 0 && n_per_model == sizeof per_model / sizeof *per_model)
            return "too many models with their own thinking";
        else if (*value) {
            if (i < 0)
                snprintf(per_model[i = (ptrdiff_t) n_per_model++].id, sizeof per_model[0].id, "%.*s", (int) id_n, key);
            per_model[i].on = strcmp(value, "off") != 0;
        }
        return nullptr;
    }
    const struct setting *const s = find(key);
    if (!s)
        return "unknown key";
    switch (s->kind) {
    case TEXT:
        snprintf(s->at, s->cap, "%s", value);
        break;
    case CHOICE: {
        size_t n     = strlen(value);
        bool   valid = !*value;
        for (const char *c = s->choices; *c && !valid; c += strcspn(c, " "), c += *c == ' ')
            valid = !strncmp(c, value, n) && (c[n] == ' ' || !c[n]);
        if (!valid)
            return snprintf(why, sizeof why, "%s is one of: %s", key, s->choices), why;
        snprintf(s->at, s->cap, "%.*s", *value ? (int) n : (int) strcspn(s->choices, " "), *value ? value : s->choices);
        break;
    }
    case NUMBER: {
        char  *end = nullptr;
        double t   = *value ? strtod(value, &end) : 0;
        if (*value && (*end || !(t >= 0 && t <= s->max)))
            return snprintf(why, sizeof why, "%s is a number from 0 to %g", key, s->max), why;
        *(double *) s->at = t;
        break;
    }
    case SWITCH:
        if (*value && strcmp(value, "on") && strcmp(value, "off"))
            return "the value is on or off";
        *(bool *) s->at = strcmp(value, "off") != 0;
        break;
    }
    return nullptr;
}

void config_thinking(const char *id, bool on) {
    char key[sizeof per_model[0].id + sizeof THINKING];
    if (!strchr(id, '/') && !strstr(id, ".gguf")) /* a catalog id, not a path */
        snprintf(key, sizeof key, "%s%s", id, THINKING), (void) set(key, on ? "on" : "off");
}

static void value_of(const struct setting *s, char *out, size_t cap) {
    if (s->kind == NUMBER)
        snprintf(out, cap, "%g", *(double *) s->at);
    else if (s->kind == SWITCH)
        snprintf(out, cap, "%s", *(bool *) s->at ? "on" : "off");
    else
        snprintf(out, cap, "%s", (const char *) s->at);
}

bool make_dirs(const char *path, unsigned mode) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            (void) mkdir(dir, (mode_t) mode);
            *p = '/';
        }
    return mkdir(dir, (mode_t) mode) == 0 || errno == EEXIST;
}

/* The geisten data folder: where the default model folder lives. */
bool data_folder(void) {
    char models[4096];
    if (geistr_models_dir(models, sizeof models) != GEISTR_OK)
        return false;
    char *slash = strrchr(models, '/');
    if (!slash)
        return false;
    *slash = 0;
    snprintf(data_dir, sizeof data_dir, "%s", models);
    /* Settings: in the data folder on macOS (Application Support) and with
     * GEISTEN_HOME; on Linux where XDG puts configuration. */
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
#ifdef __APPLE__
    bool own = true;
#else
    bool own = (getenv("GEISTEN_HOME") && *getenv("GEISTEN_HOME")) || (getenv("GEIST_HOME") && *getenv("GEIST_HOME"));
#endif
    if (own)
        snprintf(config_dir, sizeof config_dir, "%s", data_dir);
    else if (xdg && *xdg)
        snprintf(config_dir, sizeof config_dir, "%s/geisten", xdg);
    else
        snprintf(config_dir, sizeof config_dir, "%s/.config/geisten", home ? home : "");
    snprintf(config_path, sizeof config_path, "%s/geistr.conf", config_dir);
    char old[4200]; /* where geistr kept it before: moved once */
    snprintf(old, sizeof old, "%s/geistr.conf", data_dir);
    if (!own && access(config_path, F_OK) != 0 && access(old, F_OK) == 0 && make_dirs(config_dir, 0700))
        (void) rename(old, config_path);
    return true;
}

void config_load(void) {
    FILE *f = config_path[0] ? fopen(config_path, "r") : nullptr;
    char  line[2400];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *eq                  = strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        char *key = line, *value = eq + 1, *end = eq;
        while (end > key && end[-1] == ' ')
            end--;
        *end = 0;
        while (*value == ' ')
            value++;
        if (set(key, value))
            fprintf(stderr, "geistr: %s: ignored %s\n", config_path, key);
    }
    if (f)
        fclose(f);
}

bool config_save(void) {
    if (!make_dirs(config_dir, 0700)) /* the folder may not exist yet without the app */
        return false;
    char tmp[4300];
    snprintf(tmp, sizeof tmp, "%s.%ld", config_path, (long) getpid());
    FILE *f = fopen(tmp, "w");
    if (!f)
        return false;
    fputs("# geistr settings (geistr config KEY VALUE)\n", f);
    for (size_t i = 0; i < N_SETTINGS; i++) {
        char value[2048];
        value_of(&settings[i], value, sizeof value);
        fprintf(f, "%s = %s\n", settings[i].key, value);
    }
    for (size_t i = 0; i < n_per_model; i++)
        fprintf(f, "%s%s = %s\n", per_model[i].id, THINKING, per_model[i].on ? "on" : "off");
    bool ok = fclose(f) == 0 && rename(tmp, config_path) == 0;
    if (!ok)
        unlink(tmp);
    return ok;
}

int config(int n, const char **args) {
    if (!config_path[0]) {
        fputs("geistr: no settings folder: set HOME or GEISTEN_HOME\n", stderr);
        return ERROR;
    }
    if (n == 1) {
        printf("# %s\n", config_path);
        for (size_t i = 0; i < N_SETTINGS; i++) {
            char value[2048];
            value_of(&settings[i], value, sizeof value);
            printf("%-13s %s\n", settings[i].key, value);
        }
        for (size_t i = 0; i < n_per_model; i++)
            printf("%s%s %s\n", per_model[i].id, THINKING, per_model[i].on ? "on" : "off");
        return OK;
    }
    const struct setting *s = find(args[1]);
    size_t                id_len;
    if (thinking_key(args[1], &id_len)) {
        char id[sizeof per_model[0].id];
        snprintf(id, sizeof id, "%.*s", (int) id_len, args[1]);
        if (n == 2) {
            puts(thinking_on(id) ? "on" : "off");
            return OK;
        }
        /* Off only for a catalog model that thinks: its template has the switch. */
        bool                        off = !strcmp(args[2], "off");
        geistr_catalog             *c   = off ? load_catalog() : nullptr;
        const geistr_catalog_entry *m   = nullptr;
        for (size_t i = 0; c && i < geistr_catalog_count(c); i++)
            if (!strcmp(geistr_catalog_get(c, i)->id, id))
                m = geistr_catalog_get(c, i);
        const char *why = !off ? nullptr
                          : !m ? "is not a catalog model (geistr catalog lists them)"
                          : !(m->reasoning_format && !strcmp(m->reasoning_format, "think_tags")) ? "does not think"
                                                                                                 : nullptr;
        geistr_catalog_free(c);
        if (why) {
            fprintf(stderr, "geistr: %s %s\n", id, why);
            return USAGE;
        }
    } else if (!s) {
        fprintf(stderr, "geistr: unknown key %s (", args[1]);
        for (size_t i = 0; i < N_SETTINGS; i++)
            fprintf(stderr, "%s%s", i ? " " : "", settings[i].key);
        fputs(" <model>.thinking)\n", stderr);
        return USAGE;
    }
    if (n == 2 && s) {
        char value[2048];
        value_of(s, value, sizeof value);
        puts(value);
        return OK;
    }
    char value[2048] = "";
    for (int i = 2; i < n; i++) /* the rest of the line: a system prompt may have spaces */
        snprintf(value + strlen(value), sizeof value - strlen(value), "%s%s", i > 2 ? " " : "", args[i]);
    const char *why = set(args[1], value);
    if (why) {
        fprintf(stderr, "geistr: %s\n", why);
        return USAGE;
    }
    if (!config_save()) {
        fprintf(stderr, "geistr: cannot write %s: %s\n", config_path, strerror(errno));
        return ERROR;
    }
    return OK;
}


uint32_t engine_threads(void) {
    if (cfg.threads >= 1)
        return (uint32_t) cfg.threads;
#ifdef __APPLE__
    return 0; /* the engine's default */
#else
    /* One thread per physical core, not per SMT thread: the engine's OpenMP
     * threads spin while they wait, and with every logical CPU in use they
     * collapse when another program takes cores (#93: 19-32 s instead of
     * 3-4 s for a 1500-token prefill on a loaded 16-core Ryzen). */
    static uint32_t chosen;
    static bool     probed;
    if (!probed) {
        geistr_device d = {.size = sizeof d};
        probed          = true;
        if (geistr_device_probe(nullptr, &d) == GEISTR_OK && d.cores && d.cores < d.logical_cpus)
            chosen = d.cores;
    }
    return chosen; /* 0 (no SMT): the engine's default */
#endif
}

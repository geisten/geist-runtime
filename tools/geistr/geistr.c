/*
 * geistr — a minimal CLI on the runtime API (#11). The model runs in this
 * process; no service is involved.
 *
 *   geistr run <model> [prompt…]     one answer to stdout (prompt from stdin if none)
 *   geistr chat <model>              interactive; Ctrl-C stops the answer, not the chat
 *   geistr catalog [--installed | --available] [--json]
 *   geistr pull <id>                 download and verify (builds with the download module)
 *   geistr config [key [value]]      settings, remembered between runs (geistr.conf)
 *
 * <model> is a catalog id or a path to a GGUF; chat without one continues with
 * the last model (or the geisten app's). In a terminal the answer is shown as
 * Markdown with math as Unicode; the prompt shows ⚙ (CPU) or ⚡ (GPU), and
 * each answer ends with its speed. Options anywhere:
 *   --models DIR    model folder (default: the geisten app's, see geistr_models_dir)
 *   --catalog FILE  catalog JSON (default: the app's catalog.json next to the
 *                   model folder if present, else the one built in)
 *   --cpu, --gpu    the processor for this run
 *
 * Exit codes: 0 ok, 1 error, 2 usage, 130 cancelled.
 */
#include "geistr.h"
#include "geistr_catalog.h"
#include "pull.h"
#include "render.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catalog_json.h" /* embedded_catalog[], generated from models/catalog.json */

enum { OK = 0, ERROR = 1, USAGE = 2, CANCELLED = 130 };

static const char *models_dir, *catalog_file;
static char        default_models[4096];

/* ---- settings: geistr.conf next to the model folder ---------------------- */

static struct {
    char   model[256], processor[8], system[2048];
    double temperature;
    bool   markdown, stats;
} cfg = {.processor = "auto", .markdown = true, .stats = true};
static char config_path[4200], data_dir[4096];
static const char *const config_keys[] = {"model", "processor", "temperature", "system", "markdown", "stats"};

/* The geisten data folder: where the default model folder lives. */
static bool data_folder(void) {
    char models[4096];
    if (geistr_models_dir(models, sizeof models) != GEISTR_OK)
        return false;
    char *slash = strrchr(models, '/');
    if (!slash)
        return false;
    *slash = 0;
    snprintf(data_dir, sizeof data_dir, "%s", models);
    snprintf(config_path, sizeof config_path, "%s/geistr.conf", data_dir);
    return true;
}

/* nullptr when the value is valid for key and stored, else why not. */
static const char *config_set(const char *key, const char *value) {
    if (!strcmp(key, "model"))
        snprintf(cfg.model, sizeof cfg.model, "%s", value);
    else if (!strcmp(key, "processor")) {
        if (*value && strcmp(value, "auto") && strcmp(value, "cpu") && strcmp(value, "gpu"))
            return "processor is auto, cpu or gpu";
        snprintf(cfg.processor, sizeof cfg.processor, "%s", *value ? value : "auto");
    } else if (!strcmp(key, "temperature")) {
        char  *end = nullptr;
        double t   = *value ? strtod(value, &end) : 0;
        if (*value && (*end || !(t >= 0 && t <= 2)))
            return "temperature is a number from 0 to 2";
        cfg.temperature = t;
    } else if (!strcmp(key, "system"))
        snprintf(cfg.system, sizeof cfg.system, "%s", value);
    else if (!strcmp(key, "markdown") || !strcmp(key, "stats")) {
        if (*value && strcmp(value, "on") && strcmp(value, "off"))
            return "the value is on or off";
        *(!strcmp(key, "markdown") ? &cfg.markdown : &cfg.stats) = strcmp(value, "off") != 0;
    } else
        return "unknown key";
    return nullptr;
}

static void config_value(const char *key, char *out, size_t cap) {
    if (!strcmp(key, "model"))
        snprintf(out, cap, "%s", cfg.model);
    else if (!strcmp(key, "processor"))
        snprintf(out, cap, "%s", cfg.processor);
    else if (!strcmp(key, "temperature"))
        snprintf(out, cap, "%g", cfg.temperature);
    else if (!strcmp(key, "system"))
        snprintf(out, cap, "%s", cfg.system);
    else
        snprintf(out, cap, "%s", (!strcmp(key, "markdown") ? cfg.markdown : cfg.stats) ? "on" : "off");
}

static void config_load(void) {
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
        if (config_set(key, value))
            fprintf(stderr, "geistr: %s: ignored %s\n", config_path, key);
    }
    if (f)
        fclose(f);
}

static bool config_save(void) {
    char dir[4096]; /* the data folder may not exist yet without the app */
    snprintf(dir, sizeof dir, "%s", data_dir);
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            (void) mkdir(dir, 0700);
            *p = '/';
        }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return false;
    char tmp[4300];
    snprintf(tmp, sizeof tmp, "%s.%ld", config_path, (long) getpid());
    FILE *f = fopen(tmp, "w");
    if (!f)
        return false;
    fputs("# geistr settings (geistr config KEY VALUE)\n", f);
    for (size_t i = 0; i < sizeof config_keys / sizeof *config_keys; i++) {
        char value[2048];
        config_value(config_keys[i], value, sizeof value);
        fprintf(f, "%s = %s\n", config_keys[i], value);
    }
    bool ok = fclose(f) == 0 && rename(tmp, config_path) == 0;
    if (!ok)
        unlink(tmp);
    return ok;
}

static int config(int n, const char **args) {
    if (!config_path[0]) {
        fputs("geistr: no settings folder: set HOME or GEISTEN_HOME\n", stderr);
        return ERROR;
    }
    if (n == 1) {
        printf("# %s\n", config_path);
        for (size_t i = 0; i < sizeof config_keys / sizeof *config_keys; i++) {
            char value[2048];
            config_value(config_keys[i], value, sizeof value);
            printf("%-12s %s\n", config_keys[i], value);
        }
        return OK;
    }
    bool known = false;
    for (size_t i = 0; i < sizeof config_keys / sizeof *config_keys; i++)
        known |= !strcmp(args[1], config_keys[i]);
    if (!known) {
        fprintf(stderr, "geistr: unknown key %s (model processor temperature system markdown stats)\n", args[1]);
        return USAGE;
    }
    if (n == 2) {
        char value[2048];
        config_value(args[1], value, sizeof value);
        puts(value);
        return OK;
    }
    char value[2048] = "";
    for (int i = 2; i < n; i++) /* the rest of the line: a system prompt may have spaces */
        snprintf(value + strlen(value), sizeof value - strlen(value), "%s%s", i > 2 ? " " : "", args[i]);
    const char *why = config_set(args[1], value);
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

static int usage(void) {
    fputs("usage: geistr run <model> [prompt…]\n"
          "       geistr chat <model>\n"
          "       geistr catalog [--installed | --available] [--json]\n"
          "       geistr pull <id>\n"
          "       geistr config [key [value]]   (keys: model processor temperature system markdown stats)\n"
          "options: --models DIR  --catalog FILE  --cpu  --gpu\n"
          "<model> is a catalog id or a path to a .gguf file\n",
          stderr);
    return USAGE;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return nullptr;
    size_t cap = 1 << 16, n = 0, got;
    char  *s   = malloc(cap);
    while (s && (got = fread(s + n, 1, cap - n, f)) > 0)
        if ((n += got) == cap) {
            char *bigger = cap < (1u << 21) ? realloc(s, cap *= 2) : nullptr;
            if (!bigger)
                free(s);
            s = bigger;
        }
    fclose(f);
    *len = n;
    return s;
}

/* --catalog, else the app's catalog.json (the app may hold a newer one),
 * else the built-in copy. */
static geistr_catalog *load_catalog(void) {
    char            path[4200], error[256];
    geistr_catalog *c = nullptr;
    const char     *file = catalog_file;
    if (!file && models_dir == default_models) {
        snprintf(path, sizeof path, "%s", default_models);
        char *slash = strrchr(path, '/');
        if (slash) {
            strcpy(slash, "/catalog.json");
            FILE *f = fopen(path, "r");
            if (f) {
                fclose(f);
                file = path;
            }
        }
    }
    if (file) {
        size_t len  = 0;
        char  *text = slurp(file, &len);
        if (!text) {
            fprintf(stderr, "geistr: cannot read %s\n", file);
            return nullptr;
        }
        if (geistr_catalog_parse(text, len, &c, error, sizeof error) != GEISTR_OK)
            fprintf(stderr, "geistr: %s: %s\n", file, error);
        free(text);
        if (c || catalog_file)
            return c;
    }
    if (geistr_catalog_parse((const char *) embedded_catalog, sizeof embedded_catalog, &c, error, sizeof error) !=
        GEISTR_OK)
        fprintf(stderr, "geistr: built-in catalog: %s\n", error);
    return c;
}

/* Installed state; hashes once per new or changed file (then a receipt). */
static geistr_install install_state(const geistr_catalog_entry *m, bool quiet) {
    geistr_install state = GEISTR_INSTALL_MISSING;
    if (geistr_catalog_check(m, models_dir, false, &state) == GEISTR_OK && state == GEISTR_INSTALL_UNVERIFIED) {
        if (!quiet)
            fprintf(stderr, "verifying %s …\n", m->id);
        if (geistr_catalog_check(m, models_dir, true, &state) != GEISTR_OK)
            state = GEISTR_INSTALL_MISMATCH;
    }
    return state;
}

static const char *size_text(uint64_t bytes, char out[16]) {
    if (bytes >= 1000000000)
        snprintf(out, 16, "%.1f GB", (double) bytes / 1e9);
    else
        snprintf(out, 16, "%.0f MB", (double) bytes / 1e6);
    return out;
}

static const char *limit_text(const char *reason) {
    static const char *const map[][2] = {
            {"unsupported_format", "format not supported"}, {"platform", "CPU not supported"},
            {"disk", "disk too full"},                      {"ram", "too little RAM"},
            {"ram_recommended", "RAM tight"},               {"available_ram", "free RAM tight"}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(reason, map[i][0]))
            return map[i][1];
    return reason;
}

static void json_string(const char *s) {
    putchar('"');
    for (; s && *s; s++)
        if (*s == '"' || *s == '\\')
            printf("\\%c", *s);
        else if ((unsigned char) *s < 32)
            printf("\\u%04x", *s);
        else
            putchar(*s);
    putchar('"');
}

static int catalog(bool installed_only, bool available_only, bool json) {
    geistr_catalog *c = load_catalog();
    if (!c)
        return ERROR;
    geistr_device   d = {};
    geistr_ranking *r = nullptr;
    if (geistr_device_probe(models_dir, &d) != GEISTR_OK || geistr_rank(c, &d, nullptr, nullptr, &r) != GEISTR_OK) {
        fprintf(stderr, "geistr: cannot read this computer's memory\n");
        geistr_catalog_free(c);
        return ERROR;
    }
    size_t         n = geistr_catalog_count(c);
    geistr_install state[1024];
    for (size_t i = 0; i < n; i++)
        state[i] = install_state(geistr_catalog_get(c, i), json);
    if (json)
        printf("{\"schema\":1,\"models_dir\":"), json_string(models_dir), printf(",\"models\":[");
    bool first = true;
    /* Installed first, then available; catalog order within each. */
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < n; i++) {
            const geistr_catalog_entry *m         = geistr_catalog_get(c, i);
            bool                        installed = state[i] == GEISTR_INSTALL_OK;
            const geistr_fit           *f         = nullptr;
            for (size_t k = 0; k < geistr_ranking_count(r); k++)
                if (geistr_ranking_get(r, k)->entry == m)
                    f = geistr_ranking_get(r, k);
            if (installed != (pass == 0) || (installed_only && !installed) || (available_only && installed))
                continue;
            if (json) {
                static const char *const states[]    = {"available", "unverified", "installed", "mismatch"};
                static const char *const resources[] = {"fits", "limited", "unavailable"};
                printf("%s{\"id\":", first ? "" : ",");
                json_string(m->id);
                printf(",\"name\":"), json_string(m->name);
                printf(",\"quantization\":"), m->quantization ? json_string(m->quantization) : (void) printf("null");
                printf(",\"file\":"), json_string(m->file);
                printf(",\"url\":"), json_string(m->url);
                printf(",\"sha256\":"), json_string(m->sha256);
                printf(",\"bytes\":%llu,\"recommended_ram_gib\":%u,\"state\":\"%s\",\"resource\":\"%s\","
                       "\"resource_reason\":",
                       (unsigned long long) m->bytes, m->recommended_ram_gib, states[state[i]],
                       resources[f->resource]);
                json_string(f->resource_reason);
                printf("}");
            } else {
                char        size[16];
                const char *mark = installed                             ? "✓"
                                   : state[i] == GEISTR_INSTALL_MISMATCH ? "✗"
                                                                         : "↓";
                char        label[160];
                bool quant = m->quantization && !strstr(m->name, m->quantization);
                snprintf(label, sizeof label, "%s%s%s", m->name, quant ? " · " : "", quant ? m->quantization : "");
                printf("%s %-18s %-36s %8s", mark, m->id, label, size_text(m->bytes, size));
                if (state[i] == GEISTR_INSTALL_MISMATCH)
                    printf("  ✗ file does not match; geistr pull %s", m->id);
                else if (f->resource != GEISTR_RESOURCE_FITS)
                    printf("  %s %s", f->resource == GEISTR_RESOURCE_UNAVAILABLE ? "✗" : "⚠",
                           limit_text(f->resource_reason));
                putchar('\n');
            }
            first = false;
        }
    if (json)
        puts("]}");
    geistr_ranking_free(r);
    geistr_catalog_free(c);
    return OK;
}

/* A catalog id → its verified file; a path stays a path. */
static int resolve(const char *model, char *path, size_t cap, geistr_reasoning *reasoning) {
    geistr_catalog *c   = load_catalog();
    const char     *base = strrchr(model, '/') ? strrchr(model, '/') + 1 : model;
    *reasoning           = GEISTR_REASONING_NONE;
    bool is_path         = strchr(model, '/') || strstr(model, ".gguf") || !strncmp(model, "stub:", 5);
    for (size_t i = 0; c && i < geistr_catalog_count(c); i++) {
        const geistr_catalog_entry *m = geistr_catalog_get(c, i);
        if (is_path ? strcmp(base, m->file) : strcmp(model, m->id))
            continue;
        if (m->reasoning_format && !strcmp(m->reasoning_format, "think_tags"))
            *reasoning = GEISTR_REASONING_THINK_TAGS;
        if (!is_path) {
            geistr_install state = install_state(m, false);
            if (state != GEISTR_INSTALL_OK) {
                fprintf(stderr,
                        state == GEISTR_INSTALL_MISSING ? "geistr: %s is not installed; geistr pull %s\n"
                                                        : "geistr: %s does not match the catalog; geistr pull %s\n",
                        model, model);
                geistr_catalog_free(c);
                return ERROR;
            }
            snprintf(path, cap, "%s/%s", models_dir, m->file);
        }
    }
    geistr_catalog_free(c);
    if (!is_path && !path[0]) {
        fprintf(stderr, "geistr: unknown model %s (see geistr catalog)\n", model);
        return ERROR;
    }
    if (is_path)
        snprintf(path, cap, "%s", model);
    return OK;
}

static geistr_chat *volatile running; /* for the Ctrl-C handler */
static volatile sig_atomic_t interrupted;
static struct md            view;

static void on_interrupt(int signal) {
    (void) signal;
    interrupted = 1;
    if (running)
        geistr_chat_cancel(running); /* an atomic store: safe in a handler */
}

static int print_piece(void *context, const geistr_piece *piece) {
    (void) context;
    md_feed(&view, piece->text);
    return 1;
}

static bool tty_out(void) {
    return isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
}

/* "  42.3 tok/s · 1.8 s" after an answer: generation speed and the whole turn. */
static void speed(geistr_chat *chat, FILE *out) {
    geistr_stats st = {.size = sizeof st};
    if (!cfg.stats || geistr_chat_stats(chat, &st) != GEISTR_OK || !st.output_tokens)
        return;
    bool   dim  = out == stdout ? tty_out() : isatty(STDERR_FILENO);
    double rate = st.generation_ms > 0 ? st.output_tokens / (st.generation_ms / 1000) : 0;
    fprintf(out, "%s  %.1f tok/s · %.1f s%s\n", dim ? "\033[2m" : "", rate, st.total_ms / 1000, dim ? "\033[0m" : "");
}

static void chat_help(void) {
    puts("/clear  a new conversation    /exit  end (or Ctrl-D)    Ctrl-C  stop the answer");
}

static int converse(const char *model, const char *prompt, bool interactive, const char *processor) {
    char             path[4200] = "";
    geistr_reasoning reasoning;
    int              rc = resolve(model, path, sizeof path, &reasoning);
    if (rc != OK)
        return rc;
    geistr_model     *m  = nullptr;
    geistr_model_opts mo = GEISTR_MODEL_OPTS_INIT;
    mo.processor = !strcmp(processor, "cpu") ? GEISTR_PROCESSOR_CPU
                   : !strcmp(processor, "gpu") ? GEISTR_PROCESSOR_GPU
                                               : GEISTR_PROCESSOR_AUTO;
    char error[256];
    if (geistr_model_open(path, &mo, &m, error, sizeof error) != GEISTR_OK) {
        fprintf(stderr, "geistr: cannot open %s: %s\n", path, error);
        return ERROR;
    }
    geistr_model_info info = {.size = sizeof info};
    bool              gpu  = geistr_model_info_get(m, &info) == GEISTR_OK && info.backend && strcmp(info.backend, "cpu");
    geistr_chat_opts  opts = GEISTR_CHAT_OPTS_INIT;
    opts.reasoning         = reasoning;
    opts.temperature       = (float) cfg.temperature;
    opts.overflow          = interactive ? GEISTR_OVERFLOW_DROP_OLDEST : GEISTR_OVERFLOW_REFUSE;
    geistr_chat  *chat     = nullptr;
    geistr_status s        = geistr_chat_open(m, &opts, &chat);
    geistr_model_close(m); /* the chat holds its own reference */
    if (s != GEISTR_OK) {
        fprintf(stderr, "geistr: cannot chat: %s\n", geistr_status_text(s));
        return ERROR;
    }
    /* Remember the model for the next chat. */
    if (interactive && strcmp(cfg.model, model) && config_path[0]) {
        snprintf(cfg.model, sizeof cfg.model, "%s", model);
        (void) config_save();
    }
    struct sigaction sa = {.sa_handler = on_interrupt};
    sigaction(SIGINT, &sa, nullptr); /* no SA_RESTART: Ctrl-C at the prompt ends fgets */
    running             = chat;
    rc                  = OK;
    enum md_mode mode   = cfg.markdown && tty_out() ? MD_ANSI : MD_RAW;
    bool         fresh  = true; /* the system prompt opens a conversation */
    geistr_message turn[2];
    if (!interactive) {
        size_t n = 0;
        if (cfg.system[0])
            turn[n++] = (geistr_message) {"system", cfg.system};
        turn[n++] = (geistr_message) {"user", prompt};
        md_init(&view, mode, stdout);
        s = geistr_chat_run(chat, n, turn, print_piece, nullptr);
        md_finish(&view);
        putchar('\n');
        speed(chat, stderr);
        rc = s == GEISTR_OK ? OK : s == GEISTR_CANCELLED ? CANCELLED : ERROR;
    } else {
        static char line[1 << 16];
        const char *symbol = gpu ? "⚡" : "⚙";
        for (;;) {
            interrupted = 0;
            if (tty_out())
                printf("\033[2m%s\033[0m > ", symbol);
            else
                printf("%s > ", symbol);
            fflush(stdout);
            if (!fgets(line, sizeof line, stdin)) {
                if (interrupted && !feof(stdin)) { /* Ctrl-C at the prompt: a new prompt */
                    clearerr(stdin);
                    putchar('\n');
                    continue;
                }
                break;
            }
            line[strcspn(line, "\n")] = 0;
            if (!line[0])
                continue;
            if (line[0] == '/') {
                if (!strcmp(line, "/exit") || !strcmp(line, "/quit"))
                    break;
                if (!strcmp(line, "/clear")) {
                    (void) geistr_chat_rewind(chat, 0);
                    fresh = true;
                } else
                    chat_help();
                continue;
            }
            size_t n = 0;
            if (fresh && cfg.system[0])
                turn[n++] = (geistr_message) {"system", cfg.system};
            turn[n++] = (geistr_message) {"user", line};
            fresh     = false;
            md_init(&view, mode, stdout);
            s = geistr_chat_run(chat, n, turn, print_piece, nullptr);
            md_finish(&view);
            puts(s == GEISTR_CANCELLED ? " [stopped]" : "");
            if (s == GEISTR_OK)
                speed(chat, stdout);
        }
        putchar('\n');
    }
    if (s != GEISTR_OK && s != GEISTR_CANCELLED)
        fprintf(stderr, "geistr: %s: %s\n", geistr_status_text(s), geistr_chat_error(chat));
    running = nullptr;
    geistr_chat_close(chat);
    return rc;
}

static int pull(const char *id) {
    geistr_catalog *c = load_catalog();
    if (!c)
        return ERROR;
    const geistr_catalog_entry *m = geistr_catalog_find(c, id);
    int                         rc;
    if (!m) {
        fprintf(stderr, "geistr: unknown model %s (see geistr catalog)\n", id);
        rc = ERROR;
    } else if (install_state(m, false) == GEISTR_INSTALL_OK) {
        printf("✓ %s is installed\n", id);
        rc = OK;
    } else
        rc = geistr_pull(m, models_dir);
    geistr_catalog_free(c);
    return rc;
}

int main(int argc, char **argv) {
    const char *args[64];
    int         n = 0;
    bool        installed = false, available = false, json = false;
    const char *processor = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--models") && i + 1 < argc)
            models_dir = argv[++i];
        else if (!strcmp(argv[i], "--catalog") && i + 1 < argc)
            catalog_file = argv[++i];
        else if (!strcmp(argv[i], "--installed"))
            installed = true;
        else if (!strcmp(argv[i], "--available"))
            available = true;
        else if (!strcmp(argv[i], "--json"))
            json = true;
        else if (!strcmp(argv[i], "--cpu") || !strcmp(argv[i], "--gpu"))
            processor = argv[i] + 2;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
            return usage(), OK;
        else if (!strncmp(argv[i], "--", 2) || n == 64)
            return usage();
        else
            args[n++] = argv[i];
    }
    if (!n)
        return usage();
    if (!models_dir) {
        if (geistr_models_dir(default_models, sizeof default_models) != GEISTR_OK) {
            fputs("geistr: no model folder: set HOME or GEISTEN_HOME, or use --models\n", stderr);
            return ERROR;
        }
        models_dir = default_models;
    }
    if (data_folder())
        config_load();
    const char *command = args[0];
    if (!strcmp(command, "config") && n <= 64)
        return processor ? usage() : config(n, args);
    if (!processor)
        processor = cfg.processor;
    if (!strcmp(command, "catalog") && n == 1 && !(installed && available))
        return catalog(installed, available, json);
    if (installed || available || json)
        return usage();
    if (!strcmp(command, "chat") && n == 1) { /* the last model, else the geisten app's */
        static char selected[256];
        char        file[4300];
        snprintf(file, sizeof file, "%s/selected", data_dir);
        FILE *f = !cfg.model[0] && data_dir[0] ? fopen(file, "r") : nullptr;
        if (f && fgets(selected, sizeof selected, f))
            selected[strcspn(selected, "\n")] = 0;
        if (f)
            fclose(f);
        const char *model = cfg.model[0] ? cfg.model : selected;
        if (!model[0]) {
            fputs("geistr: which model? geistr chat <model> (see geistr catalog)\n", stderr);
            return USAGE;
        }
        return converse(model, nullptr, true, processor);
    }
    if (!strcmp(command, "chat") && n == 2)
        return converse(args[1], nullptr, true, processor);
    if (!strcmp(command, "pull") && n == 2)
        return pull(args[1]);
    if (!strcmp(command, "run") && n >= 2) {
        static char prompt[1 << 20];
        size_t      len = 0;
        if (n == 2)
            len = fread(prompt, 1, sizeof prompt - 1, stdin);
        else
            for (int i = 2; i < n; i++)
                len += (size_t) snprintf(prompt + len, sizeof prompt - len, "%s%s", i > 2 ? " " : "", args[i]);
        prompt[len < sizeof prompt ? len : sizeof prompt - 1] = 0;
        if (!prompt[0]) {
            fputs("geistr: empty prompt\n", stderr);
            return USAGE;
        }
        return converse(args[1], prompt, false, processor);
    }
    return usage();
}

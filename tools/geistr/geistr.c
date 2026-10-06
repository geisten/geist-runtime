/*
 * geistr — a minimal CLI on the runtime API (#11). The model runs in this
 * process; no service is involved.
 *
 *   geistr run <model> [prompt…]     one answer to stdout (prompt from stdin if none)
 *   geistr chat <model>              interactive; Ctrl-C stops the answer, not the chat
 *   geistr catalog [--installed | --available] [--json]
 *   geistr pull <id>                 download and verify (builds with the download module)
 *   geistr config [key [value]]      settings, remembered between runs (geistr.conf)
 *   geistr serve <model> [--socket=PATH] [--chats N]
 *                                    the model as a service on a Unix socket (service.h)
 *   geistr chat --socket[=PATH]      chat with that service
 *
 * <model> is a catalog id or a path to a GGUF; chat without one continues with
 * the last model (or the geisten app's). In a terminal the chat continues the
 * last conversation (--new or /clear for a new one; geistr config resume off). In a terminal the answer is shown as
 * Markdown with math as Unicode; the prompt shows ⚙ (CPU) or ⚡ (GPU), and
 * each answer ends with its speed. Options anywhere:
 *   --models DIR    model folder (default: the geisten app's, see geistr_models_dir)
 *   --catalog FILE  catalog JSON (default: the app's catalog.json next to the
 *                   model folder if present, else the one built in)
 *   --cpu, --gpu    the processor for this run
 *   --new           chat: a new conversation instead of the last one
 *   --socket[=PATH] the service's socket (default: geistr.sock next to the model folder)
 *
 * Exit codes: 0 ok, 1 error, 2 usage, 130 cancelled.
 */
#include "geistr.h"
#include "geistr_catalog.h"
#include "pull.h"
#include "lineedit.h"
#include "render.h"
#include "service.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <termios.h>
#include <time.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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
    bool   markdown, stats, intro, resume;
} cfg = {.processor = "auto", .markdown = true, .stats = true, .intro = true, .resume = true};
static char config_path[4200], config_dir[4096], data_dir[4096];
static const char *const config_keys[] = {"model", "processor", "temperature", "system", "markdown", "stats", "intro", "resume"};

static bool make_dirs(const char *path) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            (void) mkdir(dir, 0700);
            *p = '/';
        }
    return mkdir(dir, 0700) == 0 || errno == EEXIST;
}

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
    if (!own && access(config_path, F_OK) != 0 && access(old, F_OK) == 0 && make_dirs(config_dir))
        (void) rename(old, config_path);
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
    else if (!strcmp(key, "markdown") || !strcmp(key, "stats") || !strcmp(key, "intro") || !strcmp(key, "resume")) {
        if (*value && strcmp(value, "on") && strcmp(value, "off"))
            return "the value is on or off";
        *(!strcmp(key, "markdown") ? &cfg.markdown
          : !strcmp(key, "stats")  ? &cfg.stats
          : !strcmp(key, "intro")  ? &cfg.intro
                                   : &cfg.resume) = strcmp(value, "off") != 0;
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
        snprintf(out, cap, "%s",
                 (!strcmp(key, "markdown") ? cfg.markdown
                  : !strcmp(key, "stats")  ? cfg.stats
                  : !strcmp(key, "intro")  ? cfg.intro
                                           : cfg.resume)
                         ? "on"
                         : "off");
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
    if (!make_dirs(config_dir)) /* the folder may not exist yet without the app */
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
        fprintf(stderr, "geistr: unknown key %s (model processor temperature system markdown stats intro resume)\n", args[1]);
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
          "       geistr config [key [value]]   (keys: model processor temperature system markdown stats intro resume)\n"
          "       geistr serve <model> [--socket=PATH] [--chats N]\n"
          "       geistr chat --socket[=PATH]\n"
          "options: --models DIR  --catalog FILE  --cpu  --gpu  --new (chat)\n"
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

static void json_to(FILE *f, const char *s) {
    fputc('"', f);
    for (; s && *s; s++)
        if (*s == '"' || *s == '\\')
            fprintf(f, "\\%c", *s);
        else if ((unsigned char) *s < 32)
            fprintf(f, "\\u%04x", *s);
        else
            fputc(*s, f);
    fputc('"', f);
}

static void json_string(const char *s) {
    json_to(stdout, s);
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

/* The answer as the user saw it: kept for a session switch. */
static char  *answer;
static size_t answer_len, answer_cap;

static int print_piece(void *context, const geistr_piece *piece) {
    (void) context;
    md_feed(&view, piece->text);
    if (piece->part == GEISTR_PART_ANSWER && answer_len + piece->len + 1 > answer_cap) {
        size_t cap = answer_cap ? answer_cap * 2 : 4096;
        while (cap < answer_len + piece->len + 1)
            cap *= 2;
        char *grown = realloc(answer, cap);
        if (!grown)
            return 1;
        answer = grown, answer_cap = cap;
    }
    if (piece->part == GEISTR_PART_ANSWER) {
        memcpy(answer + answer_len, piece->text, piece->len + 1);
        answer_len += piece->len;
    }
    return 1;
}

static bool tty_out(void) {
    return isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
}

/* The answer view: Markdown in a terminal, sized to it (tables). */
static void view_begin(void) {
    md_init(&view, cfg.markdown && tty_out() ? MD_ANSI : MD_RAW, stdout);
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col)
        view.width = ws.ws_col;
}

/* "  42.3 tok/s · 1.8 s" after an answer: generation speed and the whole turn. */
static void speed_line(unsigned tokens, double generation_ms, double total_ms, FILE *out) {
    if (!cfg.stats || !tokens)
        return;
    bool   dim  = out == stdout ? tty_out() : isatty(STDERR_FILENO);
    double rate = generation_ms > 0 ? tokens / (generation_ms / 1000) : 0;
    fprintf(out, "%s  %.1f tok/s · %.1f s%s\n", dim ? "\033[2m" : "", rate, total_ms / 1000, dim ? "\033[0m" : "");
}

static void speed(geistr_chat *chat, FILE *out) {
    geistr_stats st = {.size = sizeof st};
    if (geistr_chat_stats(chat, &st) == GEISTR_OK)
        speed_line(st.output_tokens, st.generation_ms, st.total_ms, out);
}

/* ---- while a model loads: a spinner with its size and the time ----------- */

static struct {
    pthread_t   thread;
    atomic_bool stop;
    bool        on;
    char        label[300];
} spin;

static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + (double) t.tv_nsec / 1e9;
}

static void *spinner(void *unused) {
    (void) unused;
    static const char *const frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    double                   start    = seconds();
    for (unsigned i = 0; !atomic_load(&spin.stop); i++) {
        if (seconds() - start > 0.3) /* quick loads stay quiet */
            fprintf(stdout, "\r\033[2K\033[2m%s %s · %.0f s\033[0m", frames[i % 10], spin.label, seconds() - start);
        fflush(stdout);
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, nullptr);
    }
    return nullptr;
}

static void spinner_start(const char *name, const char *path) {
    struct stat st;
    char        size[24] = "";
    if (stat(path, &st) == 0)
        snprintf(size, sizeof size, " · %.1f GB", (double) st.st_size / 1e9);
    spin.on = tty_out() && strncmp(path, "stub:", 5) != 0;
    if (!spin.on)
        return;
    snprintf(spin.label, sizeof spin.label, "loading %s%s", name, size);
    atomic_store(&spin.stop, false);
    spin.on = pthread_create(&spin.thread, nullptr, spinner, nullptr) == 0;
}

static void spinner_stop(void) {
    if (!spin.on)
        return;
    atomic_store(&spin.stop, true);
    pthread_join(spin.thread, nullptr);
    fputs("\r\033[2K", stdout);
    fflush(stdout);
    spin.on = false;
}

/* ---- while an answer runs: Esc stops it, other keys wait for the prompt --- */

static struct {
    pthread_t      thread;
    atomic_bool    stop;
    bool           on;
    struct termios cooked;
    struct le     *editor;
} keys_watch;

static void *watch_keys(void *unused) {
    (void) unused;
    while (!atomic_load(&keys_watch.stop)) {
        struct pollfd in = {.fd = STDIN_FILENO, .events = POLLIN};
        if (poll(&in, 1, 50) <= 0)
            continue;
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1)
            break;
        if (c == 27 && poll(&in, 1, 30) == 0) { /* Esc alone: stop the answer */
            interrupted = 1; /* a service's answer (chat --socket) */
            if (running)
                geistr_chat_cancel(running);
            continue;
        }
        le_type_ahead(keys_watch.editor, &c, 1); /* for the next prompt */
    }
    return nullptr;
}

static void watch_start(struct le *editor) {
    if (tcgetattr(STDIN_FILENO, &keys_watch.cooked) != 0)
        return;
    struct termios raw = keys_watch.cooked;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON); /* ISIG stays: Ctrl-C still stops the answer */
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    keys_watch.editor = editor;
    atomic_store(&keys_watch.stop, false);
    keys_watch.on = pthread_create(&keys_watch.thread, nullptr, watch_keys, nullptr) == 0;
    if (!keys_watch.on)
        tcsetattr(STDIN_FILENO, TCSANOW, &keys_watch.cooked);
}

static void watch_stop(void) {
    if (!keys_watch.on)
        return;
    atomic_store(&keys_watch.stop, true);
    pthread_join(keys_watch.thread, nullptr);
    tcsetattr(STDIN_FILENO, TCSANOW, &keys_watch.cooked);
    keys_watch.on = false;
}

static void shortcuts(void) {
    bool dim = tty_out();
    printf("%sEnter send · Esc stop the answer · Ctrl-C clear the line, twice: exit · Ctrl-D exit\n"
           "/ commands (↑↓ choose · Tab take · Esc close) · ↑↓ earlier lines · → take the hint\n"
           "Ctrl-A/E start/end · Ctrl-U/K delete to start/end · Ctrl-W a word · Ctrl-L clear screen%s\n",
           dim ? "\033[2m" : "", dim ? "\033[0m" : "");
}

static void intro(const char *name, const char *backend, bool gpu) {
    if (!cfg.intro || !tty_out())
        return;
    printf("\033[2mgeistr · %s on %s %s   (⚙ CPU · ⚡ GPU)\n"
           "/ commands · ? shortcuts · Esc stops an answer · Ctrl-C twice exits · geistr config intro off\033[0m\n",
           name, gpu ? "⚡" : "⚙", backend);
}

/* A model with a chat on it, and the choices behind both. Replaced as a whole
 * by a runtime switch (/gpu, /model …); the conversation moves along. */
struct session {
    geistr_model *model;
    geistr_chat  *chat;
    char          name[256], processor[8], backend[16], format[16];
    double        temperature;
    uint32_t      context;
};

static void session_close(struct session *x) {
    geistr_chat_close(x->chat);
    geistr_model_close(x->model);
    *x = (struct session) {};
}

/* Open the chat on x->model with x's options (model already open). */
static bool session_chat(struct session *x, geistr_reasoning reasoning, bool interactive) {
    geistr_chat_opts opts = GEISTR_CHAT_OPTS_INIT;
    opts.reasoning        = reasoning;
    opts.temperature      = (float) x->temperature;
    opts.overflow         = interactive ? GEISTR_OVERFLOW_DROP_OLDEST : GEISTR_OVERFLOW_REFUSE;
    geistr_status s       = geistr_chat_open(x->model, &opts, &x->chat);
    if (s != GEISTR_OK)
        fprintf(stderr, "geistr: cannot chat: %s\n", geistr_status_text(s));
    return s == GEISTR_OK;
}

static int session_open(struct session *x, const char *name, const char *processor, double temperature,
                        bool interactive) {
    char             path[4200] = "";
    geistr_reasoning reasoning;
    int              rc = resolve(name, path, sizeof path, &reasoning);
    if (rc != OK)
        return rc;
    geistr_model_opts mo = GEISTR_MODEL_OPTS_INIT;
    mo.processor         = !strcmp(processor, "cpu")   ? GEISTR_PROCESSOR_CPU
                           : !strcmp(processor, "gpu") ? GEISTR_PROCESSOR_GPU
                                                       : GEISTR_PROCESSOR_AUTO;
    char error[256];
    *x = (struct session) {.temperature = temperature};
    spinner_start(name, path);
    geistr_status opened = geistr_model_open(path, &mo, &x->model, error, sizeof error);
    spinner_stop();
    if (opened != GEISTR_OK) {
        fprintf(stderr, "geistr: cannot open %s: %s\n", path, error);
        return ERROR;
    }
    geistr_model_info info = {.size = sizeof info};
    if (geistr_model_info_get(x->model, &info) == GEISTR_OK) {
        snprintf(x->backend, sizeof x->backend, "%s", info.backend ? info.backend : "cpu");
        snprintf(x->format, sizeof x->format, "%s", info.chat_format ? info.chat_format : "");
        x->context = info.context;
    }
    snprintf(x->name, sizeof x->name, "%s", name);
    snprintf(x->processor, sizeof x->processor, "%s", processor);
    if (!session_chat(x, reasoning, interactive)) {
        session_close(x);
        return ERROR;
    }
    return OK;
}

static bool on_gpu(const struct session *x) {
    return strcmp(x->backend, "cpu") != 0;
}

/* The conversation as text, so it survives a switch of model or processor. */
struct transcript {
    size_t n, cap;
    char **role, **content;
};

static void transcript_push(struct transcript *t, const char *role, const char *content) {
    if (t->n == t->cap) {
        size_t cap  = t->cap ? t->cap * 2 : 16;
        char **r    = realloc(t->role, cap * sizeof *r);
        char **c    = r ? realloc(t->content, cap * sizeof *c) : nullptr;
        if (r)
            t->role = r;
        if (!c)
            return;
        t->content = c, t->cap = cap;
    }
    t->role[t->n]    = strdup(role);
    t->content[t->n] = strdup(content);
    if (t->role[t->n] && t->content[t->n])
        t->n++;
}

static void transcript_clear(struct transcript *t) {
    for (size_t i = 0; i < t->n; i++)
        free(t->role[i]), free(t->content[i]);
    t->n = 0;
}

/* ---- the conversation across runs: <data>/chats/<ms>-<pid>.jsonl ------------
 * Each chat writes its own file after every answer (role and content per
 * line); the next chat continues the newest and, once it writes, removes it.
 * Only in a terminal: piped chats stay reproducible. */
static char chat_file[4400], resumed_from[4400];
static bool fresh; /* --new */

static void chat_file_new(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    snprintf(chat_file, sizeof chat_file, "%s/chats/%lld%03ld-%ld.jsonl", data_dir, (long long) t.tv_sec,
             t.tv_nsec / 1000000, (long) getpid());
}

static void chat_store(const struct transcript *t) {
    char dir[4200], tmp[4500];
    snprintf(dir, sizeof dir, "%s/chats", data_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", chat_file);
    if (!chat_file[0] || !t->n || !make_dirs(dir))
        return;
    int   fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600); /* private: what was said */
    FILE *f  = fd >= 0 ? fdopen(fd, "w") : nullptr;
    if (!f) {
        if (fd >= 0)
            close(fd);
        return;
    }
    for (size_t i = 0; i < t->n; i++) {
        fputs("{\"role\":", f), json_to(f, t->role[i]);
        fputs(",\"content\":", f), json_to(f, t->content[i]), fputs("}\n", f);
    }
    if (fclose(f) == 0 && rename(tmp, chat_file) == 0) {
        if (resumed_from[0]) /* it lives on in this chat's file */
            unlink(resumed_from), resumed_from[0] = 0;
    } else
        unlink(tmp);
}

/* The newest conversation into t (empty if none). */
static void chat_resume(struct transcript *t) {
    char dir[4200], best[300] = "";
    snprintf(dir, sizeof dir, "%s/chats", data_dir);
    DIR   *d     = opendir(dir);
    time_t newest = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        size_t      n = strlen(e->d_name);
        struct stat st;
        char        path[4500];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (n > 6 && !strcmp(e->d_name + n - 6, ".jsonl") && n < sizeof best && stat(path, &st) == 0 &&
            (st.st_mtime > newest || (st.st_mtime == newest && strcmp(e->d_name, best) > 0)))
            newest = st.st_mtime, snprintf(best, sizeof best, "%s", e->d_name);
    }
    if (d)
        closedir(d);
    if (!best[0])
        return;
    snprintf(resumed_from, sizeof resumed_from, "%s/%s", dir, best);
    FILE  *f    = fopen(resumed_from, "r");
    char  *line = nullptr;
    size_t cap  = 0;
    for (ssize_t len; f && (len = getline(&line, &cap, f)) > 0;) {
        char *role = malloc((size_t) len + 1), *content = malloc((size_t) len + 1);
        if (role && content) {
            service_field(line, "role", role, (size_t) len + 1);
            service_field(line, "content", content, (size_t) len + 1);
            if (role[0])
                transcript_push(t, role, content);
        }
        free(role), free(content);
    }
    free(line);
    if (f)
        fclose(f);
}

/* Tab completion in the chat: the commands, and after /model the models
 * found in the model folder (receipts only: no hashing at the prompt). */
static const struct le_candidate commands[] = {
        {"/gpu", "processor: GPU (the conversation moves along)"},
        {"/cpu", "processor: CPU"},
        {"/auto", "processor: the runtime's choice"},
        {"/model ", "another model, same conversation"},
        {"/temp ", "sampling temperature, 0 to 2"},
        {"/system ", "system prompt (empty: none)"},
        {"/info", "what runs now"},
        {"/save", "keep these settings for the next chat"},
        {"/clear", "a new conversation"},
        {"/help", "the commands"},
        {"/exit", "end (or Ctrl-D)"},
};
static char   installed_ids[64][64], model_lines[64][80];
static size_t n_installed;

static void find_installed(void) {
    geistr_catalog *c = load_catalog();
    n_installed       = 0;
    for (size_t i = 0; c && i < geistr_catalog_count(c) && n_installed < 64; i++) {
        const geistr_catalog_entry *m     = geistr_catalog_get(c, i);
        geistr_install              state = GEISTR_INSTALL_MISSING;
        if (geistr_catalog_check(m, models_dir, false, &state) == GEISTR_OK &&
            (state == GEISTR_INSTALL_OK || state == GEISTR_INSTALL_UNVERIFIED))
            snprintf(installed_ids[n_installed++], sizeof installed_ids[0], "%s", m->id);
    }
    geistr_catalog_free(c);
}

static size_t complete_line(void *ctx, const char *line, struct le_candidate *out, size_t max) {
    (void) ctx;
    size_t n = 0;
    if (!strncmp(line, "/model ", 7)) {
        const char *typed = line + 7;
        for (size_t i = 0; i < n_installed && n < max; i++)
            if (!strncmp(installed_ids[i], typed, strlen(typed))) {
                snprintf(model_lines[n], sizeof model_lines[0], "/model %.63s", installed_ids[i]);
                out[n] = (struct le_candidate) {model_lines[n], nullptr};
                n++;
            }
        return n;
    }
    if (line[0] != '/' || strchr(line, ' '))
        return 0;
    for (size_t i = 0; i < sizeof commands / sizeof *commands && n < max; i++)
        if (!strncmp(commands[i].line, line, strlen(line)))
            out[n++] = commands[i];
    return n;
}

static void chat_help(void) {
    for (size_t i = 0; i < sizeof commands / sizeof *commands; i++)
        printf("%-10s %s\n", commands[i].line, commands[i].help);
    shortcuts();
}

static void status_line(const struct session *x, const char *what) {
    bool dim = tty_out();
    printf("%s%s %s · %s%s%s\n", dim ? "\033[2m" : "", on_gpu(x) ? "⚡" : "⚙", x->backend, x->name, what,
           dim ? "\033[0m" : "");
}

/* One answer to `prompt` (geistr run). */
static int answer_once(const char *name, const char *prompt, const char *processor) {
    struct session x;
    int            rc = session_open(&x, name, processor, cfg.temperature, false);
    if (rc != OK)
        return rc;
    struct sigaction sa = {.sa_handler = on_interrupt};
    sigaction(SIGINT, &sa, nullptr);
    running              = x.chat;
    geistr_message turn[2];
    size_t         n = 0;
    if (cfg.system[0])
        turn[n++] = (geistr_message) {"system", cfg.system};
    turn[n++] = (geistr_message) {"user", prompt};
    view_begin();
    geistr_status s = geistr_chat_run(x.chat, n, turn, print_piece, nullptr);
    md_finish(&view);
    putchar('\n');
    speed(x.chat, stderr);
    if (s != GEISTR_OK && s != GEISTR_CANCELLED)
        fprintf(stderr, "geistr: %s: %s\n", geistr_status_text(s), geistr_chat_error(x.chat));
    running = nullptr;
    session_close(&x);
    return s == GEISTR_OK ? OK : s == GEISTR_CANCELLED ? CANCELLED : ERROR;
}

/* chat --socket: the model is the service's; this process holds the conversation. */
static const char *remote;

static bool remote_part(void *ctx, const char *text) {
    geistr_piece p = {.size = sizeof p, .part = GEISTR_PART_ANSWER, .text = text, .len = strlen(text)};
    return print_piece(ctx, &p);
}

static bool remote_cancel(void *ctx) {
    (void) ctx;
    return interrupted;
}

static int chat(const char *name, const char *processor) {
    struct session x = {};
    if (remote) {
        char info[1024], context[16];
        if (service_info(remote, info, sizeof info) != GEISTR_OK) {
            fprintf(stderr, "geistr: no service on %s (geistr serve <model>)\n", remote);
            return ERROR;
        }
        service_field(info, "model", x.name, sizeof x.name);
        service_field(info, "backend", x.backend, sizeof x.backend);
        service_field(info, "chat_format", x.format, sizeof x.format);
        service_field(info, "context", context, sizeof context);
        x.context     = (uint32_t) strtoul(context, nullptr, 10);
        x.temperature = cfg.temperature;
    } else {
        int rc = session_open(&x, name, processor, cfg.temperature, true);
        if (rc != OK)
            return rc;
        if (strcmp(cfg.model, name) && config_path[0]) { /* remember the model for the next chat */
            snprintf(cfg.model, sizeof cfg.model, "%s", name);
            (void) config_save();
        }
    }
    struct sigaction sa = {.sa_handler = on_interrupt};
    sigaction(SIGINT, &sa, nullptr); /* no SA_RESTART: Ctrl-C at the prompt ends fgets */
    running = x.chat;
    intro(x.name, x.backend, on_gpu(&x));
    double last_ctrl_c = -10;
    char   system[sizeof cfg.system];
    snprintf(system, sizeof system, "%s", cfg.system);
    struct transcript said  = {};
    bool              carry = false; /* a new session reads `said` with the next message */
    static char line[1 << 16];
    /* In a terminal: the line editor (Tab completion, history); else plain lines. */
    bool      edit = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    struct le editor;
    if (edit) {
        find_installed();
        le_init(&editor, stdout, 80, complete_line, nullptr);
    }
    if (edit && cfg.resume && data_dir[0]) { /* the conversation outlives the chat */
        chat_file_new();
        if (!fresh)
            chat_resume(&said);
    }
    if (said.n) { /* where it was: its system prompt, its size, the last question */
        snprintf(system, sizeof system, "%s", !strcmp(said.role[0], "system") ? said.content[0] : "");
        carry            = true;
        const char *last = "";
        for (size_t i = said.n; i-- > 0 && !last[0];)
            if (!strcmp(said.role[i], "user"))
                last = said.content[i];
        int cut = 0; /* 60 characters, not bytes */
        for (int chars = 0; last[cut] && chars < 60; chars++)
            for (cut++; ((unsigned char) last[cut] & 0xC0) == 0x80; cut++) {
            }
        printf("\033[2m↻ %zu · „%.*s%s“ · /clear new\033[0m\n", said.n, cut, last, last[cut] ? "…" : "");
    }
    for (;;) {
        /* A Ctrl-C between two reads of the editor (the terminal is cooked
         * then, so it arrives as SIGINT) is a Ctrl-C at the prompt. */
        bool pressed = edit && interrupted;
        interrupted  = 0;
        char prompt[64];
        snprintf(prompt, sizeof prompt, tty_out() ? "\033[2m%s\033[0m > " : "%s > ", on_gpu(&x) ? "⚡" : "⚙");
        if (edit) {
            enum le_event ev = pressed ? LE_INTERRUPT : le_read(&editor, prompt);
            if (ev == LE_EOF)
                break;
            if (ev == LE_INTERRUPT) { /* on an empty line: twice to exit, as in Claude Code */
                if (seconds() - last_ctrl_c < 2)
                    break;
                last_ctrl_c = seconds();
                puts("\033[2m  Ctrl-C again to exit\033[0m");
                continue;
            }
            if (ev == LE_HELP) {
                shortcuts();
                continue;
            }
            snprintf(line, sizeof line, "%s", editor.buf);
            le_remember(&editor, line);
        } else {
            fputs(prompt, stdout);
            fflush(stdout);
            if (!fgets(line, sizeof line, stdin)) {
                if (interrupted && !feof(stdin)) { /* Ctrl-C at the prompt: a new prompt */
                    clearerr(stdin);
                    putchar('\n');
                    continue;
                }
                break;
            }
        }
        line[strcspn(line, "\n")] = 0;
        if (!line[0])
            continue;
        if (line[0] == '/') {
            char *arg = strchr(line, ' ');
            if (arg)
                *arg++ = 0;
            else
                arg = line + strlen(line);
            const char *processor_now = !strcmp(line, "/gpu")    ? "gpu"
                                        : !strcmp(line, "/cpu")  ? "cpu"
                                        : !strcmp(line, "/auto") ? "auto"
                                                                 : nullptr;
            if (!strcmp(line, "/exit") || !strcmp(line, "/quit"))
                break;
            if (remote && (processor_now || !strcmp(line, "/model"))) {
                puts("the service has its model: geistr serve <model> [--cpu | --gpu]");
                continue;
            }
            if (processor_now || (!strcmp(line, "/model") && *arg)) {
                /* Open the new session first: a failure keeps the current one. */
                struct session next;
                const char    *name_now = processor_now ? x.name : arg;
                if (session_open(&next, name_now, processor_now ? processor_now : x.processor, x.temperature, true) !=
                    OK)
                    continue;
                running = nullptr;
                session_close(&x);
                x       = next;
                running = x.chat;
                carry   = said.n > 0;
                status_line(&x, carry ? " · the conversation moves along" : "");
            } else if (!strcmp(line, "/temp")) {
                char  *end = nullptr;
                double t   = strtod(arg, &end);
                if (!*arg || *end || !(t >= 0 && t <= 2)) {
                    puts("/temp 0 … 2");
                    continue;
                }
                x.temperature = t;
                if (remote) {
                    printf("temperature %g\n", t);
                    continue;
                }
                geistr_chat_close(x.chat); /* sampling is a chat option: a new chat, same model */
                geistr_reasoning reasoning;
                char             path[4200];
                if (resolve(x.name, path, sizeof path, &reasoning) != OK || !session_chat(&x, reasoning, true))
                    break;
                running = x.chat;
                carry   = said.n > 0;
                printf("temperature %g\n", t);
            } else if (!strcmp(line, "/system")) {
                snprintf(system, sizeof system, "%s", arg);
                if (said.n && !strcmp(said.role[0], "system")) { /* replace it in the conversation */
                    free(said.content[0]);
                    said.content[0] = strdup(system);
                    if (!system[0]) {
                        free(said.role[0]), free(said.content[0]);
                        memmove(said.role, said.role + 1, (said.n - 1) * sizeof *said.role);
                        memmove(said.content, said.content + 1, (said.n - 1) * sizeof *said.content);
                        said.n--;
                    }
                } else if (said.n && system[0]) {
                    transcript_push(&said, "system", system); /* then move it to the front */
                    char *r = said.role[said.n - 1], *c = said.content[said.n - 1];
                    memmove(said.role + 1, said.role, (said.n - 1) * sizeof *said.role);
                    memmove(said.content + 1, said.content, (said.n - 1) * sizeof *said.content);
                    said.role[0] = r, said.content[0] = c;
                }
                if (said.n && !remote) { /* the chat holds the old one: read the conversation anew */
                    geistr_chat_close(x.chat);
                    geistr_reasoning reasoning;
                    char             path[4200];
                    if (resolve(x.name, path, sizeof path, &reasoning) != OK || !session_chat(&x, reasoning, true))
                        break;
                    running = x.chat;
                    carry   = true;
                }
                puts(system[0] ? "system prompt set" : "no system prompt");
            } else if (!strcmp(line, "/info")) {
                status_line(&x, remote ? " · service" : "");
                printf("chat format %s · context %u · temperature %g%s%s\n", x.format, x.context, x.temperature,
                       system[0] ? " · system: " : "", system);
            } else if (!strcmp(line, "/save")) {
                if (!remote) { /* a service's model is not this chat's choice */
                    snprintf(cfg.model, sizeof cfg.model, "%s", x.name);
                    snprintf(cfg.processor, sizeof cfg.processor, "%s", x.processor);
                }
                snprintf(cfg.system, sizeof cfg.system, "%s", system);
                cfg.temperature = x.temperature;
                puts(config_path[0] && config_save() ? "saved for the next chat" : "cannot save the settings");
            } else if (!strcmp(line, "/clear")) {
                (void) geistr_chat_rewind(x.chat, 0);
                transcript_clear(&said);
                carry = false;
                if (chat_file[0]) { /* the old one stays as it was */
                    chat_file_new();
                    resumed_from[0] = 0;
                }
            } else
                chat_help();
            continue;
        }
        /* Normally only the new message; after a switch, the conversation once.
         * A service always gets the whole conversation (it finds what it holds). */
        size_t before = said.n;
        if (!said.n && system[0])
            transcript_push(&said, "system", system);
        transcript_push(&said, "user", line);
        size_t          from  = carry || remote ? 0 : before;
        size_t          count = said.n - from;
        geistr_message *turn  = calloc(count, sizeof *turn);
        if (!turn)
            break;
        for (size_t i = 0; i < count; i++)
            turn[i] = (geistr_message) {said.role[from + i], said.content[from + i]};
        answer_len = 0;
        if (answer)
            answer[0] = 0;
        view_begin();
        if (edit)
            watch_start(&editor);
        struct svc_stats rs = {};
        char             why[512];
        geistr_status    s;
        if (remote) {
            static_assert(sizeof(struct svc_message) == sizeof(geistr_message));
            s = service_chat(remote, count, (const struct svc_message *) turn, 0, x.temperature, remote_part,
                             remote_cancel, nullptr, &rs, why, sizeof why);
        } else
            s = geistr_chat_run(x.chat, count, turn, print_piece, nullptr);
        if (edit)
            watch_stop();
        interrupted = 0; /* it stopped the answer, if it came */
        md_finish(&view);
        free(turn);
        puts(s == GEISTR_CANCELLED ? " [stopped]" : "");
        if (s == GEISTR_OK || s == GEISTR_CANCELLED) {
            carry = false;
            transcript_push(&said, "assistant", answer ? answer : "");
            chat_store(&said);
            if (s == GEISTR_OK && remote)
                speed_line(rs.output_tokens, rs.generation_ms, rs.total_ms, stdout);
            else if (s == GEISTR_OK)
                speed(x.chat, stdout);
        } else {
            said.n--; /* not part of the conversation: the chat refused it */
            free(said.role[said.n]), free(said.content[said.n]);
            fprintf(stderr, "geistr: %s: %s\n", geistr_status_text(s), remote ? why : geistr_chat_error(x.chat));
        }
    }
    if (edit)
        le_free(&editor);
    else
        putchar('\n');
    running = nullptr;
    session_close(&x);
    transcript_clear(&said);
    free(said.role), free(said.content), free(answer);
    return OK;
}

static volatile sig_atomic_t stopping;

static void on_stop(int signal) {
    (void) signal;
    stopping = 1;
}

static int serve(const char *name, const char *processor, const char *socket, size_t chats) {
    /* Idle OpenMP workers on the CPU spin forever otherwise (geistlib #651);
     * set before the engine starts its threads. */
    setenv("KMP_BLOCKTIME", "200", 0);
    struct session x;
    geistr_reasoning reasoning;
    char             path[4200];
    int              rc = resolve(name, path, sizeof path, &reasoning);
    if (rc == OK)
        rc = session_open(&x, name, processor, 0, true);
    if (rc != OK)
        return rc;
    geistr_chat_close(x.chat); /* the service opens its own */
    struct sigaction sa = {.sa_handler = on_stop};
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    struct svc_options o = {.model = x.model, .reasoning = reasoning, .name = x.name, .socket = socket,
                            .chats = chats, .stop = &stopping};
    rc = service_run(&o) ? ERROR : OK;
    geistr_model_close(x.model);
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
    setlocale(LC_CTYPE, ""); /* character widths for the view and the line editor */
    const char *args[64];
    int         n = 0;
    bool        installed = false, available = false, json = false;
    const char *processor = nullptr, *socket = nullptr;
    long        chats = 2;
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
        else if (!strcmp(argv[i], "--socket") || !strncmp(argv[i], "--socket=", 9))
            socket = argv[i][8] ? argv[i] + 9 : "";
        else if (!strcmp(argv[i], "--chats") && i + 1 < argc) {
            char *end;
            chats = strtol(argv[++i], &end, 10);
            if (*end || chats < 1 || chats > 64)
                return usage();
        } else if (!strcmp(argv[i], "--new"))
            fresh = true;
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
    static char socket_path[4300];
    if (!socket && !strcmp(command, "serve"))
        socket = "";
    if (socket && !*socket) {
        if (!data_dir[0]) {
            fputs("geistr: no data folder for the socket: use --socket=PATH\n", stderr);
            return ERROR;
        }
        snprintf(socket_path, sizeof socket_path, "%s/geistr.sock", data_dir);
        socket = socket_path;
    }
    if (!strcmp(command, "serve") && n == 2)
        return serve(args[1], processor, socket, (size_t) chats);
    if (!strcmp(command, "chat") && socket) {
        remote = socket;
        return n == 1 ? chat("", processor) : usage();
    }
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
        return chat(model, processor);
    }
    if (!strcmp(command, "chat") && n == 2)
        return chat(args[1], processor);
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
        return answer_once(args[1], prompt, processor);
    }
    return usage();
}

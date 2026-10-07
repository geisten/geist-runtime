/*
 * geistr — a minimal CLI on the runtime API (#11). The model runs in this
 * process; no service is involved.
 *
 *   geistr run <model> [prompt…]     one answer to stdout (prompt from stdin if none)
 *   geistr chat <model>              interactive; Ctrl-C stops the answer, not the chat
 *   geistr catalog [--installed | --available] [--json]
 *   geistr pull <id>                 download and verify (builds with the download module)
 *   geistr pull                      update installed models to this catalog (after a geistr update)
 *   geistr config [key [value]]      settings, remembered between runs (geistr.conf)
 *   geistr bench [model…]            measure tokens/s on CPU and GPU (shown by catalog)
 *   geistr bench --compare [A [B]]   two engines' bench speeds side by side, the change in %
 *   geistr serve <model> [--socket=PATH] [--chats N] [--http[=ADDR:PORT]]
 *                                    the model as a service on a Unix socket (service.h)
 *   geistr chat --socket[=PATH]      chat with that service
 *   geistr decide <model> --config FILE --question TEXT --option ID DESC…
 *                                    fixed-option scoring (EXPERIMENTAL, default-off; docs/DECISIONS.md)
 *   geistr catalog --decision-config FILE
 *                                    also show each model's decision permission (no model is loaded)
 *
 * <model> is a catalog id or a path to a GGUF; chat without one continues with
 * the last model (or the geisten app's). In a terminal the chat continues the
 * last conversation (--new or /clear for a new one; geistr config resume off). In a terminal the answer is shown as
 * Markdown with math as Unicode; the prompt shows ⚙ (CPU) or ⚡ (GPU), and
 * each answer ends with its speed. Options anywhere:
 *   --models DIR    model folder (default: the geisten app's, see geistr_models_dir)
 *   --catalog FILE  catalog JSON (default: the one built in, which matches
 *                   this engine; a new geistr brings a new one)
 *   --cpu, --gpu    the processor for this run
 *   --new           chat: a new conversation instead of the last one
 *   --socket[=PATH] the service's socket (default: geistr.sock next to the model folder)
 *
 * Exit codes: 0 ok, 1 error, 2 usage, 130 cancelled.
 */
#include "geistr.h"
#include "geistr_catalog.h"
#include "pull.h"
#include "service.h"
#include "json.h"
#include "cli.h"
#include "decide.h"

#include <time.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catalog_json.h" /* embedded_catalog[], generated from models/catalog.json */

/* Before every other constructor: the engine is built for this release's CPU
 * baseline (Linux: x86-64-v3, or ARMv8.2 with dotprod), and some of its own
 * constructors already use those instructions; on an older CPU the process
 * would die with "Illegal instruction" before main could say why. This file
 * is compiled without -march, so this check runs anywhere. */
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#if defined(__aarch64__)
#include <sys/auxv.h>
#endif
[[gnu::constructor(101)]] static void cpu_check(void) {
#if defined(__x86_64__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("x86-64-v3"))
        return;
    static const char need[] = "geistr: this CPU lacks AVX2 and FMA (x86-64-v3: Intel since 2013, AMD since 2015), "
                               "which this build needs\n";
#else
    const unsigned long required = HWCAP_ATOMICS | HWCAP_FPHP | HWCAP_ASIMDHP | HWCAP_ASIMDDP;
    if ((getauxval(AT_HWCAP) & required) == required)
        return;
    static const char need[] = "geistr: this CPU lacks ARMv8.2 with dotprod (e.g. Raspberry Pi 5, AWS Graviton 2), "
                               "which this build needs\n";
#endif
    (void) !write(STDERR_FILENO, need, sizeof need - 1);
    _exit(1);
}
#endif



const char        *models_dir;
static const char *catalog_file;
static char        default_models[4096];
static const char *decision_config_file;

static int usage(void) {
    fputs("usage: geistr run <model> [prompt…]\n"
          "       geistr chat <model>\n"
          "       geistr catalog [--installed | --available] [--json]\n"
          "       geistr pull [id]               a model, or: update the installed ones to this catalog\n"
          "       geistr config [key [value]]   (keys: model processor temperature system markdown stats intro resume)\n"
          "       geistr bench [model…]          tokens/s on ⚙ CPU and ⚡ GPU, shown in geistr catalog\n"
          "       geistr bench --compare [A [B]] two geistlib commits' bench speeds and the change in %\n"
          "       geistr serve <model> [--socket=PATH] [--chats N] [--http[=ADDR:PORT]]\n"
          "                                       (--http: OpenAI and Ollama APIs, default 127.0.0.1:11434)\n"
          "       geistr chat --socket[=PATH]\n"
          "       geistr decide <model> --config FILE --question TEXT --option ID DESC [--option ID DESC…]\n"
          "decision: --question-file FILE|-  --context TEXT  --processor auto|cpu|gpu\n"
          "          --mode dense|selected_rows  --profile NAME\n"
          "catalog: --decision-config FILE (permission only; does not load a model)\n"
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

/* --catalog, else the built-in copy: it ships with this engine, so it lists
 * only models the engine runs. An update of geistr is the catalog's update. */
geistr_catalog *load_catalog(void) {
    char            error[256];
    geistr_catalog *c = nullptr;
    if (catalog_file) {
        size_t len  = 0;
        char  *text = slurp(catalog_file, &len);
        if (!text) {
            fprintf(stderr, "geistr: cannot read %s\n", catalog_file);
            return nullptr;
        }
        if (geistr_catalog_parse(text, len, &c, error, sizeof error) != GEISTR_OK)
            fprintf(stderr, "geistr: %s: %s\n", catalog_file, error);
        free(text);
        return c;
    }
    if (geistr_catalog_parse((const char *) embedded_catalog, sizeof embedded_catalog, &c, error, sizeof error) !=
        GEISTR_OK)
        fprintf(stderr, "geistr: built-in catalog: %s\n", error);
    return c;
}

/* geistr decide's catalog: the same policy as every other command (its
 * arguments are this file's --models and --catalog). */
static geistr_catalog *decide_catalog(const char *directory, const char *file) {
    (void) directory, (void) file;
    return load_catalog();
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

static int catalog(bool installed_only, bool available_only, bool json) {
    geistr_catalog *c = load_catalog();
    if (!c)
        return ERROR;
    geistr_decision_config *permissions = nullptr;
    if (decision_config_file) {
        char error[256];
        if (geistr_decide_config_read(sizeof error, decision_config_file, &permissions, error) != GEISTR_OK) {
            fprintf(stderr, "geistr: %s\n", error); geistr_catalog_free(c); return ERROR;
        }
    }
    size_t         n = geistr_catalog_count(c);
    geistr_install state[1024];
    geistr_local  *local = calloc(n ? n : 1, sizeof *local);
    for (size_t i = 0; i < n; i++)
        state[i] = install_state(geistr_catalog_get(c, i), json);
    for (size_t i = 0; local && i < n; i++)
        local[i] = (geistr_local) {.size = sizeof *local, .installed = state[i] == GEISTR_INSTALL_OK};
    if (local)
        speeds_load(c, local);
    geistr_device   d = {};
    geistr_ranking *r = nullptr;
    if (!local || geistr_device_probe(models_dir, &d) != GEISTR_OK ||
        geistr_rank(c, &d, local, nullptr, &r) != GEISTR_OK) {
        fprintf(stderr, "geistr: cannot read this computer's memory\n");
        free(local);
        geistr_catalog_free(c);
        geistr_decision_config_free(permissions);
        return ERROR;
    }
    /* The bars share one scale; their width is what the terminal leaves. */
    bool           tty = tty_out();
    struct winsize ws;
    int            columns = tty && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col ? ws.ws_col : 80;
    int            bar     = (columns - 66 - 2 * 9) / 2;
    bar                    = !tty ? 0 : bar > 12 ? 12 : bar < 4 ? 0 : bar;
    double max             = 0;
    for (size_t i = 0; i < n; i++) {
        double v[] = {local[i].cpu.rate, local[i].gpu.rate, reference_rate(geistr_catalog_get(c, i), "cpu"),
                      reference_rate(geistr_catalog_get(c, i), "gpu")};
        for (size_t k = 0; k < 4; k++)
            max = v[k] > max ? v[k] : max;
    }
    if (json)
        printf("{\"schema\":1,\"models_dir\":"), json_write(stdout, models_dir), printf(",\"models\":[");
    bool first = true;
    /* Installed first, then available; catalog order within each. */
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < n; i++) {
            const geistr_catalog_entry *m         = geistr_catalog_get(c, i);
            const geistr_decision_policy *permission = geistr_decision_config_find(m->sha256, permissions);
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
                json_write(stdout, m->id);
                printf(",\"name\":"), json_write(stdout, m->name);
                printf(",\"quantization\":"), m->quantization ? json_write(stdout, m->quantization) : (void) printf("null");
                printf(",\"file\":"), json_write(stdout, m->file);
                printf(",\"url\":"), json_write(stdout, m->url);
                printf(",\"sha256\":"), json_write(stdout, m->sha256);
                printf(",\"bytes\":%llu,\"recommended_ram_gib\":%u,\"state\":\"%s\",\"resource\":\"%s\","
                       "\"resource_reason\":",
                       (unsigned long long) m->bytes, m->recommended_ram_gib, states[state[i]],
                       resources[f->resource]);
                json_write(stdout, f->resource_reason);
                printf(",\"tokens_per_s\":{\"cpu\":");
                local[i].cpu.rate > 0 ? (void) printf("%.1f", local[i].cpu.rate) : (void) printf("null");
                printf(",\"gpu\":");
                local[i].gpu.rate > 0 ? (void) printf("%.1f", local[i].gpu.rate) : (void) printf("null");
                printf("}");
                if (decision_config_file) {
                    printf(",\"decision\":{\"configured\":%s,\"profile\":", permission && permission->enabled ? "true" : "false");
                    if (permission && permission->profile) json_write(stdout, geistr_decision_profile_name(permission->profile));
                    else printf("null");
                    printf(",\"support\":\"not_loaded\",\"verified\":false}");
                }
                printf("}");
            } else {
                char        size[16];
                const char *mark = installed                             ? "✓"
                                   : state[i] == GEISTR_INSTALL_MISMATCH ? "⟳"
                                                                         : "↓";
                char        label[160];
                bool quant = m->quantization && !strstr(m->name, m->quantization);
                snprintf(label, sizeof label, "%s%s%s", m->name, quant ? " · " : "", quant ? m->quantization : "");
                printf("%s %-18s %-36s %8s", mark, m->id, label, size_text(m->bytes, size));
                speed_bar("⚙", local[i].cpu.rate, reference_rate(m, "cpu"), max, bar, tty);
                speed_bar("⚡", local[i].gpu.rate, reference_rate(m, "gpu"), max, bar, tty);
                if (state[i] == GEISTR_INSTALL_MISMATCH)
                    printf("  ⟳ not this catalog's file: geistr pull");
                else if (f->resource != GEISTR_RESOURCE_FITS)
                    printf("  %s %s", f->resource == GEISTR_RESOURCE_UNAVAILABLE ? "✗" : "⚠",
                           limit_text(f->resource_reason));
                if (permission && permission->enabled)
                    printf("  decision configured (%s; support not loaded)", geistr_decision_profile_name(permission->profile));
                putchar('\n');
            }
            first = false;
        }
    if (json)
        puts("]}");
    else if (tty)
        puts("\033[2m⚙ CPU · ⚡ GPU: tokens/s measured here (geistr bench), dim: the catalog's reference\033[0m");
    geistr_ranking_free(r);
    geistr_decision_config_free(permissions);
    geistr_catalog_free(c);
    free(local);
    return OK;
}

/* A catalog id → its verified file; a path stays a path. */
int resolve(const char *model, char *path, size_t cap, geistr_reasoning *reasoning) {
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

static geistr_decision *volatile running_decision;

/* geistr decide's Ctrl-C: cancels the scoring (an atomic store). */
static void on_decide_interrupt(int signal) {
    (void) signal;
    interrupted = 1;
    if (running_decision)
        (void) geistr_decision_cancel(running_decision);
}

/* A single-threaded dispatcher. SIGINT is blocked while installing/clearing
 * the borrowed pointer; the handler cannot outlive the scoring instance. */
static void decision_active(geistr_decision *decision, void *context) {
    (void) context;
    sigset_t set, previous; sigemptyset(&set); sigaddset(&set, SIGINT);
    if (sigprocmask(SIG_BLOCK, &set, &previous) != 0) abort();
    running_decision = decision;
    if (decision && interrupted && geistr_decision_cancel(decision) != GEISTR_OK) abort();
    if (sigprocmask(SIG_SETMASK, &previous, nullptr) != 0) abort();
}

static bool comparing; /* bench --compare */

/* geistr bench [model…]: one fixed answer per model on ⚙ and ⚡ (where there
 * is a GPU), recorded like any other; then the installed models' chart. */
static int discard(void *context, const geistr_piece *piece) {
    (void) context, (void) piece;
    return 1;
}

static int bench(int n, const char **ids) {
    geistr_catalog *c = load_catalog();
    geistr_device   d = {.size = sizeof d};
    if (!c || geistr_device_probe(models_dir, &d) != GEISTR_OK) {
        geistr_catalog_free(c);
        return ERROR;
    }
    const char *all[64];
    if (!n) /* every installed model */
        for (size_t i = 0; i < geistr_catalog_count(c) && n < 64; i++)
            if (install_state(geistr_catalog_get(c, i), true) == GEISTR_INSTALL_OK)
                all[n++] = geistr_catalog_get(c, i)->id;
    const char *const *models = ids ? ids : all;
    struct sigaction   sa     = {.sa_handler = on_interrupt};
    sigaction(SIGINT, &sa, nullptr);
    const geistr_message ask[] = {{"user", "Write a short paragraph about the sea."}};
    for (int i = 0; i < n && !interrupted; i++)
        for (int k = 0; k < (d.gpu ? 2 : 1) && !interrupted; k++) { /* ponytail: Vulkan is not probed yet */
            struct session x;
            if (session_open(&x, models[i], k ? "gpu" : "cpu", 0, false) != OK)
                continue;
            running         = x.chat;
            geistr_status s = geistr_chat_limit(x.chat, 128);
            if (s == GEISTR_OK)
                s = geistr_chat_run(x.chat, 1, ask, discard, nullptr);
            running = nullptr;
            printf("%s %-18s", on_gpu(&x) ? "⚡" : "⚙", models[i]);
            if (s == GEISTR_OK)
                speed(x.chat, x.name, x.backend, true, "bench", stdout);
            else
                printf("  %s\n", geistr_status_text(s));
            session_close(&x);
        }
    geistr_catalog_free(c);
    if (interrupted)
        return CANCELLED;
    putchar('\n');
    return catalog(true, false, false);
}

static volatile sig_atomic_t stopping;

static void on_stop(int signal) {
    (void) signal;
    stopping = 1;
}

static int serve(const char *name, const char *processor, const char *socket, const char *http, size_t chats) {
    struct session x;
    int            rc = session_open(&x, name, processor, 0, true);
    if (rc != OK)
        return rc;
    geistr_chat_close(x.chat); /* the service opens its own */
    struct sigaction sa = {.sa_handler = on_stop};
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    struct svc_options o = {.model = x.model, .reasoning = x.reasoning, .name = x.name, .socket = socket,
                            .http = http, .chats = chats, .stop = &stopping};
    rc = service_run(&o) ? ERROR : OK;
    geistr_model_close(x.model);
    return rc;
}

/* geistr pull without a model: every installed model whose file is not the
 * one this catalog lists (a new geistr brought a new catalog), downloaded
 * again. The old file stays until the new one is complete. */
static int pull_all(void) {
    geistr_catalog *c = load_catalog();
    if (!c)
        return ERROR;
    int rc = OK, updated = 0;
    for (size_t i = 0; i < geistr_catalog_count(c) && rc != CANCELLED; i++)
        if (install_state(geistr_catalog_get(c, i), true) == GEISTR_INSTALL_MISMATCH) {
            int one = geistr_pull(geistr_catalog_get(c, i), models_dir);
            rc      = one == OK ? rc : one;
            updated += one == OK;
        }
    if (rc == OK && !updated)
        printf("✓ the installed models are current (catalog revision %u)\n", geistr_catalog_revision(c));
    geistr_catalog_free(c);
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
    /* Preserve leading global options, then pass decision arguments intact:
     * option descriptions and UTF-8 IDs are never interpreted by chat parsing. */
    int first = 1;
    while (first + 1 < argc && (!strcmp(argv[first], "--models") || !strcmp(argv[first], "--catalog"))) {
        if (!strcmp(argv[first], "--models")) models_dir = argv[first + 1];
        else catalog_file = argv[first + 1];
        first += 2;
    }
    if (first < argc && !strcmp(argv[first], "decide")) {
        if (!models_dir) {
            if (geistr_models_dir(default_models, sizeof default_models) != GEISTR_OK) {
                fputs("geistr: no model folder: set HOME or GEISTEN_HOME, or use --models\n", stderr);
                return ERROR;
            }
            models_dir = default_models;
        }
        struct sigaction sa = {.sa_handler = on_decide_interrupt}; sigemptyset(&sa.sa_mask);
        if (sigaction(SIGINT, &sa, nullptr) != 0) return ERROR;
        const geistr_decide_host host = {models_dir, catalog_file, decide_catalog, decision_active, nullptr};
        return geistr_decide_command((size_t)(argc - first - 1), (const char *const *)(argv + first + 1), &host);
    }
    const char *args[64];
    int         n = 0;
    bool        installed = false, available = false, json = false;
    const char *processor = nullptr, *socket = nullptr, *http = nullptr;
    bool        fresh     = false; /* chat --new */
    long        chats = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--models") && i + 1 < argc)
            models_dir = argv[++i];
        else if (!strcmp(argv[i], "--catalog") && i + 1 < argc)
            catalog_file = argv[++i];
        else if (!strcmp(argv[i], "--decision-config") && i + 1 < argc)
            decision_config_file = argv[++i];
        else if (!strcmp(argv[i], "--installed"))
            installed = true;
        else if (!strcmp(argv[i], "--available"))
            available = true;
        else if (!strcmp(argv[i], "--json"))
            json = true;
        else if (!strcmp(argv[i], "--http") || !strncmp(argv[i], "--http=", 7))
            http = argv[i][6] ? argv[i] + 7 : "127.0.0.1:11434";
        else if (!strcmp(argv[i], "--socket") || !strncmp(argv[i], "--socket=", 9))
            socket = argv[i][8] ? argv[i] + 9 : "";
        else if (!strcmp(argv[i], "--chats") && i + 1 < argc) {
            char *end;
            chats = strtol(argv[++i], &end, 10);
            if (*end || chats < 1 || chats > 64)
                return usage();
        } else if (!strcmp(argv[i], "--compare"))
            comparing = true;
        else if (!strcmp(argv[i], "--new"))
            fresh = true;
        else if (!strcmp(argv[i], "--cpu") || !strcmp(argv[i], "--gpu"))
            processor = argv[i] + 2;
        else if (!strcmp(argv[i], "--version")) {
            geistr_catalog *c = nullptr;
            char            error[8];
            (void) geistr_catalog_parse((const char *) embedded_catalog, sizeof embedded_catalog, &c, error,
                                        sizeof error);
            printf("geistr %s · catalog revision %u · engine %.7s\n", geistr_version(),
                   c ? geistr_catalog_revision(c) : 0, GEISTR_ENGINE);
            geistr_catalog_free(c);
            return OK;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
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
    if (!strcmp(command, "config"))
        return processor ? usage() : config(n, args);
    if (!processor)
        processor = cfg.processor;
    if (!strcmp(command, "catalog") && n == 1 && !(installed && available))
        return catalog(installed, available, json);
    if (installed || available || json || decision_config_file)
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
        return serve(args[1], processor, socket, http, (size_t) chats);
    if (!strcmp(command, "chat") && socket)
        return n == 1 ? chat("", processor, socket, fresh) : usage();
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
        return chat(model, processor, nullptr, fresh);
    }
    if (!strcmp(command, "chat") && n == 2)
        return chat(args[1], processor, nullptr, fresh);
    if (!strcmp(command, "bench"))
        return comparing ? speed_compare(n - 1, args + 1) : bench(n - 1, n > 1 ? args + 1 : nullptr);
    if (!strcmp(command, "pull") && n <= 2)
        return n == 2 ? pull(args[1]) : pull_all();
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

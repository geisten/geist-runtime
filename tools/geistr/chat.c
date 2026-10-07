/* chat.c — geistr chat and run (see cli.h): a session (a model with a chat on
 * it), the terminal around it (spinner, the key watcher, the Markdown view,
 * the line editor), the slash commands and the service mode. What was said
 * lives in conversation.c. */
#include "cli.h"
#include "json.h"
#include "lineedit.h"
#include "render.h"
#include "service.h"
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>

geistr_chat *volatile running; /* for the Ctrl-C handler */
volatile sig_atomic_t interrupted;

void on_interrupt(int signal) {
    (void) signal;
    interrupted = 1;
    if (running)
        geistr_chat_cancel(running); /* an atomic store: safe in a handler */
}

/* The answer being shown: its Markdown view, and its text for the conversation. */
struct shown {
    struct md view;
    char     *text;
    size_t    len, cap;
};

static int print_piece(void *context, const geistr_piece *piece) {
    struct shown *a = context;
    md_feed(&a->view, piece->text);
    if (piece->part != GEISTR_PART_ANSWER)
        return 1;
    if (a->len + piece->len + 1 > a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 4096;
        while (cap < a->len + piece->len + 1)
            cap *= 2;
        char *grown = realloc(a->text, cap);
        if (!grown)
            return 1;
        a->text = grown, a->cap = cap;
    }
    memcpy(a->text + a->len, piece->text, piece->len + 1);
    a->len += piece->len;
    return 1;
}

/* A new answer: Markdown in a terminal, sized to it (tables). */
static void view_begin(struct shown *a) {
    md_init(&a->view, cfg.markdown && tty_out() ? MD_ANSI : MD_RAW, stdout);
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col)
        a->view.width = ws.ws_col;
    a->len = 0;
    if (a->text)
        a->text[0] = 0;
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
        if (c == 3 || (c == 27 && poll(&in, 1, 30) == 0)) { /* Ctrl-C, or Esc alone: stop the answer */
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
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON); /* Ctrl-C is a key here too (keys_only) */
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

/* In the interactive chat Ctrl-C is a key, never a signal, also between two
 * reads of the editor: a SIGINT may be handled by any thread (the engine's
 * workers too), after the editor looked for it, and the Ctrl-C was lost. As
 * a key it waits in the input until read. Restored on exit, and on SIGTERM
 * and SIGHUP. */
static struct termios original_term;
static volatile sig_atomic_t term_changed;

static void restore_term(int signal) {
    if (term_changed)
        tcsetattr(STDIN_FILENO, TCSANOW, &original_term); /* async-signal-safe */
    term_changed = 0;
    if (signal) {
        sigaction(signal, &(struct sigaction) {.sa_handler = SIG_DFL}, nullptr);
        raise(signal);
    }
}

static void keys_only(bool on) {
    if (!on) {
        restore_term(0);
        return;
    }
    if (tcgetattr(STDIN_FILENO, &original_term) != 0)
        return;
    struct termios t = original_term;
    t.c_lflag &= (tcflag_t) ~ISIG;
    term_changed = tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0;
    struct sigaction sa = {.sa_handler = restore_term};
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
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


void session_close(struct session *x) {
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
    if (s != GEISTR_OK) /* the model says why: e.g. its chat format */
        fprintf(stderr, "geistr: cannot chat: %s\n", geistr_model_error(x->model));
    return s == GEISTR_OK;
}

int session_open(struct session *x, const char *name, const char *processor, double temperature,
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

bool on_gpu(const struct session *x) {
    return strcmp(x->backend, "cpu") != 0;
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
int answer_once(const char *name, const char *prompt, const char *processor) {
    struct session x;
    int            rc = session_open(&x, name, processor, cfg.temperature, false);
    if (rc != OK)
        return rc;
    struct sigaction sa = {.sa_handler = on_interrupt};
    sigaction(SIGINT, &sa, nullptr);
    running = x.chat;
    geistr_message turn[2];
    size_t         n = 0;
    if (cfg.system[0])
        turn[n++] = (geistr_message) {"system", cfg.system};
    turn[n++]        = (geistr_message) {"user", prompt};
    struct shown out = {};
    view_begin(&out);
    geistr_status s = geistr_chat_run(x.chat, n, turn, print_piece, &out);
    md_finish(&out.view);
    putchar('\n');
    speed(x.chat, x.name, x.backend, s == GEISTR_OK, "answer", stderr);
    geistr_stats done = {.size = sizeof done};
    if (geistr_chat_stats(x.chat, &done) == GEISTR_OK && done.finish == GEISTR_FINISH_REPETITION)
        fputs("geistr: the answer repeated itself; stopped there\n", stderr);
    if (s != GEISTR_OK && s != GEISTR_CANCELLED)
        fprintf(stderr, "geistr: %s: %s\n", geistr_status_text(s), geistr_chat_error(x.chat));
    running = nullptr;
    session_close(&x);
    free(out.text);
    return s == GEISTR_OK ? OK : s == GEISTR_CANCELLED ? CANCELLED : ERROR;
}

/* chat --socket: the model is the service's; this process holds the conversation. */
static bool remote_part(void *ctx, const char *text) {
    geistr_piece p = {.size = sizeof p, .part = GEISTR_PART_ANSWER, .text = text, .len = strlen(text)};
    return print_piece(ctx, &p);
}

static bool remote_cancel(void *ctx) {
    (void) ctx;
    return interrupted;
}

/* A new chat on the same model (sampling or the system prompt changed); it
 * reads the conversation anew. */
static bool reopen(struct session *x, struct conversation *said) {
    geistr_chat_close(x->chat);
    x->chat = nullptr;
    geistr_reasoning reasoning;
    char             path[4200];
    if (resolve(x->name, path, sizeof path, &reasoning) != OK || !session_chat(x, reasoning, true))
        return false;
    running     = x->chat;
    said->carry = said->n > 0;
    return true;
}

enum { GO_ON, LEAVE };

/* A slash command (line is changed: the argument is cut off). */
static int command(struct session *x, struct conversation *said, char *line, const char *remote) {
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
        return LEAVE;
    if (remote && (processor_now || !strcmp(line, "/model"))) {
        puts("the service has its model: geistr serve <model> [--cpu | --gpu]");
        return GO_ON;
    }
    if (processor_now || (!strcmp(line, "/model") && *arg)) {
        /* Open the new session first: a failure keeps the current one. */
        struct session next;
        if (session_open(&next, processor_now ? x->name : arg, processor_now ? processor_now : x->processor,
                         x->temperature, true) != OK)
            return GO_ON;
        running = nullptr;
        session_close(x);
        *x          = next;
        running     = x->chat;
        said->carry = said->n > 0;
        status_line(x, said->carry ? " · the conversation moves along" : "");
    } else if (!strcmp(line, "/temp")) {
        char  *end = nullptr;
        double t   = strtod(arg, &end);
        if (!*arg || *end || !(t >= 0 && t <= 2)) {
            puts("/temp 0 … 2");
            return GO_ON;
        }
        x->temperature = t; /* sampling is a chat option: a new chat, same model */
        if (!remote && !reopen(x, said))
            return LEAVE;
        printf("temperature %g\n", t);
    } else if (!strcmp(line, "/system")) {
        if (conv_system(said, arg) && !remote && !reopen(x, said))
            return LEAVE;
        puts(said->system[0] ? "system prompt set" : "no system prompt");
    } else if (!strcmp(line, "/info")) {
        status_line(x, remote ? " · service" : "");
        printf("chat format %s · context %u · temperature %g%s%s\n", x->format, x->context, x->temperature,
               said->system[0] ? " · system: " : "", said->system);
    } else if (!strcmp(line, "/save")) {
        if (!remote) { /* a service's model is not this chat's choice */
            snprintf(cfg.model, sizeof cfg.model, "%s", x->name);
            snprintf(cfg.processor, sizeof cfg.processor, "%s", x->processor);
        }
        snprintf(cfg.system, sizeof cfg.system, "%s", said->system);
        cfg.temperature = x->temperature;
        puts(config_path[0] && config_save() ? "saved for the next chat" : "cannot save the settings");
    } else if (!strcmp(line, "/clear")) {
        if (x->chat)
            (void) geistr_chat_rewind(x->chat, 0);
        conv_clear(said);
    } else
        chat_help();
    return GO_ON;
}

int chat(const char *name, const char *processor, const char *remote, bool fresh) {
    struct session x = {};
    if (remote) {
        char info[1024], context[16];
        if (service_info(remote, info, sizeof info) != GEISTR_OK) {
            fprintf(stderr, "geistr: no service on %s (geistr serve <model>)\n", remote);
            return ERROR;
        }
        json_get(info, "model", x.name, sizeof x.name);
        json_get(info, "backend", x.backend, sizeof x.backend);
        json_get(info, "chat_format", x.format, sizeof x.format);
        json_get(info, "context", context, sizeof context);
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
    double              last_ctrl_c = -10;
    struct conversation said        = {};
    struct shown        shown       = {};
    snprintf(said.system, sizeof said.system, "%s", cfg.system);
    static char line[1 << 16];
    /* In a terminal: the line editor (Tab completion, history); else plain lines. */
    bool      edit = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    struct le editor;
    if (edit) {
        find_installed();
        le_init(&editor, stdout, 80, complete_line, nullptr);
        editor.interrupted = &interrupted;
        keys_only(true);
    }
    if (edit && cfg.resume && data_dir[0]) { /* the conversation outlives the chat; piped ones do not */
        conv_file_new(&said);
        if (!fresh)
            conv_resume(&said);
    }
    if (said.n) { /* where it was: its size, the last question */
        int         bytes;
        const char *last = conv_last_question(&said, &bytes);
        printf("\033[2m↻ %zu · „%.*s%s“ · /clear new\033[0m\n", said.n, bytes, last, last[bytes] ? "…" : "");
    }
    for (;;) {
        if (!edit) /* the editor takes a Ctrl-C that came between two reads */
            interrupted = 0;
        char prompt[64];
        snprintf(prompt, sizeof prompt, tty_out() ? "\033[2m%s\033[0m > " : "%s > ", on_gpu(&x) ? "⚡" : "⚙");
        if (edit) {
            enum le_event ev = le_read(&editor, prompt);
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
            if (command(&x, &said, line, remote) == LEAVE)
                break;
            continue;
        }
        /* Normally only the new message; after a switch, the conversation once. */
        size_t          from  = conv_say(&said, line, remote != nullptr);
        size_t          count = said.n - from;
        geistr_message *turn  = calloc(count, sizeof *turn);
        if (!turn)
            break;
        for (size_t i = 0; i < count; i++)
            turn[i] = (geistr_message) {said.role[from + i], said.content[from + i]};
        view_begin(&shown);
        if (edit)
            watch_start(&editor);
        struct svc_stats rs = {};
        char             why[512];
        geistr_status    s = remote ? service_chat(remote, count, turn, 0, x.temperature, remote_part, remote_cancel,
                                                   &shown, &rs, why, sizeof why)
                                    : geistr_chat_run(x.chat, count, turn, print_piece, &shown);
        if (edit)
            watch_stop();
        interrupted = 0; /* it stopped the answer, if it came */
        md_finish(&shown.view);
        free(turn);
        puts(s == GEISTR_CANCELLED ? " [stopped]" : "");
        geistr_stats done = {.size = sizeof done};
        if (remote ? !strcmp(rs.finish, "repetition")
                   : geistr_chat_stats(x.chat, &done) == GEISTR_OK && done.finish == GEISTR_FINISH_REPETITION)
            printf("%s  ⟲ it repeated itself: stopped there · /clear for a fresh conversation%s\n",
                   tty_out() ? "\033[2m" : "", tty_out() ? "\033[0m" : "");
        if (s == GEISTR_OK || s == GEISTR_CANCELLED) {
            conv_answered(&said, shown.text);
            if (s == GEISTR_OK && remote) {
                speed_line(rs.output_tokens, rs.generation_ms, rs.total_ms, stdout);
                speed_record(x.name, x.backend, rs.output_tokens, rs.generation_ms, rs.prefill_ms, "answer");
            } else if (s == GEISTR_OK)
                speed(x.chat, x.name, x.backend, true, "answer", stdout);
        } else {
            conv_refused(&said); /* not part of the conversation: the chat refused it */
            fprintf(stderr, "geistr: %s: %s\n", geistr_status_text(s), remote ? why : geistr_chat_error(x.chat));
        }
    }
    if (edit) {
        le_free(&editor);
        keys_only(false);
    } else
        putchar('\n');
    running = nullptr;
    session_close(&x);
    conv_free(&said);
    free(shown.text);
    return OK;
}

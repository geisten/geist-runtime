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
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/ioctl.h>
#include <fcntl.h>
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
    bool      started; /* something visible is shown: leading blank lines are not */
};

/* "geistr: <status>: <detail>", the detail only when it says more. */
static void report(geistr_status s, const char *detail) {
    const char *status = geistr_status_text(s);
    if (detail && *detail && strcmp(detail, status))
        fprintf(stderr, "geistr: %s: %s\n", status, detail);
    else
        fprintf(stderr, "geistr: %s\n", status);
}

static void spinner_phase(const char *phase);
static void spinner_stop(void);

static int print_piece(void *context, const geistr_piece *piece) {
    struct shown *a    = context;
    const char   *show = piece->text;
    if (piece->part == GEISTR_PART_THINKING) { /* not shown, not kept: it only tells the wait apart */
        spinner_phase("thinking");
        return 1;
    }
    if (!a->started) { /* an answer that begins with blank lines (qwen3): from its text on */
        show += strspn(show, " \t\r\n");
        a->started = *show != 0;
        if (a->started)
            spinner_stop(); /* the first word: the wait is over */
    }
    md_feed(&a->view, show);
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
        a->view.width = ws.ws_col, a->view.wrap = true; /* prose at word boundaries */
    a->len     = 0;
    a->started = false;
    if (a->text)
        a->text[0] = 0;
}

/* ---- while a model loads: a spinner with its size and the time ----------- */

static struct {
    pthread_t   thread;
    atomic_bool stop;
    bool        on;
    char        label[300];
    _Atomic(const char *) phase; /* while an answer is awaited: "reading", "thinking" */
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
            fprintf(stdout, "\r\033[2K\033[2m%s %s · %.0f s\033[0m", frames[i % 10],
                    atomic_load(&spin.phase) ? atomic_load(&spin.phase) : spin.label, seconds() - start);
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
    atomic_store(&spin.phase, nullptr);
    atomic_store(&spin.stop, false);
    spin.on = pthread_create(&spin.thread, nullptr, spinner, nullptr) == 0;
}

/* Between sending and the first word: "⠋ reading · 3 s", then "thinking". */
static void spinner_wait(void) {
    spin.on = tty_out();
    if (!spin.on)
        return;
    atomic_store(&spin.phase, "reading");
    atomic_store(&spin.stop, false);
    spin.on = pthread_create(&spin.thread, nullptr, spinner, nullptr) == 0;
}

static void spinner_phase(const char *phase) {
    if (spin.on)
        atomic_store(&spin.phase, phase);
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
    raw.c_iflag &= (tcflag_t) ~ICRNL;           /* Enter stays \r: \n is Ctrl-J, a new line */
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

/* In the interactive chat Ctrl-C is a key, never a signal, and nothing is
 * echoed, also between two reads of the editor: a SIGINT may be handled by any thread (the engine's
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
    t.c_lflag &= (tcflag_t) ~(ISIG | ECHO | ICANON); /* keys typed while a command runs: not echoed in
                                                      * between, the editor shows them when it reads */
    t.c_iflag &= (tcflag_t) ~ICRNL; /* typed ahead, Enter stays \r (sends): \n is Ctrl-J (a new line) */
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    term_changed = tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0;
    struct sigaction sa = {.sa_handler = restore_term};
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
}

/* The terminal's columns; 0 when output is not a terminal. */
static unsigned columns(void) {
    struct winsize ws;
    return isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 ? ws.ws_col : 0;
}

/* A line of the chat's own (styles included), wrapped at words to the
 * terminal: continuation lines under the text after a leading symbol, or at
 * hang. Piped: as it is. */
static void say_at(unsigned hang, const char *fmt, ...) {
    char    text[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    unsigned w = columns();
    if (w)
        md_say(stdout, text, w, hang);
    else
        fputs(text, stdout);
}
#define say(...) say_at(0, __VA_ARGS__)

static void shortcuts(void) {
    bool       faint = tty_out();
    const char *keys[] = {"Enter send", "Ctrl-J or \\ + Enter a new line", "Esc stop the answer",
                          "Ctrl-C clear the line, twice: exit", "Ctrl-D exit",
                          "/ commands (↑↓ choose · Tab take · Esc close)", "↑↓ earlier lines", "→ take the hint",
                          "Ctrl-R search earlier lines", "Ctrl-A/E start/end", "Ctrl-U/K delete to start/end",
                          "Ctrl-W a word", "Ctrl-L clear screen"};
    unsigned    w      = columns();
    bool        narrow = w && w < 60; /* one per line */
    char        text[1024] = "";
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        bool row_end = i == 4 || i == 7; /* three groups in a wide terminal */
        strcat(text, keys[i]);
        strcat(text, i + 1 == sizeof keys / sizeof *keys ? "" : narrow || row_end ? "\n" : " · ");
    }
    say("%s%s%s\n", dim(faint), text, normal(faint));
}

static void intro(const char *name, const char *backend, bool gpu) {
    if (!cfg.intro || !tty_out())
        return;
    say("\033[2mgeistr · %s on %s %s   (⚙ CPU · ⚡ GPU)\033[0m\n", name, gpu ? "⚡" : "⚙", backend);
    unsigned w = columns();
    if (!w || w >= 60) /* narrower: ? shows the keys */
        say("\033[2m/ commands · ? shortcuts · Esc stops an answer · Ctrl-C twice exits · geistr config intro "
            "off\033[0m\n");
}

void session_close(struct session *x) {
    geistr_chat_close(x->chat);
    geistr_model_close(x->model);
    *x = (struct session) {};
}

/* Open the chat on x->model with x's options (model already open). */
static bool session_chat(struct session *x, bool interactive) {
    geistr_chat_opts opts = GEISTR_CHAT_OPTS_INIT;
    opts.reasoning        = x->reasoning;
    opts.temperature      = (float) x->temperature;
    opts.overflow         = interactive ? GEISTR_OVERFLOW_DROP_OLDEST : GEISTR_OVERFLOW_REFUSE;
    opts.thinking         = interactive; /* the chat shows "thinking" while it lasts (print_piece) */
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
#ifdef GEISTR_TESTING
    if (getenv("GEISTR_TEST_CONTEXT")) /* a small window: the tests fill it quickly */
        mo.context = (uint32_t) strtoul(getenv("GEISTR_TEST_CONTEXT"), nullptr, 10);
#endif
    char error[256];
    *x = (struct session) {.temperature = temperature, .reasoning = reasoning};
    spinner_start(name, path);
    geistr_status opened = geistr_model_open(path, &mo, &x->model, error, sizeof error);
    spinner_stop();
    if (opened != GEISTR_OK) {
        if (strstr(error, path)) /* the engine named the file already */
            fprintf(stderr, "geistr: %s\n", error);
        else
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
    if (!session_chat(x, interactive)) {
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
        {"/system ", "system prompt (off: none)"},
        {"/info", "what runs now"},
        {"/save", "keep these settings for the next chat"},
        {"/retry", "the last answer again (at temperature 0: once at 0.7)"},
        {"/copy", "the last answer to the clipboard (/copy code: its last code block)"},
        {"/clear", "a new conversation"},
        {"/help", "the commands"},
        {"/exit", "end (or Ctrl-D)"},
};
static char   installed_ids[64][64], model_lines[64][80];
static size_t n_installed;

/* The installed models, best for this computer first (as geistr catalog
 * ranks them); in catalog order if the ranking fails. */
static void find_installed(void) {
    geistr_catalog *c     = load_catalog();
    size_t          n     = c ? geistr_catalog_count(c) : 0;
    geistr_local   *local = n ? calloc(n, sizeof *local) : nullptr;
    n_installed           = 0;
    for (size_t i = 0; local && i < n; i++) {
        geistr_install state = GEISTR_INSTALL_MISSING;
        bool ok  = geistr_catalog_check(geistr_catalog_get(c, i), models_dir, false, &state) == GEISTR_OK &&
                   (state == GEISTR_INSTALL_OK || state == GEISTR_INSTALL_UNVERIFIED);
        local[i] = (geistr_local) {.size = sizeof *local, .installed = ok};
    }
    if (local)
        speeds_load(c, local);
    geistr_device   d      = {};
    geistr_ranking *r      = nullptr;
    bool            ranked = local && geistr_device_probe(models_dir, &d) == GEISTR_OK &&
                             geistr_rank(c, &d, local, nullptr, &r) == GEISTR_OK;
    for (size_t k = 0; local && k < n && n_installed < 64; k++) {
        const geistr_catalog_entry *m = ranked ? geistr_ranking_get(r, k)->entry : geistr_catalog_get(c, k);
        for (size_t i = 0; i < n; i++) /* ponytail: linear lookup, the catalog has a handful of models */
            if (geistr_catalog_get(c, i) == m && local[i].installed)
                snprintf(installed_ids[n_installed++], sizeof installed_ids[0], "%s", m->id);
    }
    geistr_ranking_free(r);
    free(local);
    geistr_catalog_free(c);
}

/* What was typed in the chat, kept between chats in the data folder (one
 * JSON object per line, private). Not kept with history off, or resume off
 * (then nothing is); a line starting with a space never (as in a shell). */
enum { HISTORY_KEEP = 500 };
static char history_file[4200];

static void history_load(struct le *e) {
    history_file[0] = 0;
    if (!cfg.resume || !cfg.history || !data_dir[0] || !make_dirs(data_dir, 0700))
        return;
    snprintf(history_file, sizeof history_file, "%s/history", data_dir);
    FILE *f = fopen(history_file, "r");
    if (!f)
        return;
    static char line[1 << 16], text[4096];
    size_t      lines = 0;
    while (fgets(line, sizeof line, f)) {
        json_get(line, "line", text, sizeof text);
        le_remember(e, text);
        lines++;
    }
    fclose(f);
    if (lines <= 2 * HISTORY_KEEP) /* else: rewrite it with the lines the editor kept */
        return;
    f = fopen(history_file, "w");
    for (size_t i = 0; f && i < e->n_history; i++)
        fputs("{\"line\":", f), json_write(f, e->history[i]), fputs("}\n", f);
    if (f)
        fclose(f);
}

static void history_add(const char *text) {
    if (!history_file[0] || !*text || *text == ' ')
        return;
    int   fd = open(history_file, O_WRONLY | O_CREAT | O_APPEND, 0600);
    FILE *f  = fd >= 0 ? fdopen(fd, "a") : nullptr;
    if (!f) {
        if (fd >= 0)
            close(fd);
        return;
    }
    fputs("{\"line\":", f), json_write(f, text), fputs("}\n", f);
    fclose(f);
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
        say_at(11, "%-10s %s\n", commands[i].line, commands[i].help);
    shortcuts();
}

/* A dim line: how to use a command, what it shows. */
static void usage(const char *text) {
    bool faint = tty_out();
    say("%s%s%s\n", dim(faint), text, normal(faint));
}

/* What a resumed conversation (or the first send on another model) re-reads
 * at most: a quarter of the context, or resume_tokens if less, in bytes.
 * ponytail: about 4 bytes a token, no tokenizer here; the runtime still
 * drops more if the estimate falls short. */
static size_t resume_bytes(const struct session *x) {
    double tokens = x->context ? x->context / 4.0 : cfg.resume_tokens;
    return (size_t) (4 * (cfg.resume_tokens < tokens ? cfg.resume_tokens : tokens));
}

/* How full the context is after the last answer, in percent. */
static unsigned fill(const struct session *x) {
    return x->context ? (unsigned) ((uint64_t) x->used * 100 / x->context) : 0;
}

/* The prompt: the processor, and from 50 % how full the context is (yellow
 * from 80, red from 95). */
static void prompt_text(const struct session *x, char *out, size_t cap) {
    const char *symbol = on_gpu(x) ? "⚡" : "⚙";
    unsigned    pct    = fill(x);
    if (!tty_out()) {
        pct >= 50 ? snprintf(out, cap, "%s %u%% > ", symbol, pct) : snprintf(out, cap, "%s > ", symbol);
        return;
    }
    const char *color = pct >= 95 ? "\033[31m" : pct >= 80 ? "\033[33m" : "\033[2m";
    if (pct >= 50)
        snprintf(out, cap, "\033[2m%s\033[0m %s%u%%\033[0m > ", symbol, color, pct);
    else
        snprintf(out, cap, "\033[2m%s\033[0m > ", symbol);
}

static void status_line(const struct session *x, const char *what) {
    bool faint = tty_out();
    say("%s%s %s · %s%s%s\n", dim(faint), on_gpu(x) ? "⚡" : "⚙", x->backend, x->name, what,
           normal(faint));
}

/* geistr run's pieces: after ^C, what was already on its way stays unshown. */
static int run_piece(void *context, const geistr_piece *piece) {
    return interrupted ? 1 : print_piece(context, piece);
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
    geistr_status s = geistr_chat_run(x.chat, n, turn, run_piece, &out);
    md_finish(&out.view);
    putchar('\n');
    speed(x.chat, x.name, x.backend, s == GEISTR_OK, "answer", stderr);
    geistr_stats done = {.size = sizeof done};
    if (geistr_chat_stats(x.chat, &done) == GEISTR_OK && done.finish == GEISTR_FINISH_REPETITION)
        fputs("geistr: the answer repeated itself; stopped there\n", stderr);
    if (s != GEISTR_OK && s != GEISTR_CANCELLED)
        report(s, geistr_chat_error(x.chat));
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
    if (!session_chat(x, true))
        return false;
    running     = x->chat;
    said->carry = said->n > 0;
    return true;
}

enum { GO_ON, LEAVE };

/* A slash command (line is changed: the argument is cut off). */
/* text to the system clipboard: OSC 52 in a terminal known to take it (it
 * works over SSH too), else pbcopy, wl-copy or xclip. What did it, or nullptr. */
static const char *clipboard(const char *text, size_t len) {
    const char *program = getenv("TERM_PROGRAM"), *term = getenv("TERM");
    bool        osc52   = (program && (strstr(program, "iTerm") || !strcmp(program, "WezTerm") ||
                                     !strcmp(program, "ghostty"))) ||
                   getenv("KITTY_WINDOW_ID") || getenv("WT_SESSION") || getenv("TMUX") ||
                   (term && (strstr(term, "kitty") || strstr(term, "alacritty") || strstr(term, "foot")));
    if (osc52 && isatty(STDOUT_FILENO)) {
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        fputs("\033]52;c;", stdout);
        for (size_t i = 0; i < len; i += 3) {
            unsigned v = (unsigned char) text[i] << 16 | (i + 1 < len ? (unsigned char) text[i + 1] << 8 : 0) |
                         (i + 2 < len ? (unsigned char) text[i + 2] : 0);
            putchar(b64[v >> 18 & 63]), putchar(b64[v >> 12 & 63]);
            putchar(i + 1 < len ? b64[v >> 6 & 63] : '='), putchar(i + 2 < len ? b64[v & 63] : '=');
        }
        fputs("\a", stdout);
        fflush(stdout);
        return "OSC 52";
    }
    static const struct {
        const char *command, *needs; /* needs: an environment variable that must be set */
    } tools[] = {{"pbcopy", nullptr}, {"wl-copy", "WAYLAND_DISPLAY"}, {"xclip -selection clipboard", "DISPLAY"}};
    for (size_t i = 0; i < sizeof tools / sizeof *tools; i++) {
        char command[96];
        snprintf(command, sizeof command, "%s 2>/dev/null", tools[i].command);
        FILE *p = tools[i].needs && !getenv(tools[i].needs) ? nullptr : popen(command, "w");
        if (!p)
            continue;
        bool written = fwrite(text, 1, len, p) == len;
        if (pclose(p) == 0 && written) /* a missing tool exits 127 */
            return tools[i].command;
    }
    return nullptr;
}

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
    if (!strcmp(line, "/model") && !*arg) { /* what runs, and what else could */
        status_line(x, remote ? " · service" : "");
        if (remote)
            usage("the service has its model: geistr serve <model> [--cpu | --gpu]");
        else if (n_installed) {
            char  *text = nullptr;
            size_t len  = 0;
            FILE  *f    = open_memstream(&text, &len);
            for (size_t i = 0; f && i < n_installed; i++)
                fprintf(f, "%s%s", i ? " · " : "installed: ", installed_ids[i]);
            if (f)
                fputs(" · /model <id> switches", f), fclose(f);
            usage(text ? text : "");
            free(text);
        } else
            usage("/model <id or .gguf path> switches (geistr catalog lists models)");
        return GO_ON;
    }
    if (remote && (processor_now || !strcmp(line, "/model"))) {
        puts("the service has its model: geistr serve <model> [--cpu | --gpu]");
        return GO_ON;
    }
    if (processor_now || (!strcmp(line, "/model") && *arg)) {
        const char    *name = processor_now ? x->name : arg;
        const char    *proc = processor_now ? processor_now : x->processor;
        struct session next;
        if (on_gpu(x) && strcmp(proc, "cpu") != 0) {
            /* Both may be on the GPU, and two models rarely fit its memory
             * together: the current one goes first. The conversation is kept
             * here (said); a failure reopens the previous model. */
            char prev_name[sizeof x->name], prev_proc[sizeof x->processor];
            char want[sizeof x->name], want_proc[sizeof x->processor];
            snprintf(prev_name, sizeof prev_name, "%s", x->name);
            snprintf(prev_proc, sizeof prev_proc, "%s", x->processor);
            snprintf(want, sizeof want, "%s", name); /* name and proc may point into *x */
            snprintf(want_proc, sizeof want_proc, "%s", proc);
            char             path[4200] = "";
            geistr_reasoning reasoning;
            if (resolve(want, path, sizeof path, &reasoning) != OK) /* a typo keeps the model */
                return GO_ON;
            const double temperature = x->temperature;
            running                  = nullptr;
            session_close(x);
            if (session_open(&next, want, want_proc, temperature, true) != OK) {
                if (session_open(x, prev_name, prev_proc, temperature, true) != OK) {
                    fprintf(stderr, "geistr: cannot reopen %s either\n", prev_name);
                    return LEAVE;
                }
                running     = x->chat;
                said->carry = said->n > 0;
                status_line(x, " · back on the previous model");
                return GO_ON;
            }
        } else {
            /* Open the new session first: a failure keeps the current one. */
            if (session_open(&next, name, proc, x->temperature, true) != OK)
                return GO_ON;
            running = nullptr;
            session_close(x);
        }
        *x          = next;
        running     = x->chat;
        said->carry = said->n > 0;
        status_line(x, said->carry ? " · the conversation moves along" : "");
    } else if (!strcmp(line, "/temp")) {
        char  *end = nullptr;
        double t   = strtod(arg, &end);
        if (!*arg) {
            printf("temperature %g\n", x->temperature);
            usage("/temp 0 … 2 sets it (0: always the most likely word)");
            return GO_ON;
        }
        if (*end || !(t >= 0 && t <= 2)) {
            puts("/temp 0 … 2");
            return GO_ON;
        }
        x->temperature = t; /* sampling is a chat option: a new chat, same model */
        if (!remote && !reopen(x, said))
            return LEAVE;
        printf("temperature %g\n", t);
    } else if (!strcmp(line, "/system")) {
        if (!*arg) { /* show it; an empty /system no longer clears it by accident */
            puts(said->system[0] ? said->system : "no system prompt");
            usage("/system <text> sets it · /system off removes it");
            return GO_ON;
        }
        if (conv_system(said, strcmp(arg, "off") ? arg : "") && !remote && !reopen(x, said))
            return LEAVE;
        puts(said->system[0] ? "system prompt set" : "no system prompt");
    } else if (!strcmp(line, "/info")) {
        status_line(x, remote ? " · service" : "");
        say("chat format %s · context %u of %u tokens (%u %%) · temperature %g%s%s\n", x->format, x->used,
               x->context, fill(x), x->temperature, said->system[0] ? " · system: " : "", said->system);
    } else if (!strcmp(line, "/copy")) {
        size_t      len;
        bool        code = !strcmp(arg, "code");
        const char *text = conv_last_answer(said, code, &len);
        if (!text) {
            usage(code ? "/copy code: the last answer has no code block" : "/copy: no answer yet");
            return GO_ON;
        }
        const char *how = clipboard(text, len);
        if (how)
            say("%s⧉ copied %.1f kB%s%s\n", dim(tty_out()), (double) len / 1000, code ? " of code" : "", normal(tty_out()));
        else
            puts("/copy: no clipboard here (a terminal with OSC 52, or pbcopy, wl-copy, xclip)");
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
        x->used = 0;
        bool faint = tty_out(); /* what goes on: the system prompt stays */
        say("%s○ a new conversation%s%s%s\n", dim(faint), said->system[0] ? " · system: " : "",
               said->system, normal(faint));
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
        history_load(&editor);
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
        size_t      skip = remote ? 0 : conv_budget(&said, resume_bytes(&x));
        char        part[96] = "";
        if (skip) /* the rest stays in the file, but is not read again */
            snprintf(part, sizeof part, " · resumes the last %zu", said.n - skip);
        unsigned char mark = said.mark[said.n - 1];
        if (mark != MARK_NONE) /* how the last answer ended */
            snprintf(part + strlen(part), sizeof part - strlen(part), " · %s",
                     mark == MARK_STOPPED ? "its last answer was stopped" : "its last answer looped (cut)");
        say("\033[2m↻ %zu · „%.*s%s“%s · /clear new\033[0m\n", said.n, bytes, last, last[bytes] ? "…" : "",
               part);
    }
    for (;;) {
        if (!edit) /* the editor takes a Ctrl-C that came between two reads */
            interrupted = 0;
        char prompt[96];
        prompt_text(&x, prompt, sizeof prompt);
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
            history_add(line);
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
        if (!edit) /* fgets keeps the line's end; a pasted line keeps its breaks */
            line[strcspn(line, "\n")] = 0;
        if (!line[0])
            continue;
        bool warmer = false; /* /retry at temperature 0: this one answer at 0.7 */
        if (!strcmp(line, "/retry")) {
            if (!conv_retract(&said, line, sizeof line)) {
                usage("/retry: no answer to retry yet");
                continue;
            }
            warmer = x.temperature == 0;
            if (warmer) { /* a new chat samples; it reads the conversation anew */
                x.temperature = 0.7;
                if (!remote && !reopen(&x, &said))
                    break;
            } else if (!remote) { /* the same chat, back to before the question */
                size_t length = geistr_chat_length(x.chat);
                if (length < 2 || geistr_chat_rewind(x.chat, length - 2) != GEISTR_OK)
                    (void) geistr_chat_rewind(x.chat, 0), said.carry = said.n > 0;
            }
            say("%s↻ retry%s%s\n", dim(tty_out()), warmer ? " at temperature 0.7" : "", normal(tty_out()));
        } else if (line[0] == '/') {
            if (command(&x, &said, line, remote) == LEAVE)
                break;
            continue;
        }
        /* Normally only the new message; after a switch, the conversation once. */
        size_t from = conv_say(&said, line, remote != nullptr);
        /* All of it again (resumed, another model): only the newest within the
         * budget, and the system prompt; a service matches the whole. */
        size_t          skip   = !from && !remote ? conv_budget(&said, resume_bytes(&x)) : 0;
        bool            system = skip && !strcmp(said.role[0], "system");
        size_t          count  = (skip ? said.n - skip : said.n - from) + system;
        geistr_message *turn   = calloc(count, sizeof *turn);
        if (!turn)
            break;
        if (system)
            turn[0] = (geistr_message) {said.role[0], said.content[0]};
        for (size_t i = system; i < count; i++)
            turn[i] = (geistr_message) {said.role[(skip ? skip : from) + i - system],
                                        said.content[(skip ? skip : from) + i - system]};
        view_begin(&shown);
        if (edit)
            watch_start(&editor);
        spinner_wait();
        struct svc_stats rs = {};
        char             why[512];
        geistr_status    s = remote ? service_chat(remote, count, turn, 0, x.temperature, remote_part, remote_cancel,
                                                   &shown, &rs, why, sizeof why)
                                    : geistr_chat_run(x.chat, count, turn, print_piece, &shown);
        spinner_stop(); /* no word came (stopped, refused, empty) */
        if (edit)
            watch_stop();
        interrupted = 0; /* it stopped the answer, if it came */
        md_finish(&shown.view);
        free(turn);
        puts(s == GEISTR_CANCELLED ? " [stopped]" : "");
        geistr_stats done  = {.size = sizeof done};
        bool         known = !remote && geistr_chat_stats(x.chat, &done) == GEISTR_OK;
        if (s == GEISTR_OK || s == GEISTR_CANCELLED)
            x.used = remote ? rs.context_tokens : known ? done.context_tokens : x.used;
        bool looped = remote ? !strcmp(rs.finish, "repetition") : known && done.finish == GEISTR_FINISH_REPETITION;
        if (looped)
            say("%s  ⟲ it repeated itself: kept up to the repeat · /retry or /clear%s\n", dim(tty_out()),
                   normal(tty_out()));
        if (known && done.dropped_messages) /* the model forgets the start: say so */
            say("%s  ↥ %u oldest message%s left out to fit the context (%u tokens) · /clear starts fresh%s\n",
                   dim(tty_out()), done.dropped_messages, done.dropped_messages == 1 ? "" : "s", x.context,
                   normal(tty_out()));
        if (s == GEISTR_OK || s == GEISTR_CANCELLED) {
            /* A loop does not go back to the model as it was: it copies its own
             * repeats. Kept up to the repeat, and the chat gets that version. */
            if (looped && shown.text)
                shown.text[conv_loop_cut(shown.text)] = 0;
            conv_answered(&said, shown.text, s == GEISTR_CANCELLED ? MARK_STOPPED : looped ? MARK_CUT : MARK_NONE);
            if (looped && !remote) { /* the chat drops its looping answer; the cut one goes with the next send */
                size_t length = geistr_chat_length(x.chat);
                if (length && geistr_chat_rewind(x.chat, length - 1) == GEISTR_OK)
                    said.unsent = said.n - 1;
                else
                    (void) geistr_chat_rewind(x.chat, 0), said.carry = said.n > 0;
            }
            if (s == GEISTR_OK && remote) {
                speed_line(rs.output_tokens, rs.generation_ms, rs.total_ms, stdout);
                speed_record(x.name, x.backend, rs.output_tokens, rs.generation_ms, rs.prefill_ms, "answer");
            } else if (s == GEISTR_OK)
                speed(x.chat, x.name, x.backend, true, "answer", stdout);
        } else {
            conv_refused(&said); /* not part of the conversation: the chat refused it */
            report(s, remote ? why : geistr_chat_error(x.chat));
        }
        if (warmer) { /* back to temperature 0: a new chat, it reads the conversation anew */
            x.temperature = 0;
            if (!remote && !reopen(&x, &said))
                break;
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

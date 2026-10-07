/* speed.c — tokens/s measured here (see cli.h). <data>/speed.tsv gets a line
 * per complete answer: model, cpu|gpu, tokens/s, seconds to the first answer
 * text, time, geistlib commit, source (bench: the fixed prompt, comparable
 * across engines; answer: chat or run). The catalog shows the median of the
 * last ten per model and processor measured with this engine: another one may
 * be faster or slower. Only ever appended to, never shortened: the history
 * compares engines. */
#include "cli.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int compare_doubles(const void *a, const void *b) {
    double x = *(const double *) a, y = *(const double *) b;
    return (x > y) - (x < y);
}

static double median(double *v, size_t n) {
    qsort(v, n, sizeof *v, compare_doubles);
    return n ? (n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2) : 0;
}

/* The speeds measured here (speed.tsv) into local[], indexed like the catalog:
 * the median of the last ten per model and processor. A model recorded by
 * path counts for the catalog entry with that file name. */
void speeds_load(const geistr_catalog *c, geistr_local *local) {
    enum { LAST = 10 };
    size_t n = geistr_catalog_count(c);
    struct {
        double   rate[LAST], first[LAST];
        unsigned count;
    } (*seen)[2] = calloc(n, sizeof *seen);
    char path[4200];
    snprintf(path, sizeof path, "%s/speed.tsv", data_dir);
    FILE  *f    = seen && data_dir[0] ? fopen(path, "r") : nullptr;
    char  *line = nullptr;
    size_t cap  = 0;
    while (f && getline(&line, &cap, f) > 0) { /* ponytail: reads it all; an index if it ever gets slow,
                                                * never trimmed: it is the history across engines */
        char *model = strtok(line, "\t"), *proc = strtok(nullptr, "\t"), *rate = strtok(nullptr, "\t"),
             *first = strtok(nullptr, "\t"), *when = strtok(nullptr, "\t"), *engine = strtok(nullptr, "\t\n");
        /* the seventh column, the source, does not matter here: every answer counts */
        if (!model || !proc || !rate || !first || !when || !engine || strcmp(engine, GEISTR_ENGINE))
            continue; /* another engine's speed */
        const char *base = strrchr(model, '/') ? strrchr(model, '/') + 1 : model;
        for (size_t i = 0; i < n; i++) {
            const geistr_catalog_entry *m = geistr_catalog_get(c, i);
            if (strcmp(m->id, model) && strcmp(m->file, base))
                continue;
            unsigned k = !strcmp(proc, "gpu"), slot = seen[i][k].count++ % LAST;
            seen[i][k].rate[slot]  = strtod(rate, nullptr);
            seen[i][k].first[slot] = strtod(first, nullptr);
        }
    }
    free(line);
    if (f)
        fclose(f);
    for (size_t i = 0; seen && i < n; i++)
        for (unsigned k = 0; k < 2; k++) {
            size_t        m = seen[i][k].count < LAST ? seen[i][k].count : LAST;
            geistr_speed *s = k ? &local[i].gpu : &local[i].cpu;
            s->rate         = median(seen[i][k].rate, m);
            s->first        = median(seen[i][k].first, m);
        }
    free(seen);
}

/* The catalog's reference tokens/s for cpu or gpu (another computer), or 0. */
double reference_rate(const geistr_catalog_entry *m, const char *proc) {
    double best = 0;
    for (const char *o = m->reference ? strchr(m->reference, '{') : nullptr; o; o = strchr(o + 1, '{')) {
        const char *end = strchr(o, '}');
        char        object[512], backend[16], rate[32];
        if (!end || (size_t) (end - o) >= sizeof object)
            break;
        snprintf(object, sizeof object, "%.*s", (int) (end - o + 1), o);
        json_get(object, "backend", backend, sizeof backend);
        json_get(object, "tokens_per_s", rate, sizeof rate);
        if (!strcmp(backend, proc) && strtod(rate, nullptr) > best)
            best = strtod(rate, nullptr);
    }
    return best;
}

/* "⚙  41 ███▌      ": the value and a bar of width cells (eighths), dim when
 * it is the catalog's reference rather than measured here. */
void speed_bar(const char *symbol, double measured, double reference, double max, int width, bool tty) {
    static const char *const eighths[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
    if (!tty) /* piped: measured values only, nothing to tell them apart */
        reference = 0;
    double v     = measured > 0 ? measured : reference;
    bool   faint = tty && measured <= 0 && reference > 0;
    printf("  %s%s ", dim(faint), symbol);
    if (v <= 0) {
        printf("%4s%*s", "·", width ? width + 1 : 0, "");
        return;
    }
    printf("%4.0f%s", v, width ? " " : "");
    int units = max > 0 ? (int) (v / max * width * 8 + 0.5) : 0, cells = 0;
    for (; units >= 8; units -= 8, cells++)
        fputs("█", stdout);
    if (units)
        fputs(eighths[units], stdout), cells++;
    printf("%*s%s", width - cells, "", normal(faint));
}


void speed_line(unsigned tokens, double generation_ms, double total_ms, FILE *out) {
    if (!cfg.stats || !tokens)
        return;
    bool   faint = out == stdout ? tty_out() : isatty(STDERR_FILENO);
    double rate  = generation_ms > 0 ? tokens / (generation_ms / 1000) : 0;
    fprintf(out, "%s  %.1f tok/s · %.1f s%s\n", dim(faint), rate, total_ms / 1000, normal(faint));
}

void speed_record(const char *model, const char *backend, unsigned tokens, double generation_ms,
                         double first_ms, const char *source) {
    char path[4200];
    snprintf(path, sizeof path, "%s/speed.tsv", data_dir);
    if (!data_dir[0] || tokens < 8 || generation_ms <= 0 || !make_dirs(data_dir, 0700)) /* too short to tell */
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "%s\t%s\t%.1f\t%.3f\t%lld\t%s\t%s\n", model, strcmp(backend, "cpu") ? "gpu" : "cpu",
            tokens / (generation_ms / 1000), first_ms >= 0 ? first_ms / 1000 : -1, (long long) time(nullptr),
            GEISTR_ENGINE, source);
    fclose(f);
}

/* An answer's speed line; a complete one is also recorded. */
void speed(geistr_chat *chat, const char *model, const char *backend, bool complete, const char *source,
                  FILE *out) {
    geistr_stats st = {.size = sizeof st};
    if (geistr_chat_stats(chat, &st) != GEISTR_OK)
        return;
    speed_line(st.output_tokens, st.generation_ms, st.total_ms, out);
    if (complete)
        speed_record(model, backend, st.output_tokens, st.generation_ms, st.first_answer_ms, source);
}


/* geistr bench --compare [A [B]]: the bench rows of two geistlib commits side
 * by side, per model and processor (median of the last ten each), and the
 * change in %. Default: the two engines measured last; A and B are commit
 * prefixes. */
struct bench_row {
    char      model[128], engine[48];
    bool      gpu;
    double    rate;
    long long when;
};

/* The engine with the latest bench row, other than but; nullptr if none. */
static const char *newest_engine(const struct bench_row *rows, size_t count, const char *but) {
    const char *engine = nullptr;
    long long   last   = -1;
    for (size_t i = 0; i < count; i++)
        if (rows[i].when > last && (!but || strcmp(rows[i].engine, but)))
            last = rows[i].when, engine = rows[i].engine;
    return engine;
}

int speed_compare(int n, const char **refs) {
    char path[4200];
    snprintf(path, sizeof path, "%s/speed.tsv", data_dir);
    FILE             *f    = data_dir[0] ? fopen(path, "r") : nullptr;
    struct bench_row *rows = nullptr;
    size_t            count = 0, cap = 0, line_cap = 0;
    char             *line = nullptr;
    while (f && getline(&line, &line_cap, f) > 0) {
        char *field[7] = {};
        field[0]       = strtok(line, "\t\n");
        for (int k = 1; k < 7 && field[k - 1]; k++)
            field[k] = strtok(nullptr, "\t\n");
        if (!field[6] || strcmp(field[6], "bench")) /* only the fixed prompt compares */
            continue;
        if (count == cap) {
            struct bench_row *grown = realloc(rows, (cap = cap ? cap * 2 : 64) * sizeof *rows);
            if (!grown)
                break;
            rows = grown;
        }
        struct bench_row *r = &rows[count++];
        snprintf(r->model, sizeof r->model, "%s", field[0]);
        snprintf(r->engine, sizeof r->engine, "%s", field[5]);
        r->gpu  = !strcmp(field[1], "gpu");
        r->rate = strtod(field[2], nullptr);
        r->when = strtoll(field[4], nullptr, 10);
    }
    free(line);
    if (f)
        fclose(f);
    /* A and B by name; else B is the engine measured last, A the one before. */
    const char *engine[2] = {};
    for (int k = 0; k < n && k < 2; k++)
        for (size_t i = 0; i < count && !engine[k]; i++)
            if (strlen(refs[k]) >= 4 && !strncmp(rows[i].engine, refs[k], strlen(refs[k])))
                engine[k] = rows[i].engine;
    if (n == 0)
        engine[1] = newest_engine(rows, count, nullptr), engine[0] = newest_engine(rows, count, engine[1]);
    else if (n == 1 && engine[0])
        engine[1] = newest_engine(rows, count, engine[0]);
    if (!engine[0] || !engine[1]) {
        if (n)
            fprintf(stderr, "geistr: no bench rows for %s%s%s\n", refs[0], n > 1 ? " or " : "", n > 1 ? refs[1] : "");
        else if (count)
            printf("only one engine measured so far (%.7s): geistr bench again after a geistlib update\n",
                   rows[0].engine);
        else
            puts("no bench measurements yet: geistr bench");
        free(rows);
        return n ? ERROR : OK;
    }
    bool tty = tty_out(), noisy = false;
    printf("%-22s %14.7s %14.7s\n", "", engine[0], engine[1]);
    for (size_t i = 0; i < count; i++) { /* each model and processor once, as first measured */
        bool first = true;
        for (size_t j = 0; j < i && first; j++)
            first = strcmp(rows[j].model, rows[i].model) || rows[j].gpu != rows[i].gpu;
        if (!first)
            continue;
        double v[2];
        size_t runs[2];
        bool   spread[2]; /* the runs differ by more than 25 %: a busy machine? */
        for (int k = 0; k < 2; k++) {
            double last[10];
            size_t m = 0;
            for (size_t j = count; j-- > 0 && m < 10;)
                if (!strcmp(rows[j].model, rows[i].model) && rows[j].gpu == rows[i].gpu &&
                    !strcmp(rows[j].engine, engine[k]))
                    last[m++] = rows[j].rate;
            v[k]      = median(last, m); /* sorts last */
            runs[k]   = m;
            spread[k] = m > 1 && (last[m - 1] - last[0]) > 0.25 * v[k];
            noisy |= spread[k];
        }
        if (v[0] <= 0 && v[1] <= 0)
            continue;
        printf("%s %-20s", rows[i].gpu ? "⚡" : "⚙", rows[i].model);
        for (int k = 0; k < 2; k++) /* "  179.3 ×10 ⚠": the median, its runs, a warning */
            v[k] > 0 ? printf(" %8.1f ×%-2zu %s", v[k], runs[k], spread[k] ? "⚠" : " ") : printf(" %8s     ", "·");
        if (v[0] > 0 && v[1] > 0) {
            double change = (v[1] / v[0] - 1) * 100;
            bool   up     = change >= 0;
            printf("   %s%s %.1f %%%s", tty ? (up ? "\033[32m" : "\033[31m") : "", up ? "▲" : "▼", up ? change : -change,
                   normal(tty));
        }
        putchar('\n');
    }
    if (noisy)
        printf("%s⚠ its runs differ by more than 25 %%: measured on a busy machine? geistr bench again%s\n",
               dim(tty), normal(tty));
    free(rows);
    return OK;
}


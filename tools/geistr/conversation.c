/* conversation.c — what was said in a chat (see cli.h): the transcript, its
 * system prompt, what the next send carries, and its file. No model and no
 * terminal here, so tests/test_chat.c covers all of it. */
#include "cli.h"
#include "json.h"
#include <dirent.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

void conv_push(struct conversation *c, const char *role, const char *content) {
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 16;
        char **r   = realloc(c->role, cap * sizeof *r);
        if (r)
            c->role = r;
        char **t = r ? realloc(c->content, cap * sizeof *t) : nullptr;
        if (t)
            c->content = t;
        unsigned char *m = t ? realloc(c->mark, cap) : nullptr;
        if (!m)
            return;
        c->mark = m, c->cap = cap;
    }
    if (!c->n) /* a new conversation: the chat holds what it is sent */
        c->unsent = SIZE_MAX;
    c->mark[c->n]    = MARK_NONE;
    c->role[c->n]    = strdup(role);
    c->content[c->n] = strdup(content);
    if (c->role[c->n] && c->content[c->n])
        c->n++;
    else
        free(c->role[c->n]), free(c->content[c->n]);
}

static void drop(struct conversation *c, size_t i) {
    free(c->role[i]), free(c->content[i]);
    memmove(c->role + i, c->role + i + 1, (c->n - i - 1) * sizeof *c->role);
    memmove(c->content + i, c->content + i + 1, (c->n - i - 1) * sizeof *c->content);
    memmove(c->mark + i, c->mark + i + 1, c->n - i - 1);
    c->n--;
}

void conv_clear(struct conversation *c) {
    while (c->n)
        drop(c, c->n - 1);
    c->carry = false;
    if (c->file[0]) { /* the old file stays as it was */
        conv_file_new(c);
        c->resumed_from[0] = 0;
    }
}

void conv_free(struct conversation *c) {
    conv_clear(c);
    free(c->role), free(c->content), free(c->mark);
    *c = (struct conversation) {};
}

bool conv_system(struct conversation *c, const char *text) {
    snprintf(c->system, sizeof c->system, "%s", text);
    bool has = c->n && !strcmp(c->role[0], "system");
    if (has && !text[0])
        drop(c, 0);
    else if (has) {
        free(c->content[0]);
        c->content[0] = strdup(text);
    } else if (c->n && text[0]) { /* add it, then move it to the front */
        conv_push(c, "system", text);
        char *r = c->role[c->n - 1], *t = c->content[c->n - 1];
        memmove(c->role + 1, c->role, (c->n - 1) * sizeof *c->role);
        memmove(c->content + 1, c->content, (c->n - 1) * sizeof *c->content);
        memmove(c->mark + 1, c->mark, c->n - 1);
        c->role[0] = r, c->content[0] = t, c->mark[0] = MARK_NONE;
    }
    if (c->n) /* a chat that read the old one reads the conversation anew */
        c->carry = true;
    return c->n > 0;
}

size_t conv_say(struct conversation *c, const char *text, bool whole) {
    size_t before = c->n;
    if (!c->n && c->system[0])
        conv_push(c, "system", c->system);
    conv_push(c, "user", text);
    return c->carry || whole ? 0 : c->unsent < before ? c->unsent : before;
}

size_t conv_budget(const struct conversation *c, size_t bytes) {
    size_t first = c->n && !strcmp(c->role[0], "system") ? 1 : 0, start = c->n, used = 0;
    for (size_t i = c->n; i-- > first;) {
        used += strlen(c->content[i]);
        if (strcmp(c->role[i], "user"))
            continue;
        if (used > bytes && start < c->n) /* this question would not fit: start after it */
            break;
        start = i;
        if (used > bytes) /* the newest question goes even when it alone is too long */
            break;
    }
    return start <= first || start == c->n ? 0 : start;
}

void conv_answered(struct conversation *c, const char *answer, int mark) {
    c->carry  = false;
    c->unsent = SIZE_MAX; /* the chat has it all now */
    conv_push(c, "assistant", answer ? answer : "");
    if (c->n && !strcmp(c->role[c->n - 1], "assistant"))
        c->mark[c->n - 1] = (unsigned char) mark;
    conv_store(c);
}

size_t conv_loop_cut(const char *t) {
    size_t n = strlen(t);
    for (size_t p = 1; p <= n / 3 && p <= 4096; p++) { /* the shortest period whose copies end the text */
        size_t run = 0; /* bytes at the end equal to those p before */
        while (run < n - p && t[n - 1 - run] == t[n - 1 - run - p])
            run++;
        if (run >= 2 * p && run + p >= 24) { /* three copies or more, not a short ornament */
            size_t keep = n - run;           /* through the first copy … */
            if (!strchr(" \n\t.,;:!?", t[keep - 1])) /* … ending at a word: a cycle has no natural start */
                for (size_t k = keep; k > keep - p && k > 1; k--)
                    if (strchr(" \n\t", t[k - 1])) {
                        keep = k;
                        break;
                    }
            while (keep < n && ((unsigned char) t[keep] & 0xC0) == 0x80)
                keep++;
            while (keep && strchr(" \n\t", t[keep - 1]))
                keep--;
            return keep;
        }
    }
    return n;
}

void conv_refused(struct conversation *c) {
    if (c->n)
        drop(c, c->n - 1);
}

bool conv_retract(struct conversation *c, char *out, size_t cap) {
    if (c->n < 2 || strcmp(c->role[c->n - 1], "assistant") || strcmp(c->role[c->n - 2], "user"))
        return false;
    snprintf(out, cap, "%s", c->content[c->n - 2]);
    drop(c, c->n - 1), drop(c, c->n - 1);
    return true;
}

const char *conv_last_answer(const struct conversation *c, bool code, size_t *len) {
    const char *a = c->n && !strcmp(c->role[c->n - 1], "assistant") ? c->content[c->n - 1] : nullptr;
    if (!a || !code) {
        *len = a ? strlen(a) : 0;
        return a;
    }
    const char *body = nullptr, *end = nullptr; /* the last ``` … ``` pair, at line starts */
    for (const char *p = a; (p = strstr(p, "```"));) {
        bool at_line = p == a || p[-1] == '\n';
        const char *nl = strchr(p, '\n');
        if (!at_line) {
            p += 3;
            continue;
        }
        if (!body || end) { /* an opening fence: the code starts after its line */
            if (!nl)
                break;
            body = nl + 1, end = nullptr;
        } else /* the closing one */
            end = p;
        p = nl ? nl + 1 : p + 3;
    }
    if (body && !end) /* unclosed: up to the end */
        end = a + strlen(a);
    *len = body ? (size_t) (end - body) : 0;
    return body;
}

const char *conv_last_question(const struct conversation *c, int *bytes) {
    const char *last = "";
    for (size_t i = c->n; i-- > 0 && !last[0];)
        if (!strcmp(c->role[i], "user"))
            last = c->content[i];
    int cut = 0; /* 60 characters, not bytes */
    for (int chars = 0; last[cut] && chars < 60; chars++)
        for (cut++; ((unsigned char) last[cut] & 0xC0) == 0x80; cut++) {
        }
    *bytes = cut;
    return last;
}

/* ---- its file: <data>/chats/<ms>-<pid>.jsonl ---------------------------------
 * A chat writes its own file after every answer (role and content per line);
 * the next chat continues the newest and, once it writes, removes it. */

void conv_file_new(struct conversation *c) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    snprintf(c->file, sizeof c->file, "%s/chats/%lld%03ld-%ld.jsonl", data_dir, (long long) t.tv_sec,
             t.tv_nsec / 1000000, (long) getpid());
}

void conv_store(struct conversation *c) {
    char dir[4200], tmp[4500];
    snprintf(dir, sizeof dir, "%s/chats", data_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", c->file);
    if (!c->file[0] || !c->n || !make_dirs(dir, 0700))
        return;
    int   fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600); /* private: what was said */
    FILE *f  = fd >= 0 ? fdopen(fd, "w") : nullptr;
    if (!f) {
        if (fd >= 0)
            close(fd);
        return;
    }
    for (size_t i = 0; i < c->n; i++) {
        fputs("{\"role\":", f), json_write(f, c->role[i]);
        fputs(",\"content\":", f), json_write(f, c->content[i]);
        fputs(c->mark[i] == MARK_STOPPED ? ",\"stopped\":true}\n" : c->mark[i] == MARK_CUT ? ",\"cut\":true}\n" : "}\n", f);
    }
    if (fclose(f) == 0 && rename(tmp, c->file) == 0) {
        if (c->resumed_from[0]) /* it lives on in this chat's file */
            unlink(c->resumed_from), c->resumed_from[0] = 0;
    } else
        unlink(tmp);
}

void conv_resume(struct conversation *c) {
    char dir[4200], best[300] = "";
    snprintf(dir, sizeof dir, "%s/chats", data_dir);
    DIR   *d      = opendir(dir);
    time_t newest = 0;
    for (struct dirent *e; d && (e = readdir(d));) { /* names too long for best are not ours */
        size_t      n = strlen(e->d_name);
        struct stat st;
        char        path[4500];
        if (n <= 6 || n >= sizeof best || strcmp(e->d_name + n - 6, ".jsonl") ||
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int) sizeof path || stat(path, &st) != 0)
            continue;
        if (st.st_mtime > newest || (st.st_mtime == newest && strcmp(e->d_name, best) > 0))
            newest = st.st_mtime, memcpy(best, e->d_name, n + 1);
    }
    if (d)
        closedir(d);
    if (!best[0] || snprintf(c->resumed_from, sizeof c->resumed_from, "%s/%s", dir, best) >=
                        (int) sizeof c->resumed_from) { /* none, or a path too long to open */
        c->resumed_from[0] = 0;
        return;
    }
    FILE  *f    = fopen(c->resumed_from, "r");
    char  *line = nullptr;
    size_t cap  = 0;
    for (ssize_t len; f && (len = getline(&line, &cap, f)) > 0;) {
        char *role = malloc((size_t) len + 1), *content = malloc((size_t) len + 1);
        if (role && content) {
            json_get(line, "role", role, (size_t) len + 1);
            json_get(line, "content", content, (size_t) len + 1);
            if (role[0])
                conv_push(c, role, content);
            char flag[8];
            json_get(line, "stopped", flag, sizeof flag);
            if (c->n && !strcmp(flag, "true"))
                c->mark[c->n - 1] = MARK_STOPPED;
            json_get(line, "cut", flag, sizeof flag);
            if (c->n && !strcmp(flag, "true"))
                c->mark[c->n - 1] = MARK_CUT;
        }
        free(role), free(content);
    }
    free(line);
    if (f)
        fclose(f);
    if (c->n) { /* its system prompt; the next send carries it all */
        snprintf(c->system, sizeof c->system, "%s", !strcmp(c->role[0], "system") ? c->content[0] : "");
        c->carry = true;
    }
}

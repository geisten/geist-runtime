/* conversation.c — what was said in a chat (see cli.h): the transcript, its
 * system prompt, what the next send carries, and its file. No model and no
 * terminal here, so tests/test_chat.c covers all of it. */
#include "cli.h"
#include "json.h"
#include <dirent.h>
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
        if (!t)
            return;
        c->content = t, c->cap = cap;
    }
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
    free(c->role), free(c->content);
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
        c->role[0] = r, c->content[0] = t;
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
    return c->carry || whole ? 0 : before;
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

void conv_answered(struct conversation *c, const char *answer) {
    c->carry = false;
    conv_push(c, "assistant", answer ? answer : "");
    conv_store(c);
}

void conv_refused(struct conversation *c) {
    if (c->n)
        drop(c, c->n - 1);
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
        fputs(",\"content\":", f), json_write(f, c->content[i]), fputs("}\n", f);
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

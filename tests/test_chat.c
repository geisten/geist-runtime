/* test_chat.c — the chat's conversation (tools/geistr/conversation.c) without a
 * model or a terminal: what each send carries, the system prompt, /clear,
 * storing and resuming. */
#include "cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

static bool is(const struct conversation *c, size_t i, const char *role, const char *content) {
    return i < c->n && !strcmp(c->role[i], role) && !strcmp(c->content[i], content);
}

static int files(void) { /* in <data>/chats */
    char cmd[4400];
    snprintf(cmd, sizeof cmd, "ls '%s/chats' 2>/dev/null | grep -c jsonl", data_dir);
    FILE *p = popen(cmd, "r");
    int   n = 0;
    if (p && fscanf(p, "%d", &n) != 1)
        n = 0;
    if (p)
        pclose(p);
    return n;
}

int main(void) {
    char tmp[] = "/tmp/geistr-chat-XXXXXX";
    CHECK(mkdtemp(tmp));
    snprintf(data_dir, sizeof data_dir, "%s", tmp);

    /* The first message brings the system prompt; later ones only themselves. */
    struct conversation c = {};
    snprintf(c.system, sizeof c.system, "Be brief.");
    conv_file_new(&c);
    CHECK(conv_say(&c, "Hi", false) == 0);
    CHECK(is(&c, 0, "system", "Be brief.") && is(&c, 1, "user", "Hi") && c.n == 2);
    conv_answered(&c, "Hello.");
    CHECK(!c.carry && is(&c, 2, "assistant", "Hello.") && files() == 1);
    struct stat st;
    CHECK(stat(c.file, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(conv_say(&c, "And?", false) == 3);  /* the chat holds the rest */
    CHECK(conv_say(&c, "And?", true) == 0);   /* a service gets it all */
    conv_refused(&c);
    CHECK(c.n == 4 && is(&c, 3, "user", "And?"));
    conv_refused(&c);

    /* /system: replaced in place, removed, or moved to the front; the chat
     * reads the conversation anew. */
    CHECK(conv_system(&c, "Be kind.") && c.carry && is(&c, 0, "system", "Be kind.") && c.n == 3);
    CHECK(conv_say(&c, "Why?", false) == 0);
    conv_answered(&c, "Because.");
    CHECK(conv_system(&c, "") && c.n == 4 && is(&c, 0, "user", "Hi"));
    CHECK(conv_system(&c, "New.") && c.n == 5 && is(&c, 0, "system", "New.") && is(&c, 1, "user", "Hi"));
    struct conversation empty = {};
    CHECK(!conv_system(&empty, "Only the setting.") && !empty.n && !strcmp(empty.system, "Only the setting."));

    /* The last question for "↻ … „…“", cut at 60 characters, not bytes. */
    int         bytes;
    const char *q = conv_last_question(&c, &bytes);
    CHECK(!strcmp(q, "Why?") && bytes == 4);
    conv_say(&c, "äöü äöü äöü äöü äöü äöü äöü äöü äöü äöü äöü äöü äöü äöü äöü äöü", false);
    q = conv_last_question(&c, &bytes);
    CHECK(bytes == 60 * 2 - 15); /* 45 umlauts of 2 bytes and 15 spaces */
    conv_answered(&c, "…");

    /* The next chat continues the newest, with its system prompt, carrying
     * it all; once it writes, the old file is gone. */
    struct conversation next = {};
    conv_file_new(&next);
    conv_resume(&next);
    CHECK(next.n == c.n && next.carry && !strcmp(next.system, "New.") && is(&next, 1, "user", "Hi"));
    CHECK(conv_say(&next, "More?", false) == 0);
    conv_answered(&next, "Yes.");
    CHECK(files() == 1 && access(c.file, F_OK) != 0);

    /* /clear: a new conversation in a new file; the old file stays. */
    char before[4400];
    snprintf(before, sizeof before, "%s", next.file);
    conv_clear(&next);
    CHECK(!next.n && !next.carry && strcmp(next.file, before) && access(before, F_OK) == 0);
    conv_say(&next, "Fresh.", false);
    conv_answered(&next, "Yes.");
    CHECK(files() == 2);

    conv_free(&c), conv_free(&next), conv_free(&empty);
    char cmd[64];
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmp);
    CHECK(system(cmd) == 0);
    if (failures)
        return 1;
    puts("chat conversation: what each send carries, system prompt, /clear, store and resume passed");
    return 0;
}

/*
 * test_api.c — conformance tests for include/geistr.h.
 *
 * Written against the header only: today they run against src/stub.c, later
 * unchanged against the real runtime (models "stub:*" are then replaced by
 * the reference model through GEISTR_TEST_MODEL).
 */
#include "geistr.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, what);                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static bool valid_utf8(const char *s, size_t len) {
    for (size_t i = 0; i < len;) {
        unsigned char b    = (unsigned char) s[i];
        size_t        need = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        if (!need || i + need > len)
            return false;
        for (size_t k = 1; k < need; k++)
            if (((unsigned char) s[i + k] >> 6) != 2)
                return false;
        i += need;
    }
    return true;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec * 1e3 + (double) t.tv_nsec / 1e6;
}

struct collected {
    char   answer[8192], thinking[8192];
    size_t pieces;
    bool   utf8_ok, nul_ok;
};

/* Pull every piece until END (or an error), checking each one. */
static geistr_status drain(geistr_chat *chat, struct collected *out) {
    *out = (struct collected) {.utf8_ok = true, .nul_ok = true};
    geistr_piece  piece = {.size = sizeof piece};
    geistr_status s;
    while ((s = geistr_chat_next(chat, &piece)) == GEISTR_OK && piece.part != GEISTR_PART_END) {
        out->pieces++;
        out->utf8_ok &= valid_utf8(piece.text, piece.len);
        out->nul_ok &= strlen(piece.text) == piece.len;
        char *into = piece.part == GEISTR_PART_THINKING ? out->thinking : out->answer;
        strncat(into, piece.text, sizeof out->answer - strlen(into) - 1);
    }
    return s;
}

static geistr_model *open_model(const char *name, uint32_t context) {
    geistr_model_opts opts  = GEISTR_MODEL_OPTS_INIT;
    opts.context            = context;
    geistr_model     *model = nullptr;
    char              error[128];
    geistr_status     s = geistr_model_open(name, &opts, &model, error, sizeof error);
    CHECK(s == GEISTR_OK && model, "open model");
    return model;
}

static geistr_chat *open_chat(geistr_model *model, geistr_chat_opts opts) {
    geistr_chat *chat = nullptr;
    CHECK(geistr_chat_open(model, &opts, &chat) == GEISTR_OK && chat, "open chat");
    return chat;
}

static void test_basics(void) {
    CHECK(geistr_version() && *geistr_version(), "version string");
    for (int s = GEISTR_OK; s <= GEISTR_CANCELLED; s++)
        CHECK(geistr_status_text((geistr_status) s)[0], "status text");

    geistr_model *model = nullptr;
    char          error[128] = "";
    CHECK(geistr_model_open(nullptr, nullptr, &model, error, sizeof error) == GEISTR_INVALID && !model,
          "null path is invalid");
    CHECK(geistr_model_open("/no/such/model.gguf", nullptr, &model, error, sizeof error) == GEISTR_IO &&
              !model && error[0],
          "missing file: IO with a reason");
    CHECK(geistr_model_open("stub:noformat", nullptr, &model, nullptr, 0) == GEISTR_OK, "open without error buffer");
    geistr_chat *chat = nullptr;
    CHECK(geistr_chat_open(model, nullptr, &chat) == GEISTR_FORMAT && !chat && geistr_model_error(model)[0],
          "unknown chat format: FORMAT on chat open");
    geistr_model_close(model);
    geistr_model_close(nullptr);
    geistr_chat_close(nullptr);

    static const char blob[] = "stub:echo";
    CHECK(geistr_model_open_memory(blob, sizeof blob, nullptr, &model, nullptr, 0) == GEISTR_OK && model,
          "open from memory");
    geistr_model_close(model);
}

static void test_abi(void) {
    geistr_model     *model = nullptr;
    geistr_model_opts big   = GEISTR_MODEL_OPTS_INIT;
    big.size += 8;
    CHECK(geistr_model_open("stub:echo", &big, &model, nullptr, 0) == GEISTR_INVALID,
          "options from a newer header are refused");
    geistr_model_opts tiny = {.size = sizeof(size_t), .processor = GEISTR_PROCESSOR_GPU, .context = 9};
    CHECK(geistr_model_open("stub:echo", &tiny, &model, nullptr, 0) == GEISTR_OK, "options from an older header");
    geistr_model_info info = {.size = sizeof info};
    CHECK(geistr_model_info_get(model, &info) == GEISTR_OK && info.context > 9 && !strcmp(info.backend, "cpu"),
          "fields beyond size take their defaults");
    geistr_model_info old = {.size = offsetof(geistr_model_info, context), .context = 12345};
    CHECK(geistr_model_info_get(model, &old) == GEISTR_OK && old.context == 12345 && old.arch &&
              old.size == offsetof(geistr_model_info, context),
          "an older info struct is filled only up to its size");
    geistr_chat     *chat = open_chat(model, (geistr_chat_opts) GEISTR_CHAT_OPTS_INIT);
    geistr_message   hi   = {"user", "hi"};
    struct collected got;
    CHECK(geistr_chat_send(chat, 1, &hi) == GEISTR_OK && drain(chat, &got) == GEISTR_OK, "answer");
    geistr_stats stats = {.size = offsetof(geistr_stats, prefill_ms)};
    CHECK(geistr_chat_stats(chat, &stats) == GEISTR_OK && stats.output_tokens > 0 &&
              stats.size == offsetof(geistr_stats, prefill_ms),
          "older stats struct");
    geistr_piece small = {.size = 1};
    CHECK(geistr_chat_next(chat, &small) == GEISTR_INVALID, "a piece struct too small is refused");
    geistr_chat_close(chat);
    geistr_model_close(model);
}

static void test_answer(void) {
    geistr_model *model = open_model("stub:echo", 0);
    geistr_chat  *chat  = open_chat(model, (geistr_chat_opts) GEISTR_CHAT_OPTS_INIT);
    geistr_piece  piece = {.size = sizeof piece};
    CHECK(geistr_chat_next(chat, &piece) == GEISTR_INVALID, "next before send");
    geistr_message none[] = {{"assistant", "I start"}};
    CHECK(geistr_chat_send(chat, 0, none) == GEISTR_INVALID, "no messages");
    CHECK(geistr_chat_send(chat, 1, none) == GEISTR_INVALID && geistr_chat_error(chat)[0],
          "last message from the assistant");

    geistr_message   msg[] = {{"user", "Grüße aus Köln 🌍 – ok?"}};
    struct collected got;
    CHECK(geistr_chat_send(chat, 1, msg) == GEISTR_OK, "send");
    CHECK(drain(chat, &got) == GEISTR_OK, "drain to END");
    CHECK(!strcmp(got.answer, "Echo: Grüße aus Köln 🌍 – ok?"), "answer text intact");
    CHECK(got.utf8_ok && got.nul_ok && got.pieces > 1, "every piece is complete UTF-8, NUL-terminated");
    geistr_stats st = {.size = sizeof st};
    CHECK(geistr_chat_stats(chat, &st) == GEISTR_OK && st.finish == GEISTR_FINISH_STOP && st.prompt_tokens > 0 &&
              st.output_tokens > 0 && st.first_answer_ms >= 0 && st.prefill_ms >= 0 && st.total_ms >= st.first_answer_ms,
          "stats of a finished answer");
    CHECK(geistr_chat_next(chat, &piece) == GEISTR_OK && piece.part == GEISTR_PART_END, "END repeats");

    geistr_chat_close(chat);
    geistr_model_close(model);
}

static void test_thinking(void) {
    geistr_model    *model = open_model("stub:echo", 0);
    geistr_chat_opts opts  = GEISTR_CHAT_OPTS_INIT;
    opts.reasoning         = GEISTR_REASONING_THINK_TAGS;
    geistr_chat     *quiet = open_chat(model, opts);
    opts.thinking          = 1;
    geistr_chat     *shown = open_chat(model, opts);
    geistr_message   msg   = {"user", "Why?"};
    struct collected got;

    CHECK(geistr_chat_send(quiet, 1, &msg) == GEISTR_OK && drain(quiet, &got) == GEISTR_OK, "quiet answer");
    CHECK(!strcmp(got.answer, "Echo: Why?") && !got.thinking[0], "thinking is discarded by default");
    CHECK(geistr_chat_send(shown, 1, &msg) == GEISTR_OK && drain(shown, &got) == GEISTR_OK, "shown answer");
    CHECK(!strcmp(got.thinking, "Weighing the question.") && !strcmp(got.answer, "Echo: Why?"),
          "thinking delivered separately, markers removed even when split across tokens");
    geistr_stats st = {.size = sizeof st};
    CHECK(geistr_chat_stats(shown, &st) == GEISTR_OK && st.first_answer_ms >= 0, "first answer after thinking");

    geistr_chat_close(quiet);
    geistr_chat_close(shown);
    geistr_model_close(model);
}

static void test_limits(void) {
    geistr_model    *model = open_model("stub:echo", 0);
    geistr_chat_opts opts  = GEISTR_CHAT_OPTS_INIT;
    opts.max_tokens        = 2;
    geistr_chat     *chat  = open_chat(model, opts);
    geistr_message   msg   = {"user", "a long enough question"};
    struct collected got;
    CHECK(geistr_chat_send(chat, 1, &msg) == GEISTR_OK && drain(chat, &got) == GEISTR_OK, "limited answer");
    geistr_stats st = {.size = sizeof st};
    CHECK(geistr_chat_stats(chat, &st) == GEISTR_OK && st.finish == GEISTR_FINISH_LENGTH && st.output_tokens == 2,
          "max_tokens ends with LENGTH");
    geistr_chat_close(chat);
    geistr_model_close(model);

    /* A 30-token window: the conversation needs 38 tokens, without the long turn 20. */
    model                  = open_model("stub:echo", 30);
    const char     long1[] = "first question that takes a fair number of tokens to say";
    geistr_message conv[]  = {{"system", "Be brief."}, {"user", long1}, {"assistant", "ok"}, {"user", "and now?"}};
    chat                   = open_chat(model, (geistr_chat_opts) GEISTR_CHAT_OPTS_INIT);
    CHECK(geistr_chat_send(chat, 4, conv) == GEISTR_CONTEXT, "REFUSE: too long is GEISTR_CONTEXT");
    geistr_chat_close(chat);

    opts          = (geistr_chat_opts) GEISTR_CHAT_OPTS_INIT;
    opts.overflow = GEISTR_OVERFLOW_DROP_OLDEST;
    chat          = open_chat(model, opts);
    CHECK(geistr_chat_send(chat, 4, conv) == GEISTR_OK && drain(chat, &got) == GEISTR_OK, "DROP_OLDEST answers");
    CHECK(geistr_chat_stats(chat, &st) == GEISTR_OK && st.dropped_messages > 0 && st.prompt_tokens < 30,
          "oldest turns dropped to fit");
    char           huge[400];
    memset(huge, 'x', sizeof huge - 1);
    huge[sizeof huge - 1] = 0;
    geistr_message alone  = {"user", huge};
    CHECK(geistr_chat_send(chat, 1, &alone) == GEISTR_CONTEXT, "a single message too long even after dropping");
    geistr_chat_close(chat);
    geistr_model_close(model);
}

static void test_reuse(void) {
    geistr_model    *model   = open_model("stub:echo", 0);
    geistr_chat     *chat    = open_chat(model, (geistr_chat_opts) GEISTR_CHAT_OPTS_INIT);
    geistr_message   turn1[] = {{"system", "You are terse."}, {"user", "Name a colour."}};
    geistr_message   turn2[] = {{"system", "You are terse."}, {"user", "Name a colour."}, {"assistant", "Blue."}, {"user", "Another?"}};
    struct collected got;
    geistr_stats     st = {.size = sizeof st};
    CHECK(geistr_chat_send(chat, 2, turn1) == GEISTR_OK && drain(chat, &got) == GEISTR_OK, "turn 1");
    CHECK(geistr_chat_stats(chat, &st) == GEISTR_OK && st.reused_tokens == 0, "nothing to reuse at first");
    uint32_t first = st.prompt_tokens;
    CHECK(geistr_chat_send(chat, 4, turn2) == GEISTR_OK && drain(chat, &got) == GEISTR_OK, "turn 2");
    CHECK(geistr_chat_stats(chat, &st) == GEISTR_OK && st.reused_tokens > 0 && st.reused_tokens <= first &&
              st.reused_tokens < st.prompt_tokens,
          "the unchanged prefix is reused");
    geistr_chat_close(chat);
    geistr_model_close(model);
}

static void *cancel_later(void *chat) {
    struct timespec wait = {.tv_nsec = 30 * 1000000};
    nanosleep(&wait, nullptr);
    geistr_chat_cancel((geistr_chat *) chat);
    return nullptr;
}

static int stop_after_first(void *count, const geistr_piece *) {
    return ++*(int *) count < 1;
}

static void test_cancel(void) {
    geistr_model  *model = open_model("stub:slow", 4096);
    geistr_chat   *chat  = open_chat(model, (geistr_chat_opts) GEISTR_CHAT_OPTS_INIT);
    geistr_message msg   = {"user", "talk for a while"};
    CHECK(geistr_chat_send(chat, 1, &msg) == GEISTR_OK, "slow send");
    pthread_t thread;
    pthread_create(&thread, nullptr, cancel_later, chat);
    double        start = now_ms();
    geistr_piece  piece = {.size = sizeof piece};
    geistr_status s;
    while ((s = geistr_chat_next(chat, &piece)) == GEISTR_OK && piece.part != GEISTR_PART_END) {
    }
    pthread_join(thread, nullptr);
    CHECK(s == GEISTR_CANCELLED, "cancel from another thread ends next with CANCELLED");
    CHECK(now_ms() - start < 2000, "cancellation is prompt");
    geistr_stats st = {.size = sizeof st};
    CHECK(geistr_chat_stats(chat, &st) == GEISTR_OK && st.finish == GEISTR_FINISH_CANCELLED, "finish CANCELLED");
    CHECK(geistr_chat_next(chat, &piece) == GEISTR_OK && piece.part == GEISTR_PART_END, "after CANCELLED: END");
    geistr_chat_close(chat);

    geistr_chat_opts opts = GEISTR_CHAT_OPTS_INIT;
    opts.max_tokens       = 6;
    chat                  = open_chat(model, opts);
    int count             = 0;
    CHECK(geistr_chat_run(chat, 1, &msg, stop_after_first, &count) == GEISTR_CANCELLED && count == 1,
          "emit returning 0 cancels run");
    CHECK(geistr_chat_send(chat, 1, &msg) == GEISTR_OK && geistr_chat_next(chat, &piece) == GEISTR_OK,
          "the chat stays usable after a cancel");
    geistr_chat_close(chat);
    geistr_model_close(model);
}

static void test_lifetime(void) {
    geistr_model *model = open_model("stub:echo", 0);
    geistr_chat  *chat  = open_chat(model, (geistr_chat_opts) GEISTR_CHAT_OPTS_INIT);
    geistr_model_close(model); /* the chat keeps it alive */
    geistr_message   msg = {"user", "still there?"};
    struct collected got;
    CHECK(geistr_chat_send(chat, 1, &msg) == GEISTR_OK && drain(chat, &got) == GEISTR_OK &&
              !strcmp(got.answer, "Echo: still there?"),
          "a chat outlives the caller's model reference");
    geistr_chat_close(chat);
}

int main(void) {
    test_basics();
    test_abi();
    test_answer();
    test_thinking();
    test_limits();
    test_reuse();
    test_cancel();
    test_lifetime();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("geistr API: basics, ABI sizes, answer, thinking, limits, reuse, cancellation, lifetime passed");
    return 0;
}

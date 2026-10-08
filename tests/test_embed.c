/* test_embed.c — geistr_embed (#91) against BitNet-embedding-0.6B: the model
 * card's printed vector, the same vector twice, retrieval order (English and
 * German), several threads at once, too little room, too long a text, and a
 * model that generates text. Usage: GEIST_TEST_EMBED_MODEL=<gguf>
 * [GEIST_TEST_MODEL=<generative gguf>] test_embed */
#include "geistr.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, what);                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define DIMS 1024
static geistr_model *model;

static double dot(const float *a, const float *b) {
    double d = 0;
    for (size_t i = 0; i < DIMS; i++)
        d += (double) a[i] * (double) b[i];
    return d;
}

static bool embed(const char *text, float out[DIMS]) {
    size_t        dims = 0;
    geistr_status s    = geistr_embed(model, text, out, DIMS, &dims, nullptr);
    if (s != GEISTR_OK)
        fprintf(stderr, "embed: %s\n", geistr_model_error(model));
    return s == GEISTR_OK && dims == DIMS;
}

static const char *const QUERY = "Instruct: Given a question, retrieve passages that answer the question\n"
                                 "Query: Where does the cat sleep?";
static const char *const DOCS[] = {"The cat sleeps on the red sofa in the living room.",
                                   "Die Katze schläft auf dem roten Sofa im Wohnzimmer.",
                                   "Quarterly revenue grew by twelve percent.",
                                   "The train to Hamburg leaves at nine."};

static void *in_thread(void *arg) {
    float *out = arg;
    for (int k = 0; k < 4; k++)
        if (!embed(DOCS[k], out + k * DIMS))
            out[k * DIMS] = NAN;
    return nullptr;
}

int main(void) {
    const char *path = getenv("GEIST_TEST_EMBED_MODEL");
    if (!path) {
        fprintf(stderr, "test_embed: set GEIST_TEST_EMBED_MODEL\n");
        return 1;
    }
    char              error[256];
    geistr_model_opts o = GEISTR_MODEL_OPTS_INIT;
    o.processor         = GEISTR_PROCESSOR_CPU;
    o.context           = 256;
    if (geistr_model_open(path, &o, &model, error, sizeof error) != GEISTR_OK) {
        fprintf(stderr, "test_embed: %s\n", error);
        return 1;
    }
    static float a[DIMS], b[DIMS], docs[4][DIMS], threaded[2][4 * DIMS];

    /* The model card's vector (llama-embedding, "query: What is BitNet?"). */
    static const float card[] = {0.0239517f, 0.6826404f, -0.0f, -0.0644535f, 0.0613754f, 0.0473094f, 0.0114330f};
    uint32_t           tokens = 0;
    size_t             dims   = 0;
    CHECK(geistr_embed(model, "query: What is BitNet?", a, DIMS, &dims, &tokens) == GEISTR_OK && dims == DIMS &&
                  tokens > 4,
          "an embedding of 1024 dimensions");
    double e = 0;
    for (size_t i = 0; i < sizeof card / sizeof *card; i++)
        e += (a[i] - card[i]) * (a[i] - card[i]);
    const double rmse = sqrt(e / (double) (sizeof card / sizeof *card));
    CHECK(rmse < 5e-3, "the model card's vector");
    CHECK(fabs(dot(a, a) - 1) < 1e-3, "L2-normalised");
    CHECK(embed("query: What is BitNet?", b) && dot(a, b) > 0.9999, "the same text, the same vector");

    /* Retrieval: the cat, in English and German, before the rest. */
    CHECK(embed(QUERY, a), "the query");
    double score[4];
    for (int k = 0; k < 4; k++) {
        CHECK(embed(DOCS[k], docs[k]), "a document");
        score[k] = dot(a, docs[k]);
    }
    CHECK(fmin(score[0], score[1]) > fmax(score[2], score[3]) + 0.05, "retrieval order");

    /* Two threads at once: calls on one model take turns, the vectors match. */
    pthread_t t[2];
    for (int k = 0; k < 2; k++)
        pthread_create(&t[k], nullptr, in_thread, threaded[k]);
    for (int k = 0; k < 2; k++)
        pthread_join(t[k], nullptr);
    for (int k = 0; k < 2; k++)
        for (int d = 0; d < 4; d++)
            CHECK(dot(threaded[k] + d * DIMS, docs[d]) > 0.9999, "the same vectors from two threads");

    /* Too little room: the dimension, to ask again. Too long: the context. */
    dims = 0;
    CHECK(geistr_embed(model, "x", a, 8, &dims, nullptr) == GEISTR_INVALID && dims == DIMS &&
                  strstr(geistr_model_error(model), "room"),
          "too little room says how much");
    char long_text[8000];
    memset(long_text, 0, sizeof long_text);
    for (size_t at = 0; at + 6 < sizeof long_text; at += 6)
        memcpy(long_text + at, "words ", 6);
    CHECK(geistr_embed(model, long_text, a, DIMS, &dims, nullptr) == GEISTR_CONTEXT, "longer than the window");
    CHECK(embed(DOCS[0], b) && dot(b, docs[0]) > 0.9999, "after a refusal it embeds as before");
    geistr_model_close(model);

    const char *generative = getenv("GEIST_TEST_MODEL");
    if (generative) {
        geistr_model *g = nullptr;
        CHECK(geistr_model_open(generative, &o, &g, error, sizeof error) == GEISTR_OK, error);
        if (g) {
            CHECK(geistr_embed(g, "hi", a, DIMS, &dims, nullptr) == GEISTR_FORMAT &&
                          strstr(geistr_model_error(g), "not an embedding model"),
                  "a model that generates text refuses");
            geistr_model_close(g);
        }
    }
    if (failures)
        return fprintf(stderr, "test_embed: %d check(s) failed\n", failures), 1;
    printf("geistr_embed: card vector (RMSE %.1e), deterministic, retrieval (en, de), two threads, room, context%s passed\n",
           rmse, generative ? ", generative model refused" : "");
    return 0;
}

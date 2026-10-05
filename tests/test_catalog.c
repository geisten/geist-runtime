/* test_catalog.c — catalog parsing, SHA-256, install states and receipts (#5).
 * Usage: test_catalog [catalog.json]   (default models/catalog.json)
 *        test_catalog --validate        catalog on stdin; "ok" or the reason,
 *                                       exit 1 on refusal (tests/test_catalog.py) */
#include "geistr_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

static char *slurp(FILE *f, size_t *len) {
    size_t cap = 1 << 16, n = 0, got;
    char  *s   = malloc(cap);
    while (s && (got = fread(s + n, 1, cap - n, f)) > 0)
        if ((n += got) == cap)
            s = realloc(s, cap *= 2);
    *len = n;
    return s;
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static void sha256_vectors(const char *dir) {
    char path[512], sum[65];
    snprintf(path, sizeof path, "%s/v", dir);
    write_file(path, "");
    CHECK(geistr_sha256_file(path, sum) == GEISTR_OK);
    CHECK(!strcmp(sum, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    write_file(path, "abc");
    CHECK(geistr_sha256_file(path, sum) == GEISTR_OK);
    CHECK(!strcmp(sum, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    write_file(path, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    CHECK(geistr_sha256_file(path, sum) == GEISTR_OK);
    CHECK(!strcmp(sum, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    FILE *f = fopen(path, "w"); /* one million 'a': many blocks, partial reads */
    for (int i = 0; i < 1000000; i++)
        fputc('a', f);
    fclose(f);
    CHECK(geistr_sha256_file(path, sum) == GEISTR_OK);
    CHECK(!strcmp(sum, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    snprintf(path, sizeof path, "%s/absent", dir);
    CHECK(geistr_sha256_file(path, sum) == GEISTR_IO);
}

/* geist-serve's catalog loads unchanged. */
static void shipped(const char *file) {
    FILE *f = fopen(file, "r");
    CHECK(f);
    if (!f)
        return;
    size_t          len;
    char           *text = slurp(f, &len), error[128];
    geistr_catalog *c;
    fclose(f);
    CHECK(geistr_catalog_parse(text, len, &c, error, sizeof error) == GEISTR_OK);
    if (!c) {
        fprintf(stderr, "%s: %s\n", file, error);
        free(text);
        return;
    }
    CHECK(geistr_catalog_count(c) >= 1 && geistr_catalog_revision(c) >= 1);
    const geistr_catalog_entry *first = geistr_catalog_get(c, 0);
    CHECK(geistr_catalog_find(c, first->id) == first);
    CHECK(!geistr_catalog_find(c, "custom") && !geistr_catalog_get(c, geistr_catalog_count(c)));
    for (size_t i = 0; i < geistr_catalog_count(c); i++) {
        const geistr_catalog_entry *m = geistr_catalog_get(c, i);
        CHECK(strlen(m->sha256) == 64 && m->bytes && m->group_id && m->group_name);
        CHECK((m->backends & GEISTR_BACKEND_CPU) || m->unsupported_format);
        CHECK(m->quality_passed <= m->quality_total);
    }
    printf("catalog: %s, revision %u, %zu models\n", file, geistr_catalog_revision(c), geistr_catalog_count(c));
    geistr_catalog_free(c);
    free(text);
}

static void install_states(const char *dir) {
    char path[512], sum[65], json[1024], error[128];
    snprintf(path, sizeof path, "%s/tiny.gguf", dir);
    write_file(path, "GGUF tiny model bytes");
    CHECK(geistr_sha256_file(path, sum) == GEISTR_OK);
    snprintf(json, sizeof json,
             "{\"schema\":2,\"revision\":1,\"models\":[{\"id\":\"tiny\",\"name\":\"Tiny\",\"file\":\"tiny.gguf\","
             "\"url\":\"https://huggingface.co/x/y/resolve/main/tiny.gguf\",\"sha256\":\"%s\",\"bytes\":21,"
             "\"working_mib\":1,\"recommended_ram_gib\":1,\"backends\":[\"cpu\"],\"group_id\":\"tiny\","
             "\"group_name\":\"Tiny\",\"quantization\":\"Q8_0\"},"
             "{\"id\":\"gone\",\"name\":\"Gone\",\"file\":\"gone.gguf\","
             "\"url\":\"https://huggingface.co/x/y/resolve/main/gone.gguf\",\"sha256\":\"%064d\",\"bytes\":21,"
             "\"working_mib\":1,\"recommended_ram_gib\":1,\"backends\":[\"cpu\"],\"group_id\":\"gone\","
             "\"group_name\":\"Gone\",\"quantization\":\"Q8_0\"}]}\n",
             sum, 0);
    geistr_catalog *c;
    CHECK(geistr_catalog_parse(json, strlen(json), &c, error, sizeof error) == GEISTR_OK);
    if (!c) {
        fprintf(stderr, "fixture: %s\n", error);
        return;
    }
    const geistr_catalog_entry *tiny = geistr_catalog_find(c, "tiny"), *gone = geistr_catalog_find(c, "gone");
    geistr_install              state;

    CHECK(geistr_catalog_check(gone, dir, true, &state) == GEISTR_OK && state == GEISTR_INSTALL_MISSING);
    CHECK(geistr_catalog_check(tiny, dir, false, &state) == GEISTR_OK && state == GEISTR_INSTALL_UNVERIFIED);
    CHECK(geistr_catalog_check(tiny, dir, true, &state) == GEISTR_OK && state == GEISTR_INSTALL_OK);
    /* The receipt answers without hashing. */
    CHECK(geistr_catalog_check(tiny, dir, false, &state) == GEISTR_OK && state == GEISTR_INSTALL_OK);

    /* Tampered: same size, different bytes. The receipt no longer applies. */
    sleep(1); /* coarse-ctime file systems */
    write_file(path, "GGUF tiny model BYTES");
    CHECK(geistr_catalog_check(tiny, dir, false, &state) == GEISTR_OK && state == GEISTR_INSTALL_UNVERIFIED);
    CHECK(geistr_catalog_check(tiny, dir, true, &state) == GEISTR_OK && state == GEISTR_INSTALL_MISMATCH);
    write_file(path, "short");
    CHECK(geistr_catalog_check(tiny, dir, false, &state) == GEISTR_OK && state == GEISTR_INSTALL_MISMATCH);
    /* A folder of that name is not an installed model. */
    unlink(path);
    CHECK(mkdir(path, 0700) == 0);
    CHECK(geistr_catalog_check(tiny, dir, true, &state) == GEISTR_OK && state == GEISTR_INSTALL_MISMATCH);
    rmdir(path);

    CHECK(geistr_catalog_check(tiny, nullptr, true, &state) == GEISTR_INVALID);
    CHECK(geistr_catalog_check(nullptr, dir, true, &state) == GEISTR_INVALID);
    geistr_catalog_free(c);
}

static void models_dir(void) {
    char  out[512];
    char *saved = getenv("GEISTEN_HOME") ? strdup(getenv("GEISTEN_HOME")) : nullptr;
    setenv("GEISTEN_HOME", "/data/geisten", 1);
    CHECK(geistr_models_dir(out, sizeof out) == GEISTR_OK && !strcmp(out, "/data/geisten/models"));
    CHECK(geistr_models_dir(out, 8) == GEISTR_INVALID);
    unsetenv("GEISTEN_HOME");
    if (!getenv("GEIST_HOME") && getenv("HOME")) {
        CHECK(geistr_models_dir(out, sizeof out) == GEISTR_OK && strstr(out, "geisten/models"));
        printf("models folder: %s\n", out);
    }
    if (saved) {
        setenv("GEISTEN_HOME", saved, 1);
        free(saved);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--validate")) {
        size_t          len;
        char           *text = slurp(stdin, &len), error[128];
        geistr_catalog *c;
        geistr_status   s = geistr_catalog_parse(text, len, &c, error, sizeof error);
        puts(s == GEISTR_OK ? "ok" : error);
        geistr_catalog_free(c);
        free(text);
        return s != GEISTR_OK;
    }
    char dir[] = "/tmp/geistr-catalog-XXXXXX";
    CHECK(mkdtemp(dir));
    sha256_vectors(dir);
    shipped(argc > 1 ? argv[1] : "models/catalog.json");
    install_states(dir);
    models_dir();

    geistr_catalog *c = (geistr_catalog *) 1;
    char            error[64];
    CHECK(geistr_catalog_parse("{}", 2, &c, error, sizeof error) == GEISTR_FORMAT && !c && error[0]);
    CHECK(geistr_catalog_parse("x", 1, nullptr, nullptr, 0) == GEISTR_INVALID);
    CHECK(geistr_catalog_count(nullptr) == 0 && !geistr_catalog_find(nullptr, "x"));

    char command[600];
    snprintf(command, sizeof command, "rm -rf '%s'", dir);
    CHECK(system(command) == 0);
    if (failures)
        fprintf(stderr, "test_catalog: %d failures\n", failures);
    else
        puts("test_catalog: all passed");
    return failures != 0;
}

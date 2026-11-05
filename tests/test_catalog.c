/* test_catalog.c — catalog parsing, SHA-256, install states and receipts (#5).
 * Also the verified-profile artifact check by receipt (#287).
 * Usage: test_catalog [catalog.json]   (default models/catalog.json)
 *        test_catalog --validate        catalog on stdin; "ok" or the reason,
 *                                       exit 1 on refusal (tests/test_catalog.py) */
#include "geistr_catalog.h"
#include "common.h"
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
        /* a vision tower (#92) comes whole or not at all */
        CHECK(!m->vision_url == !m->vision_sha256 && !m->vision_url == !m->vision_bytes);
        /* Gemma 4 thinks when asked: <|think|>, its reasoning in channel tags (#270) */
        if (!strncmp(m->id, "gemma4-", 7))
            CHECK(m->reasoning_format && !strcmp(m->reasoning_format, "channel_tags"));
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
    CHECK(geistr_catalog_check(tiny, dir, true, &state) == GEISTR_OK && state == GEISTR_INSTALL_CORRUPT);
    write_file(path, "short");
    CHECK(geistr_catalog_check(tiny, dir, false, &state) == GEISTR_OK && state == GEISTR_INSTALL_MISMATCH);
    /* A folder of that name is not an installed model. */
    unlink(path);
    CHECK(mkdir(path, 0700) == 0);
    CHECK(geistr_catalog_check(tiny, dir, true, &state) == GEISTR_OK && state == GEISTR_INSTALL_MISMATCH);
    rmdir(path);

    /* A symlink is never an installed model (the hash opens with O_NOFOLLOW), even to a good file (#151). */
    char shared[512];
    snprintf(shared, sizeof shared, "%s/shared.gguf", dir);
    write_file(shared, "GGUF tiny model bytes");
    CHECK(symlink(shared, path) == 0);
    CHECK(geistr_catalog_check(tiny, dir, true, &state) == GEISTR_OK && state == GEISTR_INSTALL_MISMATCH);
    unlink(path), unlink(shared);

    /* a vision tower is checked like a model; other file kinds are not (#92) */
    geistr_catalog_entry tower = *tiny;
    tower.file                 = "vision_tower.safetensors";
    CHECK(geistr_catalog_check(&tower, dir, true, &state) == GEISTR_OK && state == GEISTR_INSTALL_MISSING);
    tower.file = "run.sh";
    CHECK(geistr_catalog_check(&tower, dir, true, &state) == GEISTR_INVALID);
    tower.file = "../vision_tower.safetensors";
    CHECK(geistr_catalog_check(&tower, dir, true, &state) == GEISTR_INVALID);
    CHECK(geistr_catalog_check(tiny, nullptr, true, &state) == GEISTR_INVALID);
    CHECK(geistr_catalog_check(nullptr, dir, true, &state) == GEISTR_INVALID);
    geistr_catalog_free(c);
}

static void copy_file(const char *to, const char *from) {
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    for (int ch; in && out && (ch = fgetc(in)) != EOF;)
        fputc(ch, out);
    if (in)
        fclose(in);
    if (out)
        fclose(out);
}

/* The receipt geistr_catalog_check writes for entry, copied under the claimed
 * SHA-256 (in its name and its stamp). */
static void forge_receipt(const geistr_catalog_entry *entry, const char *dir, const char *real, const char *forged,
                          const char *claimed) {
    geistr_install state;
    char           text[256] = "";
    CHECK(geistr_catalog_check(entry, dir, true, &state) == GEISTR_OK && state == GEISTR_INSTALL_OK);
    FILE *f = fopen(real, "r");
    CHECK(f && fread(text, 1, sizeof text - 1, f) > 70);
    if (f)
        fclose(f);
    CHECK(!strncmp(text, "v1 ", 3) && !strncmp(text + 3, entry->sha256, 64));
    memcpy(text + 3, claimed, 64);
    write_file(forged, text);
}

/* A verified profile's file (#287): a receipt matching the file's identity
 * stands for its hash; a changed, swapped or receipt-less file is hashed and
 * a wrong one refused. The receipt is forged here (the stamp of a real check,
 * its SHA-256 replaced), so a hash would refuse: GEISTR_OK proves none ran. */
static void artifact_receipts(const char *dir) {
    char path[512], swap[512], receipts[512], real[600], forged[600], sum[65];
    const char claimed[] = "b0c1d2e3f405162738495a6b7c8d9eafb0c1d2e3f405162738495a6b7c8d9eaf";
    snprintf(path, sizeof path, "%s/artifact.gguf", dir);
    snprintf(swap, sizeof swap, "%s/swap.gguf", dir);
    snprintf(receipts, sizeof receipts, "%s/.verified", dir);
    write_file(path, "GGUF artifact bytes");
    CHECK(geistr_sha256_file(path, sum) == GEISTR_OK);

    /* No receipt: hashed. Its own hash passes, another is refused. */
    CHECK(artifact_verify(path, sum) == GEISTR_OK);
    CHECK(artifact_verify(path, claimed) == GEISTR_FORMAT);
    CHECK(artifact_verify(path, "not a hash") == GEISTR_INVALID);
    CHECK(artifact_verify(nullptr, sum) == GEISTR_INVALID);

    /* A real check leaves the receipt; forge one for the claimed hash. */
    const geistr_catalog_entry entry = {.file = "artifact.gguf", .sha256 = sum, .bytes = 19};
    snprintf(real, sizeof real, "%s/%s", receipts, sum);
    snprintf(forged, sizeof forged, "%s/%s", receipts, claimed);
    forge_receipt(&entry, dir, real, forged, claimed);
    CHECK(artifact_verify(path, claimed) == GEISTR_OK); /* the receipt: not hashed */
    CHECK(artifact_verify(path, sum) == GEISTR_OK);     /* its real receipt */

    /* Swapped: another file (same size) renamed into place. */
    write_file(swap, "GGUF artifact BYTES");
    CHECK(rename(swap, path) == 0);
    CHECK(artifact_verify(path, claimed) == GEISTR_FORMAT);
    CHECK(artifact_verify(path, sum) == GEISTR_FORMAT);

    /* Changed in place after its receipt (a stale receipt), here back to the
     * right bytes: the receipt no longer counts, the hash decides. */
    write_file(path, "GGUF artifact bytes");
    forge_receipt(&entry, dir, real, forged, claimed);
    CHECK(artifact_verify(path, claimed) == GEISTR_OK);
    sleep(1); /* coarse-ctime file systems */
    write_file(path, "GGUF artifact bytes");
    CHECK(artifact_verify(path, claimed) == GEISTR_FORMAT);
    CHECK(artifact_verify(path, sum) == GEISTR_OK);

    /* A copy elsewhere carries no receipt; a symlink never counts. */
    char other[512], link[512];
    snprintf(other, sizeof other, "%s/copy", dir);
    CHECK(mkdir(other, 0700) == 0);
    snprintf(other, sizeof other, "%s/copy/artifact.gguf", dir);
    copy_file(other, path);
    CHECK(artifact_verify(other, claimed) == GEISTR_FORMAT);
    snprintf(link, sizeof link, "%s/link.gguf", dir);
    CHECK(symlink(path, link) == 0);
    CHECK(artifact_verify(link, sum) == GEISTR_IO);
    unlink(link), unlink(other), unlink(path), unlink(real), unlink(forged);
    snprintf(other, sizeof other, "%s/copy", dir);
    rmdir(other);
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
    artifact_receipts(dir);
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

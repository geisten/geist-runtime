/* json.c — see json.h. */
#include "json.h"
#define JSMN_STATIC
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "jsmn.h"
#include <stdlib.h>
#include <string.h>

#define T(j) ((jsmntok_t *) (j)->tokens)

bool json_parse(struct json *j, const char *s, size_t len) {
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, s, len, nullptr, 0);
    *j    = (struct json) {.s = s};
    if (n < 1 || !(j->tokens = malloc((size_t) n * sizeof(jsmntok_t))))
        return false;
    jsmn_init(&p);
    j->n = jsmn_parse(&p, s, len, T(j), (unsigned) n);
    return j->n >= 1 && T(j)[0].type == JSMN_OBJECT;
}

void json_free(struct json *j) {
    free(j->tokens);
    *j = (struct json) {};
}

int json_field(const struct json *j, int obj, const char *key) {
    size_t           kl = strlen(key);
    const jsmntok_t *t  = T(j);
    for (int i = obj + 1; obj >= 0 && i + 1 < j->n && t[i].start < t[obj].end; i++)
        if (t[i].parent == obj && t[i].type == JSMN_STRING && (size_t) (t[i].end - t[i].start) == kl &&
            !memcmp(j->s + t[i].start, key, kl))
            return i + 1;
    return -1;
}

int json_count(const struct json *j, int t) {
    return t >= 0 && T(j)[t].type == JSMN_ARRAY ? T(j)[t].size : -1;
}

int json_item(const struct json *j, int array, int k) {
    for (int i = array + 1; array >= 0 && i < j->n; i++)
        if (T(j)[i].parent == array && k-- == 0)
            return i;
    return -1;
}

/* UTF-8 of code point cp into out; returns the bytes written. */
static size_t utf8(unsigned cp, char *out) {
    if (cp < 0x80)
        return out[0] = (char) cp, 1;
    if (cp < 0x800)
        return out[0] = (char) (0xC0 | (cp >> 6)), out[1] = (char) (0x80 | (cp & 0x3F)), 2;
    if (cp < 0x10000)
        return out[0] = (char) (0xE0 | (cp >> 12)), out[1] = (char) (0x80 | ((cp >> 6) & 0x3F)),
               out[2] = (char) (0x80 | (cp & 0x3F)), 3;
    return out[0] = (char) (0xF0 | (cp >> 18)), out[1] = (char) (0x80 | ((cp >> 12) & 0x3F)),
           out[2] = (char) (0x80 | ((cp >> 6) & 0x3F)), out[3] = (char) (0x80 | (cp & 0x3F)), 4;
}

static unsigned hex4(const char *s) {
    char hex[5] = {s[0], s[1], s[2], s[3], 0};
    return (unsigned) strtoul(hex, nullptr, 16);
}

char *json_string(const struct json *j, int t) {
    if (t < 0 || T(j)[t].type != JSMN_STRING)
        return nullptr;
    const char *s   = j->s + T(j)[t].start;
    size_t      n   = (size_t) (T(j)[t].end - T(j)[t].start), o = 0;
    char       *out = malloc(n + 1); /* never longer than escaped */
    for (size_t i = 0; out && i < n; i++) {
        if (s[i] != '\\' || i + 1 >= n) {
            out[o++] = s[i];
            continue;
        }
        char e = s[++i];
        if (e == 'u' && i + 4 < n) { /* \uXXXX, a surrogate pair as one code point */
            unsigned cp = hex4(s + i + 1);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < n && s[i + 1] == '\\' && s[i + 2] == 'u') {
                unsigned lo = hex4(s + i + 3);
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00), i += 6;
            }
            o += utf8(cp, out + o);
            continue;
        }
        out[o++] = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
    }
    if (out)
        out[o] = 0;
    return out;
}

double json_number(const struct json *j, int t, double dflt) {
    return t >= 0 && T(j)[t].type == JSMN_PRIMITIVE ? strtod(j->s + T(j)[t].start, nullptr) : dflt;
}

bool json_bool(const struct json *j, int t, bool dflt) {
    if (t < 0 || T(j)[t].type != JSMN_PRIMITIVE)
        return dflt;
    const char *v = j->s + T(j)[t].start;
    return *v == 't' ? true : *v == 'f' ? false : dflt;
}

void json_get(const char *object, const char *key, char *out, size_t cap) {
    struct json j = {};
    out[0]        = 0;
    if (json_parse(&j, object, strlen(object))) {
        int   t = json_field(&j, 0, key);
        char *s = json_string(&j, t);
        if (s)
            snprintf(out, cap, "%s", s);
        else if (t >= 0)
            snprintf(out, cap, "%.*s", T(&j)[t].end - T(&j)[t].start, object + T(&j)[t].start);
        free(s);
    }
    json_free(&j);
}

void json_write(FILE *f, const char *s) {
    fputc('"', f);
    for (; s && *s; s++)
        if (*s == '"' || *s == '\\')
            fprintf(f, "\\%c", *s);
        else if ((unsigned char) *s < 0x20)
            fprintf(f, "\\u%04x", (unsigned char) *s);
        else
            fputc(*s, f);
    fputc('"', f);
}

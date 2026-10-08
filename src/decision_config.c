/* Decision configuration/input validation (#13). No engine or mutable global
 * policy. The deliberately small JSON grammar accepts only this schema. */
#include "geistr_decision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct geistr_decision_config {
    size_t count;
    geistr_decision_policy policies[GEISTR_DECISION_MAX_POLICIES];
};

static const char *const names[] = {"none", "bonsai2-27b-pq2-v1", "gemma4-e2b-q4-v1"};
static const char *const hashes[] = {"", "3907dc1658db1f78a9826bf8d5bcb8dc65db0d466388937af57f2294fae62ec1",
                                     "740185b21d22ceb83a11c3aa62ad5842ef32c70f6096d756bbee85a1e4ec34b8"};

const char *geistr_decision_profile_name(geistr_decision_profile p) {
    return p == GEISTR_DECISION_PROFILE_NONE || p == GEISTR_DECISION_BONSAI2_27B_PQ2 ||
                   p == GEISTR_DECISION_GEMMA4_E2B_Q4
               ? names[p]
               : "unknown";
}

const char *geistr_decision_profile_sha256(geistr_decision_profile p) {
    return p == GEISTR_DECISION_PROFILE_NONE || p == GEISTR_DECISION_BONSAI2_27B_PQ2 ||
                   p == GEISTR_DECISION_GEMMA4_E2B_Q4
               ? hashes[p]
               : "";
}

static bool hash_valid(const char *s) {
    if (!s || strnlen(s, 65) != 64)
        return false;
    for (size_t i = 0; i < 64; ++i)
        if (!(s[i] >= '0' && s[i] <= '9') && !(s[i] >= 'a' && s[i] <= 'f'))
            return false;
    return true;
}

geistr_status geistr_decision_policy_validate(const geistr_decision_policy *p) {
    if (!p || !hash_valid(p->sha256) ||
        (p->mode != GEISTR_DECISION_DENSE && p->mode != GEISTR_DECISION_SELECTED_ROWS) ||
        (p->profile != GEISTR_DECISION_PROFILE_NONE && p->profile != GEISTR_DECISION_BONSAI2_27B_PQ2 &&
         p->profile != GEISTR_DECISION_GEMMA4_E2B_Q4))
        return GEISTR_INVALID;
    if (p->profile != GEISTR_DECISION_PROFILE_NONE &&
        strcmp(p->sha256, geistr_decision_profile_sha256(p->profile)))
        return GEISTR_FORMAT;
    return p->enabled && p->profile == GEISTR_DECISION_PROFILE_NONE ? GEISTR_FORMAT : GEISTR_OK;
}

void geistr_decision_config_free(geistr_decision_config *c) { free(c); }
size_t geistr_decision_config_count(const geistr_decision_config *c) { return c ? c->count : 0; }
const geistr_decision_policy *geistr_decision_config_get(size_t i, const geistr_decision_config *c) {
    return c && i < c->count ? &c->policies[i] : nullptr;
}
const geistr_decision_policy *geistr_decision_config_find(const char *sha, const geistr_decision_config *c) {
    if (!c || !hash_valid(sha))
        return nullptr;
    for (size_t i = 0; i < c->count; ++i)
        if (!strcmp(sha, c->policies[i].sha256))
            return &c->policies[i];
    return nullptr;
}

struct input {
    const char *p, *end;
};
static void space(struct input *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n'))
        ++j->p;
}
static bool take(struct input *j, char ch) {
    space(j);
    if (j->p == j->end || *j->p != ch)
        return false;
    ++j->p;
    return true;
}
static bool literal(struct input *j, const char *s) {
    space(j);
    const size_t n = strlen(s);
    if (n > (size_t)(j->end - j->p) || memcmp(j->p, s, n))
        return false;
    j->p += n;
    return true;
}
/* Configuration has only ASCII literal keys and fixed profile/hash values.
 * cap includes the terminator. A failed parse never advances past end. */
static bool text(size_t cap, struct input *j, char *out) {
    if (!take(j, '"'))
        return false;
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        const unsigned char ch = (unsigned char)*j->p;
        if (ch < 32 || ch >= 127 || ch == '\\' || n >= cap - 1)
            return false;
        out[n++] = *j->p++;
    }
    out[n] = 0;
    return take(j, '"');
}

static bool policy(struct input *j, geistr_decision_policy *p) {
    if (!take(j, '{'))
        return false;
    *p = (geistr_decision_policy){.mode = GEISTR_DECISION_DENSE};
    unsigned seen = 0;
    for (;;) {
        char key[16], value[65];
        if (!text(sizeof key, j, key) || !take(j, ':'))
            return false;
        unsigned bit;
        if (!strcmp(key, "sha256")) {
            bit = 1;
            if (!text(sizeof p->sha256, j, p->sha256))
                return false;
        } else if (!strcmp(key, "enabled")) {
            bit = 2;
            if (literal(j, "true"))
                p->enabled = true;
            else if (literal(j, "false"))
                p->enabled = false;
            else
                return false;
        } else if (!strcmp(key, "profile")) {
            bit = 4;
            if (!text(sizeof value, j, value))
                return false;
            bool found = false;
            for (size_t i = 1; i < sizeof names / sizeof names[0]; ++i)
                if (!strcmp(value, names[i])) {
                    p->profile = (geistr_decision_profile)i;
                    found = true;
                }
            if (!found)
                return false;
        } else if (!strcmp(key, "mode")) {
            bit = 8;
            if (!text(sizeof value, j, value))
                return false;
            if (!strcmp(value, "dense"))
                p->mode = GEISTR_DECISION_DENSE;
            else if (!strcmp(value, "selected_rows"))
                p->mode = GEISTR_DECISION_SELECTED_ROWS;
            else
                return false;
        } else
            return false;
        if (seen & bit)
            return false;
        seen |= bit;
        if (take(j, '}'))
            break;
        if (!take(j, ','))
            return false;
    }
    return (seen & 1) && geistr_decision_policy_validate(p) == GEISTR_OK;
}

static bool policies(struct input *j, geistr_decision_config *c) {
    if (!take(j, '['))
        return false;
    if (take(j, ']'))
        return true;
    for (;;) {
        if (c->count == GEISTR_DECISION_MAX_POLICIES || !policy(j, &c->policies[c->count]))
            return false;
        for (size_t i = 0; i < c->count; ++i)
            if (!strcmp(c->policies[i].sha256, c->policies[c->count].sha256))
                return false;
        ++c->count;
        if (take(j, ']'))
            return true;
        if (!take(j, ','))
            return false;
    }
}

static geistr_status error(size_t cap, char *out, geistr_status status, const char *why) {
    if (out && cap)
        snprintf(out, cap, "%s", why);
    return status;
}

geistr_status geistr_decision_config_parse(size_t len, size_t cap, const char *json,
                                           geistr_decision_config **out, char *err) {
    if (out)
        *out = nullptr;
    if (err && cap)
        err[0] = 0;
    if (!out || !json || !len || len > GEISTR_DECISION_MAX_CONFIG || memchr(json, 0, len))
        return error(cap, err, GEISTR_INVALID, "decision configuration: invalid input extent");
    geistr_decision_config *c = calloc(1, sizeof *c);
    if (!c)
        return error(cap, err, GEISTR_NO_MEMORY, "decision configuration: allocation failed");
    struct input j = {.p = json, .end = json + len};
    unsigned seen = 0;
    bool valid = take(&j, '{');
    while (valid) {
        char key[16];
        if (!text(sizeof key, &j, key) || !take(&j, ':')) {
            valid = false;
            break;
        }
        unsigned bit = 0;
        if (!strcmp(key, "schema")) {
            bit = 1;
            valid = literal(&j, "1");
        } else if (!strcmp(key, "models")) {
            bit = 2;
            valid = policies(&j, c);
        } else
            valid = false;
        if (!valid || (seen & bit)) {
            valid = false;
            break;
        }
        seen |= bit;
        if (take(&j, '}'))
            break;
        valid = take(&j, ',');
    }
    space(&j);
    if (!valid || seen != 3 || j.p != j.end) {
        free(c);
        return error(cap, err, GEISTR_FORMAT, "decision configuration: invalid schema, policy or JSON");
    }
    *out = c;
    return GEISTR_OK;
}

/* Decode complete scalar values: reject overlongs, surrogates and tails.
 * Length checks precede every access (same subtraction rule as AGENT.md). */
static bool utf8(size_t n, const char *s, bool id) {
    if (!s)
        return n == 0;
    size_t i = 0;
    while (i < n) {
        uint32_t cp = (unsigned char)s[i++];
        size_t tail = 0;
        uint32_t minimum = 0;
        if (cp >= 0xc2 && cp <= 0xdf) {
            cp &= 0x1f;
            tail = 1;
            minimum = 0x80;
        } else if (cp >= 0xe0 && cp <= 0xef) {
            cp &= 0x0f;
            tail = 2;
            minimum = 0x800;
        } else if (cp >= 0xf0 && cp <= 0xf4) {
            cp &= 7;
            tail = 3;
            minimum = 0x10000;
        } else if (cp >= 0x80)
            return false;
        if (tail > n - i)
            return false;
        for (size_t k = 0; k < tail; ++k) {
            const unsigned char next = (unsigned char)s[i++];
            if ((next & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp == 0)
            return false;
        if (id && (cp < 32 || (cp >= 0x7f && cp <= 0x9f) || cp == 0x2028 || cp == 0x2029))
            return false;
    }
    return true;
}

static bool extent(size_t n, size_t maximum, size_t *total) {
    if (n > maximum || n > GEISTR_DECISION_MAX_REQUEST - *total)
        return false;
    *total += n;
    return true;
}

geistr_status geistr_decision_request_validate(size_t cap, const geistr_decision_request *r, char *err) {
    if (err && cap)
        err[0] = 0;
    if (!r || r->size != sizeof *r || r->operation != GEISTR_OPERATION_DECISION || !r->question_len ||
        !r->n_options || r->n_options > GEISTR_DECISION_MAX_OPTIONS || !r->options)
        return error(cap, err, GEISTR_INVALID,
                     "decision request: explicit operation and bounded options required");
    size_t total = 0;
    if (!extent(r->question_len, GEISTR_DECISION_MAX_QUESTION, &total) ||
        !extent(r->context_len, GEISTR_DECISION_MAX_CONTEXT, &total) ||
        !utf8(r->question_len, r->question, false) || !utf8(r->context_len, r->context, false))
        return error(cap, err, GEISTR_INVALID, "decision request: invalid question/context length or UTF-8");
    for (size_t i = 0; i < r->n_options; ++i) {
        const geistr_decision_option *o = &r->options[i];
        if (!o->id_len || !o->description_len || !extent(o->id_len, GEISTR_DECISION_MAX_ID, &total) ||
            !extent(o->description_len, GEISTR_DECISION_MAX_DESCRIPTION, &total) ||
            !utf8(o->id_len, o->id, true) || !utf8(o->description_len, o->description, false))
            return error(cap, err, GEISTR_INVALID,
                         "decision request: invalid option ID, description or byte extent");
        for (size_t k = 0; k < i; ++k)
            if (o->id_len == r->options[k].id_len && !memcmp(o->id, r->options[k].id, o->id_len))
                return error(cap, err, GEISTR_INVALID, "decision request: duplicate external ID");
    }
    return GEISTR_OK;
}

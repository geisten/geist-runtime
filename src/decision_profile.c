#include "decision_profile.h"
#include "template.h"
#include <stdlib.h>
#include <stdckdint.h>
#include <string.h>

/* Indexed by geistr_decision_profile. The template hash is of the artifact's
 * embedded chat template, which the native fixtures render independently. */
static const struct decision_profile profiles[] = {
    [GEISTR_DECISION_BONSAI2_27B_PQ2] = {"bonsai2-27b-pq2-v1",
                                         "3907dc1658db1f78a9826bf8d5bcb8dc65db0d466388937af57f2294fae62ec1",
                                         "c3cf9e34abf4f9e36c2d72165aa9c132d3e2a725b6c2586aaa3a8af9d7a81041", "",
                                         "<|im_start|>system\n", "<|im_end|>\n<|im_start|>user\n",
                                         "<|im_end|>\n<|im_start|>assistant\n", "<think>\n\n</think>\n\n"},
    [GEISTR_DECISION_GEMMA4_E2B_Q4] = {"gemma4-e2b-q4-v1",
                                       "740185b21d22ceb83a11c3aa62ad5842ef32c70f6096d756bbee85a1e4ec34b8",
                                       "241c50d86bdfe5e43307da87f559cd2416aacd67a8de46c15acc0105ef2200b7", "<bos>",
                                       "<|turn>system\n", "<turn|>\n<|turn>user\n", "<turn|>\n<|turn>model\n", ""}};

const struct decision_profile *decision_profile_get(geistr_decision_profile p) {
    return p > GEISTR_DECISION_PROFILE_NONE && (size_t)p < sizeof profiles / sizeof profiles[0] ? &profiles[p]
                                                                                                 : nullptr;
}
const char *decision_pre_prompt(void) {
    return "Select exactly one supplied option. Answer only with its key. Do not explain.";
}
const char *decision_template_hash(geistr_decision_profile p) {
    const struct decision_profile *e = decision_profile_get(p);
    return e ? e->template_sha256 : "";
}

static bool controls(size_t n, const char *s) {
    static const char *const reserved[] = {
        "<|",      "|>",       "<turn|>",     "<channel|>",   "<bos>",           "<eos>",
        "<think>", "</think>", "<tool_call>", "</tool_call>", "<tool_response>", "</tool_response>"};
    for (size_t k = 0; k < sizeof reserved / sizeof reserved[0]; ++k) {
        const size_t len = strlen(reserved[k]);
        if (len > n)
            continue;
        for (size_t i = 0; i <= n - len; ++i)
            if (!memcmp(s + i, reserved[k], len))
                return true;
    }
    return false;
}

struct writer {
    size_t cap, used;
    char *out;
    bool valid;
};
static void bytes(size_t n, const char *s, struct writer *w) {
    if (!w->valid || n > w->cap - 1 - w->used) {
        w->valid = false;
        return;
    }
    if (n)
        memcpy(w->out + w->used, s, n);
    w->used += n;
    w->out[w->used] = 0;
}
static void text(const char *s, struct writer *w) { bytes(strlen(s), s, w); }

static void prefix(const struct decision_profile *e, size_t n, const char *context, struct writer *w) {
    text(e->bos, w);
    text(e->system, w);
    text(decision_pre_prompt(), w);
    text(e->user, w);
    if (n) {
        text("Context:\n", w);
        bytes(n, context, w);
        text("\n", w);
    }
}

bool decision_profile_from_family(struct decision_family_profile *out, int family, bool thinks) {
    if (!out)
        return false;
    *out = (struct decision_family_profile){};
    /* Two placeholder messages through the chat renderer; the text around them
     * is the family's markers. */
    const geistr_message msgs[2] = {{"system", "\x01"}, {"user", "\x02"}};
    struct tpl_state state;
    tpl_init(&state, (enum tpl_family)family);
    char *text = tpl_render_send(&state, 2, msgs);
    tpl_free(&state);
    char *a = text ? strchr(text, '\x01') : nullptr, *b = a ? strchr(a, '\x02') : nullptr;
    const char *off = thinks ? tpl_think_off((enum tpl_family)family) : nullptr;
    size_t la = a ? (size_t)(a - text) : 0, lb = b ? (size_t)(b - a - 1) : 0, lc = b ? strlen(b + 1) : 0,
           ld = off ? strlen(off) : 0;
    if (!a || !b || la + lb + lc + ld + 5 > sizeof out->markers) {
        free(text);
        return false;
    }
    const char *parts[4] = {text, a + 1, b + 1, off ? off : ""};
    const size_t lengths[4] = {la, lb, lc, ld};
    const char *copies[4];
    char *m = out->markers;
    for (size_t i = 0; i < 4; ++i) {
        memcpy(m, parts[i], lengths[i]);
        m[lengths[i]] = 0;
        copies[i] = m;
        m += lengths[i] + 1;
    }
    out->profile = (struct decision_profile){.name = "none", .artifact_sha256 = "", .template_sha256 = "", .bos = "",
                                             .system = copies[0], .user = copies[1], .assistant = copies[2],
                                             .thinking_off = copies[3]};
    free(text);
    return true;
}

geistr_status decision_render(size_t *written, size_t cap, geistr_decision_profile p,
                              const geistr_decision_request *r, char *out) {
    return decision_render_with(written, cap, decision_profile_get(p), r, out);
}

geistr_status decision_render_with(size_t *written, size_t cap, const struct decision_profile *e,
                                   const geistr_decision_request *r, char *out) {
    if (written)
        *written = 0;
    if (out && cap)
        out[0] = 0;
    if (!written || !out || !cap || geistr_decision_request_validate(0, r, nullptr) != GEISTR_OK)
        return GEISTR_INVALID;
    if (!e)
        return GEISTR_FORMAT;
    if (controls(r->question_len, r->question) || controls(r->context_len, r->context))
        return GEISTR_INVALID;
    for (size_t i = 0; i < r->n_options; ++i)
        if (controls(r->options[i].id_len, r->options[i].id) ||
            controls(r->options[i].description_len, r->options[i].description))
            return GEISTR_INVALID;
    struct writer w = {.cap = cap, .out = out, .valid = true};
    prefix(e, r->context_len, r->context, &w);
    text("Question:\n", &w);
    bytes(r->question_len, r->question, &w);
    text("\nOptions:\n", &w);
    for (size_t i = 0; i < r->n_options; ++i) {
        const char key = (char)('A' + i);
        bytes(1, &key, &w);
        text(" [", &w);
        bytes(r->options[i].id_len, r->options[i].id, &w);
        text("]: ", &w);
        bytes(r->options[i].description_len, r->options[i].description, &w);
        text("\n", &w);
    }
    text("Choose one key.", &w);
    text(e->assistant, &w);
    text(e->thinking_off, &w);
    if (!w.valid) {
        out[0] = 0;
        return GEISTR_CONTEXT;
    }
    *written = w.used;
    return GEISTR_OK;
}

geistr_status decision_render_prefix(size_t *written, size_t cap, geistr_decision_profile p, size_t n,
                                     const char *context, char *out) {
    if (written)
        *written = 0;
    if (out && cap)
        out[0] = 0;
    /* The context alone, under exactly the request's rules. */
    const geistr_decision_option probe = {1, 1, "A", "A"};
    const geistr_decision_request r = {.size = sizeof r,
                                       .operation = GEISTR_OPERATION_DECISION,
                                       .question_len = 1,
                                       .context_len = n,
                                       .n_options = 1,
                                       .question = "?",
                                       .context = context,
                                       .options = &probe};
    if (!written || !out || !cap || geistr_decision_request_validate(0, &r, nullptr) != GEISTR_OK)
        return GEISTR_INVALID;
    const struct decision_profile *e = decision_profile_get(p);
    if (!e)
        return GEISTR_FORMAT;
    if (controls(n, context))
        return GEISTR_INVALID;
    struct writer w = {.cap = cap, .out = out, .valid = true};
    prefix(e, n, context, &w);
    if (!w.valid) {
        out[0] = 0;
        return GEISTR_CONTEXT;
    }
    *written = w.used;
    return GEISTR_OK;
}

geistr_status decision_map(size_t *n_prompt, size_t cap, size_t n_options, size_t text_cap,
                           decision_tokenize_fn tokenize, char *text, int32_t *prompt, int32_t *scratch,
                           int32_t *candidates, void *context) {
    if (n_prompt)
        *n_prompt = 0;
    if (!cap || !n_prompt || !n_options || n_options > GEISTR_DECISION_MAX_OPTIONS || !text || !text_cap ||
        !tokenize || !prompt || !scratch || !candidates)
        return GEISTR_INVALID;
    size_t candidate_bytes;
    if (ckd_mul(&candidate_bytes, n_options, sizeof *candidates))
        return GEISTR_INVALID;
    memset(candidates, 0, candidate_bytes);
    int32_t keys[GEISTR_DECISION_MAX_OPTIONS] = {};
    const size_t len = strnlen(text, text_cap);
    if (len == text_cap || text_cap - len < 2)
        return GEISTR_CONTEXT;
    size_t base = 0;
    geistr_status status = tokenize(&base, cap, text, prompt, context);
    if (status != GEISTR_OK)
        return status;
    if (!base || base >= cap)
        return GEISTR_CONTEXT;
    size_t prefix_bytes;
    if (ckd_mul(&prefix_bytes, base, sizeof *prompt))
        return GEISTR_INVALID;
    for (size_t i = 0; i < n_options; ++i) {
        text[len] = (char)('A' + i);
        text[len + 1] = 0;
        size_t count = 0;
        status = tokenize(&count, cap, text, scratch, context);
        text[len] = 0;
        if (status != GEISTR_OK)
            return status;
        if (count > cap || count != base + 1 || memcmp(prompt, scratch, prefix_bytes))
            return GEISTR_FORMAT;
        if (scratch[base] < 0)
            return GEISTR_FORMAT;
        keys[i] = scratch[base];
        for (size_t j = 0; j < i; ++j)
            if (keys[j] == keys[i])
                return GEISTR_FORMAT;
    }
    *n_prompt = base;
    memcpy(candidates, keys, candidate_bytes);
    return GEISTR_OK;
}

geistr_status decision_prefix_cut(size_t *n_cached, size_t cap, size_t chunk, decision_tokenize_fn tokenize,
                                  const char *prefix_text, int32_t *ids, void *context) {
    if (n_cached)
        *n_cached = 0;
    if (!n_cached || !cap || !chunk || !tokenize || !prefix_text || !ids)
        return GEISTR_INVALID;
    size_t n = 0;
    const geistr_status status = tokenize(&n, cap, prefix_text, ids, context);
    if (status != GEISTR_OK)
        return status;
    if (n > cap)
        return GEISTR_CONTEXT;
    if (!n)
        return GEISTR_FORMAT;
    *n_cached = (n - 1) / chunk * chunk;
    return GEISTR_OK;
}

geistr_status decision_suffix(size_t *n_suffix, size_t n_cached, const int32_t *cached, size_t n_prompt,
                              const int32_t *prompt) {
    if (n_suffix)
        *n_suffix = 0;
    if (!n_suffix || !prompt || (n_cached && !cached))
        return GEISTR_INVALID;
    /* n_cached < n_prompt: the byte count fits the caller's prompt array. */
    if (n_cached >= n_prompt || (n_cached && memcmp(cached, prompt, n_cached * sizeof *prompt)))
        return GEISTR_FORMAT;
    *n_suffix = n_prompt - n_cached;
    return GEISTR_OK;
}

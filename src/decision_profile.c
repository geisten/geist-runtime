#include "decision_profile.h"
#include <stdckdint.h>
#include <string.h>

const char *decision_pre_prompt(void) {
    return "Select exactly one supplied option. Answer only with its key. Do not explain.";
}
const char *decision_template_hash(geistr_decision_profile p) {
    switch (p) {
    case GEISTR_DECISION_BONSAI2_27B_PQ2:
        return "c3cf9e34abf4f9e36c2d72165aa9c132d3e2a725b6c2586aaa3a8af9d7a81041";
    case GEISTR_DECISION_GEMMA4_E2B_Q4:
        return "241c50d86bdfe5e43307da87f559cd2416aacd67a8de46c15acc0105ef2200b7";
    default:
        return "";
    }
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

geistr_status decision_render(size_t *written, size_t cap, geistr_decision_profile p,
                              const geistr_decision_request *r, char *out) {
    if (written)
        *written = 0;
    if (out && cap)
        out[0] = 0;
    if (!written || !out || !cap || geistr_decision_request_validate(0, r, nullptr) != GEISTR_OK)
        return GEISTR_INVALID;
    if (p != GEISTR_DECISION_BONSAI2_27B_PQ2 && p != GEISTR_DECISION_GEMMA4_E2B_Q4)
        return GEISTR_FORMAT;
    if (controls(r->question_len, r->question) || controls(r->context_len, r->context))
        return GEISTR_INVALID;
    for (size_t i = 0; i < r->n_options; ++i)
        if (controls(r->options[i].id_len, r->options[i].id) ||
            controls(r->options[i].description_len, r->options[i].description))
            return GEISTR_INVALID;
    const bool gemma = p == GEISTR_DECISION_GEMMA4_E2B_Q4;
    struct writer w = {.cap = cap, .out = out, .valid = true};
    text(gemma ? "<bos><|turn>system\n" : "<|im_start|>system\n", &w);
    text(decision_pre_prompt(), &w);
    text(gemma ? "<turn|>\n<|turn>user\n" : "<|im_end|>\n<|im_start|>user\n", &w);
    if (r->context_len) {
        text("Context:\n", &w);
        bytes(r->context_len, r->context, &w);
        text("\n", &w);
    }
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
    text(gemma ? "<turn|>\n<|turn>model\n" : "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n",
         &w);
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

/* template.c — see template.h. Moved from geist-serve src/template.c and
 * src/model.c (stop markers), rendering made incremental (#2). */
#include "template.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Family detection                                                          */
/* ------------------------------------------------------------------------ */

enum tpl_family tpl_family_from_template(const char *tpl) {
    if (tpl == nullptr)
        return TPL_UNKNOWN;
    if (strstr(tpl, "<|turn>"))
        return TPL_GEMMA4;
    if (strstr(tpl, "<start_of_turn>"))
        return TPL_GEMMA3;
    if (strstr(tpl, "<|im_start|>"))
        return TPL_CHATML;
    if (strstr(tpl, "<|start_header_id|>"))
        return TPL_LLAMA3;
    if (strstr(tpl, "BITNETAssistant"))
        return TPL_BITNET;
    return TPL_UNKNOWN;
}

enum tpl_family tpl_family_from_arch(const char *arch) {
    if (arch == nullptr)
        return TPL_UNKNOWN;
    if (strcmp(arch, "gemma4") == 0)
        return TPL_GEMMA4;
    if (strncmp(arch, "gemma", 5) == 0)
        return TPL_GEMMA3;
    if (strncmp(arch, "qwen", 4) == 0)
        return TPL_CHATML;
    if (strcmp(arch, "llama") == 0)
        return TPL_LLAMA3;
    if (strncmp(arch, "bitnet", 6) == 0)
        return TPL_BITNET;
    return TPL_UNKNOWN;
}

enum tpl_family tpl_family_detect(const char *tpl, const char *arch) {
    enum tpl_family f = tpl_family_from_template(tpl);
    return f != TPL_UNKNOWN ? f : tpl_family_from_arch(arch);
}

const char *tpl_family_name(enum tpl_family f) {
    switch (f) {
    case TPL_GEMMA3:
        return "gemma3";
    case TPL_GEMMA4:
        return "gemma4";
    case TPL_CHATML:
        return "chatml";
    case TPL_LLAMA3:
        return "llama3";
    case TPL_BITNET:
        return "bitnet";
    default:
        return "unknown";
    }
}

enum tpl_family tpl_family_from_name(const char *name) {
    for (enum tpl_family f = TPL_GEMMA3; name && f <= TPL_BITNET; f++)
        if (strcmp(name, tpl_family_name(f)) == 0)
            return f;
    return TPL_UNKNOWN;
}

/* ------------------------------------------------------------------------ */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------ */

void tpl_init(struct tpl_state *st, enum tpl_family family) {
    *st = (struct tpl_state) {.family = family};
}

void tpl_free(struct tpl_state *st) {
    free(st->folded);
    st->folded = nullptr;
}

struct out {
    char  *p;
    size_t len, cap;
    bool   oom;
};

static void put(struct out *o, const char *s) {
    size_t n = strlen(s);
    if (o->oom)
        return;
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 1024;
        while (cap < o->len + n + 1)
            cap *= 2;
        char *np = realloc(o->p, cap);
        if (np == nullptr) {
            o->oom = true;
            return;
        }
        o->p   = np;
        o->cap = cap;
    }
    memcpy(o->p + o->len, s, n + 1);
    o->len += n;
}

static bool is_role(const geistr_message *m, const char *role) {
    return m->role != nullptr && strcmp(m->role, role) == 0;
}

/* Turn-marker families share one shape: open(role) content close. The
 * system message is a turn of its own where the model has a system role,
 * and is folded into the first user turn (Gemma 3) where it has none. */
struct turn_fmt {
    const char *open, *role_end, *close, *sys_role, *user_role, *asst_role;
};

static const struct turn_fmt FMT_GEMMA3 = {"<start_of_turn>", "\n", "<end_of_turn>\n", nullptr, "user", "model"};
static const struct turn_fmt FMT_GEMMA4 = {"<|turn>", "\n", "<turn|>\n", "system", "user", "model"};
static const struct turn_fmt FMT_CHATML = {"<|im_start|>", "\n", "<|im_end|>\n", "system", "user", "assistant"};
static const struct turn_fmt FMT_LLAMA3 = {
        "<|start_header_id|>", "<|end_header_id|>\n\n", "<|eot_id|>", "system", "user", "assistant"};

static const struct turn_fmt *turn_fmt(enum tpl_family f) {
    switch (f) {
    case TPL_GEMMA3:
        return &FMT_GEMMA3;
    case TPL_GEMMA4:
        return &FMT_GEMMA4;
    case TPL_CHATML:
        return &FMT_CHATML;
    case TPL_LLAMA3:
        return &FMT_LLAMA3;
    default:
        return nullptr;
    }
}

/* *folded: the pending Gemma 3 system text on entry, what is still pending on return. */
static void render_turns(struct out            *o,
                         const struct turn_fmt *f,
                         bool                   started,
                         const char           **folded,
                         size_t                 n,
                         const geistr_message   msgs[]) {
    size_t i = 0;
    if (!started && n > 0 && is_role(&msgs[0], "system")) {
        if (f->sys_role != nullptr) {
            put(o, f->open);
            put(o, f->sys_role);
            put(o, f->role_end);
            put(o, msgs[0].content);
            put(o, f->close);
        } else {
            *folded = msgs[0].content;
        }
        i = 1;
    }
    for (; i < n; i++) {
        bool asst = is_role(&msgs[i], "assistant");
        put(o, f->open);
        put(o, asst ? f->asst_role : f->user_role);
        put(o, f->role_end);
        if (*folded != nullptr && !asst) {
            put(o, *folded);
            put(o, "\n\n");
            *folded = nullptr;
        }
        put(o, msgs[i].content);
        put(o, f->close);
    }
    put(o, f->open);
    put(o, f->asst_role);
    put(o, f->role_end);
}

/* BitNet b1.58 2B-4T (Llama-3 vocab): the turn format from the model card,
 * "System: …<|eot_id|>User: …<|eot_id|>Assistant: ". The GGUF's own
 * "Human: … BITNETAssistant: " template never ends the turn, and Llama-3
 * headers make the model end it at the first line break (geist-serve #106). */
static void render_bitnet(struct out *o, size_t n, const geistr_message msgs[]) {
    for (size_t i = 0; i < n; i++) {
        put(o, is_role(&msgs[i], "system") ? "System: " : is_role(&msgs[i], "assistant") ? "Assistant: " : "User: ");
        put(o, msgs[i].content);
        put(o, "<|eot_id|>");
    }
    put(o, "Assistant: ");
}

char *tpl_render_send(struct tpl_state *st, size_t n, const geistr_message msgs[]) {
    struct out             o      = {};
    const struct turn_fmt *f      = turn_fmt(st->family);
    const char            *folded = st->folded;
    if (st->family == TPL_BITNET)
        render_bitnet(&o, n, msgs);
    else if (f)
        render_turns(&o, f, st->started, &folded, n, msgs);
    else
        return nullptr;
    /* Commit the state only once the text exists: a failed send changes nothing. */
    char *keep = nullptr;
    if (!o.oom && folded != nullptr && folded != st->folded && !(keep = strdup(folded)))
        o.oom = true;
    if (o.oom) {
        free(o.p);
        return nullptr;
    }
    if (folded != st->folded) {
        free(st->folded);
        st->folded = keep;
    }
    st->started = true;
    return o.p;
}

const char *tpl_answer_close(enum tpl_family family, const char *end_marker) {
    const struct turn_fmt *f     = turn_fmt(family);
    const char            *close = family == TPL_BITNET ? "<|eot_id|>" : f ? f->close : "";
    size_t                 n     = end_marker ? strlen(end_marker) : 0;
    return n && strncmp(close, end_marker, n) == 0 ? close + n : close;
}

/* ------------------------------------------------------------------------ */
/* Stop tokens                                                               */
/* ------------------------------------------------------------------------ */

const char *const tpl_end_markers[] = {
        "<end_of_turn>", "<turn|>", "<|im_end|>", "<|eot_id|>", "<|end_of_text|>", nullptr};

static size_t add_id(int32_t id, size_t n, size_t cap, int32_t ids[]) {
    if (id < 0 || n >= cap)
        return n;
    for (size_t i = 0; i < n; i++)
        if (ids[i] == id)
            return n;
    ids[n] = id;
    return n + 1;
}

size_t tpl_stop_ids(tpl_lookup_fn lookup, void *context, int32_t eos, size_t cap, int32_t ids[]) {
    size_t n = add_id(eos, 0, cap, ids);
    for (const char *const *m = tpl_end_markers; *m != nullptr; m++)
        n = add_id(lookup(context, *m), n, cap, ids);
    return n;
}

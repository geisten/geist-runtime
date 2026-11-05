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

void tpl_init_extra(struct tpl_state *st, enum tpl_family family, const struct tpl_extra *extra) {
    *st = (struct tpl_state) {.family = family, .extra = extra ? *extra : (struct tpl_extra) {}};
}

void tpl_free(struct tpl_state *st) {
    free(st->folded);
    st->folded = nullptr;
}

struct out {
    char             *p;
    size_t            len, cap;
    bool              oom;
    struct tpl_spans *spans; /* nullptr: not wanted */
    struct tpl_extra  extra;
};

static void put_n(struct out *o, const char *s, size_t n) {
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
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = 0;
}

static void put(struct out *o, const char *s) {
    put_n(o, s, strlen(s));
}

/* A message body s[0, n): put, and its range into o->spans. */
static void put_body_n(struct out *o, const char *s, size_t n) {
    const size_t at = o->len;
    put_n(o, s, n);
    if (o->oom || o->spans == nullptr || o->len == at)
        return;
    struct tpl_spans *sp = o->spans;
    if (sp->n == sp->cap) {
        size_t           cap  = sp->cap ? sp->cap * 2 : 16;
        struct tpl_span *grow = realloc(sp->v, cap * sizeof *grow);
        if (grow == nullptr) {
            o->oom = true;
            return;
        }
        sp->v   = grow;
        sp->cap = cap;
    }
    sp->v[sp->n++] = (struct tpl_span) {at, o->len};
}

/* ---- Jinja's trim: Python's str.strip(), on UTF-8 (#269) ------------------ */

/* Whether code point u is whitespace to str.isspace(). */
static bool py_space(uint32_t u) {
    return (u >= 0x09 && u <= 0x0d) || (u >= 0x1c && u <= 0x20) || u == 0x85 || u == 0xa0 || u == 0x1680 ||
           (u >= 0x2000 && u <= 0x200a) || u == 0x2028 || u == 0x2029 || u == 0x202f || u == 0x205f || u == 0x3000;
}

/* The bytes of a whitespace code point starting at s[0, n), else 0. */
static size_t space_at(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *) s;
    if (n >= 1 && p[0] < 0x80)
        return py_space(p[0]) ? 1 : 0;
    if (n >= 2 && (p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80)
        return py_space((uint32_t) (p[0] & 0x1f) << 6 | (p[1] & 0x3f)) ? 2 : 0;
    if (n >= 3 && (p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80)
        return py_space((uint32_t) (p[0] & 0x0f) << 12 | (uint32_t) (p[1] & 0x3f) << 6 | (p[2] & 0x3f)) ? 3 : 0;
    return 0; /* no whitespace beyond U+FFFF */
}

/* The bytes of a whitespace code point ending at s[n), else 0. */
static size_t space_before(const char *s, size_t n) {
    for (size_t k = 1; k <= 3 && k <= n; k++)
        if (space_at(s + n - k, k) == k)
            return k;
    return 0;
}

/* s[*b, *e) without leading and trailing whitespace. */
static void trim_range(const char *s, size_t *b, size_t *e) {
    for (size_t k; *b < *e && (k = space_at(s + *b, *e - *b));)
        *b += k;
    for (size_t k; *e > *b && (k = space_before(s + *b, *e - *b));)
        *e -= k;
}

/* A body, trimmed if the style says so. */
static void put_body(struct out *o, const char *s) {
    size_t b = 0, e = strlen(s);
    if (o->extra.style.trim)
        trim_range(s, &b, &e);
    put_body_n(o, s + b, e - b);
}

/* What put_body would write is empty. */
static bool blank(const struct out *o, const char *s) {
    size_t b = 0, e = strlen(s);
    if (o->extra.style.trim)
        trim_range(s, &b, &e);
    return b == e;
}

void tpl_spans_free(struct tpl_spans *sp) {
    free(sp->v);
    *sp = (struct tpl_spans) {};
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

/* Where s[b, e) holds needle, from the left (last false) or the right; e if nowhere. */
static size_t find(const char *s, size_t b, size_t e, const char *needle, bool last) {
    const size_t k = strlen(needle);
    size_t       at = e;
    for (size_t i = b; i + k <= e; i++)
        if (!memcmp(s + i, needle, k)) {
            at = i;
            if (!last)
                break;
        }
    return at;
}

/* An assistant message as Qwen3.5's template renders it (#269):
 *   content = content|trim
 *   if reasoning_content is string: reasoning = reasoning_content (#282)
 *   elif '</think>' in content:
 *       reasoning = content.split('</think>')[0].rstrip('\n').split('<think>')[-1].lstrip('\n')
 *       content = content.split('</think>')[-1].lstrip('\n')
 *   in loop: '<think>\n' + reasoning|trim + '\n</think>\n\n' + content; else content
 * and as Qwen3's does (#276): untrimmed, reasoning.strip('\n') (the split
 * strips the line breaks already), and in the loop the block only where
 * there is reasoning (or for the last message, which a send never is);
 * there content.lstrip('\n'), which the split did already. reasoning: the
 * message's reasoning_content, nullptr for none. */
static void put_split_answer(struct out *o, const char *s, const char *reasoning, bool in_loop) {
    const struct tpl_style *y = &o->extra.style;
    size_t                  b = 0, e = strlen(s), rb = 0, re = 0;
    const char             *r = reasoning ? reasoning : s; /* the reasoning is r[rb, re) */
    bool                    has;                           /* Qwen3: the reasoning is not empty */
    if (y->trim)
        trim_range(s, &b, &e);
    if (reasoning) { /* given: the content is not split */
        re  = strlen(reasoning);
        has = re > 0;
        if (y->lean) {
            while (rb < re && r[rb] == '\n')
                rb++;
            while (re > rb && r[re - 1] == '\n')
                re--;
            while (in_loop && has && b < e && s[b] == '\n')
                b++;
        } else
            trim_range(r, &rb, &re);
    } else {
        const size_t close = find(s, b, e, "</think>", false);
        if (close < e) {
            re = close;
            while (re > b && s[re - 1] == '\n')
                re--;
            const size_t open = find(s, b, re, "<think>", true);
            rb                = open < re ? open + strlen("<think>") : b;
            while (rb < re && s[rb] == '\n')
                rb++;
            if (y->trim)
                trim_range(s, &rb, &re);
            b = find(s, b, e, "</think>", true) + strlen("</think>");
            while (b < e && s[b] == '\n')
                b++;
        }
        has = re > rb;
    }
    if (in_loop && (!y->lean || has)) {
        put(o, "<think>\n");
        put_body_n(o, r + rb, re - rb);
        put(o, "\n</think>\n\n");
    }
    put_body_n(o, s + b, e - b);
}

/* An assistant message's text after its role line, before the close.
 * Qwen3.6+ (history, preserve_thinking): '<think>\n' + reasoning_content|trim
 * + '\n</think>\n\n' + content, the empty block without reasoning (#282). */
static void put_answer(struct out *o, const char *content, const char *reasoning, bool in_loop) {
    if (o->extra.style.loop) {
        put_split_answer(o, content, reasoning, in_loop);
        return;
    }
    if (reasoning && o->extra.history && *o->extra.history) {
        size_t rb = 0, re = strlen(reasoning);
        trim_range(reasoning, &rb, &re);
        put(o, "<think>\n");
        put_body_n(o, reasoning + rb, re - rb);
        put(o, "\n</think>\n\n");
    } else if (o->extra.history)
        put(o, o->extra.history);
    put_body(o, content);
}

bool tpl_is_query(const struct tpl_style *y, const geistr_message *m) {
    if (is_role(m, "system") || is_role(m, "assistant") || is_role(m, "tool"))
        return false;
    static const char head[] = "<tool_response>", tail[] = "</tool_response>";
    if (y->lean) /* Qwen3: not('<tool_response>' in content and '</tool_response>' in content) */
        return !(strstr(m->content, head) && strstr(m->content, tail));
    size_t b = 0, e = strlen(m->content);
    trim_range(m->content, &b, &e);
    const size_t h = sizeof head - 1, t = sizeof tail - 1;
    return !(e - b >= h && !memcmp(m->content + b, head, h) && e - b >= t && !memcmp(m->content + e - t, tail, t));
}

bool tpl_has_query(const struct tpl_style *y, size_t n, const geistr_message msgs[]) {
    for (size_t i = 0; i < n; i++)
        if (tpl_is_query(y, &msgs[i]))
            return true;
    return false;
}

/* *folded: the pending Gemma 3 system text on entry, what is still pending on return. */
static void render_turns(struct out            *o,
                         const struct turn_fmt *f,
                         bool                   started,
                         const char           **folded,
                         size_t                 n,
                         const geistr_message   msgs[],
                         const char *const      reasoning[]) {
    size_t                  i      = 0;
    const char             *lead   = f->sys_role ? o->extra.lead : nullptr;
    const struct tpl_style *y      = &o->extra.style;
    const bool              system = !started && n > 0 && is_role(&msgs[0], "system");
    if (!started && lead && *lead) { /* a thinking chat's system turn (#266) */
        put(o, f->open);
        put(o, f->sys_role);
        put(o, f->role_end);
        put(o, lead);
        if (system && !blank(o, msgs[0].content)) {
            put(o, o->extra.join ? o->extra.join : "");
            put_body(o, msgs[0].content);
        }
        put(o, f->close);
        i = system ? 1 : 0;
    } else if (system) {
        if (f->sys_role == nullptr) {
            *folded = msgs[0].content;
        } else if (!(y->bare && blank(o, msgs[0].content))) {
            put(o, f->open);
            put(o, f->sys_role);
            put(o, f->role_end);
            put_body(o, msgs[0].content);
            put(o, f->close);
        }
        i = 1;
    }
    /* The tool loop (#269, #276): the assistant messages after the last user query. */
    size_t query = SIZE_MAX;
    for (size_t k = i; y->loop && k < n; k++)
        if (tpl_is_query(y, &msgs[k]))
            query = k;
    for (; i < n; i++) {
        const bool asst = is_role(&msgs[i], "assistant");
        if (y->tools && is_role(&msgs[i], "tool")) { /* consecutive results share a user turn */
            if (i == 0 || !is_role(&msgs[i - 1], "tool")) {
                put(o, f->open);
                put(o, f->user_role);
            }
            put(o, "\n<tool_response>\n");
            put_body(o, msgs[i].content);
            put(o, "\n</tool_response>");
            if (i + 1 == n || !is_role(&msgs[i + 1], "tool"))
                put(o, f->close);
            continue;
        }
        put(o, f->open);
        put(o, asst ? f->asst_role : f->user_role);
        put(o, f->role_end);
        if (*folded != nullptr && !asst) {
            put_body(o, *folded);
            put(o, "\n\n");
            *folded = nullptr;
        }
        if (asst)
            put_answer(o, msgs[i].content, reasoning ? reasoning[i] : nullptr, query == SIZE_MAX || i > query);
        else
            put_body(o, msgs[i].content);
        put(o, f->close);
    }
    put(o, f->open);
    put(o, f->asst_role);
    put(o, f->role_end);
}

char *tpl_render_answer(const struct tpl_extra *extra, const char *content, const char *reasoning, bool in_loop) {
    struct out o = {.extra = *extra};
    put(&o, "");
    put_answer(&o, content, reasoning, in_loop);
    if (o.oom) {
        free(o.p);
        return nullptr;
    }
    return o.p;
}

/* BitNet b1.58 2B-4T (Llama-3 vocab): the turn format from the model card,
 * "System: …<|eot_id|>User: …<|eot_id|>Assistant: ". The GGUF's own
 * "Human: … BITNETAssistant: " template never ends the turn, and Llama-3
 * headers make the model end it at the first line break (geist-serve #106). */
static void render_bitnet(struct out *o, size_t n, const geistr_message msgs[]) {
    for (size_t i = 0; i < n; i++) {
        put(o, is_role(&msgs[i], "system") ? "System: " : is_role(&msgs[i], "assistant") ? "Assistant: " : "User: ");
        put_body(o, msgs[i].content);
        put(o, "<|eot_id|>");
    }
    put(o, "Assistant: ");
}

char *tpl_render_send(struct tpl_state *st, size_t n, const geistr_message msgs[]) {
    return tpl_render_send_spans(st, n, msgs, nullptr, nullptr);
}

char *tpl_render_send_spans(struct tpl_state    *st,
                            size_t               n,
                            const geistr_message msgs[],
                            const char *const    reasoning[],
                            struct tpl_spans    *spans) {
    struct out             o      = {.spans = spans, .extra = st->extra};
    const size_t           n0     = spans ? spans->n : 0;
    const struct turn_fmt *f      = turn_fmt(st->family);
    const char            *folded = st->folded;
    if (st->family == TPL_BITNET)
        render_bitnet(&o, n, msgs);
    else if (f)
        render_turns(&o, f, st->started, &folded, n, msgs, reasoning);
    else
        return nullptr;
    /* Commit the state only once the text exists: a failed send changes nothing. */
    char *keep = nullptr;
    if (!o.oom && folded != nullptr && folded != st->folded && !(keep = strdup(folded)))
        o.oom = true;
    if (o.oom) {
        free(o.p);
        if (spans)
            spans->n = n0;
        return nullptr;
    }
    if (folded != st->folded) {
        free(st->folded);
        st->folded = keep;
    }
    st->started = true;
    return o.p;
}

const char *tpl_generation_prompt(enum tpl_family family) {
    /* open + assistant role + role_end, as render_turns ends; BitNet's own. */
    static const char *const prompts[] = {
            [TPL_UNKNOWN] = "",
            [TPL_GEMMA3]  = "<start_of_turn>model\n",
            [TPL_GEMMA4]  = "<|turn>model\n",
            [TPL_CHATML]  = "<|im_start|>assistant\n",
            [TPL_LLAMA3]  = "<|start_header_id|>assistant<|end_header_id|>\n\n",
            [TPL_BITNET]  = "Assistant: ",
    };
    return family >= TPL_UNKNOWN && family <= TPL_BITNET ? prompts[family] : "";
}

static const char THINK_EMPTY[] = "<think>\n\n</think>\n\n";

const char *tpl_think_off(enum tpl_family family) {
    return family == TPL_CHATML ? THINK_EMPTY : nullptr;
}

/* Fingerprints are the templates' own Jinja conditions, as the GGUFs carry them:
 *   Qwen3:    enable_thinking false → the empty block; nothing when thinking
 *   Qwen3.5:  "enable_thinking is defined and enable_thinking is true" → "<think>\n",
 *             else the empty block: it thinks only when asked
 *   Qwen3.6+: as Qwen3.5 but thinking by default, reasoning_effort's instruction
 *             in the system turn, preserve_thinking (Qwen3.8, Bonsai 2)
 *   Gemma 4:  enable_thinking (default false) puts <|think|> into the system
 *             turn; the reasoning comes as <|channel>thought … <channel|> */
struct tpl_thinking tpl_thinking_of(enum tpl_family family, const char *tpl) {
    struct tpl_thinking t = {.on = "", .history = "", .open = "<think>", .close = "</think>"};
    if (tpl == nullptr)
        return t;
    if (family == TPL_GEMMA4 && strstr(tpl, "<|think|>") && strstr(tpl, "enable_thinking"))
        return (struct tpl_thinking) {.thinks    = true,
                                      .on        = "",
                                      .off       = "",
                                      .history   = "",
                                      .system_on = "<|think|>\n",
                                      .open      = "<|channel>thought",
                                      .close     = "<channel|>"};
    if (family != TPL_CHATML || !strstr(tpl, "<think>"))
        return t;
    const bool sw = strstr(tpl, "enable_thinking") != nullptr;
    t.thinks      = true;
    t.off         = sw ? THINK_EMPTY : nullptr;
    t.on          = strstr(tpl, "{{- '<think>\\n' }}") ? "<think>\n" : "";
    t.default_on  = !(sw && strstr(tpl, "enable_thinking is defined and enable_thinking is true"));
    t.effort      = sw && strstr(tpl, "reasoning_effort") && strstr(tpl, "Reasoning effort is set to");
    t.history     = strstr(tpl, "preserve_thinking is undefined or preserve_thinking is true") ? THINK_EMPTY : "";
    return t;
}

/* The templates' own Jinja (#269), as the GGUFs carry them:
 *   trim:  Qwen3.5+ "render_content(message.content, true)|trim",
 *          Gemma 4 "message['content'] | trim"
 *   tools: Qwen3.x "'\n<tool_response>\n'" in the tool branch
 *   bare:  Qwen3.6+ "{%- elif reasoning_instructions %}": no turn for an empty system message
 *   loop:  Qwen3.5 "'\n<think>\n' + reasoning_content + '\n</think>\n\n' + content" after
 *          "loop.index0 > ns.last_query_index", with the split at </think> and
 *          without preserve_thinking (Qwen3.6+ keeps the block everywhere: history);
 *          Qwen3 (lean, #276) "'\n<think>\n' + reasoning_content.strip('\n') +
 *          '\n</think>\n\n' + content.lstrip('\n')" there, under
 *          "loop.last or (not loop.last and reasoning_content)" */
struct tpl_style tpl_style_of(enum tpl_family family, const char *tpl) {
    if (tpl == nullptr)
        return (struct tpl_style) {};
    const bool chatml = family == TPL_CHATML;
    const bool loop   = chatml && strstr(tpl, "loop.index0 > ns.last_query_index") &&
                      strstr(tpl, "content.split('</think>')") && !strstr(tpl, "preserve_thinking");
    const bool lean   = loop && strstr(tpl, "'\\n<think>\\n' + reasoning_content.strip('\\n') + "
                                            "'\\n</think>\\n\\n' + content.lstrip('\\n')") &&
                      strstr(tpl, "loop.last or (not loop.last and reasoning_content)");
    return (struct tpl_style) {
            .trim  = (chatml && strstr(tpl, "render_content(message.content, true)|trim")) ||
                     (family == TPL_GEMMA4 && strstr(tpl, "message['content'] | trim")),
            .tools = chatml && strstr(tpl, "message.role == \"tool\"") && strstr(tpl, "'\\n<tool_response>\\n'"),
            .bare  = chatml && strstr(tpl, "{%- elif reasoning_instructions %}"),
            .loop  = lean ||
                    (loop && strstr(tpl, "'\\n<think>\\n' + reasoning_content + '\\n</think>\\n\\n' + content")),
            .lean  = lean,
    };
}

struct tpl_extra tpl_thinking_extra(const struct tpl_thinking *t, bool on, geistr_effort effort) {
    const char *lead = !on ? nullptr : t->system_on ? t->system_on : t->effort ? tpl_effort_text(effort) : nullptr;
    return (struct tpl_extra) {.lead = lead, .join = t->system_on ? "" : "\n\n", .history = t->history};
}

const char *tpl_effort_text(geistr_effort effort) {
    switch (effort) {
    case GEISTR_EFFORT_LOW:
        return "Reasoning effort is set to low. Keep your thinking brief and focused, moving directly to the "
               "conclusion without unnecessary elaboration.";
    case GEISTR_EFFORT_MEDIUM:
        return nullptr;
    default:
        return "Reasoning effort is set to xhigh. Please think carefully through the task, validate key "
               "assumptions, consider plausible alternatives, and prioritize correctness, consistency, and "
               "clarity in the final answer.";
    }
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

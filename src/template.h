/*
 * template.h — chat messages → prompt text, per model family (#2). Internal.
 *
 * Moved from geist-serve (src/template.c, src/model.c) and made incremental
 * for the stateful chat (docs/API.md D2): a send renders only its new
 * messages, and after an answer the turn is closed. The concatenation of
 *
 *     render_send(first messages) + answer + answer_close + render_send(next) …
 *
 * is byte-identical to geist-serve's render of the whole conversation
 * (tests/test_template.c, `make parity`).
 *
 * The family is fingerprinted from the GGUF's tokenizer.chat_template
 * (markers, not Jinja), with general.architecture as the fallback. A
 * template we do not recognise is TPL_UNKNOWN (GEISTR_FORMAT).
 */
#pragma once
#include "geistr.h"

#include <stddef.h>
#include <stdint.h>

enum tpl_family {
    TPL_UNKNOWN = 0,
    TPL_GEMMA3, /* <start_of_turn>user … <end_of_turn>; no system role */
    TPL_GEMMA4, /* <|turn>user … <turn|> */
    TPL_CHATML, /* <|im_start|>user … <|im_end|>  (Qwen, SmolLM2) */
    TPL_LLAMA3, /* <|start_header_id|>user<|end_header_id|> … <|eot_id|> */
    TPL_BITNET, /* User: … <|eot_id|> Assistant: (model card, geist-serve #106) */
};

enum tpl_family tpl_family_from_template(const char *tpl); /* nullptr ok */
enum tpl_family tpl_family_from_arch(const char *arch);    /* nullptr ok */
/* The template's markers first (SmolLM2 is arch "llama" but speaks ChatML), then the arch. */
enum tpl_family tpl_family_detect(const char *tpl, const char *arch);
const char     *tpl_family_name(enum tpl_family family); /* "chatml", …, "unknown" */
enum tpl_family tpl_family_from_name(const char *name);  /* the inverse; TPL_UNKNOWN otherwise */

/* What a chat template does with thinking (#266), read from its text
 * (tokenizer.chat_template). The strings are static template text. */
struct tpl_thinking {
    bool        thinks;     /* the template knows reasoning: Qwen3.x <think>, Gemma 4 <|think|> */
    bool        default_on; /* without enable_thinking it thinks: Qwen3, Qwen3.6+; not Qwen3.5, Gemma 4 */
    bool        effort;     /* reasoning_effort: an instruction leads the system turn (Qwen3.6+) */
    const char *on;         /* after the generation prompt, thinking: "" or "<think>\n" (Qwen3.5+) */
    const char *off;        /* after it, not thinking: the empty block; nullptr: no switch */
    const char *history;    /* before an earlier answer: Qwen3.6+'s preserve_thinking keeps an
                               empty block there (the runtime keeps no reasoning); else "" */
    const char *system_on;  /* Gemma 4: opens the system turn while thinking; else nullptr */
    const char *open, *close; /* the reasoning's markers in the output */
};

/* family, and the template's text (nullptr: no thinking). Recognised by the
 * Jinja conditions of Qwen3, Qwen3.5, Qwen3.6+ (Qwen3.8, Bonsai 2) and Gemma 4;
 * any other ChatML template with <think> thinks by default, without a switch. */
struct tpl_thinking tpl_thinking_of(enum tpl_family family, const char *tpl);

/* Qwen3.6+'s system instruction for an effort; nullptr for MEDIUM (none). */
const char *tpl_effort_text(geistr_effort effort);

/* How a chat template renders the messages themselves (#269), read from its
 * text by the templates' own Jinja (tpl_style_of):
 *   trim:  message contents are |trim'med, as Python's str.strip() does
 *          (Qwen3.5, Qwen3.6+, Gemma 4)
 *   tools: a "tool" message is a <tool_response> block in a user turn,
 *          consecutive ones share it (Qwen3, Qwen3.5, Qwen3.6+)
 *   bare:  an empty system message renders no system turn (Qwen3.6+)
 *   loop:  the tool loop of Qwen3.5 and Qwen3 (#276): an assistant message
 *          is split at </think> into reasoning and answer; after the last
 *          user query (a user message that is not a <tool_response>) it
 *          renders as "<think>\n" reasoning "\n</think>\n\n" answer,
 *          before it the answer alone
 *   lean:  Qwen3's loop (with loop): an answer without reasoning renders as
 *          the answer alone in the loop too, the reasoning loses only its
 *          line breaks at the ends, and a user message is a query unless it
 *          contains both <tool_response> and </tool_response> (Qwen3.5: it
 *          is one block, trimmed). The template also renders the block for
 *          an assistant message that ends the conversation; a send never
 *          ends with one. */
struct tpl_style {
    bool trim, tools, bare, loop, lean;
};

struct tpl_style tpl_style_of(enum tpl_family family, const char *tpl); /* tpl nullptr: none */

/* A user query in loop's sense: not a system, assistant or tool message, and
 * its trimmed content not a <tool_response> block (style lean: its content
 * not holding both <tool_response> and </tool_response>). */
bool tpl_is_query(const struct tpl_style *style, const geistr_message *m);
bool tpl_has_query(const struct tpl_style *style, size_t n, const geistr_message msgs[]); /* one of them is */

/* Template text a thinking chat adds to the turns (#266): lead opens the
 * system turn (a system turn of its own without a system message), joined to
 * a system message by join; history precedes every assistant message. style:
 * how messages render (#269). */
struct tpl_extra {
    const char      *lead, *join, *history; /* static; nullptr or "": none */
    struct tpl_style style;
};

/* What an assistant message with content renders as between its turn's
 * opening and close: after the last user query (in_loop) or before it. A
 * malloc'd string, nullptr when out of memory. The chat compares it with what
 * the context holds after an answer (#266, #269). */
char *tpl_render_answer(const struct tpl_extra *extra, const char *content, bool in_loop);

/* A chat's extra text with thinking on or off (enable_thinking), at effort.
 * The style is left empty: the chat sets it (tpl_style_of). */
struct tpl_extra tpl_thinking_extra(const struct tpl_thinking *thinking, bool on, geistr_effort effort);

/* What a conversation's rendering depends on beyond the current send. */
struct tpl_state {
    enum tpl_family  family;
    bool             started; /* a send was rendered: a system message no longer leads */
    char            *folded;  /* Gemma 3: system text awaiting the first user turn (owned) */
    struct tpl_extra extra;
};

void tpl_init(struct tpl_state *state, enum tpl_family family);
/* tpl_init with a thinking chat's extra text (extra nullptr: none). */
void tpl_init_extra(struct tpl_state *state, enum tpl_family family, const struct tpl_extra *extra);
void tpl_free(struct tpl_state *state);

/* Render msgs[0..n) as turns, then the generation prompt. A leading system
 * message of the conversation is rendered per family; later system messages
 * count as user text (BitNet writes every one as "System: "); unknown roles
 * count as user, and so do tool messages unless the style has tools. The
 * state's extra.style applies (#269); an assistant message is in loop when no
 * user query follows it in this send. Returns a malloc'd
 * string, or nullptr for TPL_UNKNOWN or out of memory (state unchanged).
 * After a rewind, the caller rebuilds the state by rendering the kept
 * messages again into a fresh state and discarding the text. */
char *tpl_render_send(struct tpl_state *state, size_t n, const geistr_message msgs[]);

/* A byte range [begin, end) of a rendered text. */
struct tpl_span {
    size_t begin, end;
};
struct tpl_spans {
    struct tpl_span *v;
    size_t           n, cap;
};
void tpl_spans_free(struct tpl_spans *spans);

/* tpl_render_send, and where the message bodies landed (#261): the range of
 * every non-empty content it writes (a system message, a user, assistant or
 * tool message, Gemma 3's folded system text) is appended to *spans, in text
 * order. Everything outside them is the template's own: turn markers, role
 * names, separators. The chat tokenizes the bodies with special tokens off,
 * so a message cannot spell a role marker. On failure (nullptr) *spans is
 * unchanged. spans nullptr: tpl_render_send. */
char *tpl_render_send_spans(struct tpl_state *state, size_t n, const geistr_message msgs[], struct tpl_spans *spans);

/* The generation prompt alone (opens the assistant turn): the suffix of
 * every tpl_render_send. The chat tokenizes it apart from the turns before
 * it, so a rewind can cut the context in front of it (#4). "" for
 * TPL_UNKNOWN. */
const char *tpl_generation_prompt(enum tpl_family family);

/* What follows the generation prompt for a thinking model told not to
 * think (GEISTR_REASONING_THINK_OFF): Qwen3's template with
 * enable_thinking=false. nullptr where the family has no such switch.
 * The decision profiles' (one per family); a chat uses tpl_thinking_of. */
const char *tpl_think_off(enum tpl_family family);

/* Text that closes an answer turn, before the next send. If the model ended
 * the answer with an end marker of its own (end_marker, its text), only the
 * rest of the close is returned; otherwise (length, stop string, cancel, an
 * unrelated end token) the whole close. Never nullptr. */
const char *tpl_answer_close(enum tpl_family family, const char *end_marker);

/* End-of-turn markers of all supported families. Some GGUFs mark them as
 * EOS and some do not (Gemma), so all of them stop generation. */
extern const char *const tpl_end_markers[]; /* nullptr-terminated */

/* Token ids that end an answer: eos (if >= 0) plus every marker the
 * vocabulary knows (lookup returns < 0 for unknown text). Returns the count
 * written to ids[0..cap). */
typedef int32_t (*tpl_lookup_fn)(void *context, const char *text);
size_t tpl_stop_ids(tpl_lookup_fn lookup, void *context, int32_t eos, size_t cap, int32_t ids[]);

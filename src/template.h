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

/* What a conversation's rendering depends on beyond the current send. */
struct tpl_state {
    enum tpl_family family;
    bool            started; /* a send was rendered: a system message no longer leads */
    char           *folded;  /* Gemma 3: system text awaiting the first user turn (owned) */
};

void tpl_init(struct tpl_state *state, enum tpl_family family);
void tpl_free(struct tpl_state *state);

/* Render msgs[0..n) as turns, then the generation prompt. A leading system
 * message of the conversation is rendered per family; later system messages
 * count as user text (BitNet writes every one as "System: "); unknown roles
 * count as user. Returns a malloc'd
 * string, or nullptr for TPL_UNKNOWN or out of memory (state unchanged).
 * After a rewind, the caller rebuilds the state by rendering the kept
 * messages again into a fresh state and discarding the text. */
char *tpl_render_send(struct tpl_state *state, size_t n, const geistr_message msgs[]);

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

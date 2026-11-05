# geist-runtime API (#1)

The optional EXPERIMENTAL fixed-option decision contract (#13) is in
[`DECISIONS.md`](DECISIONS.md) and `include/geistr_decision.h`. Model permission
is copied at open and defaults to disabled; it never changes ordinary chat.

`include/geistr.h` is the C interface every embedder uses: apps, the `geistr`
CLI (#11), the Python package (#9) and geist-serve itself (geist-serve#148).
It sits on geistlib and turns tokens into a conversation:

```
geistlib   load models, compute tokens            (application-neutral)
geistr     chat, templates, streaming, context,   ← this API
           cancellation, catalog, device fit
apps       geist-serve, geistr CLI, Python, …
```

Status: **agreed design** (review of #1, 2026-10-05). Everything is
EXPERIMENTAL until 1.0. `src/stub.c`
implements the contract without geistlib, so the API can be reviewed, tried
(`examples/chat.c`) and tested (`tests/test_api.c`) before code moves.

## Using it

```c
geistr_model *model;
geistr_model_open("gemma-4-E2B-it-Q4_K_M.gguf", nullptr, &model, error, sizeof error);

geistr_chat_opts opts = GEISTR_CHAT_OPTS_INIT;
opts.overflow = GEISTR_OVERFLOW_DROP_OLDEST;
geistr_chat *chat;
geistr_chat_open(model, &opts, &chat);
geistr_model_close(model);              /* the chat keeps its own reference */

geistr_message first[] = {{"system", "Be brief."}, {"user", "Hello!"}};
geistr_chat_send(chat, 2, first);
geistr_piece piece = {.size = sizeof piece};
while (geistr_chat_next(chat, &piece) == GEISTR_OK && piece.part != GEISTR_PART_END)
    fwrite(piece.text, 1, piece.len, stdout);

geistr_message next = {"user", "And in one word?"};   /* only the new message */
geistr_chat_send(chat, 1, &next);
/* … next() again; the first turn is not processed a second time */

geistr_chat_close(chat);
```

`geistr_chat_run` does send + next with a callback, for callers that prefer
push. `examples/chat.c` is a complete terminal chat with Ctrl-C cancellation.

## Design decisions

| # | Decision | Why |
| :-- | :-- | :-- |
| D1 | Prefix `geistr_`, header `geistr.h`, library `libgeistr` (the ticket said `geist_chat_*`) | `geist_*` belongs to geistlib; a runtime call must never collide with a future engine call. Same name as the CLI and the Python package. |
| D2 | **The chat holds the conversation:** `send` takes only the new messages; the answer joins the conversation by itself; `rewind(keep)` goes back to an earlier message | Every token is processed once; nothing is copied, compared or re-rendered per turn. Regenerating and editing are a `rewind` plus a `send`. Callers that receive whole conversations (HTTP clients) compare and rewind themselves (geist-serve#148). Decided in the review: as performant as possible. |
| D3 | **Pull** (`next`) as the base, a callback (`run`) on top | Same shape as geistlib's decode loop; trivial over FFI (Python, Swift, Kotlin); the caller owns the loop and threading. |
| D4 | Pieces are **complete UTF-8**; thinking output is a separate part and **discarded by default** | Every caller had to reassemble split code points (`app_utf8_feed`). Thinking is never shown as or kept in the answer (geist-serve #93). |
| D5 | Status codes, plus detail text per object; open errors go into a caller buffer | No errno, no global or thread-local error state. Codes map 1:1 to what geist-serve answers today (see mapping). |
| D6 | Options and results start with `size`; fields beyond `size` take defaults; larger sizes are refused | A caller built against an older header keeps working; a newer caller never has options silently ignored. `int` instead of `bool` in structs for plain FFI. |
| D7 | A chat is used by one thread at a time; **`cancel` from any thread**; chats on one model may run in parallel; the model is reference-counted | Covers the UI (cancel button), servers (one chat per request) and Ctrl-C (`cancel` is an atomic store). Closing in any order is safe. |
| D8 | Context: by default the model's **full window, reduced to what fits into memory** (`info.context` says what was chosen). Overflow is explicit: `REFUSE` (default, the chat stays unchanged) or `DROP_OLDEST` (a leading system message stays; the kept turns are processed again) | Long conversations as far as the device allows, without a fixed cap; never a silent truncation. |
| D11 | Stop strings in the chat options; the answer ends before the first match, which is never delivered | Editors and agents need them (geistd has them); a partial match is held back across tokens. |
| D9 | Catalog and device fit get their own header (`geistr_catalog.h`, #5/#6) that works **without loading geistlib** | geist-app ranks and verifies models without an engine in its process. |
| D10 | Out of scope for now: token-level access, audio in messages. Images came as `geistr_chat_image` (#92), tool calls in the HTTP layer, embeddings as `geistr_embed` (#91), the logits of given tokens after a prompt as `geistr_logits` (a reranker's yes/no, #150) | Token level stays geistlib/geistd (agents such as geistshell). The others are later additions to `geistr_message`/options, possible without breaking the ABI (D6). |

## Thread safety

| Call | Rule |
| :-- | :-- |
| `geistr_model_open*` | any thread; several at once |
| `geistr_model_info_get`, `geistr_model_error` | any thread, also while chats run |
| `geistr_model_close` | any time; the model lives until its last chat is closed |
| `geistr_chat_open` | any thread |
| `send`, `next`, `run`, `rewind`, `length`, `stats`, `error`, `close` | one thread at a time per chat |
| `geistr_chat_cancel` | any thread, any time, idempotent; safe in a signal handler |

## ABI rules

- `GEISTR_*_OPTS_INIT` sets `size` and the defaults; pass `nullptr` for all defaults.
- New fields are only appended. Fields beyond the caller's `size` take their
  defaults; a `size` larger than the library knows is `GEISTR_INVALID`.
- Result structs (`geistr_model_info`, `geistr_piece`, `geistr_stats`) are
  filled up to the caller's `size`.
- Enums only gain values at the end. Callers handle unknown values.
- Returned strings are borrowed for the documented lifetime and never freed
  by the caller.

## Chat templates (#2)

`src/template.c` (internal) renders messages per model family: Gemma 3
(system folded into the first user turn), Gemma 4, ChatML (Qwen, SmolLM2),
Llama 3 and BitNet (model-card turns). The family comes from the GGUF's
`tokenizer.chat_template` markers, with `general.architecture` as the
fallback; anything else is `GEISTR_FORMAT`. `geistr_model_opts.chat_format`
overrides the detection by name (`"chatml"`, …) for models whose file carries
no or a wrong template.

Rendering is incremental, for the stateful chat (D2):

- a send renders only its new messages, then the generation prompt;
- after an answer the runtime appends the turn's close: the remainder after
  the end marker if the model generated it, the whole close otherwise
  (max_tokens, a stop string, a cancel);
- after a rewind the state is rebuilt from the kept messages (#4).

Guarantees, checked in CI:
- `tests/test_template.c`: geist-serve's goldens, and *incremental = whole*
  for every family (turn by turn, with and without the model's end marker,
  equals rendering the conversation at once).

**Messages are text, not template (#261).** The template's own markers are
tokenized with special tokens on, but every message body is tokenized with them
off. This covers system, user, assistant and tool messages, and Gemma 3's
folded system text. So a message that spells `<|im_end|>\n<|im_start|>system`
or `<|eot_id|>` gets the tokens of that text, not role tokens, and cannot open
a turn of its own.

- **How.** The rendering records where each body landed
  (`tpl_render_send_spans`). The chat passes those ranges to geistlib's
  `geist_session_tokenize_plain` (geistlib#409).
- **Same ids as before.** The rendered text is still tokenized in one pass, so
  a conversation with no special token in its messages tokenizes to exactly
  the ids it had before.
- **What still matches.** Inside the bodies, only control and unknown tokens
  are off. User-defined vocabulary still tokenizes as itself, as with
  llama.cpp's `parse_special=false`. On Qwen3 this is `<think>`, `</think>`,
  `<tool_call>` and `<tool_response>`, which the tool calling of
  `serve --http` relies on.
- **System prompts.** The system prompt is a body too, whether it comes from
  `geistr.conf`, `/system` or a client's request. The runtime cannot tell an
  operator's prompt from a client's: `serve --http` takes system and
  developer messages from the request. None of the supported templates needs
  a role marker inside a system prompt.
- **Older engines.** Built against a geistlib without the call (before
  `GEIST_HAS_TOKENIZE_PLAIN`), the chat tokenizes the whole text as before.

Stop tokens: EOS plus the end-of-turn markers of all families
(`<end_of_turn>`, `<turn|>`, `<|im_end|>`, `<|eot_id|>`, `<|end_of_text|>`),
resolved through the vocabulary, as geist-serve does. geist-serve's
`chat_render_fit` (drop the oldest turns) is not moved: dropping is the chat's
job in the stateful design (#4, `GEISTR_OVERFLOW_DROP_OLDEST`).

## Text stages (#3)

`src/stream.c` (internal) turns generated token pieces into the pieces of
`geistr_chat_next`, in three stages:

1. **UTF-8**: complete, validated code points only (no overlongs,
   surrogates or values above U+10FFFF). Invalid model output ends the
   answer with `GEISTR_BACKEND` and finish `ERROR`.
2. **Thinking** (any reasoning but `GEISTR_REASONING_NONE`, and `AUTO` on a
   model that thinks): `<think>` … `</think>` blocks before the answer
   (Gemma 4: `<|channel>thought` … `<channel|>`) become
   `GEISTR_PART_THINKING` (or are discarded without `opts.thinking`),
   without their markers. Where the prompt opened the block already
   (Qwen3.5+'s `<think>\n`), the text starts as reasoning. Nesting up to 16
   deep; beyond that, and for unfinished thinking, the rest is discarded and
   never shown. Up to 32 leading whitespace bytes are kept, so an answer may
   start with whitespace, as in geist-serve. A `<think>` after the answer
   has started is literal text. What the prompt gets for thinking on and off
   is in [Thinking on and off](#thinking-on-and-off-212-266).
3. **Stop strings** (D11): the answer ends before the first match; a
   possible start of one is held back across tokens.

Besides the text, the answer's tokens are watched for a loop (`str_repeats`):
when it ends in one cycle repeated back to back (a period of 4 to 256
tokens, at least 3 times and 48 tokens in all; runs of 1 to 3 tokens such
as 64 zeros are data, not a loop), the answer ends there with finish
`REPETITION`. Small models do this, and the more so when their earlier
loops are in the conversation. With a temperature above 0 each chat gets a
fresh sampling seed: geistlib's seed 0 is one fixed seed, so every process
would otherwise sample the same answer.

Stages 1 and 2 are geist-serve's `app_utf8_feed` and `src/app/output.c`,
moved with only names changed; their tests are ported unchanged
(`tests/test_stream.c`: every split position, literal Markdown, Unicode,
a 6 MB bounded discard, 1 MB of thinking on character boundaries).
geist-serve's reasoning cases (`tests/app/reasoning_test.py`) also run
through the API (`tests/test_api.c`, model `stub:raw`).

## Thinking on and off (#212, #266)

Hybrid reasoning models switch thinking in their chat template
(`enable_thinking`, and Qwen3.6+ also `reasoning_effort`). The runtime reads
the switch from the GGUF's `tokenizer.chat_template` (`tpl_thinking_of`, by
the templates' own Jinja conditions) and renders what the template renders:

| template | default | on (`THINK_TAGS`) | off (`THINK_OFF`) | earlier answers |
|---|---|---|---|---|
| Qwen3 | on | generation prompt as it is | + `<think>\n\n</think>\n\n` | text only; in a tool loop `<think>\n` reasoning `\n</think>\n\n` text where there is reasoning |
| Qwen3.5 | off | + `<think>\n` | + `<think>\n\n</think>\n\n` | text only; in a tool loop `<think>\n` reasoning `\n</think>\n\n` text |
| Qwen3.6+ (Qwen3.8, Bonsai 2) | on, effort `xhigh` | + `<think>\n`; the effort's instruction leads the system turn (a turn of its own without a system message) | + `<think>\n\n</think>\n\n`, no instruction | `<think>\n\n</think>\n\n` + text (`preserve_thinking`) |
| Gemma 4 | off | `<\|think\|>\n` opens the system turn (a turn of its own without one) | as it is | text only |
| other ChatML with `<think>` | on | as it is | `GEISTR_FORMAT` | text only |

- `GEISTR_REASONING_AUTO` is the template's default; on a model without
  thinking it is `NONE`. `geistr_model_info.thinking` says which (1: thinks
  unless told not to, 2: only when asked), `thinking_off` whether off
  works, `effort` whether `geistr_chat_opts.effort` changes anything.
- `geistr_chat_opts.effort` (`LOW`, `MEDIUM`, `HIGH`; `DEFAULT` the
  template's `xhigh`) is Qwen3.6+'s `reasoning_effort`: `low` and `xhigh`
  put an instruction into the system turn, `medium` none. Only with
  thinking on, as in the template.
- `AUTO` is the default (`GEISTR_CHAT_OPTS_INIT`, nullptr options; before
  #266 it was `NONE`): a caller that sets nothing gets the template's
  default and the reasoning apart from the answer. `NONE` keeps the old
  behaviour for callers that want the raw text: no switch rendered, the
  generated text as it is, so a thinking model's `<think>` arrives as
  answer text.
- **Earlier answers.** By default an earlier answer is what the template
  renders for an assistant message without `reasoning_content` (except in
  the tool loop below; with a caller's reasoning, see "Reasoning sent
  back"). Where the context holds something else after an
  answer (its reasoning; Qwen3's and Qwen3.5's empty block of thinking
  off), the next send takes the answer out of the context and puts it back
  as the template has it (a prefill of the answer, #96). Qwen3.6+ with
  thinking off keeps its context as it is: the template has the empty block
  there too.
- **Checked against the templates.** `tests/test_template.c` renders the
  real templates' conversations (`tests/fixtures/templates/`: system
  prompt or none, earlier turns) with thinking unset, on, off and each
  effort, and compares the bytes with transformers' `apply_chat_template`
  (`tools/thinking_goldens.py`, jinja2), which agree with llama.cpp's
  `/apply-template` in every explicit mode. On the real models (Qwen3
  0.6B, Qwen3.5 0.8B, Gemma 4 E2B: default, on, off; Bonsai 2 27B: off, on
  at `low`, default) the context before a second answer has as many tokens
  as `llama-tokenize` gives for the template's text of the conversation;
  `test_real` checks that a continued chat's context equals a fresh chat's
  built from the plain messages, in each mode, also through a tool loop
  with padded messages. Since #269 the goldens include messages padded with
  whitespace, a blank system message and tool loops.
- **Trimmed messages (#269).** Where the template trims message contents
  (`|trim`: Qwen3.5, Qwen3.6+, Gemma 4), the runtime does too, as Python's
  `str.strip()` does (Unicode whitespace included). Only the body is
  trimmed, so it is still tokenized with special tokens off (#261). An
  answer that ends in whitespace comes back trimmed before the next turn,
  as the other differences do (not with `GEISTR_REASONING_NONE`, which keeps
  the generated text, and leaves out the tool loop below too). Qwen3.6+
  renders no system turn for an empty (or blank) system message, as its
  template does.
- **Tool messages (#269).** In the Qwen3 family a message with role `tool`
  is a `<tool_response>` block in a user turn, consecutive ones in one turn,
  as the templates have it (before, a user turn of its own text).
- **The tool loop of Qwen3.5 (#269) and Qwen3 (#276).** After the last
  user query (a user message that is not a `<tool_response>` block) the
  templates of Qwen3.5 and Qwen3 keep each answer's reasoning: `<think>\n`
  reasoning `\n</think>\n\n` answer. So while tool results follow, the chat
  keeps the reasoning in the context; the next query takes it out of every
  answer of the loop again (a rewind to the loop's first answer and a
  prefill, #96). An assistant message sent by the caller is split at
  `</think>` as the template splits it. The two templates differ in the
  details, and the runtime follows each:
  - Qwen3.5 trims the reasoning and keeps the block also for an answer
    without reasoning, so with thinking off the context keeps the empty
    block it already holds.
  - Qwen3 strips only line breaks from the reasoning and renders an answer
    without reasoning as its text alone, so with thinking off the empty
    block leaves the context after each answer, as before a query. A user
    message is no query for Qwen3 when it holds both `<tool_response>` and
    `</tool_response>` anywhere (Qwen3.5: when it is one such block).
  - Qwen3.6+ needs none of this: it keeps the empty block everywhere
    (`preserve_thinking`).
- **Reasoning sent back (#282).** `geistr_chat_send_reasoning(chat, count,
  messages, reasoning)` is `geistr_chat_send` with each message's
  reasoning, as a client sends an earlier answer back with it (OpenAI
  `reasoning_content`, Ollama `thinking`): `reasoning[i]` belongs to
  `messages[i]`, nullptr for none. The template gets it as
  `reasoning_content`, and the rendering is the template's for those
  messages:
  - Qwen3 and Qwen3.5 render it in the tool loop and drop it before the
    last query; the content is then not split at `</think>`, also for an
    empty string (as transformers renders it; llama.cpp takes an empty one
    as none, which differs only where the content holds `</think>`). Qwen3
    strips only line breaks from it and renders no block for empty
    reasoning; Qwen3.5 trims it.
  - Qwen3.6+ renders it, trimmed, in every earlier answer
    (`preserve_thinking`), in place of the empty block.
  - Gemma 4 renders it only after the last user message, which needs its
    tool format (not rendered); other templates have none. There, and with
    `GEISTR_REASONING_NONE`, it is ignored.
  - It is text: tokenized with special tokens off, as message contents are.

  `geistr_chat_opts.keep_reasoning` makes the chat's own answers stay as the
  template renders them sent back with their reasoning, which changes only
  Qwen3.6+ (it keeps each answer's reasoning; default: the empty block, as
  #266 decided). `geistr serve` sets it unless `--client-reasoning=ignore`, and
  keeps each answer with its reasoning to match the next request against.
  Checked by goldens (`tests/test_template.c`: conversations with
  `reasoning_content` in tool loops, padded, empty and null, for the real
  templates), and on the real models by `test_real`'s tool loop: the
  continued context equals a fresh chat's of the messages with their
  reasoning sent apart.

## The runtime on geistlib (#4)

`src/runtime.c` implements `geistr.h` on the pinned geistlib (`make runtime`).

**Opening a model.** `geist_model_plan` reads the header (milliseconds, no
weights), then the window is chosen (`src/window.h`) and the model is loaded
**once** with it:
- an explicit `opts.context` is used as given, capped at the trained window;
- otherwise the trained window (`<arch>.context_length`, 4096 if unknown),
  reduced to what fits into three quarters of physical memory after the
  weights, at the KV + model bytes per position the plan reports, in steps
  of 256; below 512 positions the open fails with `GEISTR_NO_MEMORY`;
- `PROCESSOR_AUTO` takes the GPU (Metal, Vulkan) for models of 1 GiB and
  more, as geist-serve does.
Measured on an M1 Max (64 GiB): SmolLM2 gets its trained 8192, Qwen3.8 27B
its full 262144 (35.6 GB of a 51.5 GB budget).

**A send** tokenizes three parts apart: the previous answer's turn close,
the new turns, the generation prompt. So it processes only what is new (a
follow-up turn with SmolLM2: 14 tokens instead of 34 for the first), and
every message knows where it starts in the session.

**Rewind** truncates the session at that position
(`geist_session_truncate`). Where geistlib refuses (recurrent DeltaNet
layers such as Qwen3.5) or a position inside a multi-message send is not
known, the kept messages are rendered and prefilled again; both paths are
tested (same answer, same context as before).

**Overflow**: `REFUSE` leaves the chat as it was; `DROP_OLDEST` drops the
oldest turns (a leading system message stays) until the conversation,
rendered again, fits, and prefills it again (`input_tokens` says so).

**Cancellation** is checked between decode steps and between prefill
chunks. geistlib cannot interrupt a prefill call, so the runtime slices it:
the first chunk is 32 tokens, later ones aim at 200 ms each and grow at most
fourfold per step. Measured: cancel during a 5,000-token prefill lands
after 15 ms on an M1 Max CPU and 546 ms in a slow Linux container; prefill
throughput stays within measurement noise of one unsliced call (157 vs
151-162 tokens/s on the CPU). A cancel inside geistlib would make the
slicing unnecessary (geistlib#628).

**Threads**: chats on one model run in parallel on the CPU (tested: two
chats answer as one alone). On GPU backends the runtime serialises the engine
calls of a model's chats (geistlib#576).

## Batched chats (geistlib#26)

`geistr_chats_next(n, chats, pieces, statuses)` is `geistr_chat_next` for
each of n chats of one model, in lock step: every chat gets exactly one piece
(or its END, or its error) per call.
- **Batched decoding.** The chats that need a decode step at the same time
  take it together, from one `geist_session_decode_batch` call. That call
  holds at most `geist_model_decode_batch_max` sessions.
- **Fallback.** Where that maximum is 0, and for chats with a draft model or
  chats replaying a verified batch, each chat decodes on its own, exactly as
  `geistr_chat_next` does.
- **Driving it.** A server drives its open answers with one call per round
  and drops a chat from the array once it reaches END.
- **Implementation.** `geistr_chat_next` is now `chat_poll` (run the text
  stages until a token is needed or a piece is ready) plus one engine step
  plus `chat_take` (feed the token through the stages).
  `geistr_chats_next` reuses both halves, so the answers go through the same
  stages, stop strings and limits.

**What the answer of a batched chat depends on:**
- Not on the other chats in the call. The engine's isolation guarantee holds
  for a given batch size.
- It can differ from the chat decoded alone, because batched rows round
  differently. On Bonsai 2 27B, 471 of 500 long reasoning answers were
  token-identical, and accuracy was unchanged (geistlib#264).

The engine batches Qwen3.5-family models (Qwen3.5, Bonsai 2) on Metal and
Vulkan, up to 8 sessions per pass; there 8 chats decode at about 3.5x the
tokens/s of one.

## Answer probes and early exit (geistlib#236)

`geistr_chat_probe_set(chat, &probe)` makes a thinking chat probe its answer
every `probe.interval` generated tokens while it thinks. A probe saves the
session state, prefills `\n</think>\n\n`, reads the logits of each option's
first token and restores the state. Probes are transparent: a probe that does
not exit leaves the answer token for token the same.

With `probe.threshold > 0` the answer ends (`GEISTR_FINISH_EARLY_EXIT`) at the
first probe whose top probability over the options (softmax of logits /
temperature) reaches it. `geistr_chat_probe_result` reports:
- the number of probes and their time;
- the exit position, option and probability;
- the last probe's distribution.

Settings are checked when set:
- thinking chats only (`GEISTR_FORMAT` otherwise);
- options with distinct first tokens;
- no draft model.

The state image is shared by the model's chats and allocated on the first
probe. With `geistr_chats_next` a probe runs on its chat alone, between
batched steps. `geistr decide --escalate chat` uses it with
`--early-exit`, and by default for the Bonsai 2 27B profile (geistlib#286);
docs/DECISIONS.md has the measured accuracy and its uncertainty.

## Catalog and verification (#5)

[`include/geistr_catalog.h`](../include/geistr_catalog.h), no engine and no
network needed:

- `geistr_catalog_parse` reads geist-serve's `models/catalog.json` format
  (schema 1 and 2) with geist-serve's rules: strict keys, safe file names,
  only `https://huggingface.co/…/resolve/` URLs, validated quality and speed
  evidence. A bad catalog is refused as a whole, with the reason.
  `models/catalog.json` started as geist-serve's file.
- `geistr_catalog_check(entry, models_dir, hash)` gives the install state:
  `MISSING`, `UNVERIFIED` (right size, not hashed yet), `OK` (SHA-256
  matches), `CORRUPT` (the right size but another SHA-256: a damaged or
  changed copy, #149) or `MISMATCH` (another size, or not a regular file,
  such as a symlink: links are not followed, #151). Never load either of
  the last two; nothing is deleted.
- Receipts: a match writes `<models>/.verified/<sha256>` with the file's
  device, inode, size, mode, owner, links, mtime and ctime (geist-serve's
  stamp). While these are unchanged, `check` answers without hashing; so
  `check(…, hash=false)` lists a folder instantly, and only new or changed
  files cost a hash.
- `geistr_models_dir` is the folder shared with the geisten app.
- SHA-256: CommonCrypto on Apple (790 MB/s on M1), portable C elsewhere
  (about 140 MB/s). Hardware SHA instructions follow if first-time checks
  of large models get slow on Linux.
- Download stays out of the library (`geistr pull`, #11).
- Family, template and context are not in the catalog: the GGUF is their
  source (`geistr_model_info`).

## Device fit and ranking (#6)

Also in `geistr_catalog.h`, without an engine:

- `geistr_device_probe(models_dir)`: RAM, available RAM, free disk, cores,
  OS and whether the engine's CPU baseline is met; `gpu` is Metal on Apple
  Silicon (Vulkan is not probed yet; set it from the engine's backends).
- `geistr_rank(catalog, device, local, opts)`: per model the resource fit
  (fits / limited / unavailable), the verdict (good / usable / not
  recommended / unknown) with a reason code, seconds per typical answer on
  CPU and GPU, measured or estimated from the other models measured here,
  and the faster processor. `geistr_ranking_get` is the suitability order,
  `geistr_ranking_best` the one recommendation (installed wins a tie).
- `local` carries what only the caller knows: installed, partial download,
  measured speed per processor. Thresholds and the quality task are options.
- Logic moved unchanged from geist-serve (`app_assess`, `app_judge`,
  `app_estimate_seconds`, `app_candidate_better`, the ranking in
  `status.c`). Reasons are codes, the wording stays with the app.
  During the move a parity harness compared both on 20,000 random devices,
  catalogs and measurements (identical fits, verdicts, order and
  recommendation); `tests/test_fit.c` keeps the cases.
- Not moved: the app's first-run default (`app_recommend`: fixed model ids
  per platform) and the speed hint from the last replies
  (`app_assess_device`); both are app policy.

This is what `geistr catalog` (#11) prints: installed (✓), available (↓), fit (⚠).

## Decisions from the review (2026-10-05)

1. Naming `geistr_` / `geistr.h` / `libgeistr`: **yes** (D1).
2. Context: **the model's full window, or what fits into memory** (D8).
3. Stop strings: **now** (D11).
4. Metadata getter in geistlib: **yes**, as part of geistlib#622.
5. Sampling: **temperature and top_p** for now; more fields are appended later (D6).
6. **The chat holds the conversation; send only the new text** (D2), instead
   of the stateless send of the first draft. geist-serve adapts (#148).

## Tests

`make test` runs `tests/test_api.c` (conformance: basics, ABI sizes, answer,
thinking, limits, stop strings, conversation, rewind, cancellation from
another thread, lifetime),
`tests/test_cxx.cpp` (the header as C++), the catalog
(`tests/test_catalog.c`: SHA-256 vectors, geist-serve's catalog, install
states, tampering, receipts; `tests/test_catalog.py`: geist-serve's invalid
catalogs), the fit (`tests/test_fit.c`: geist-serve's assessment, verdict,
estimate and ranking fixtures) and the example. `make sanitize`
repeats them under ASan and UBSan (leak checks on Linux). The conformance
tests use only the header; the real runtime must pass them unchanged.

# geist-runtime API (#1)

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
| D10 | Out of scope for now: token-level access, tool calls, images/audio in messages, embeddings | Token level stays geistlib/geistd (agents such as geistshell). The others are later additions to `geistr_message`/options, possible without breaking the ABI (D6). |

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
2. **Thinking** (`GEISTR_REASONING_THINK_TAGS`): `<think>` … `</think>` blocks
   before the answer become `GEISTR_PART_THINKING` (or are discarded without
   `opts.thinking`), without their closing markers. Nesting up to 16 deep;
   beyond that, and for unfinished thinking, the rest is discarded and never
   shown. Up to 32 leading whitespace bytes are kept, so an answer may start
   with whitespace, as in geist-serve. A `<think>` after the answer has
   started is literal text.
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
  matches) or `MISMATCH` (wrong size or hash, or not a regular file: never
  load it). Nothing is deleted.
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

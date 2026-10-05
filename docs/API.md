# geist-runtime API (draft for review, #1)

`include/geistr.h` is the C interface every embedder uses: apps, the `geistr`
CLI (#11), the Python package (#9) and geist-serve itself (geist-serve#148).
It sits on geistlib and turns tokens into a conversation:

```
geistlib   load models, compute tokens            (application-neutral)
geistr     chat, templates, streaming, context,   ← this API
           cancellation, catalog, device fit
apps       geist-serve, geistr CLI, Python, …
```

Status: **draft**. Everything is EXPERIMENTAL until 1.0. `src/stub.c`
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

geistr_message conversation[] = {{"system", "Be brief."}, {"user", "Hello!"}};
geistr_chat_send(chat, 2, conversation);
geistr_piece piece = {.size = sizeof piece};
while (geistr_chat_next(chat, &piece) == GEISTR_OK && piece.part != GEISTR_PART_END)
    fwrite(piece.text, 1, piece.len, stdout);

geistr_chat_close(chat);
```

`geistr_chat_run` does send + next with a callback, for callers that prefer
push. `examples/chat.c` is a complete terminal chat with Ctrl-C cancellation.

## Design decisions (please review)

| # | Decision | Why |
| :-- | :-- | :-- |
| D1 | Prefix `geistr_`, header `geistr.h`, library `libgeistr` (the ticket said `geist_chat_*`) | `geist_*` belongs to geistlib; a runtime call must never collide with a future engine call. Same name as the CLI and the Python package. |
| D2 | **Stateless send:** every send carries the whole conversation; the runtime reuses the KV cache of the unchanged prefix | This is how geist-serve and the OpenAI API work today. No hidden conversation state to keep in sync; editing or regenerating a turn needs no extra API. |
| D3 | **Pull** (`next`) as the base, a callback (`run`) on top | Same shape as geistlib's decode loop; trivial over FFI (Python, Swift, Kotlin); the caller owns the loop and threading. |
| D4 | Pieces are **complete UTF-8**; thinking output is a separate part and **discarded by default** | Every caller had to reassemble split code points (`app_utf8_feed`). Thinking is never shown as or kept in the answer (geist-serve #93). |
| D5 | Status codes, plus detail text per object; open errors go into a caller buffer | No errno, no global or thread-local error state. Codes map 1:1 to what geist-serve answers today (see mapping). |
| D6 | Options and results start with `size`; fields beyond `size` take defaults; larger sizes are refused | A caller built against an older header keeps working; a newer caller never has options silently ignored. `int` instead of `bool` in structs for plain FFI. |
| D7 | A chat is used by one thread at a time; **`cancel` from any thread**; chats on one model may run in parallel; the model is reference-counted | Covers the UI (cancel button), servers (one chat per request) and Ctrl-C (`cancel` is an atomic store). Closing in any order is safe. |
| D8 | Context overflow is explicit: `REFUSE` (default) or `DROP_OLDEST` (a leading system message stays) | The same two behaviours geist-serve has (`chat_render_fit`, 400 "does not fit"); never a silent truncation. |
| D9 | Catalog and device fit get their own header (`geistr_catalog.h`, #5/#6) that works **without loading geistlib** | geist-app ranks and verifies models without an engine in its process. |
| D10 | Out of scope for now: token-level access, tool calls, images/audio in messages, embeddings | Token level stays geistlib/geistd (agents such as geistshell). The others are later additions to `geistr_message`/options, possible without breaking the ABI (D6). |

## Thread safety

| Call | Rule |
| :-- | :-- |
| `geistr_model_open*` | any thread; several at once |
| `geistr_model_info_get`, `geistr_model_error` | any thread, also while chats run |
| `geistr_model_close` | any time; the model lives until its last chat is closed |
| `geistr_chat_open` | any thread |
| `send`, `next`, `run`, `stats`, `error`, `close` | one thread at a time per chat |
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

## Mapping: every geist-serve use

| geist-serve today | Where | In the runtime |
| :-- | :-- | :-- |
| `app_daemon_chat`: template, tokenize, prefill with reuse, generate with stops, temperature/top_p, max, cancel callback, stats | `src/app/daemon.c`, used by the UI chat, `/v1/chat/completions`, Ollama `/api/chat` (`src/app/chat.c`) and the CPU/GPU comparison (`src/app/compare.c`) | `chat_open` + `chat_send` + `next`/`run`; options → `geistr_chat_opts`; `emit` → pieces; cancel callback → `chat_cancel` or `emit` returning 0 |
| `app_daemon_run` (one prompt) | `geisten test`, single-prompt routes | one user message |
| `struct app_run_stats` | logs, measurements, activity | `geistr_stats` (prompt/reused/output tokens, prefill, first answer, generation, total; `limited` → `finish` LENGTH/CONTEXT) |
| HTTP 400 "chat format not supported" | daemon.c | `GEISTR_FORMAT` |
| HTTP 400 "does not fit the context" | daemon.c | `GEISTR_CONTEXT` |
| HTTP 499 cancelled | daemon.c | `GEISTR_CANCELLED` |
| HTTP 502/504 engine failure, timeouts | daemon.c | `GEISTR_BACKEND`; **timeouts stay in geist-app** (process watchdog) |
| `chat_render`, `chat_render_fit`, `chat_family_*` | `src/template.c` (geistd, legacy server) | internal (#2); visible as `geistr_model_info.chat_format` and `overflow` |
| `model_is_stop`, `stop_ids`, `stop_strings` | `src/model.c`, geistd `generate` | internal (#2) |
| `app_utf8_feed` | `src/app/core.c` | internal (#3); pieces are complete UTF-8 |
| `app_output_*` (think tags, #93) | `src/app/output.c` | internal (#3); `reasoning` + `thinking` options, `GEISTR_PART_THINKING` |
| geistd session cache, `pin_prefix`, reuse counting | `src/geistd.c` | internal (#4) behind the stateless send |
| backend probe, CPU/GPU choice | `src/app/child.c` (`--backends`) | `geistr_model_opts.processor`, `geistr_model_info.backend` |
| catalog parse/apply, SHA-256 verification | `src/app/catalog.c`, `jobs.c` | `geistr_catalog.h` (#5) |
| `app_assess_device`, `app_judge`, `app_candidate_better`, ranking | `src/app/status.c`, `tasks.c` | `geistr_catalog.h` (#6) |
| performance history, export, settings | `src/app/performance.c`, `prefs.c` | **stays in geist-serve** (product data); measured speeds are an input to #6 |
| comparison procedure (warm-up, three answers per processor) | `src/app/compare.c` | **stays in geist-serve**; it opens the model per processor and reads `geistr_stats` |
| geistd token-level ops (`open`, `tokenize`, `prefill`, `step`, `peek`) for agents | `src/geistd.c` | **out of scope** (D10): geistd keeps serving them from geistlib |

### geist-serve after #148

geist-app keeps process isolation, the watchdog, HTTP and the UI. geistd links
libgeistr and gains one message-level operation ("chat": messages in, pieces
and stats out), so geist-app no longer renders templates or counts tokens.
Catalog and fit run in geist-app through `geistr_catalog.h`, without an engine.
The legacy `geist-serve` executable either uses the same chat calls or is
retired; that is decided in #148.

## What the runtime needs from geistlib (input for geistlib#622)

Used today by geist-serve and needed by the runtime, to be STABLE:

- backend: `geist_backend_create`, `_destroy`, `_name`, `_errmsg`, `geist_backend_resources_snapshot`
- model: `geist_model_load_with_opts`, `geist_model_load_from_memory`, `_destroy`, `_errmsg`, `_arch`,
  `geist_model_bos_token`, `_eos_token`, `_add_bos`, `_token_by_text`
- session: `geist_session_create`, `_destroy`, `_errmsg`, `_reset`, `_tokenize`,
  `_prefill_tokens`, `_decode_step`, `_token_to_str`, `_pin_prefix`, `_get_stats`

**Missing:** a metadata getter (e.g. `geist_model_metadata_str(m, "tokenizer.chat_template")`).
geist-serve parses the GGUF file a second time for the chat template
(`src/gguf.c`); that does not work for `geistr_model_open_memory`. Adding the
getter to geistlib keeps a single GGUF parser.

## Draft for #5/#6 (not binding)

```c
/* geistr_catalog.h — no engine needed */
geistr_status geistr_catalog_parse(const char *json, size_t len, geistr_catalog **out, char *error, size_t cap);
size_t        geistr_catalog_count(const geistr_catalog *);
geistr_status geistr_catalog_entry(const geistr_catalog *, size_t i, geistr_catalog_entry *out);
geistr_status geistr_catalog_scan(geistr_catalog *, const char *models_dir);   /* installed = present + SHA-256 */
geistr_status geistr_device_probe(geistr_device *out);
geistr_status geistr_fit(const geistr_catalog *, const geistr_device *, const geistr_measurements *, geistr_ranking *out);
```

This is what `geistr catalog` (#11) prints: installed (✓), available (↓), fit (⚠).

## Open questions for the review

1. **D1 naming:** `geistr_` / `geistr.h` instead of `geist_chat_*`. Agreed?
2. **Context cap:** geist-serve caps the context at 4096 tokens. Keep that as
   the runtime default, or use the model's full window unless the caller limits it?
3. **Stop strings:** geistd accepts caller stop strings. Add
   `const char *const *stop; size_t n_stop` to `geistr_chat_opts` now, or
   wait for a user who needs them (editors currently do not send any)?
4. **Metadata getter in geistlib** (above): agreed as part of geistlib#622?
5. **Sampling:** temperature and top_p only (what geist-serve uses). Seed,
   top_k or repetition penalty later, appended per D6?

## Tests

`make test` runs `tests/test_api.c` (conformance: basics, ABI sizes, answer,
thinking, limits, prefix reuse, cancellation from another thread, lifetime),
`tests/test_cxx.cpp` (the header as C++) and the example. `make sanitize`
repeats them under ASan and UBSan (leak checks on Linux). The conformance
tests use only the header; the real runtime must pass them unchanged.

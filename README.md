# geist-runtime

An embeddable model runner on top of [geistlib](https://github.com/geisten/geistlib):
open a model, hold a chat, read the answer as text. Templates, streaming,
thinking output, context limits, KV reuse and cancellation are handled here,
behind one C API: [`include/geistr.h`](include/geistr.h).

Users:
- the `geistr` CLI (#11),
- the Python package `geistr` (#9),
- [geist-serve](https://github.com/geisten/geist-serve) (geist-serve#148),
- apps that embed models directly.

Status: in are the API (#1), templates (#2), streaming text (#3), the
runtime on geistlib with context management (#4), the model catalog with
SHA-256 verification (#5) and device fit with the model ranking (#6), both in
[`include/geistr_catalog.h`](include/geistr_catalog.h), and the `geistr` CLI
(#11) and the Python package `geistr` (#9). `src/stub.c` implements the API without an engine, for the fast
conformance tests.

```sh
make core        # build/libgeistr-core.a: catalog, fit, templates, text — no engine
make test        # conformance tests (C and C++) and the example chat, against the stub
make sanitize    # the same under ASan + UBSan
printf 'Hello\n' | build/chat stub:echo

make runtime     # the real libgeistr.a on the pinned geistlib
make fetch-model && make test-real   # the real runtime against SmolLM2
make chat-real && build/chat-real model.gguf
```

- [docs/API.md](docs/API.md): usage, design decisions, thread and ABI rules,
  and the mapping of every geist-serve use.
- Plan: [geist-serve#149](https://github.com/geisten/geist-serve/issues/149).

## geistr

A minimal CLI on the runtime; the model runs in-process (`make geistr`).

```sh
geistr catalog                     # ✓ installed (SHA-256 verified), ↓ available, ⚠/✗ fit on this computer
geistr catalog --installed | --available | --json
geistr run gemma4-e2b "prompt"     # one answer to stdout; a catalog id or a .gguf path; prompt from stdin if none
geistr chat gemma4-e2b             # Ctrl-C stops the answer, not the chat; /help in the chat
geistr chat                        # the last model again (else the geisten app's)
geistr pull gemma4-e4b             # download, resume, verify (builds with the download module)
```

In a terminal the chat shows Markdown (headings, **bold**, *italic*, `code`,
lists, quotes, code blocks, tables: compact and aligned, wrapped to the
terminal, one record per row when the columns cannot fit) and LaTeX math as Unicode (`$e^{i\pi}$`, `$$\frac{a}{b}$$`
→ e^(iπ), a/b; α, ∑, ², ₁, √, ℝ …); piped output stays plain text. The prompt
shows the processor (⚙ CPU, ⚡ GPU), and each answer ends with its speed
(`79.4 tok/s · 4.1 s`).

In a terminal the chat line is editable: Tab completes the `/` commands and,
after `/model `, the installed models (several matches: their common part,
then a list with what each does); the rest of a unique command appears dim
and → or Tab takes it; ↑/↓ recall earlier lines; Ctrl-A/E/U/K/W/L as usual.
UTF-8 aware, no readline or libedit dependency (`tools/geistr/lineedit.c`).

In the chat, switch while it runs; the conversation moves along (the new
session reads it once with your next message), and a switch that fails keeps
the current session:

```
/gpu /cpu /auto          processor (GPU: Metal on macOS, Vulkan on Linux)
/model qwen3-0.6b        another model, same conversation
/temp 0.7                sampling temperature
/system Sei knapp.       system prompt (empty: none)
/info                    what runs now: backend, model, chat format, context
/save                    keep model, processor, temperature, system for next time
/clear  /exit
```

Settings live in `geistr.conf` next to the model folder:

```sh
geistr config                              # all settings and the file
geistr config processor gpu                # auto (default), cpu, gpu; --cpu/--gpu for one run
geistr config temperature 0.7              # 0 to 2
geistr config system Antworte auf Deutsch. # a system prompt for every new chat
geistr config markdown off                 # plain text
geistr config stats off                    # no speed line
geistr config model ""                     # forget the last model
```

Options: `--models DIR` (default: the geisten app's model folder, so models
are shared), `--catalog FILE` (default: the app's `catalog.json` if present,
else the built-in copy). Exit codes: 0 ok, 1 error, 2 usage, 130 cancelled.
`make geistr PULL=0` builds without the download module and without any
network code. `--json` is schema 1: `schema`, `models_dir`, and per model
`id`, `name`, `quantization`, `file`, `url`, `sha256`, `bytes`,
`recommended_ram_gib`, `state` (available, unverified, installed, mismatch),
`resource` (fits, limited, unavailable), `resource_reason`.

## Python

```python
import geistr

for m in geistr.catalog(installed=True):      # the same models as `geistr catalog`
    print(m.id, m.name, m.state)

with geistr.chat("gemma4-e2b", system="Answer briefly.") as chat:
    for piece in chat.send("What is the capital of France?"):   # streamed
        print(piece, end="", flush=True)
    print(chat.ask("And of Italy?"))           # only the new message is processed
```

`make wheel` builds `build/wheel/geistr-*.whl`: ctypes over `libgeistr`
(only `geistr_*` exported), no compiled extension, no dependencies.
`geistr.open(model)` gives a `Model` (`.info`, `.chat(...)`); a `Chat` has
`send`, `ask`, `cancel` (any thread), `rewind`, `len()`, `stats`. Leaving a
`for` over `send()` early, or Ctrl-C, stops the answer; the chat goes on.
Failures raise `geistr.GeistrError` with `.status` ("io", "context", …).
CI builds wheels for macOS arm64 and Linux x86_64/arm64 (manylinux via
auditwheel) and runs `examples/chat.py` from a fresh `pip install`.

## CI

Every PR and push to `main` runs `.github/workflows/ci.yml`; the job
`ci-ok` is the required check and passes only if every platform passed.

| job | platforms | what |
|---|---|---|
| `test` | macOS arm64 (clang), Linux x86_64 (gcc-14, clang-18), Linux arm64 (gcc-14) | `make test` (stub conformance, templates, streams, catalog, fit), `make sanitize`, `make parity` with the pinned geist-serve |
| `real` | macOS arm64, Linux x86_64, Linux arm64 | the runtime on the pinned geistlib against SmolLM2 360M (from the catalog, SHA-256 verified, cached): `make test-real` (also under ASan/UBSan), `make test-geistr`, `make test-python`; the wheels as artifacts |

On failure each job uploads its logs (`build/logs/`) as `evidence-*`. A run
takes about 3 minutes; jobs time out at 15 and 30 minutes.

Flaky tests: there are no retries, in CI or in the tests. A test that fails
without a code change is fixed, or quarantined in the same PR that opens
an issue for it (`flaky` label), with the issue named next to the quarantine.
Tests do not guess timing: e.g. cancellation is triggered once the answer
streams, and measured from the cancel call.

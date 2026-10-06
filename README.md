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

A minimal CLI on the runtime; the model runs in-process.

### Install

```sh
git clone https://github.com/geisten/geist-runtime && cd geist-runtime
make geistr                        # fetches and builds the pinned geistlib, then build/geistr
sudo make install                  # → /usr/local/bin/geistr
make install PREFIX=~/.local       # or without sudo (~/.local/bin on the PATH)
make install DESTDIR=/tmp/stage    # staged, for packaging
make uninstall                     # the same PREFIX/DESTDIR
```

Needs a C23 compiler (clang 18+ or gcc 14+) and python3; on macOS also
`brew install libomp`. With libcurl (`curl-config`) it gets the download
module for `pull`, else it builds without network code (`PULL=0` forces
that). The binary is self-contained: geistlib and, on macOS, libomp are
linked statically; it needs only system libraries (macOS: Accelerate,
libcurl; Linux: libc, libm, libgomp, libcurl).

### Commands

| command | does |
|---|---|
| `geistr catalog [--installed \| --available] [--json]` | the models: ✓ installed (SHA-256 verified), ↓ available, ⟳ to update, ⚠/✗ fit on this computer, tokens/s on ⚙ CPU and ⚡ GPU |
| `geistr pull <id>` | download, resume, verify |
| `geistr pull` | update the installed models to this catalog (after a geistr update) |
| `geistr run <model> [prompt…]` | one answer to stdout; the prompt from stdin if none |
| `geistr chat [<model>]` | interactive (see below); without a model the last one; continues the last conversation |
| `geistr chat --new` | a new conversation instead of the last one |
| `geistr bench [model…]` | tokens/s on ⚙ and ⚡ with a fixed prompt (default: every installed model) |
| `geistr bench --compare [A [B]]` | two geistlib commits' bench speeds and the change (▲/▼ %) |
| `geistr serve <model> [--socket=PATH] [--chats N]` | the model as a service on a Unix socket |
| `geistr chat --socket[=PATH]` | chat with that service |
| `geistr config [key [value]]` | settings, remembered between runs |
| `geistr --version` | geistr, catalog revision, engine commit |

`<model>` is a catalog id or a path to a `.gguf` file. Options anywhere:
`--cpu`/`--gpu` (the processor for this run), `--models DIR`, `--catalog FILE`.

In a terminal the chat shows Markdown (headings, **bold**, *italic*, `code`,
lists, quotes, code blocks, tables: compact and aligned, wrapped to the
terminal, one record per row when the columns cannot fit) and LaTeX math as Unicode (`$e^{i\pi}$`, `$$\frac{a}{b}$$`
→ e^(iπ), a/b; α, ∑, ², ₁, √, ℝ …); piped output stays plain text. The prompt
shows the processor (⚙ CPU, ⚡ GPU), and each answer ends with its speed
(`79.4 tok/s · 4.1 s`).

Every complete answer (chat, run, bench) records its speed and the geistlib
commit in `speed.tsv` in the data folder. `geistr catalog` draws the median of
the last ten per model and processor with this engine as bars on one scale (`⚙ 113 ██████▉  ⚡ 196 ████████████`);
values from the catalog's reference computer are dim until measured here.
The file is only ever appended to, so it keeps the history to compare
geistlib versions (a line: model, cpu|gpu, tokens/s, seconds to the first
answer, Unix time, geistlib commit, source: `bench` or `answer`). `geistr bench --compare` sets the bench rows of two engines side by side
(default: the one measured last against the one before; or commit prefixes
A and B), per model and processor the median of the last ten each:

```
                         5dd7e17   a1b2c3d
⚙ smollm2-360m             112.8     130.1   ▲ 15.3 %
⚡ smollm2-360m             195.9     191.0   ▼ 2.5 %
```
The measured speeds also feed the fit verdicts (`geistr_rank`).

In a terminal the chat continues the last conversation: `↻ 6 · „the last
question“ · /clear new` shows where it was, and only on its first message does
the model read it again (with `geistr serve`, not even that). Each chat keeps
its own file in `chats/` in the data folder (0600, one JSON message per line),
written after every answer; `/clear` starts a new one and keeps the old.
`geistr config resume off` keeps nothing. Piped chats neither continue nor
keep anything, so scripts stay reproducible.

In a terminal the chat starts with a two-line intro (what ⚙/⚡ mean, the
keys; `geistr config intro off`), shows a spinner with size and time while a
model loads, and uses Claude Code's keys:

| key | does |
|---|---|
| `/` | a list of the commands under the line, filtered as you type; ↑↓ choose, Tab takes, Enter takes and runs (or waits for the argument, e.g. `/model `), Esc closes; after `/model ` the installed models |
| Esc | stops the answer (Ctrl-C too) |
| Ctrl-C | clears the line; on an empty line twice: exit |
| `?` | on an empty line: the shortcuts |
| ↑↓ | earlier lines; → takes the dim hint |
| Ctrl-A/E/U/K/W/L, Ctrl-D | as in a shell; Ctrl-D on an empty line exits |

Keys typed while an answer runs are kept for the next prompt. UTF-8 aware, no
readline or libedit dependency (`tools/geistr/lineedit.c`).

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

Settings live in `geistr.conf`: on macOS in `~/Library/Application Support/geisten/`
(next to the models), on Linux in `$XDG_CONFIG_HOME/geisten/` (`~/.config/geisten/`;
an older one next to the models moves there once), with `GEISTEN_HOME` in that folder.
`geistr config` shows the file:

```sh
geistr config                              # all settings and the file
geistr config processor gpu                # auto (default), cpu, gpu; --cpu/--gpu for one run
geistr config temperature 0.7              # 0 to 2
geistr config system Antworte auf Deutsch. # a system prompt for every new chat
geistr config markdown off                 # plain text
geistr config stats off                    # no speed line
geistr config intro off                    # no intro at the start
geistr config resume off                   # every chat starts new, nothing is kept
geistr config model ""                     # forget the last model
```

Options: `--models DIR` (default: the geisten app's model folder, so models
are shared), `--catalog FILE` (default: the built-in copy, see Updates). Exit codes: 0 ok, 1 error, 2 usage, 130 cancelled.
`make geistr PULL=0` builds without the download module and without any
network code. `--json` is schema 1: `schema`, `models_dir`, and per model
`id`, `name`, `quantization`, `file`, `url`, `sha256`, `bytes`,
`recommended_ram_gib`, `state` (available, unverified, installed, mismatch),
`resource` (fits, limited, unavailable), `resource_reason`, `tokens_per_s`
(`cpu`, `gpu`: measured here, or null).

### Updates

The catalog ships inside geistr and lists only models its engine runs, so a
new model arrives with a new geistr (`brew upgrade`, `pip install -U
geistr`); there is no separate catalog download. After an update `geistr
catalog` marks installed models whose file the new catalog replaced with ⟳,
and `geistr pull` downloads them again (the old file stays until the new one
is complete and verified). `geistr --version` names all three:
`geistr 0.1.0 · catalog revision 9 · engine 5dd7e17`.

### As a service

One process holds the model; chats connect to it over a Unix socket (owner
only, 0600):

```sh
geistr serve gemma4-e2b [--chats 2]        # SIGTERM or Ctrl-C stops it
geistr chat --socket                       # in another terminal
```

The socket defaults to `geistr.sock` next to the model folder
(`--socket=PATH` for another). The client sends the whole conversation with
every message; the service keeps up to `--chats` conversations and continues
the one that matches, processing only what is new. `/model`, `/gpu`, `/cpu`
and `/auto` belong to the service. The protocol is one JSON object per line,
documented in `tools/geistr/service.h`:

```sh
echo '{"op":"chat","messages":[{"role":"user","content":"Hi"}]}' | nc -U ~/…/geistr.sock
```

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

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
geistr chat gemma4-e2b             # Ctrl-C stops the answer, not the chat
geistr pull gemma4-e4b             # download, resume, verify (builds with the download module)
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

The optional fixed-option decision contract (#13) is documented in
[docs/DECISIONS.md](docs/DECISIONS.md). It is default-off and EXPERIMENTAL;
configuration support alone establishes neither backend decision support nor
quality-matched speedups.

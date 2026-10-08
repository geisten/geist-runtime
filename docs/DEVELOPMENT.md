# Developing geist-runtime

geist-runtime is an embeddable model runner on top of
[geistlib](https://github.com/geisten/geistlib): open a model, hold a chat,
read the answer as text. Templates, streaming, thinking output, context
limits, KV reuse and cancellation are handled here, behind one C API:
[`include/geistr.h`](../include/geistr.h). The model catalog (SHA-256
verification) and device fit with model ranking are in
[`include/geistr_catalog.h`](../include/geistr_catalog.h).

Users: the `geistr` CLI, the Python package `geistr`, and apps that embed
models directly. [API.md](API.md) has usage, design decisions, thread and
ABI rules.

## The library

```sh
make core        # build/libgeistr-core.a: catalog, fit, templates, text — no engine
make test        # conformance tests (C and C++) and the example chat, against the stub
make sanitize    # the same under ASan + UBSan
printf 'Hello\n' | build/chat stub:echo

make runtime     # the real libgeistr.a on the pinned geistlib
make fetch-model && make test-real   # the real runtime against SmolLM2
make chat-real && build/chat-real model.gguf
```

`src/stub.c` implements the API without an engine, for the fast conformance
tests; `src/runtime.c` binds it to geistlib; `src/common.c` is what both
share.

## Build geistr from source

```sh
git clone https://github.com/geisten/geist-runtime && cd geist-runtime
make geistr                        # fetches and builds the pinned geistlib, then build/geistr
sudo make install                  # → /usr/local/bin/geistr
make install PREFIX=~/.local       # or without sudo (~/.local/bin on the PATH)
make install DESTDIR=/tmp/stage    # staged, for packaging
make uninstall                     # the same PREFIX/DESTDIR
make test-geistr                   # the CLI against the reference model
```

Needs a C23 compiler (clang 18+ or gcc 14+) and python3; on macOS also
`brew install libomp`. With libcurl (`curl-config`) it gets the download
module for `pull`, else it builds without network code (`PULL=0` forces
that). The binary is self-contained: geistlib and, on macOS, libomp are
linked statically; it needs only system libraries (macOS: Accelerate,
libcurl; Linux: libc, libm, libgomp, libcurl).

For a fully static Linux build (musl), as in the releases:
`docker run --rm -v "$PWD:/src" -w /src alpine:3.21 sh scripts/build-static.sh`
→ `build/static/geistr`.

## Releases

`.github/workflows/release.yml` builds them: a `vX.Y.Z` tag makes a draft
release, publishing is a maintainer's step. The Linux binaries are fully
static (musl, with a minimal libcurl, OpenSSL and libgomp). The archives
carry `THIRD_PARTY_LICENSES`: every component in the binary and its license.
`install.sh` downloads `geistr-<os>-<arch>.tar.gz`, checks it against the
release's `SHA256SUMS` and installs to `$PREFIX/bin`.

## geistr internals

- `geistr catalog --json` is schema 1: `schema`, `models_dir`, and per model
  `id`, `name`, `quantization`, `file`, `url`, `sha256`, `bytes`,
  `recommended_ram_gib`, `state` (available, unverified, installed, mismatch),
  `resource` (fits, limited, unavailable), `resource_reason`, `tokens_per_s`
  (`cpu`, `gpu`: measured here, or null).
- `speed.tsv` in the data folder is only appended to, so it keeps the history
  to compare geistlib versions. A line: model, cpu|gpu, tokens/s, seconds to
  the first answer, Unix time, geistlib commit, source (`bench` or `answer`).
  `geistr bench --compare` sets two engines side by side (default: the one
  measured last against the one before; or commit prefixes A and B):

  ```
                           5dd7e17   a1b2c3d
  ⚙ smollm2-360m             112.8     130.1   ▲ 15.3 %
  ⚡ smollm2-360m             195.9     191.0   ▼ 2.5 %
  ```
  The measured speeds also feed the fit verdicts (`geistr_rank`).
- Chats are kept in `chats/` in the data folder (0600, one JSON message per
  line), written after every answer.
- Settings live in `geistr.conf`: on macOS in
  `~/Library/Application Support/geisten/` (next to the models), on Linux in
  `$XDG_CONFIG_HOME/geisten/` (`~/.config/geisten/`), with `GEISTEN_HOME` in
  that folder.
- The line editor is UTF-8 aware, without readline or libedit
  (`tools/geistr/lineedit.c`).

## Python

`make wheel` builds `build/wheel/geistr-*.whl`: ctypes over `libgeistr`
(only `geistr_*` exported), no compiled extension, no dependencies.
`make test-python` installs it into a fresh venv and runs the example and
the tests.

`geistr.open(model)` gives a `Model` (`.info`, `.chat(...)`); a `Chat` has
`send`, `ask`, `cancel` (any thread), `rewind`, `len()`, `stats`. Leaving a
`for` over `send()` early, or Ctrl-C, stops the answer; the chat goes on.
Failures raise `geistr.GeistrError` with `.status` ("io", "context", …).
`geistr.catalog(installed=True)` lists the same models as `geistr catalog`.
CI builds wheels for macOS arm64 and Linux x86_64/arm64 (manylinux via
auditwheel) and runs `examples/chat.py` from a fresh `pip install`.

The optional fixed-option decision contract (#13) is documented in
[DECISIONS.md](DECISIONS.md). It is default-off and EXPERIMENTAL;
configuration support alone establishes neither backend decision support nor
quality-matched speedups.

## CI

Every PR and push to `main` runs `.github/workflows/ci.yml`; the job
`ci-ok` is the required check and passes only if every platform passed.

| job | platforms | what |
|---|---|---|
| `test` | macOS arm64 (clang), Linux x86_64 (gcc-14, clang-18), Linux arm64 (gcc-14) | `make test` (stub conformance, templates, streams, catalog, fit), `make sanitize` |
| `real` | macOS arm64, Linux x86_64, Linux arm64 | the runtime on the pinned geistlib against SmolLM2 360M (from the catalog, SHA-256 verified, cached): `make test-real` (also under ASan/UBSan), `make test-geistr`, `make test-python`; the wheels as artifacts |

On failure each job uploads its logs (`build/logs/`) as `evidence-*`. A run
takes about 3 minutes; jobs time out at 15 and 30 minutes.

Flaky tests: there are no retries, in CI or in the tests. A test that fails
without a code change is fixed, or quarantined in the same PR that opens
an issue for it (`flaky` label), with the issue named next to the quarantine.
Tests do not guess timing: e.g. cancellation is triggered once the answer
streams, and measured from the cancel call.

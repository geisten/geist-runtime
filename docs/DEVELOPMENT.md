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
make test-clients                  # serve --http against the openai and ollama Python clients
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

`.gitea/workflows/release.yml` builds them on git.geisten.net's runners when a
`vX.Y.Z` tag is pushed (or by hand with the input `tag`, to repeat one): the
archives `geistr-<os>-<arch>.tar.gz`, the Python wheels and `SHA256SUMS` go to
`https://www.geisten.net/download/geistr/vX.Y.Z/` and `latest` points there;
then the Homebrew tap, then PyPI (with the secret `PYPI_TOKEN`). The server
keeps the **last 5 releases** (`KEEP` in the `prune` job,
`scripts/prune-releases.sh`, after a successful release): older `vX.Y.Z`
folders are deleted in version order; `latest`, `install.sh` and the release
just uploaded never are. The repository variable `PRUNE_DRY_RUN=1` makes it
only print what it would delete (#148). The Linux
binaries are fully static (musl, with a minimal libcurl, OpenSSL and libgomp);
the macOS binary needs macOS 15 or later (`MACOSX_DEPLOYMENT_TARGET`, checked).
The archives carry `THIRD_PARTY_LICENSES`. `install.sh` downloads the archive
for this computer, checks it against `SHA256SUMS` and installs to
`$PREFIX/bin`.

After the upload the `smoke` job installs what users get: the archive through
`install.sh` on Debian 12, Ubuntu 22.04 and 24.04, the wheel from PyPI on
Debian 11; each answers once (#172).

A release PR sets the version in `src/runtime.c`, `src/stub.c`,
`include/geistr.h`, `python/geistr/__init__.py` and, where README.md and
`install.sh` name a release (`vX.Y.Z`), there too: `make test` runs
`scripts/check-version.sh`, which fails while any of them names another
version.

A release is done when, in addition:

1. **macOS 15**: on a Mac (or VM) with macOS 15, `curl -fsSL
   https://www.geisten.net/download/geistr/install.sh | PREFIX=~/.local sh`,
   then `~/.local/bin/geistr --version` and one `geistr run`. No runner has an
   older macOS than the build machine, so nothing else checks this; v0.3.2 and
   the first v0.3.3 build needed macOS 27 and nobody noticed (#172).
2. `brew upgrade geistr` gives the new version (`brew test geistr`).

## geistr internals

- `geistr catalog --json` is schema 1: `schema`, `models_dir`, and per model
  `id`, `name`, `quantization`, `file`, `url`, `sha256`, `bytes`,
  `recommended_ram_gib`, `state` (available, unverified, installed, mismatch, corrupt),
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
CI builds wheels for macOS arm64 and Linux x86_64 (manylinux via
auditwheel) and runs `examples/chat.py` from a fresh `pip install`. The release
workflow builds them for macOS arm64 and Linux x86_64/arm64, checks that the
wheel's version is the tag, attaches them to the release folder on
www.geisten.net and, if the secret `PYPI_TOKEN` is set, uploads them to PyPI.
The Linux wheels are built in the manylinux_2_28 image (glibc 2.28 and newer:
Debian 10+, Ubuntu 20.04+, RHEL 8+); the release checks one on Debian 11.

The optional fixed-option decision contract (#13) is documented in
[DECISIONS.md](DECISIONS.md). It is default-off and EXPERIMENTAL;
configuration support alone establishes neither backend decision support nor
quality-matched speedups.

## End-to-end test

[E2E-TESTPLAN.md](E2E-TESTPLAN.md) is the manual or agent-driven test of the
apps on real terminals, models and hardware: install, catalog, run, chat,
service APIs, Python, decisions. Run it before a release.

## CI

Every PR and push to `main` runs `.github/workflows/ci.yml`; the job
`ci-ok` is the required check and passes only if every platform passed.

| job | platforms | what |
|---|---|---|
| `test` | macOS arm64 (clang), Linux x86_64 (gcc-14, clang-18), Linux arm64 (gcc-14) | `make test` (stub conformance, templates, streams, catalog, fit), `make sanitize` |
| `real` | macOS arm64, Linux x86_64, Linux arm64 | the runtime on the pinned geistlib against SmolLM2 360M (from the catalog, SHA-256 verified, cached): `make test-real` (also under ASan/UBSan), `make test-geistr`, `make test-clients` (the pinned `openai` and `ollama` Python clients against `serve --http`: SmolLM2, Qwen3 0.6B tools, BitNet-embedding), `make test-python`; the wheels as artifacts |

On failure each job uploads its logs (`build/logs/`) as `evidence-*`. A run
takes about 3 minutes; jobs time out at 15 and 30 minutes.

Flaky tests: there are no retries, in CI or in the tests. A test that fails
without a code change is fixed, or quarantined in the same PR that opens
an issue for it (`flaky` label), with the issue named next to the quarantine.
Tests do not guess timing: e.g. cancellation is triggered once the answer
streams, and measured from the cancel call.

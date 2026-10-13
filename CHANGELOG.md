# Changelog

## Unreleased

### Security

- `geistr serve --http` refuses requests from a web page of another site
  (`Origin` header): before, any page could start answers on this computer
  with a simple request (text/plain, no preflight), keeping the engine busy.
  `GEISTR_ORIGINS` allows more origins, as Ollama's `OLLAMA_ORIGINS`. The
  `Host` check no longer takes a name like `127.evil.example` for a loopback
  address (#171).

### Fixes

- `geistr serve --http`: a request that names another model (`"model":
  "gemma4-e2b"` to a qwen3-0.6b service) gets 404 `model_not_found` instead of
  an answer from the served model. Its id, `id:latest`, its file name or no
  model are accepted as before (#193).
- `--files` finds the right passages more often: the question is embedded as
  it is, without the `Instruct: …\nQuery: ` line before it, which made the
  ranking almost independent of the question. On the new retrieval benchmark
  (`scripts/retrieval-bench.py`, 36 English and German passages in 12 files)
  the right file is now first for 86 % of the questions (was 64 %) and among
  the four passages put before the question for 97 % (was 92 %). Existing
  indexes stay valid: passages are embedded as before (#157).
- `scripts/retrieval-bench.py --sources` measures which files `sources:`
  would name: the questions are labelled with the files that answer them,
  with 3 questions that need two files and 16 (English and German) that no
  file answers; it scores rules for picking files from the top 4 by precision,
  recall and how often an off-topic question gets no source, and `--rerank`
  adds a cross-encoder for comparison. No cosine rule separates relevant from
  unrelated files, so `sources:` stays as it is (#150).
- `geistr index` says which files it leaves out and why: a PDF without text
  (a scan), a PDF when `pdftotext` is missing, an unreadable file. Before they
  were skipped without a word (#173).
- `geistr --help` says what `serve --max-request` limits and its default
  (16 MiB) (#147).
- `NO_COLOR` makes `geistr chat` plain: before, the `/` list, the hint, the
  Ctrl-R search line, "Ctrl-C again to exit" and the resumed `↻` line were
  still styled, and the intro and the spinner were left out altogether (#134).
- The macOS archive, the Homebrew binary and the macOS wheel were built with the
  runner's own macOS (27) as the minimum, so they did not start on macOS 15 or
  26 and PyPI refused the wheel tag `macosx_27_0`. They are built for macOS 15
  and later now (`minos 15.0`, checked in the release workflow) (#152).

### Release

- The Linux wheels are built in the manylinux_2_28 image: `pip install geistr`
  works on glibc 2.28 and newer (Debian 10+, Ubuntu 20.04+), not only on 2.38/2.39
  as with 0.3.3. After each upload a `smoke` job installs the release on Debian
  12, Ubuntu 22.04 and 24.04 (install.sh) and from PyPI on Debian 11 (#172).
- www.geisten.net/download/geistr/ keeps the last 5 releases: after a release
  the `prune` job deletes older version folders, never `latest`, `install.sh` or
  the new one. `make test` fails while README.md or install.sh name another
  version than the current one (#148).

## 0.3.3

### New

- `geistr serve --max-request=MiB`: the size of one request (default 16, as
  before); a larger one is refused at once, naming the flag (#147).
- Python wheels (macOS arm64, Linux x86_64/arm64) are built by the release
  workflow, listed in `SHA256SUMS`, put next to the archives, and uploaded to
  PyPI when the release has the token (#152).

### Fixes

- Ranking: a model with no quality data that is short of memory (under the
  recommended RAM) ranks behind models that fit and have a measured, imperfect
  quality. On an 8 GB PC `bonsai2-27b-pq2` no longer comes before
  `qwen3-0.6b`; on a 16 GB Mac `bonsai2-27b-pq2` and `qwen38-27b-q4` follow the
  small models (#136).
- A model file that is a symlink is not used (the hash never follows links);
  `geistr catalog` and `geistr run` now say so and how to fix it, and
  `geistr pull` tells when it replaces a link (#151).
- `geistr catalog` tells a damaged model file (right size, other SHA-256: `✗`,
  JSON state `corrupt`) from one that is not this catalog's file (`⟳`, `mismatch`);
  `geistr run` says "is damaged" and `geistr pull` repairs both (#149).
- `install.sh`: a release that is not on the server (`GEISTR_VERSION=…`) is
  reported as "no release …" instead of curl's bare 404 (#148).
- `geistr serve`: SIGTERM and, over HTTP, a client that goes away now end a
  long prompt that is still being processed. Before, the service finished it
  first (minutes for a prompt near the context size) (#146).

## 0.3.2

### Release

- Releases are built on git.geisten.net's runners and published at
  https://www.geisten.net/download/geistr/ (`install.sh` installs from there);
  the Homebrew tap is updated by the release itself. GitHub is a mirror.
- The catalog lists models in ranking order, every download is pinned to a
  commit, and a weekly check compares size and SHA-256.
- Chat file names stay unique within one millisecond.

## 0.3.1

### Engine

- geistlib `d53560d` (from `4afbfcd`). On the CPU each weight is resident
  once: Bonsai 2 27B uses 7.2 GB instead of 13.7, Qwen3.8 27B 15.0 GB instead
  of 27.6 (geistlib#729). A closed CPU model returns its memory: 0.03–0.18 GB
  stay instead of 1.4 (geistlib#733). Outputs are unchanged.

### Decisions (EXPERIMENTAL)

- The numeric reference is always the latest llama.cpp release
  (`tools/llama_oracle.py`, now v0.6.0); the old Prism revision computed
  Gemma 4 differently (geistlib#728).
- The #94 contract is in force: Gemma passes on cpu_x86, cpu_neon and Metal
  at the current engine (#17, #94, `docs/DECISION_EVIDENCE_17.md`).
- A `decision-evidence` workflow records the x86 evidence on demand.

## 0.3.0

### New

- Python wheels (macOS arm64, Linux x86_64/arm64) are built by the release
  workflow, listed in `SHA256SUMS`, put next to the archives, and uploaded to
  PyPI when the release has the token (#152).

- **Your files:** `geistr index <folder>`, then `geistr chat --files <folder>`
  (or `/files` in a chat, `geistr run --files`): each question gets the most
  relevant passages of your text, Markdown and PDF files. Embeddings with
  BitNet-embedding 0.6B, also over HTTP (`/v1/embeddings`, `/api/embed`) (#91).
- **Tool calling** in `geistr serve --http` for Qwen3 models: OpenAI `tools` /
  `tool_calls` and Ollama `tools`, streamed too (#92).
- **Images** for Gemma 4 E2B over HTTP (`image_url` data URLs, Ollama `images`);
  `geistr pull gemma4-e2b` also fetches its vision tower (#92).
- **First run:** `geistr chat` with no model installed suggests one for this
  computer and downloads it on Enter (#89).
- **Threads:** one engine thread per physical core by default, so a shared CPU
  no longer slows geistr down badly; `geistr config threads N` and `--threads N` (#93).

### Engine

- geistlib `4afbfcd` (from `b682ef8`): the context window on a GPU is sized
  from the device's free memory (#88), the thread setting works on x86, and
  cpu_x86 / cpu_neon activation-scale fixes (geistlib#697, #698).

### Fixes

- `geistr serve`: SIGTERM ends the running answer at once; a data folder too
  long for a socket path uses a private runtime folder (#100).
- `geistr catalog` rows align, and fit 120 columns; `geistr config --help`
  lists every key; `geistr decide --help`; one fewer token after Ctrl-C in
  `geistr run`; speeds recorded under a `.gguf` path count only for that
  same file (#100).

### Decisions (EXPERIMENTAL)

- Decisions use an FP32 KV cache, as the independent reference; measured free
  on Apple (#17).
- Apple evidence on the current engine (#17, `docs/DECISION_EVIDENCE_17.md`):
  configuration and the 2 % overhead gate pass; Gemma numerics and the CPU
  lifecycle do not yet (geistlib#728, #729). Still default-off.

## 0.2.0

### geistr chat

- Input history kept between chats, Ctrl-R search, multi-line input
  (Ctrl-J, Alt-Enter, `\`+Enter) and bracketed paste as one message.
- A context meter in `/info`, a notice when old messages leave the window, and a
  resume budget for long conversations.
- `/retry` and `/copy` (OSC 52; `/copy code`).
- Code blocks in a frame with the language, OSC 8 links, word wrap for the
  chat's own lines.
- Looping and degenerate answers are cut; a stopped answer is noted on resume.

### Fixes

- Thinking models (Qwen3): earlier reasoning leaves the history, as their
  templates expect; the second answer was empty (#96). No answer starts with
  the blank lines after `</think>`.
- HTTP streams send their headers once the request is accepted, and SSE
  `: thinking` comments while a model reasons (#99).
- `geistr pull`: a failed update keeps the previous file; a size mismatch
  says so instead of "No error" (#98).
- A code block whose closing fence ends the answer gets its closing rule (#97).
- The GPU window and a model switch in the chat (#86).

### Decisions (EXPERIMENTAL)

- Add EXPERIMENTAL fixed-option request/configuration contracts in
  `geistr_decision.h`: default-off model policy, exact artifact binding,
  strict separate schema-1 configuration and bounded UTF-8 inputs.
- Append a copied decision policy to `geistr_model_opts`; previous option
  sizes work. Ordinary chat defaults and the schema-2 catalog are unchanged.
- Add checked memory SHA-256 for enabled borrowed-memory model policies.
- Add isolated decision scoring, capability/resource observation, cancellation
  and owned result storage with explicit unsupported-mode errors.
- Add `geistr decide`, offline permission inspection and ctypes decision bindings.
- Add independent native token/numerical fixtures. Gemma numerical acceptance
  is blocked by recorded candidate-logit/selection drift at the engine pin;
  no quality or speed eligibility is implied.

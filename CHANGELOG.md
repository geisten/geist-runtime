# Changelog

## Unreleased

### Features

- Thinking can be switched off per model: `geistr config
  qwen3-0.6b.thinking off` (or `geistr config thinking off` for every
  thinking model without its own value), `/think off` and `/think on` in a
  chat, kept by `/save`. Off, the model does not reason at all: the prompt
  ends as its template renders `enable_thinking=false`, so the chat goes
  from `reading` straight to the answer (qwen3-0.6b on an M1 Max CPU,
  `geistr run`: the first word after 0.10–0.14 s instead of 1.6–10.6 s). A
  model that does not think refuses it. `/info` and `geistr catalog`
  (`--json`: `thinking`) show the setting. In the C API:
  `GEISTR_REASONING_THINK_OFF` (#212).

- `geistr serve --http` without a model offers every installed model:
  `/v1/models` and `/api/tags` list them (Open WebUI shows them all), and a
  request's `model` chooses. Only one model is in memory: a request for
  another one unloads the loaded model completely and loads the requested
  one (qwen3-0.6b ⇄ gemma4-e2b: 0.5–0.7 s on an M1 Max, RSS steady over
  20 switches), for small computers. Conversations are processed anew after
  a switch. A model that is not installed gets 404; nothing is
  downloaded. `geistr serve <model>` is unchanged (#195).

### Fixes

- `geistr serve --http`: a request that names another model (`"model":
  "gemma4-e2b"` to a qwen3-0.6b service) gets 404 `model_not_found` instead of
  an answer from the served model. Its id, `id:latest`, its file name or no
  model are accepted as before (#193).
- `geistr pull` refuses a vision tower checkpoint whose header has a negative,
  fractional or huge tensor offset, naming the tensor, instead of asking the
  server for a bogus range. `geistr chat --socket` clamps token counts from
  the service's stats line that are negative or too large: casting them was
  undefined behaviour (#203).
- `geistr serve`: a socket line over `--max-request` is answered
  `{"error":"the request is over the limit of N MiB (geistr serve
  --max-request=MiB)","status":"too_large"}`, as HTTP's 413 says it. Before it
  was `"one JSON object per line"` with the status `invalid`, as bad JSON
  (#207).
- `geistr chat`: a system prompt set or removed with `/system` in a
  conversation is written to its file at once. Before it was written only
  with the next answer, so a chat that ended before it resumed without the
  system prompt `/info` had shown (#205).
- `geistr pull` onto a disk that is too small says so before it downloads:
  `no space left in <folder> (<model> needs N MB, M MB free); free some space
  or use --models`. A write that fails while downloading (the disk filled up
  meanwhile, a file size limit) names the error. Before it was taken for the
  network ("Failure when receiving data from the peer", "Failed writing
  received data") with the hint to resume, which failed the same way (#206).
- `geistr catalog` shows the median of the last ten `geistr bench` runs per
  model and processor, so that it measures what `geistr bench` printed just
  before; a model never benched shows the median of its last ten answers, as
  before for all. Chat and run answers have other lengths than the fixed
  prompt and pulled the catalog's number away from the bench result (125
  tok/s after the bench, 108 in the catalog). The legend says it (#217).

### Changed

- `geistr chat` the first time: instead of one yes/no question, a list of the
  models that can run on this computer, the suggestion first, to choose with
  ↑↓ and Enter (Ctrl-C twice cancels). With models installed but none
  chosen, the only one is used, or one is chosen from a list (in a terminal) or
  named (#189).
- `geistr chat`, `run`, `serve`, `/model` and `index` offer, in a terminal, to
  download a model that is not installed (or outdated, damaged) and go on with
  it; Ctrl-C twice keeps the old message. Without a terminal nothing changes:
  scripts still get the `geistr pull` line (#191).

### Decisions (EXPERIMENTAL)

- Preparation for several questions over one context (#162; the API follows
  in #163): one registry entry per verified profile (artifact, template,
  roles, thinking-off), a common prefix without question or IDs, a cached cut
  before the prefix's last token on a prefill-chunk boundary, and an exact
  prefix check of every full prompt. Single-question prompts are unchanged.
  Native fixtures for both profiles (`*_context.json`).
- Reusable contexts and atomic question batches (#163):
  `geistr_decision_context_open` prefills the context once into an engine
  snapshot. `geistr_decision_context_score_batch` answers 1–32 independent
  questions over it, restoring the snapshot before each. Every question is
  checked before any is scored. Answers are published only when all
  succeed; otherwise there are none, and the failing question is named.
  There is also cancel, info (cached tokens, snapshot bytes, chunk, hashes)
  and close. All of it is new, EXPERIMENTAL API; the existing calls are
  unchanged. It needs `make runtime DECISION=1 DECISION_CONTEXT=1` and the
  model's permission; otherwise every call answers "unsupported".

### Engine

- geistlib `3841fa6` (from `d53560d`): snapshot-backed prefix scoring for
  decisions (geistlib#5, built only with `DECISION_CONTEXT=1`). The range
  also brings AVX2/FMA kernels for the native GEMM on x86 (geistlib#12) and
  Vulkan attention for head_dim 96 (geistlib#739).

## 0.3.4

### Security

- `geistr serve --http` refuses requests from a web page of another site
  (`Origin` header): before, any page could start answers on this computer
  with a simple request (text/plain, no preflight), keeping the engine busy.
  `GEISTR_ORIGINS` allows more origins, as Ollama's `OLLAMA_ORIGINS`. The
  `Host` check no longer takes a name like `127.evil.example` for a loopback
  address (#171).

### Features

- `--files` names only the files that answer: with the new catalog model
  `qwen3-reranker-0.6b` installed (Qwen3-Reranker-0.6B, Q8_0, 639 MB, Apache
  2.0), each paragraph of the 8 nearest passages is judged by the reranker,
  only the passages it finds relevant go before the question (at most 4),
  and `sources:` names only their files. A question the files do not
  answer gets no passages, no `sources:` line and the note `nothing relevant
  in your files`. Without the reranker `--files` works as before and says
  once how to get it. On `scripts/retrieval-bench.py --sources`: precision
  0.95 (was 0.24), recall 0.90 (was 0.90), no file named for 88 % of the
  off-topic questions (was 0 %); about 1.3 s per question on an M1 Max's GPU,
  6 s on its CPU. Same for `/files` and `run --files`; HTTP is unchanged
  (#150).
- `geistr_logits` (C API): the model's logits of given tokens after a
  prompt, without generating; a reranker's yes/no (#150).
- The catalog knows the kind `"reranker"` (`embedding` = 2 in
  `geistr_catalog_entry`): not a chat model, never recommended (#150).
- `scripts/retrieval-bench.py --rerank-gguf … [--pieces] [--lib libgeistr]`
  scores a Qwen3-Reranker GGUF (through llama-server or `geistr_logits`), with
  a leave-one-out threshold row and recall split by questions in the files'
  language and across it (#150).

### Fixes

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
- The macOS binary of 0.3.2 and of the first 0.3.3 build needed macOS 27 (the
  runner's own), so it did not start on macOS 15 or 26, and PyPI refused the
  wheel tag `macosx_27_0`. Releases are built for macOS 15 and later now
  (`minos 15.0`, checked); 0.3.3 was published again that way (#152).

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

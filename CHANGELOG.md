# Changelog

## 0.4.0

### Features

- Experimental: early exit from reasoning (geistlib#236).
  - `geistr decide ... --escalate chat --early-exit [--probe-interval N]
    [--exit-threshold P]` probes the thinking model's answer every N tokens
    (default 128): save the state, prefill the thinking close, read the
    options, restore.
  - The answer stops at the first probe whose top probability reaches P
    (default 0.8). Probes that do not exit leave the generation unchanged.
  - Library: `geistr_chat_probe_set` / `geistr_chat_probe_result`, also with
    `geistr_chats_next`, and the new finish reason
    `GEISTR_FINISH_EARLY_EXIT`.
  - Measured on Bonsai 2 27B, 500 held-out MMLU questions at cap 8192: the
    same accuracy as full reasoning (85.6 %) at 0.23x the generated tokens.
    The paired interval is +/-4.74 pp, so a 2 pp non-inferiority margin is
    not established.
- Experimental: batched chats (geistlib#26).
  `geistr_chats_next(n, chats, pieces, statuses)` gives each of n chats of
  one model its next piece in one call.
  - Chats that need a decode step at the same time share one
    `geist_session_decode_batch` pass. On Bonsai 2 27B on an M1 Max, 8 chats
    decode at about 3.5x the aggregate tokens/s of one.
  - A chat's answer does not depend on which chats share the call. It can
    differ slightly from the chat alone (batched rounding).
  - Models the engine does not batch fall back to one chat at a time,
    identical to `geistr_chat_next`.
- Ubuntu and Debian: a `.deb` per architecture with a GNOME launcher
  (`geistr chat` in a terminal, and "New conversation") and the systemd user
  service `geistr` (`geistr serve --http`, every installed model). Windows:
  how to run geistr in WSL 2 (README).
- Experimental: speculative decoding with a draft model (geistlib#192).
  `geistr_model_opts.draft_model` (CLI: the `GEISTR_DRAFT_MODEL` environment
  variable) loads a small model with the same vocabulary, such as Qwen3.5
  0.8B for Bonsai 2 27B. Greedy chats then draft tokens with it and verify
  them in one pass of the model; the answers stay token for token the same.
  The chat measures ms per token of both ways and decodes plainly while
  speculation loses. On an M1 Max (Bonsai 2 27B, Metal, three alternating
  runs) the gain is small: median +3 % on code, -2 % on prose. Off by default.
- `geistr decide` as a selectable two-stage policy (geistlib#210):
  `--max-tokens N` caps the `--escalate chat` answer (thinking included;
  default 4096 as before, at most 28672). An answer cut there before it names
  an option exits with 3. `--temperature T` makes `--threshold` compare
  softmax(logits / T) over the options, a calibration the caller fitted
  (default 1, unchanged). The diagnostics JSON is now written once, after any
  escalation. It adds `temperature`, `direct_index` and `direct_probabilities`
  (the first stage), `answered_by` (`direct`, `chat`, `model` or `none`),
  `answer_index`, `escalation_ms`, a `chat` object (cap, finish reason,
  input/output tokens, prefill/generation/total ms) and
  `second_top_probability` for `--escalate MODEL`. docs/DECISIONS.md gives
  the measured accuracy/cost curve on Bonsai 2 27B / MMLU and explains how to
  pick a threshold and cap for your own task.

- `geistr decide --threshold P`: below probability P a decision is not
  taken. Without more, it exits with 3 and prints nothing on stdout; with
  `--escalate chat` the same model writes the answer in chat mode (thinking,
  if the model thinks) and the answer is read as one of the options; with
  `--escalate MODEL` the first model is closed and MODEL decides (below P
  again: exit 3). The diagnostics JSON reports `top_probability`, `threshold`
  and `escalation`.

- Decisions without setup: release binaries and wheels are built with the
  decision engine (`DECISION=1` is now the Makefile default), and
  `geistr decide` works without a `--config` file for every chat model.
  Gemma 4 E2B and Bonsai 2 27B keep their verified profiles (bound to the
  artifact's SHA-256, checked against llama.cpp); any other model is decided
  *unverified*: the prompt is rendered from its own chat template family
  (the renderer the chat uses, thinking off where the template thinks), with
  no hashing at open. For Gemma 4 and ChatML that rendering is
  byte-identical to the verified profiles. A `--config` file still replaces
  the defaults. In the C API, an enabled policy without a profile now means
  unverified instead of an error. A short form takes the question and the option IDs as
  plain arguments: `geistr decide gemma4-e2b "Which team?" billing technical
  sales`. On a terminal it also prints each option's probability; piped,
  stdout stays exactly the chosen ID, and the diagnostics JSON on stderr is
  written only when stderr is not a terminal. In a chat, `/decide question |
  option | option` gives the model's choice and each option's probability;
  the first `/decide` reopens the model with its verified profile (the file
  is hashed once) or unverified (no hashing); the conversation moves along. Still EXPERIMENTAL
  (docs/DECISIONS.md).

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

- `install.sh` runs only once it has been downloaded completely (a `curl | sh`
  that breaks off installs nothing), retries downloads and works with `wget`
  when there is no `curl`.

- The Linux release binary checks a pulled model at disk speed: SHA-256
  through OpenSSL (SHA-NI, ARMv8 SHA), which the static build links for
  libcurl anyway, instead of the portable code at ~20 MB/s. Verifying
  bonsai2-27b-pq2 (7.2 GB) took about 6 minutes, now 19 s from a cold disk
  (2.8 s of CPU). A build without OpenSSL keeps the portable code (#254).

- `scripts/retrieval-bench.py --rerank-gguf` (through llama-server) takes
  the exact log-probs of "yes" and "no": when one was not among the top 50
  tokens, the 50th one's stood in, which raised P(yes) of unrelated passages
  (0.0029 instead of 0.0002 on the first piece of #225, "yes" outside the top
  50 for 449 of 480 pieces). geistr_logits was right all along: same token
  ids as llama-server, yes − no within 0.02 of llama-server's on Metal and
  0.08 on the CPU, and the same pieces cross `--files`' 0.2 (#225).
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
- `--files` with the reranker on Metal takes half the time: 0.63 s per
  question instead of 1.28 s on an M1 Max (top 8 chunks in paragraph pieces;
  llama-server on the same GGUF takes 0.62 s). `geistr_logits` on Metal now
  processes a multiple of 32 new tokens and goes over up to 31 cached tokens
  again for that. The engine runs any other count on slower kernels
  (geistlib#35). P(yes) moves by 0.002 at most. The CPU is unchanged (#201).

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
- `geistr decide-batch MODEL --config FILE --shared-context --request-file
  FILE|-` (#164): the questions of one JSON request (a shared context, 1–32
  questions with their options) over one context. stdout gets
  `question_id<TAB>option_id` per question in input order, only when all of
  them succeeded; otherwise nothing, and the failing question is named. The
  request is read bounded (1 MiB) and checked completely before the model is
  loaded. `--context-tokens` (512) and `--snapshot-budget-mib` (256) as in
  the C API. One JSON record on stderr with hashes, backend, cached and
  suffix tokens, restores, snapshot bytes and the times up to the result
  and up to the end. Needs `make geistr DECISION=1 DECISION_CONTEXT=1` and
  the model's permission; `geistr decide-batch --help` says whether the
  build has it.

### Engine

- geistlib `eedaae6` (from `36e335d`) brings
  `geist_session_decode_batch` / `geist_model_decode_batch_max`
  (geistlib#26, experimental): one decode step for several sessions of one
  model in a single pass. On Bonsai 2 27B on an M1 Max, 8 sessions decode at
  3.47x the aggregate tokens/s of one, and reasoning accuracy over 500 MMLU
  questions stays 78.8 % vs 79.0 % unbatched.
  - The runtime does not use the new call yet; batched chats follow
    separately.
  - The range also brings:
    - Vulkan logits read from cached host memory (geistlib#219: sampled
      decode 17.8 -> 75 tok/s on the RTX 2080 Ti);
    - device-routed MoE decode on Vulkan (geistlib#162);
    - a fused Q4_K gate/up made bit-identical to the separate ops
      (geistlib#196);
    - forward/cpu_neon cleanups (geistlib#253).
  - No runtime source change was needed.
- geistlib `1f61d75` (from `e78f9ee`): Metal prefill for Bonsai 2 27B is
  faster — flash attention after llama.cpp's tiling (geistlib#91), PQ2_0
  GEMM on 64x64 tiles with f16 staging (geistlib#77, geistlib#103),
  DeltaNet prefill on simdgroup matrices (geistlib#99), shorter prefill
  command buffers. Bonsai 2 27B on an M1 Max, Metal, FP32 KV, one
  decision with filler contexts: 328 / 664 / 1245 / 2406 tokens 3.64 /
  7.09 / 13.70 / 27.37 s → 2.96 / 5.43 / 9.89 / 19.48 s (88-94 → 111-126
  tok/s); an 80-token decision stays at 0.8-1.0 s; 231 JevBench decisions
  1824 s → 1258 s, same choices as before and as Prism's llama.cpp fork
  (231/231, largest probability difference 0.0012 vs Prism). Decisions
  keep their FP32 KV cache. The range also brings a layer split across two
  backends (geistlib#49, #78), Qwen3-MoE on the CPU backends (geistlib#48),
  sampler repetition control (geistlib#695, all off by default), PQ2_0
  prefill on int8 SDOT / AVX-512 VNNI on the CPUs (geistlib#82, #85), and
  `GEIST_KV_F16=1` (geistlib#113). No runtime source change was needed.
- geistlib `e78f9ee` (from `3841fa6`): Metal runs the fast GEMMs and the
  flash attention for any row count (geistlib#35). Before, a prompt whose
  length was not a multiple of 32 (GEMM) or 8 (attention) took the bounded
  GEMM and the scalar attention for the whole chunk. Bonsai 2 27B on an M1
  Max, Metal: a 191-token decision 3.4 s before, 2.2 s now (192 tokens:
  2.1 s both); 231 JevBench decisions 2240 s → 1929 s (prompts ≤ 300 tokens
  p50 2.39 s → 1.39 s), Gemma 4 E2B 287 s → 202 s (p50 0.24 s → 0.12 s);
  same choices as before for Bonsai (231/231) and as llama.cpp for Gemma
  (231/231) (#230). Decisions keep their FP32 KV cache. The range also brings model families
  as units of shared blocks (geistlib#37), a Q6_K prefill GEMM on Metal
  (geistlib#17), SDOT prefill kernels for Q8_0 and Q4_K on cpu_neon
  (geistlib#7, geistlib#16), tiled int8 GEMMs on x86 (geistlib#18) and a
  parallel team capped at the cgroup CPU quota (geistlib#27).
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

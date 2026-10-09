# Changelog

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

# geistr user guide

geistr runs language models on your own computer. This guide covers every
command, setting and file of the `geistr` app. For a short overview see the
[README](../README.md); for building from source,
[DEVELOPMENT.md](DEVELOPMENT.md).

- [Install](#install)
- [First run](#first-run)
- [Models](#models)
- [One answer: `geistr run`](#one-answer-geistr-run)
- [Chat](#chat)
- [Your files](#your-files)
- [Serve: use the model from other programs](#serve-use-the-model-from-other-programs)
- [CPU, GPU and threads](#cpu-gpu-and-threads)
- [Speed and `geistr bench`](#speed-and-geistr-bench)
- [Settings](#settings)
- [The data folder](#the-data-folder)
- [Environment variables](#environment-variables)
- [Update and uninstall](#update-and-uninstall)
- [Troubleshooting](#troubleshooting)
- [Command reference](#command-reference)

## Install

```sh
curl -fsSL https://www.geisten.net/download/geistr/install.sh | sh
```

The script picks the build for your system (Linux x86_64, Linux arm64, macOS
on Apple Silicon), downloads the latest release, checks it against
`SHA256SUMS` and installs `geistr` to `/usr/local/bin` (with `sudo` if that
folder is not writable). It ends by printing `geistr --version`, and says so
when the folder is not on your `PATH`.

Two variables change what it does:

```sh
curl -fsSL https://www.geisten.net/download/geistr/install.sh | PREFIX=~/.local sh            # to ~/.local/bin, no sudo
curl -fsSL https://www.geisten.net/download/geistr/install.sh | GEISTR_VERSION=v0.3.2 sh      # a given release
```

A release that is not on the server is reported as
`geistr: no release v0.0.1 at …`; releases before 0.3.2 are not there. A
download that does not match `SHA256SUMS` installs nothing.

With Homebrew:

```sh
brew install geisten/tap/geistr
```

Check the install:

```sh
$ geistr --version
geistr 0.3.3 · catalog revision 12 · engine d53560d
```

That is the geistr version, the revision of the built-in model list, and the
engine (geistlib) commit.

**Requirements.** The Linux binaries are fully static and run on any
distribution. The CPU needs x86-64-v3 (AVX2 and FMA: Intel since 2013, AMD
since 2015) or ARMv8.2 with dotprod (Raspberry Pi 5, AWS Graviton 2 and
newer). On an older CPU geistr says which instructions are missing and exits.
macOS needs Apple Silicon and macOS 15 or later.

To build from source, see
[DEVELOPMENT.md](DEVELOPMENT.md#build-geistr-from-source).

## First run

```sh
geistr chat
```

With no model installed, geistr picks the model that suits this computer
best and asks before it downloads:

```
No model yet. For this computer: gemma4-e2b (Gemma 4 E2B · Q4_K_M): fits; speed not known yet
Download it now, 3.1 GB? [Y/n]
```

Enter downloads and verifies it, then the chat starts. `n` prints the two
commands to do it later. When input or output is not a terminal (a script),
nothing is downloaded: geistr prints the `geistr pull …` line and exits with
code 2.

Later, `geistr chat` without a model continues with the model you used last.

## Models

### The catalog

`geistr catalog` lists the models this geistr knows, best suited to this
computer first:

```
↓ gemma4-e2b         Gemma 4 E2B · Q4_K_M                  3.1 GB  ⚙   ·  ⚡   ·
✓ qwen3-0.6b         Qwen3 0.6B · Q8_0                     639 MB  ⚙   52 ███  ⚡   98 ██████
↓ qwen38-27b-q8      Qwen3.8 27B · Q8_0                   29.0 GB  ⚙   ·  ⚡   ·  ⚠ free RAM tight
```

Each row: the state, the id you type, the name and quantization, the
download size, and the speed in tokens per second on ⚙ CPU and ⚡ GPU. Bars
in normal colour are measured on this computer (the median of the last ten
answers); dim ones are the catalog's reference numbers; `·` means no number
yet. Whether a model is installed does not change its place in the list.

| mark | means |
|---|---|
| ✓ | installed and verified |
| ↓ | available: `geistr pull <id>` |
| ⟳ | an installed file that is not this catalog's file (a new geistr brought a new one): `geistr pull` |
| ⟳ … a symlink, not used | the model file is a symbolic link; geistr does not follow links: copy the file or use a hard link |
| ✗ checksum differs | right size, wrong SHA-256: the file is damaged; `geistr pull <id>` repairs it |
| ⚠ RAM tight, ⚠ free RAM tight | it may run, with little memory to spare |
| ✗ too little RAM, disk too full, CPU not supported, format not supported | it will not run here |

Options:

```sh
geistr catalog --installed      # only installed models
geistr catalog --available      # only those not installed
geistr catalog --json           # for scripts
```

`--json` prints `{"schema":1,"models_dir":…,"models":[…]}`; per model `id`,
`name`, `quantization`, `file`, `url`, `sha256`, `bytes`,
`recommended_ram_gib`, `state` (`available`, `unverified`, `installed`,
`mismatch`, `corrupt`), `resource` (`fits`, `limited`, `unavailable`),
`resource_reason` and `tokens_per_s` (`cpu`, `gpu`, or `null`).

The models in catalog revision 12:

| id | model | download | RAM | notes |
|---|---|---|---|---|
| `smollm2-360m` | SmolLM2 360M | 0.4 GB | 2 GB | |
| `qwen3-0.6b` | Qwen3 0.6B | 0.6 GB | 4 GB | thinks; tool calling |
| `qwen35-0.8b` | Qwen3.5 0.8B | 0.8 GB | 4 GB | thinks |
| `bitnet-2b` | BitNet b1.58 2B | 1.2 GB | 4 GB | |
| `gemma4-e2b` | Gemma 4 E2B | 3.1 GB | 8 GB | images (over HTTP) |
| `gemma4-e4b` | Gemma 4 E4B | 5.0 GB | 16 GB | |
| `bonsai2-27b-pq2` | Bonsai 2 27B (ternary) | 7.2 GB | 24 GB | thinks |
| `qwen38-27b-q4` | Qwen3.8 27B, Q4_0 | 16 GB | 32 GB | thinks |
| `qwen38-27b-q8` | Qwen3.8 27B, Q8_0 | 29 GB | 48 GB | thinks |
| `bitnet-embed-0.6b` | BitNet embedding 0.6B | 0.4 GB | 2 GB | embeddings only: [your files](#your-files), `/v1/embeddings` |

The list ships inside geistr, so a new geistr brings new models.
`--catalog FILE` uses another catalog file (JSON, same format as
`models/catalog.json` in the source) for one command.

### Download: `geistr pull`

```sh
$ geistr pull qwen3-0.6b
downloading qwen3-0.6b → ~/.local/share/geisten/models
 47% of 0.6 GB
```

- Downloads go over HTTPS from the URL the catalog pins, into
  `<id file>.part` in the model folder.
- Ctrl-C stops it (exit 130). `geistr pull qwen3-0.6b` again resumes where it
  stopped. A broken network connection fails with a message, keeps the
  `.part`, and the next pull resumes too.
- When it is complete the file is checked against its SHA-256. Only a file
  that matches is installed; anything else is removed (`does not match its
  SHA-256; removed, try again`).
- An installed and verified model is not downloaded again
  (`✓ qwen3-0.6b is installed`).
- `geistr pull gemma4-e2b` also fetches its vision tower
  (`vision_tower.safetensors`, 337 MB, verified the same way), which the
  model needs for images.

`geistr pull` without an id updates every installed model that is marked ⟳
or ✗. The old file stays until the new one is verified. With nothing to do it
says `✓ the installed models are current (catalog revision 12)`.

geistr checks a file's SHA-256 once and then keeps a receipt (in `.verified/`
in the model folder), so `catalog` and `run` do not hash large files every
time. A file that changes is checked again (`verifying qwen3-0.6b …`).

### Your own GGUF files

Anywhere a command takes a model, you can give a path to a `.gguf` file
instead of an id:

```sh
geistr chat ~/Downloads/some-model-Q4_K_M.gguf
```

A path is anything with a `/` or ending in `.gguf`. It is not verified
against the catalog. If its file name is one the catalog lists, geistr uses
that entry's settings (thinking, embedding). The engine supports the model
architectures and quantizations of the catalog models; another file may fail
with `not a supported model or chat format`.

### Another model folder

`--models DIR` uses another folder for one command, for `pull` too:

```sh
geistr pull smollm2-360m --models /Volumes/Big/models
geistr chat smollm2-360m --models /Volumes/Big/models
```

To move everything, set `GEISTEN_HOME` (see [the data folder](#the-data-folder)).

## One answer: `geistr run`

```sh
geistr run qwen3-0.6b "Name three prime numbers."
echo "Summarize: …" | geistr run qwen3-0.6b
geistr run qwen3-0.6b < question.txt > answer.md
```

The words after the model are the prompt; with none, geistr reads it from
stdin (up to 1 MB). The answer goes to stdout, the speed line to stderr.

- In a terminal the answer is shown as Markdown; piped, it is plain text as
  the model wrote it, with no escape codes.
- The system prompt and temperature from the settings apply
  (`geistr config system …`, `geistr config temperature …`).
- Nothing is kept: no conversation file, no history.
- Ctrl-C stops the answer; the exit code is then 130.
- A prompt longer than the model's context is refused
  (`the conversation does not fit the context window`).
- `--files DIR` answers from your files, see [your files](#your-files).
- `--cpu`, `--gpu` and `--threads N` choose the processor for this run.

An answer that starts to repeat itself is stopped there:
`geistr: the answer repeated itself; stopped there`.

## Chat

```sh
geistr chat qwen3-0.6b       # this model
geistr chat                  # the model of the last chat
geistr chat --new            # a new conversation instead of the last one
```

### What you see

```
geistr · qwen3-0.6b on ⚙ cpu   (⚙ CPU · ⚡ GPU)
/ commands · ? shortcuts · Esc stops an answer · Ctrl-C twice exits · geistr config intro off
⚙ >
```

- The first two lines say what runs where. `geistr config intro off` hides
  them; in a terminal narrower than 60 columns the second line is left out.
- The prompt shows the processor: ⚙ CPU, ⚡ GPU.
- From half full on, it shows how full the context is: `⚙ 62% >`, yellow
  from 80 %, red from 95 %.
- While a model loads: `⠋ loading gemma4-e2b · 3.1 GB · 2 s`. While you wait
  for the first word: `⠋ reading · 3 s`, and `thinking` while a thinking
  model reasons.
- After each answer, dim: `42.3 tok/s · 1.8 s` (`geistr config stats off`
  hides it).

Answers are shown as Markdown: headings, lists, quotes, **bold**, *italic*,
`code`, framed code blocks, aligned tables (as one record per row when the
columns do not fit), links you can click (the URL dim after the text), and
LaTeX math as Unicode (`$e^{i\pi}$` → `e^(iπ)`, `\sum`, `x_i`, `\sqrt{x}`).
Prose wraps at words to the terminal's width; code does not.
`geistr config markdown off` shows the text as the model wrote it.
`NO_COLOR=1` turns off all colours and styles.

### Commands

Type `/` for the list; ↑↓ choose, Tab takes, Enter runs, Esc closes it.

| command | does |
|---|---|
| `/model` | what runs now, and the installed models (best fit first) |
| `/model <id or path>` | switch the model; the conversation goes along |
| `/gpu`, `/cpu`, `/auto` | switch the processor; the conversation goes along |
| `/temp` | show the temperature |
| `/temp 0.7` | set it, 0 to 2 (0: always the most likely word) |
| `/system` | show the system prompt |
| `/system Be brief.` | set it; `/system off` removes it |
| `/info` | backend, model, chat format, context used of total (and %), temperature, CPU threads, system prompt |
| `/files ~/Notes` | answers from a folder of your files; `/files off` (or `/files`) stops |
| `/retry` | the last answer again (at temperature 0: this once at 0.7) |
| `/copy` | the last answer (Markdown) to the clipboard |
| `/copy code` | its last code block, without the fences |
| `/save` | keep model, processor, temperature and system prompt for the next chat |
| `/clear` | a new conversation (the system prompt stays) |
| `/help` | the commands and the keys |
| `/exit`, `/quit` | end the chat (or Ctrl-D) |

`/model` with Tab completes the installed model ids. When a switch fails (a
typo, too little memory), the current model stays. On the GPU the current
model is closed first, since two models rarely fit GPU memory together; if
the new one then fails to load, the previous one is opened again.

`/copy` uses the terminal (OSC 52: iTerm2, WezTerm, Ghostty, kitty,
Alacritty, foot, Windows Terminal, tmux; works over SSH), else `pbcopy`,
`wl-copy` or `xclip`. Without any of them it says so.

### Keys

| key | does |
|---|---|
| Enter | send |
| Ctrl-J, Alt/Option-Enter, `\` then Enter | a new line in the message (shown as ↵) |
| Esc (or Ctrl-C) while an answer runs | stop the answer (`[stopped]`) |
| Ctrl-C | clear the line; on an empty line twice within 2 s: exit |
| Ctrl-D | exit (on an empty line; else delete the character under the cursor) |
| ↑ ↓, Ctrl-P Ctrl-N | earlier lines, also from earlier chats |
| → at the end of the line | take the dim hint |
| Ctrl-R | search earlier lines: type to narrow, Ctrl-R for older, Enter takes it, Esc or Ctrl-G cancels |
| Tab | complete a command or model id |
| ← →, Ctrl-B Ctrl-F | move one character |
| Ctrl-A / Home, Ctrl-E / End | start / end of the line |
| Ctrl-U, Ctrl-K | delete to the start / to the end |
| Ctrl-W | delete the word before the cursor |
| Backspace, Delete | delete a character |
| Ctrl-L | clear the screen |
| `?` on an empty line | the shortcuts |

Pasted text with line breaks becomes one message (with ↵), it is not sent
line by line. Keys typed while an answer runs wait for the next prompt.

### History of typed lines

Lines you type are kept in `history` in the data folder (the last 500) and
come back with ↑ and Ctrl-R in later chats. A line that starts with a space
is not kept, as in a shell. `geistr config history off` keeps none.

### Your conversation stays

Close the chat, come back tomorrow: `geistr chat` continues the last
conversation, and the model still knows what was said.

```
↻ 6 · „Compare Mars and Venus in a small table…“ · /clear new
⚡ >
```

The `↻` line shows the number of messages and your last question. It also
says when only part is read again (`resumes the last 12`), and when the last
answer was stopped or cut off.

- Each conversation is a file in `chats/` in the data folder, one JSON
  message per line, written after every answer, readable only by you.
- When you continue one, the next answer writes it to a new file and removes
  the old one: there is one current conversation.
- `/clear` and `geistr chat --new` start a new conversation; the old file
  stays in `chats/` (geistr does not open it again; delete it by hand if you
  like).
- `geistr config resume off`: every chat starts new, and nothing is written,
  neither conversations nor typed lines.
- A chat whose input is not a terminal (`echo … | geistr chat qwen3-0.6b`)
  keeps nothing.

A long conversation stays fast to resume: the model re-reads only its newest
part, about 2000 tokens (`geistr config resume_tokens`, at most a quarter of
the context). The whole conversation stays in the file. The same happens
when you switch the model.

When a conversation outgrows the model's context, the oldest messages are
left out, and the chat says so once:
`↥ 12 oldest messages left out to fit the context (8192 tokens) · /clear starts fresh`.

A small model sometimes repeats itself. geistr stops such an answer, keeps
it up to the repeat and says
`⟲ it repeated itself: kept up to the repeat · /retry or /clear`.

### Switching model or processor mid-conversation

`/model gemma4-e2b` or `/gpu` loads the new model or processor and sends it
the conversation so far (its newest part, as on resume). Use a small model
to think out loud, then a bigger one for the final answer, without
explaining everything again.

### Thinking models

Qwen3, Qwen3.5, Qwen3.8 and Bonsai 2 reason before they answer. While they
do, the chat shows `⠋ thinking · N s`. The reasoning itself is not shown,
not kept in the conversation and not sent back to the model; only the
answer is. Over HTTP it is left out of the answer too.

### Embedding models do not chat

`geistr chat bitnet-embed-0.6b` refuses: the model makes vectors for
[your files](#your-files) and `/v1/embeddings`, it does not write text.

## Your files

geistr can answer from a folder of your own text, Markdown and PDF files.
Everything stays on your computer.

```sh
geistr pull bitnet-embed-0.6b      # the embedding model, 428 MB, once
geistr index ~/Notes               # read the folder
geistr chat --files ~/Notes        # or /files ~/Notes in a chat
geistr run qwen3-0.6b --files ~/Notes "When is the shed key needed?"
```

For each question, geistr finds the four passages of your files closest to
it and puts them in front of the question. The answer is followed by where
they came from:

```
  sources: garden/shed.md, notes/2026-05.md
```

**What is read.** Files ending in `.txt`, `.md`, `.markdown`, `.rst`,
`.org`, `.text` and `.pdf`, in the folder and all folders below it, up to
8 MB each. Hidden files and folders (names starting with `.`) are left out,
and symbolic links are not followed. PDFs need `pdftotext` (macOS:
`brew install poppler`; Linux: the `poppler-utils` package).

`geistr index` says which files it leaves out and why:

```
left out scans/receipt.pdf: no text in it (a scanned PDF?)
left out book.pdf: pdftotext could not read it, or is missing (macOS: brew install poppler; Linux: poppler-utils)
~/Notes: 42 files, 310 chunks (12 new)
```

**Updates.** The index is brought up to date whenever you use the folder
(`index`, `--files`, `/files`): files are compared by SHA-256, so only new
and changed files are read again, and removed files leave the index. You
need `geistr index` only to do this ahead of time. Ctrl-C stops indexing.

The index of each folder is a file in `index/` in the data folder, readable
only by you. It holds the text of your files.

## Serve: use the model from other programs

`geistr serve` keeps a model loaded and answers other programs: over a Unix
socket, and with `--http` over the OpenAI and Ollama APIs.

```sh
geistr serve gemma4-e2b --http          # socket and http://127.0.0.1:11434
geistr serve gemma4-e2b                 # the socket only
```

It prints `geistr: serving gemma4-e2b on …/geistr.sock and
http://127.0.0.1:11434 (2 conversations)` and runs until Ctrl-C or SIGTERM;
then it removes the socket and prints `geistr: service stopped`.

| option | does |
|---|---|
| `--http` | also serve HTTP on 127.0.0.1:11434 (Ollama's port) |
| `--http=ADDR:PORT` | on that address, e.g. `--http=127.0.0.1:18434`, `--http=[::1]:11434`, `--http=0.0.0.0:11434` |
| `--socket=PATH` | the Unix socket's path (default: `geistr.sock` in the data folder) |
| `--chats N` | conversations kept in memory, 1 to 64 (default 2) |
| `--max-request=MiB` | largest request, 1 to 1024 (default 16) |
| `--cpu`, `--gpu`, `--threads N` | the processor, as everywhere |

### HTTP

```sh
curl http://127.0.0.1:11434/v1/chat/completions \
  -d '{"messages":[{"role":"user","content":"Hi"}]}'
```

```python
from openai import OpenAI
client = OpenAI(base_url="http://127.0.0.1:11434/v1", api_key="unused")
reply = client.chat.completions.create(model="gemma4-e2b",
                                       messages=[{"role": "user", "content": "Hi"}])
print(reply.choices[0].message.content)
```

In Open WebUI, add `http://127.0.0.1:11434` as an Ollama connection, or
`http://127.0.0.1:11434/v1` as an OpenAI one. Editor plugins and other tools
that speak either API work the same way. The `model` field of a request is
ignored: the service has one model.

| endpoint | |
|---|---|
| `POST /v1/chat/completions` | OpenAI: `messages`, `temperature`, `max_tokens` / `max_completion_tokens`, `stop`, `stream` (SSE, `stream_options.include_usage`), `tools` / `tool_choice` |
| `POST /v1/embeddings` | OpenAI: `input` (a text or a list), `encoding_format` (`float`, `base64`); with an embedding model |
| `GET /v1/models` | the one model |
| `POST /api/chat` | Ollama: `messages`, `stream` (NDJSON, the default), `options.temperature`, `options.num_predict`, `options.stop`, `tools`, `images` |
| `POST /api/embed` | Ollama: `input` (a text or a list); with an embedding model |
| `GET /api/tags` | the model |
| `GET /api/version` | the geistr version |
| `GET /` | `geistr is running (OpenAI and Ollama APIs)` |

**Conversations are cached.** Clients send the whole conversation with every
request. The service keeps the last `--chats` conversations in memory and
continues the one that matches best, so only the new part is processed. A
third conversation with `--chats 2` replaces the least recently used one;
when that one comes back, it is processed whole, which takes longer but
gives the same answer.

**Streaming.** With `"stream": true` the OpenAI endpoint sends SSE chunks and
ends with `data: [DONE]`; the Ollama endpoint streams NDJSON unless
`"stream": false`. While a thinking model reasons, the stream gets comment
lines (`: thinking`) so clients do not time out.

**Tool calling** works with Qwen3 models (`qwen3-0.6b` and the other
Qwen3.x): send `tools`, get `tool_calls` back (`finish_reason: tool_calls`;
streamed too), send the results as `role: tool` messages, and the model
answers with them. `tool_choice: "none"` turns tools off for a request. A
model without a tool format refuses `tools` (400).

**Images** work with Gemma 4 E2B (`gemma4-e2b`, after `geistr pull
gemma4-e2b` fetched its vision tower). Send an `image_url` part with a
`data:` URL (OpenAI) or `images` (Ollama): PNG, JPEG or BMP, one per message.
The image goes with the last message, and the conversation keeps it for
follow-up questions. Other models refuse images (400).

**Embeddings**: `geistr serve bitnet-embed-0.6b --http` answers
`/v1/embeddings` (the OpenAI SDK's `embeddings.create` works) and
`/api/embed` with unit-length vectors of 1024 numbers, multilingual. For
retrieval, put a one-line task before a query (`Instruct: Given a question,
retrieve passages that answer the question\nQuery: …`) and nothing before
documents. An embedding model does not chat and a chat model does not embed
(400).

**Errors** come as JSON in each API's format: 400 for a bad request, a
conversation larger than the context (OpenAI code
`context_length_exceeded`), a model that cannot do tools or images; 403 for
the checks below; 404 for an unknown path; 405 for a wrong method; 413 for a
request over `--max-request` (it names the flag and the limit); 500 when the
engine fails.

### Security

There is no authentication. Keep that in mind before you open the port.

- By default geistr listens on loopback (127.0.0.1) only.
- On loopback it answers only requests addressed to this computer: the
  `Host` header must be `localhost`, a `127.x.y.z` address or `::1`. This
  stops DNS rebinding (403 `Host is not this computer`).
- A web page in a browser may use it only if it comes from this computer too
  (`Origin` check). A page of another site cannot start answers (403 `a web
  page of another site (Origin)`). To allow a web UI on another origin, list
  it: `GEISTR_ORIGINS=https://ui.example,https://other.example` (or `*` for
  any), as Ollama's `OLLAMA_ORIGINS`. Clients that are not browsers (curl,
  SDKs, servers) send no `Origin` and are not affected.
- `--http=0.0.0.0:11434` makes the API reachable for everyone who can reach
  the computer. geistr warns:
  `geistr: warning: the HTTP API on 0.0.0.0:11434 has no authentication: whoever reaches it can use the model`.
- One request is answered at a time; others wait. A client that disconnects
  stops its answer, also while a long prompt is still being read. A client
  that sends or reads nothing for 30 seconds is dropped.

### The Unix socket

The socket is the owner's alone (mode 0600). `geistr chat --socket` chats
through it, with the same chat as above; the model and the processor belong
to the service, so `/model`, `/gpu`, `/cpu` and `/auto` say so instead.

```sh
geistr serve qwen3-0.6b                 # terminal 1
geistr chat --socket                    # terminal 2
geistr chat --socket=/tmp/my.sock       # another path
```

The default path is `geistr.sock` in the data folder. A socket path can hold
only about 100 bytes; when the data folder's path is longer, geistr uses
`$XDG_RUNTIME_DIR` or `$TMPDIR` instead, if that folder is yours alone.
`geistr serve` refuses to start when a service already answers on the path.

The protocol is one JSON object per line: a client connects, sends one
request line, reads lines until the last, and the connection closes.

```sh
# Linux; on macOS the socket is in ~/Library/Application Support/geisten/
echo '{"op":"info"}' | nc -U ~/.local/share/geisten/geistr.sock
# {"model":"gemma4-e2b","backend":"metal","chat_format":"gemma4","context":8192,"chats":2}

echo '{"op":"chat","messages":[{"role":"user","content":"Hi"}]}' | nc -U ~/.local/share/geisten/geistr.sock
# {"part":"answer","text":"Hello"}
# …
# {"done":true,"finish":"stop","input_tokens":14,"context_tokens":52,"output_tokens":7,"prefill_ms":40.1,"generation_ms":95.3,"total_ms":136.2}
```

A chat request may also carry `"temperature"` (0 to 2) and `"max"` (answer
tokens; 0: the rest of the context). On failure the last line is
`{"error":"…","status":"…"}`. The details are in
[`tools/geistr/service.h`](../tools/geistr/service.h).

## CPU, GPU and threads

geistr runs a model on the CPU or the GPU: Metal on macOS, Vulkan on Linux.

| | for one run | in a chat | remembered |
|---|---|---|---|
| CPU | `--cpu` | `/cpu` | `geistr config processor cpu` |
| GPU | `--gpu` | `/gpu` | `geistr config processor gpu` |
| geistr chooses | (default) | `/auto` | `geistr config processor auto` |

On the CPU, geistr uses one thread per physical core on Linux (not per
hyper-thread: the engine slows down badly when another program takes cores);
on macOS the engine chooses. `--threads N` sets it for one run,
`geistr config threads N` for good, `geistr config threads 0` goes back to
the default. `/info` shows the number in a chat.

## Speed and `geistr bench`

Every complete answer records its speed in `speed.tsv` in the data folder.
`geistr catalog` shows the median of the last ten answers per model and
processor, measured with this engine.

```sh
geistr bench                     # every installed model, on ⚙ and ⚡ (where there is a GPU)
geistr bench qwen3-0.6b          # just this one
geistr bench --compare           # the last two engine versions side by side
geistr bench --compare d53560d   # this engine against the newest other one
```

`bench` gives each model the same short task (128 tokens), records the
speed, and then prints the installed models as `catalog --installed` would.
Measure on an idle computer: other programs slow the CPU numbers a lot.

`bench --compare [A [B]]` compares the bench speeds of two engine commits
(by default the one measured last and the one before), per model and
processor, with the change in % (▲ faster, ▼ slower). A `⚠` marks runs that
differ by more than 25 %: a busy machine; measure again.

## Settings

Settings are remembered between runs.

```sh
geistr config                         # all settings, and the file they are in
geistr config temperature             # one value
geistr config temperature 0.7         # set it
geistr config system ""               # back to the default (empty)
```

| key | default | values | what it does |
|---|---|---|---|
| `model` | (empty) | an id or a `.gguf` path | the model for `geistr chat` without one; set by every chat |
| `processor` | `auto` | `auto`, `cpu`, `gpu` | where models run |
| `temperature` | `0` | 0 to 2 | sampling; 0 is always the most likely word |
| `system` | (empty) | text | the system prompt for every new chat and every `run` |
| `markdown` | `on` | `on`, `off` | Markdown rendering in the terminal |
| `stats` | `on` | `on`, `off` | the speed line after each answer |
| `intro` | `on` | `on`, `off` | the two lines at the start of a chat |
| `resume` | `on` | `on`, `off` | continue the last conversation; off: keep nothing |
| `history` | `on` | `on`, `off` | keep typed lines between chats |
| `resume_tokens` | `2048` | 0 to 1000000 | what a resumed conversation re-reads at most |
| `threads` | `0` | 0 to 1024 | CPU threads; 0: geistr chooses |

A value with spaces needs no quotes: `geistr config system Answer in German.`
An empty value (`""`) resets a key to its default. In a chat, `/save` writes
the current model, processor, temperature and system prompt.

The file is `geistr.conf`, plain `key = value` lines; you can edit it by
hand. Lines starting with `#` are comments; an unknown or invalid line is
reported (`geistr: …/geistr.conf: ignored foo`) and skipped.

## The data folder

Everything geistr keeps is in one folder per user:

| system | data folder | settings file |
|---|---|---|
| macOS | `~/Library/Application Support/geisten/` | in the data folder |
| Linux | `~/.local/share/geisten/` (or `$XDG_DATA_HOME/geisten/`) | `~/.config/geisten/geistr.conf` (or `$XDG_CONFIG_HOME/geisten/`) |
| with `GEISTEN_HOME` | `$GEISTEN_HOME/` | in the data folder |

```
geisten/
├── models/                     the model files (--models replaces this folder)
│   ├── Qwen3-0.6B-Q8_0.gguf
│   ├── vision_tower.safetensors   Gemma 4 E2B's vision tower
│   ├── *.part                  unfinished downloads
│   └── .verified/              receipts of verified files
├── chats/                      conversations, <time>-<pid>.jsonl
├── history                     typed lines
├── index/                      your files' indexes, one per folder
├── speed.tsv                   measured speeds
├── geistr.conf                 settings (macOS, GEISTEN_HOME)
└── geistr.sock                 while geistr serve runs
```

What is personal is readable only by you: `chats/` and `index/` (0700),
conversations, history, indexes and the socket (0600). Model
files are public downloads and are not restricted. Nothing in the folder is
sent anywhere.

The folder is shared with the geisten app: `geistr chat` without a model and
without a `model` setting uses the app's selected model (`selected`).

To start clean for a test, point `GEISTEN_HOME` at an empty folder; to keep
the models, add `--models` with the old model folder:

```sh
GEISTEN_HOME=$(mktemp -d) geistr chat qwen3-0.6b --models ~/.local/share/geisten/models
```

## Environment variables

| variable | for |
|---|---|
| `GEISTEN_HOME` | the data folder (models, chats, settings, everything); `GEIST_HOME` is the old name and still works |
| `XDG_DATA_HOME`, `XDG_CONFIG_HOME` | Linux: where the data folder and the settings go, when `GEISTEN_HOME` is not set |
| `HOME` | the default data folder; without `HOME` or `GEISTEN_HOME` geistr needs `--models` and keeps no settings |
| `NO_COLOR` | set to anything: no colours, styles or spinners |
| `GEISTR_ORIGINS` | `geistr serve --http`: web origins allowed besides this computer's (comma separated, or `*`) |
| `XDG_RUNTIME_DIR`, `TMPDIR` | where the socket goes when the data folder's path is too long |
| `GEISTR_VERSION` | `install.sh`: the release to install (default: the latest) |
| `PREFIX` | `install.sh`: the install prefix (default `/usr/local`, the binary goes to `$PREFIX/bin`) |

## Update and uninstall

**Update** with `brew upgrade geistr`, or run the install command again. Your
models, conversations, settings and indexes stay; nothing is verified or
indexed again. The model list comes with geistr, so after an update
`geistr catalog` may mark an installed model with ⟳ (the catalog now lists
another file). `geistr pull` fetches those; the old file stays until the new
one is verified.

**Uninstall** with `brew uninstall geistr`, or delete the binary:

```sh
sudo rm /usr/local/bin/geistr        # or ~/.local/bin/geistr with PREFIX=~/.local
```

From a source build: `make uninstall` (with the same `PREFIX` as for
`make install`). Then remove the data folder (models, conversations,
indexes) and, on Linux, the settings, if you want them gone:

```sh
rm -rf ~/.local/share/geisten ~/.config/geisten/geistr.conf     # Linux
rm -rf ~/"Library/Application Support/geisten"                   # macOS
```

The data folder is shared with the geisten app; delete it only if you do
not use the app.

## Troubleshooting

| message | what to do |
|---|---|
| `geistr: unknown model X (see geistr catalog)` | a typo; `geistr catalog` lists the ids |
| `geistr: X is not installed; geistr pull X` | download it |
| `geistr: X is damaged (its checksum differs from the catalog's); geistr pull X` | the file changed on disk; pull repairs it |
| `geistr: X does not match the catalog; geistr pull X` | a new geistr lists another file for this model; pull fetches it |
| `geistr: X is a symlink; geistr does not follow links: copy the file or use a hard link` | as it says; `geistr pull` also replaces the link (the link's target is left alone) |
| `geistr: download failed: …; run geistr pull X again to resume` | the network or the disk failed; pull again |
| `geistr: X does not match its SHA-256; removed, try again` | the download was damaged; pull again |
| `geistr: X is larger/smaller than the … bytes the catalog gives; removed, the catalog may be out of date` | the file on the server changed; update geistr |
| `geistr: invalid partial download …part; remove it and retry` | delete that `.part` file |
| `geistr: which model? geistr chat <model> (see geistr catalog)` | no model given and none remembered |
| `geistr: X is an embedding model: …, but does not chat` | use a chat model; the embedding model is for files and `/v1/embeddings` |
| `geistr: the conversation does not fit the context window` | the prompt is too long for the model; shorten it |
| `geistr: cannot open …` / `not a supported model or chat format` | the `.gguf` is not one the engine runs |
| `geistr: no model folder: set HOME or GEISTEN_HOME, or use --models` | run with a home folder, or give the folders |
| `geistr: no service on …/geistr.sock (geistr serve <model>)` | start `geistr serve` first, or give the same `--socket=PATH` to both |
| `geistr: a service already answers on …` | another `geistr serve` runs; stop it or use another `--socket` |
| `geistr: cannot listen on 127.0.0.1:11434: Address already in use` | Ollama or another server has the port; `--http=127.0.0.1:18434` |
| `geistr: this CPU lacks AVX2 and FMA …` / `… ARMv8.2 with dotprod …` | this CPU is too old for the release builds |
| `left out …: pdftotext could not read it, or is missing` | install poppler (see [your files](#your-files)) |
| `(the files could not be searched: asked without them)` | the embedding model failed for this question; the question went without your files |
| `/copy: no clipboard here …` | use a terminal with OSC 52, or install `wl-copy` or `xclip` |
| `geistr: empty prompt` | `geistr run` got no prompt, neither as words nor on stdin |

Slow answers on the CPU: check that no other program keeps the cores busy,
and try `--threads` with the number of physical cores. On a computer with a
GPU, try `--gpu`. `geistr catalog` shows what to expect on this computer.

A model that is marked ⚠ may run but swap; one marked ✗ will not run. Pick
one higher up in `geistr catalog`.

## Command reference

| command | does |
|---|---|
| `geistr chat [<model>] [--new] [--files DIR]` | chat; without a model the last one |
| `geistr chat --socket[=PATH]` | chat with a running `geistr serve` |
| `geistr run <model> [prompt…] [--files DIR]` | one answer to stdout; the prompt from stdin if none |
| `geistr catalog [--installed \| --available] [--json]` | the models, their state, fit and speed |
| `geistr pull <id>` | download, resume, verify |
| `geistr pull` | update the installed models to this catalog |
| `geistr index <folder>` | index your files for `--files` |
| `geistr serve <model> [--http[=ADDR:PORT]] [--socket=PATH] [--chats N] [--max-request=MiB]` | the model as a service |
| `geistr config [key [value]]` | settings |
| `geistr bench [model…]` | measure tokens/s on CPU and GPU |
| `geistr bench --compare [A [B]]` | two engine versions' bench speeds side by side |
| `geistr --version` | geistr, catalog revision, engine commit |
| `geistr --help` | the usage |

`<model>` is a catalog id or a path to a `.gguf` file. Options may come
anywhere on the line:

| option | |
|---|---|
| `--cpu`, `--gpu` | the processor for this run |
| `--threads N` | CPU threads for this run (1 to 1024) |
| `--models DIR` | another model folder |
| `--catalog FILE` | another catalog file |
| `--new` | `chat`: a new conversation |
| `--files DIR` | `chat`, `run`: answers from your files |

Exit codes: 0 ok, 1 error, 2 usage (a wrong command or value), 130 stopped
with Ctrl-C.

`geistr decide` and `catalog --decision-config` belong to an experimental
feature that is off by default and not in the release builds; see
[DECISIONS.md](DECISIONS.md).

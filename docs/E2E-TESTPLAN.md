# geistr end-to-end test plan

A manual (human) or agent-driven test of the shipped apps: the `geistr` CLI,
its install paths, the chat, the service APIs and the Python package. It
complements the automated suites (`make test`, `make test-real`,
`make test-geistr`, `make test-python`). Those pin behaviour in a pty with a
tiny model. This plan checks what a user sees, on real terminals, real models
and real hardware, before a release or after larger changes.

Each case has an **ID**, **steps**, the **expected** result and, where it can
be checked mechanically, an **agent check**. A run records pass/fail per ID
(see [Report](#report)).

## Scope and environments

| | Minimum | Full release run |
|---|---|---|
| Platforms | one of macOS arm64 or Linux x86_64 | macOS arm64, Linux x86_64, Linux arm64 (Raspberry Pi 5) |
| Terminals | the platform's default | Terminal.app, iTerm2, GNOME Terminal, kitty or WezTerm, tmux, an SSH session |
| Processors | CPU | CPU and GPU (Metal on macOS, Vulkan on Linux) |
| Models | `smollm2-360m`, `qwen3-0.6b` | add `gemma4-e2b` (GPU), one model that does not fit (⚠/✗) |
| Install | `install.sh` | `install.sh`, Homebrew, built from source |

Use a **fresh data folder** for the run, so it touches neither your own chats
nor your settings (`geistr config` writes them):
`export GEISTEN_HOME=$(mktemp -d)`. Keep models shared to save downloads with
`--models ~/.local/share/geisten/models` (Linux) or the macOS models folder.

## How an agent drives the interactive chat

The chat needs a real terminal. An agent uses tmux: it types with
`send-keys`, waits for the prompt and reads the screen with `capture-pane`.

```sh
tmux new-session -d -s t -x 100 -y 40 "geistr chat qwen3-0.6b --new; sleep 600"
type() { tmux send-keys -t t -l "$1"; }                    # literal text
key()  { tmux send-keys -t t "$@"; }                       # Enter, Escape, C-r, C-j, M-Enter …
screen() { tmux capture-pane -t t -p; }                    # what is shown (add -e for colours)
wait_for() { for i in $(seq 1 180); do screen | grep -qE "$1" && return; sleep 1; done; return 1; }
type 'Say hi.'; key Enter; wait_for 'tok/s'
```

In zsh define `tmux` helpers as functions (`T() { tmux -L e2e "$@"; }`), not as a
variable holding the command and its flags: zsh does not split `$T`.

Rules that keep agent runs reliable:

- **Wait for state, not for time.** Before the next step, wait for the prompt
  (`⚙ >`, `⚡ >`), the speed line (`tok/s`) or a message.
- **Don't type into a running answer** unless the case tests type-ahead.
- **Send long messages as a paste**
  (`key -l $'\e[200~…\e[201~'`, then `key Enter`): a terminal paste is drawn
  once.
- **Look at it.** For visual cases (Markdown, colours, wrapping) attach a
  screenshot, or `capture-pane -e` rendered to an image (asciinema and agg,
  or the ANSI turned into HTML and shot with a headless browser), and judge
  it as a human would.
- **Watch the load.** A busy CPU slows geistr's threads badly (#93); note the
  load average in the report, and re-run timing cases on an idle machine.
- **Wait for the idle prompt**, not for a count of lines: `/clear` and
  scrolling change counts. Idle means the last non-empty line is
  `^(⚡|⚙)( NN%)? >$`.
- **Ports:** `--http` defaults to 11434, the Ollama port. Use `--http=127.0.0.1:18434`
  so a running Ollama is not the one answering.
- **One tmux server per tester** (`tmux -L <name>`), when several agents test
  at once, so they don't close each other's sessions.
- **Short paths for the service:** a Unix socket path holds about 100 bytes,
  so keep `GEISTEN_HOME` short for section H, or pass a short `--socket=`.
- **`/copy` evidence:** run the chat under `script -q -f -c '…' out.log` and
  decode the OSC 52 payload (`\e]52;c;<base64>\a`) from the log; tmux's
  own clipboard capture is unreliable.

## A. Install, update, version

| ID | Steps | Expected | Agent check |
|---|---|---|---|
| A1 | `curl -fsSL https://www.geisten.net/download/geistr/install.sh \| PREFIX=$T sh` | downloads the latest release, checks `SHA256SUMS`, installs `$T/bin/geistr`; a note if `$T/bin` is not on PATH | `$T/bin/geistr --version` prints `geistr X.Y.Z · catalog revision N · engine abcdef0` |
| A2 | same with `GEISTR_VERSION=` the current release; then with `v0.0.1` | installs exactly that release; an unknown one: "geistr: no release v0.0.1 at …", exit 1, nothing installed (#148) | `--version`; exit code, no `curl:` text |
| A3 | `brew install geisten/tap/geistr`, later `brew upgrade geistr` | installs or upgrades to the latest release | `brew test geistr` passes; `--version` matches the release |
| A4 | install.sh against a release whose archive was altered (or a test server with a wrong `SHA256SUMS`) | "does not match SHA256SUMS; nothing installed", exit ≠ 0 | target binary unchanged |
| A5 | run the binary on an unsupported CPU (x86-64 without AVX2, or ARM without dotprod) | geistr says the CPU is unsupported and exits; no crash | exit code ≠ 0, message on stderr |
| A6 | from source: `make geistr && make install PREFIX=$T` and `make uninstall PREFIX=$T` | builds with the pinned geistlib, installs, uninstalls cleanly | file present, then absent |

## B. Catalog and downloads

| ID | Steps | Expected | Agent check |
|---|---|---|---|
| B1 | `geistr catalog` on a fresh model folder | every model listed with ↓, size, RAM fit (⚠/✗ where it does not fit), ⚙/⚡ speeds as dim reference values | exit 0; 9+ rows |
| B2 | `geistr catalog --json` | schema 1: `schema`, `models_dir`, per model `id`, `sha256`, `state`, `resource`, `tokens_per_s` | `jq '.schema==1 and (.models\|length)>0'` |
| B3 | `geistr pull smollm2-360m` | progress, verify, ✓ in `geistr catalog` | `catalog --installed` lists it |
| B4 | interrupt `geistr pull qwen3-0.6b` with Ctrl-C halfway, run it again | it resumes (doesn't restart at 0), verifies | the `.part` file grew; final ✓ |
| B5 | corrupt a byte of an installed model file | `catalog` marks it `✗ checksum differs (damaged file?)` (JSON state `corrupt`; a file of another size is `⟳`/`mismatch`); `run` refuses with "is damaged …; geistr pull …" (#149) | exit 1 |
| B6 | `geistr pull` with no id after a catalog update that replaced a file | the ⟳ model is downloaded again; the old file stays until the new one is verified | — |
| B7 | `geistr catalog --installed`, `--available` | only those rows | — |
| B8 | `geistr pull qwen3-0.6b`, then cut the network halfway (Wi-Fi off, or `sudo ifconfig en0 down` / `ip link set … down`), then on again and pull again | a clear error and exit 1 (not a hang: it gives up within about a minute); the `.part` stays; the second pull resumes and verifies | exit code, time to the error, `.part` grew |
| B9 | pull into a folder on a disk that is too small (macOS: `hdiutil create -size 200m -fs APFS -volname small small.dmg` and attach; Linux: `mount -t tmpfs -o size=200m tmpfs /mnt/small`): `geistr pull smollm2-360m --models /Volumes/small` | "no space left" (or the like), exit 1; no ✓ in `catalog`; nothing half-written left that `run` would load | exit code, `catalog --models …` |

## C. One answer: `geistr run`

| ID | Steps | Expected | Agent check |
|---|---|---|---|
| C1 | `geistr run qwen3-0.6b "Name three prime numbers."` | the answer on stdout, the speed line on stderr | exit 0; stdout has digits |
| C2 | `echo "Summarize: …" \| geistr run qwen3-0.6b` | the prompt comes from stdin | exit 0 |
| C3 | Ctrl-C during the answer | it stops, exit 130 | exit code 130 |
| C4 | `geistr run nope hi`; `geistr fly`; `geistr run tiny hi` (not installed) | clear message: unknown model / usage / "not installed; geistr pull tiny"; exit 1, 2, 1 | exit codes |
| C5 | `geistr run path/to/model.gguf "Hi"` | a `.gguf` path works like an id | exit 0 |
| C6 | pipe the output: `geistr run qwen3-0.6b "Write **bold**" \| cat` | plain text, no escape codes, Markdown as written | no `\x1b` in stdout |
| C7 | `geistr run qwen3-0.6b --cpu "Hi"` and `--gpu "Hi"` (a GPU machine); on a machine without a GPU `--gpu` | each answers on that processor (the speed line differs: Metal ≈ 3–4× the CPU on an M1 Max); without a GPU: a clear message or the CPU, no crash | the two speed lines |

## D. The chat: looks and answers

Start: `geistr chat qwen3-0.6b --new` in a terminal of 100 × 40.

| ID | Steps | Expected | Agent check |
|---|---|---|---|
| D1 | start | intro: `geistr · qwen3-0.6b on ⚙ cpu (⚙ CPU · ⚡ GPU)` and the key line; prompt `⚙ >` | screen |
| D2 | ask for a list, **bold**, *italic*, `code`, a table, `$e^{i\pi}$`, a link `[docs](https://example.com)`, a fenced code block | rendered: bullets, styles, an aligned table, `e^(iπ)`, the link text underlined and clickable (OSC 8) followed by the URL dim; the code block framed with `── lang ──`, a `│ ` gutter and `──` | screenshot; `capture-pane -e` shows `\e]8;;https://example.com` |
| D3 | a long prose answer | wraps at words to the terminal width; code lines are not re-wrapped | no line wider than the terminal |
| D4 | while waiting for the first word | a dim `⠋ reading · N s` (or `thinking` for a thinking model) | screen |
| D5 | after each answer | `NN.N tok/s · N.N s` dim | screen |
| D6 | fill the context past 50 % (a long paste; or a small window in a test build with `GEISTR_TEST_CONTEXT=512`) | the prompt shows `⚙ 62% >`, yellow from 80 %, red from 95 %; `/info` shows `context N of M tokens (P %)` | screen |
| D7 | Esc during an answer | it stops, `[stopped]`, the prompt returns | screen |
| D8 | Ctrl-C on an empty line, twice | "Ctrl-C again to exit", then the chat ends; the terminal is normal again (echo, line editing) | after exit, `stty -a` shows `icanon echo` |
| D9 | `NO_COLOR=1 geistr chat …` | no colours or styles | no `\e[` SGR in `capture-pane -e` |
| D10 | resize the terminal between answers (wider, narrower) | the next answer and the editor use the new width | — |
| D11 | 40 and 48 columns | the chat's own lines (intro, `?`, `/help`, `/info`, hints) wrap at words; the intro's second line is dropped below 60; `?` lists one key per line | no line wider than the width |
| D12 | a thinking model (`qwen3-0.6b`): ask "What is 17 × 23?" twice | `⠋ thinking · N s` while it reasons; the reasoning itself is not shown; the answer comes; the second question works and the first answer's reasoning is not sent again (`/info` context grows only by the visible turns) | screen, `/info` before and after |

## E. The line editor

| ID | Steps | Expected | Agent check |
|---|---|---|---|
| E1 | type `/` | the command list under the line; ↑↓ choose; Tab takes; Enter runs; Esc closes | screen |
| E2 | `/model ` then Tab | the installed models, best fit for this computer first | screen |
| E3 | ↑ after a restart of the chat | the last line of the previous chat | screen |
| E4 | Ctrl-R, type part of an earlier line | `search: …` under the line, the match in it; Ctrl-R again → older; Enter takes it; Esc restores the line | screen |
| E5 | a line starting with a space, then restart and ↑ | it is not in the history | `$GEISTEN_HOME/history` lacks it |
| E6 | Ctrl-J, Alt/Option-Enter, and `\` + Enter in the middle of a message | each adds a line break (`↵`); Enter sends; the model gets the breaks | the chat file has `\n` in the message |
| E7 | paste three lines (bracketed paste) | one message with `↵`, not three | the chat file has one user message |
| E8 | type ahead while an answer runs, including Enter | the keys appear at the next prompt; Enter sends (it is not a line break) | screen |
| E9 | UTF-8: type `Grüße 👋`, move with ←→, delete with Backspace | whole characters move and vanish; the cursor stays aligned | screen |
| E10 | `?` on an empty line | the shortcuts | screen |
| E11 | type `one two three`, then Ctrl-A, Ctrl-E, Ctrl-W, Ctrl-U, type again, Ctrl-A + Ctrl-K, Ctrl-L (in iTerm2, Terminal.app, a Linux terminal and tmux) | start/end of line, the word before the cursor gone, the line cleared to the start / to the end, the screen cleared with the prompt kept | screen after each key |

## F. Chat commands

| ID | Steps | Expected | Agent check |
|---|---|---|---|
| F1 | `/model gemma4-e2b` (GPU machine) | `⚡ vulkan` or `metal` `· gemma4-e2b · the conversation moves along`; the next answer knows the earlier conversation | screen + a question about the earlier answer |
| F2 | `/model qwen3-0.6b`, back to `/model gemma4-e2b` (both GPU-capable) | each switch works; no "out of device memory" (the current model is closed first on a GPU switch) | screen |
| F3 | `/model doesnotexist` | an error; the current model stays | screen |
| F4 | `/cpu`, `/gpu`, `/auto` | the prompt symbol changes ⚙/⚡; the conversation moves along | screen |
| F5 | `/temp`, `/temp 0.7`, `/temp 3` | shows the value; sets it; refuses 3 | screen |
| F6 | `/system Answer like a pirate.`, `/system`, `/system off` | set, shown, removed; answers follow it | screen |
| F7 | `/info` | backend, model, chat format, context fill, temperature, system | screen |
| F8 | `/save`, restart `geistr chat` without a model | model, processor, temperature and system prompt are back | `geistr config` |
| F9 | `/clear` | `○ a new conversation`; the old chat file stays; the context meter resets | a second file in `chats/` |
| F10 | `/retry` at temperature 0 | `↻ retry at temperature 0.7`, a new answer; the old one is gone from the chat file | the file has one Q/A pair for that question |
| F11 | `/copy`; `/copy code` (after an answer with code) | `⧉ copied N kB`; the clipboard holds the Markdown / only the code without fences (iTerm2, kitty, WezTerm, tmux via OSC 52; Terminal.app via pbcopy; Linux via wl-copy or xclip) | paste and compare; or decode the OSC 52 payload in `capture-pane -e` |
| F12 | `/help`, `/exit`, Ctrl-D | the list; the chat ends cleanly | exit 0 |

## G. Conversations that stay

| ID | Steps | Expected | Agent check |
|---|---|---|---|
| G1 | chat three exchanges, exit, `geistr chat` | `↻ 6 · „last question…“ · /clear new`; the model remembers ("What did I ask first?") | screen |
| G2 | resume a long conversation (80+ messages; copy one into `chats/`) | `↻ 80 · … · resumes the last N`; the first answer comes within seconds on the CPU | time to first word < 10 s (idle machine) |
| G3 | `geistr chat --new` | starts fresh; the previous file stays | — |
| G4 | `geistr config resume off`, chat, restart | nothing resumed, no files written; `history` not written either | `chats/` empty |
| G5 | `geistr config history off` | the conversation is still kept; typed lines are not | `history` unchanged |
| G6 | `echo hi \| geistr chat qwen3-0.6b` | answers; keeps nothing | no new files |
| G7 | file permissions | `chats/*.jsonl` and `history` are 0600, `chats/` 0700 | `stat` |
| G8 | a conversation that outgrows the context (small window or many long messages) | `↥ N oldest messages left out to fit the context …` once | screen |
| G9 | a small model that loops (SmolLM2: "Say hello in five words." several times) | the loop guard ends it: `⟲ it repeated itself: kept up to the repeat · /retry or /clear`; the next answers do not grow | the chat file has `"cut": true`; later answers stay short |
| G10 | Esc an answer, exit, resume | `↻ … · its last answer was stopped` | file has `"stopped": true` |

## H. Service: socket and HTTP

| ID | Steps | Expected | Agent check |
|---|---|---|---|
| H1 | `geistr serve qwen3-0.6b` | "serving … on …/geistr.sock"; the socket is 0600 | `stat` |
| H2 | `geistr chat --socket` in a second terminal; chat; `/model` | answers; `/model` says it belongs to the service | screen |
| H3 | `echo '{"op":"chat","messages":[{"role":"user","content":"Hi"}]}' \| nc -U …/geistr.sock` | JSON lines: parts, then `done` with tokens and times | `jq` on the last line |
| H4 | `geistr serve qwen3-0.6b --http`; `curl …/v1/chat/completions -d '{"messages":[…]}'` | an OpenAI completion | `jq .choices[0].message.content` |
| H5 | the same with `"stream":true` and `stream_options.include_usage` | SSE chunks: role first, content, finish, usage, `[DONE]` | — |
| H6 | the OpenAI Python SDK example from the README | prints an answer | exit 0 |
| H7 | Ollama: `curl …/api/chat -d '{"messages":[…]}'`, `/api/tags`, `/api/version` | NDJSON stream; the model; a version | — |
| H8 | Open WebUI: add `http://127.0.0.1:11434` as Ollama (and `/v1` as OpenAI) | chat works through the UI | screenshot |
| H9 | `curl -H 'Host: evil.example' …` | 403 (DNS-rebinding guard) | status code |
| H9b | `curl …/v1/chat/completions -H 'Origin: https://evil.example' -H 'Content-Type: text/plain' -d …` (what a page of another site can send without a preflight); the same with `Origin: http://localhost:3000`; then with `GEISTR_ORIGINS=https://evil.example` | 403 naming `GEISTR_ORIGINS`; 200; 200 (#171) | status codes |
| H10 | `--http=0.0.0.0:11434` | a warning that it is reachable from the network | stderr |
| H11 | two clients at once; one disconnects mid-answer | answers are served one at a time; the disconnect stops that answer; the other completes | — |
| H12 | SIGTERM to the service | it exits cleanly and removes the socket | socket gone |
| H13 | a request bigger than the context | 400 `context_length_exceeded` (OpenAI) | status and code |
| H14 | `geistr serve qwen3-0.6b --http=127.0.0.1:18434 --chats 2`; three clients with different conversations, one after the other, then the first again | the first two are continued from the cache (`input_tokens` only the new message); the third evicts the least recently used; the first again is processed whole, still answered | `usage.prompt_tokens` / socket `input_tokens` per request |
| H15 | `--max-request=1` and a 2 MB body; the same as a socket line | 413 naming `--max-request` and the limit (HTTP); refused at once (socket); the service goes on | status, time < 1 s |
| H16 | OpenAI: `max_tokens: 5`, `stop: ["\n"]`, `temperature: 0` twice, `tool_choice: "none"` with `tools`; `GET /v1/models`; Ollama `options.num_predict: 3`; `/v1/embeddings` with `encoding_format: "base64"` | 5 tokens, `finish_reason: length`; cut at the stop; two equal answers; no tool call; the model listed; 3 tokens; base64 vectors of the same length as float | `jq` on each |
| H17 | socket: `echo '{"op":"info"}' \| nc -U …/geistr.sock` | one JSON line: model, context, chat format | `jq .context` |
| H18 | serve for an hour: 500 short requests (a loop with curl), RSS every 50 | RSS levels off after the first requests (no steady growth); every answer 200 | `ps -o rss` series |

## I. Bench, speeds, settings

| ID | Steps | Expected | Agent check |
|---|---|---|---|
| I1 | `geistr bench` (idle machine) | tokens/s per installed model on ⚙ and ⚡ | `speed.tsv` grew |
| I2 | `geistr catalog` after bench | measured speeds as bars (no longer dim) | — |
| I3 | `geistr bench --compare` (after a geistr update with a new engine) | old vs new per model and processor, ▲/▼ % | — |
| I4 | `geistr config`; `config processor gpu`; `config temperature 5`; `config model ""` | lists all settings and the file; sets; refuses 5 with the valid range; clears | exit codes 0/0/2/0 |
| I5 | `geistr --models DIR`, `--catalog FILE` | another model folder; another catalog | — |
| I6 | Linux, no `GEISTEN_HOME`: `XDG_DATA_HOME=/tmp/xd XDG_CONFIG_HOME=/tmp/xc geistr config temperature 0.5`, one chat, `geistr pull smollm2-360m`; then the same with `GEISTEN_HOME=/tmp/gh` (and the old name `GEIST_HOME`); macOS: no variables | Linux: settings in `/tmp/xc/geisten/geistr.conf`, models, chats and history in `/tmp/xd/geisten/`; a `geistr.conf` left in the data folder by an older geistr is moved there once. With `GEISTEN_HOME`/`GEIST_HOME`: everything in that folder. macOS: everything in `~/Library/Application Support/geisten/`. Folders 0700, files 0600 | `find /tmp/xd /tmp/xc /tmp/gh -ls` |

## J. Python package

| ID | Steps | Expected | Agent check |
|---|---|---|---|
| J1 | `python3 -m venv v && v/bin/pip install geistr` (the published wheel from PyPI; before the first PyPI release: the wheel from `https://www.geisten.net/download/geistr/vX.Y.Z/`), `v/bin/python -c 'import geistr; print(geistr.__version__)'` equals `geistr --version`; run the README example with a model from `geistr pull smollm2-360m` | the version matches; streams an answer; the second question processes only the new message | exit 0, the two versions equal |
| J2 | `geistr.catalog(installed=True)` | the same models as `geistr catalog --installed` | compare ids |
| J3 | break out of `for piece in chat.send(…)` early; Ctrl-C | the answer stops; the chat goes on | the next `ask` works |
| J4 | a missing model | `geistr.GeistrError` with `.status == "io"` | — |
| J5 | `pip install` on a Linux with an older glibc than the wheel's `manylinux_2_N` tag (e.g. Debian 11) | pip refuses it ("no matching distribution"), it does not install a library that fails at import | pip's message |

## K. Decisions (experimental, default-off)

Only with a build that has the decision engine (`make geistr DECISION=1`) and a
permission file (see [docs/DECISIONS.md](DECISIONS.md)).

| ID | Steps | Expected | Agent check |
|---|---|---|---|
| K1 | `geistr decide <gemma4-e2b> --config FILE --question "Which equals 4?" --option a 4 --option b 5` | stdout is exactly one option id; one JSON diagnostics line on stderr | stdout `a` or `b` |
| K2 | without permission (`enabled: false`) | refused ("disabled"), exit 1, nothing on stdout | exit code |
| K3 | a duplicate option id; `--mode auto` | usage error, exit 2 | exit code |
| K4 | `geistr catalog --decision-config FILE --json` | the `decision` object per model | `jq` |

## L. Since 0.1.2: files, tools, images, embeddings, first run, threads, ranking

| ID | Steps | Expected | Agent check |
|---|---|---|---|
| L1 | `geistr chat --models $(mktemp -d)` with no model, on a terminal (Enter) and without one | a suggestion for this computer (`fits`); with a tty, Enter downloads it; without, the `geistr pull` line and exit 0 | stdout, exit 0 |
| L2 | `geistr config threads 4`, `geistr run … --threads 2`, then `geistr config threads 0` | stored; accepted; default = physical cores | `geistr config` |
| L3 | `geistr index <folder>` (2 text/Markdown files, one PDF), `geistr run qwen3-0.6b "<question about one file>" --files <folder>`; `/files <folder>` and `/files off` in a chat | the answer uses that file's content; `sources:` names the files used | stdout has the fact |
| L4 | `geistr serve qwen3-0.6b --http=127.0.0.1:18434`; `/v1/chat/completions` with a `tools` array; the same streamed; Ollama `/api/chat` with `tools` | `finish_reason: tool_calls`, a call with JSON arguments; streamed deltas | `jq '.choices[0].message.tool_calls[0].function.name'` |
| L5 | `geistr pull gemma4-e2b` (fetches `vision_tower.safetensors`), serve it, send an `image_url` data URL of a solid red PNG; Ollama `images` | the answer names the colour; a non-vision model answers with a clear error | content contains "red" |
| L6 | `geistr serve bitnet-embed-0.6b --http`; `/v1/embeddings` with two inputs; `/api/embed`; the same against a chat model | 2 vectors of 1024 numbers; a chat model: 400 "not an embedding model" | `jq` |
| L7 | `geistr catalog` on a small computer (8 GB, Pi 5) and on a large one | models in ranking order; the first line fits this computer; unknown-speed models do not outrank measured, reliable small ones (#136) | order of ids |
| L8 | `geistr serve … --http` while another server holds 11434 | "cannot listen on …: Address already in use", exit 1 | exit code |
| L9 | `geistr serve … --http=127.0.0.1:18434`; POST a prompt of about 3/4 of the context (`'word ' * N`); (a) SIGTERM after 5 s; (b) in a new run, close the client after 5 s and send a short request | (a) exit 0 in < 5 s, socket removed, the client gets an error (#146); (b) the short request is answered in < 10 s | timings; `make test-geistr ONLY=serve,http` pins both |
| L10 | a request far beyond the context (400 000 words) | 400 `context_length_exceeded` in < 5 s | status, time |

## Report

One file per run, `e2e-<date>-<platform>.md`:

```
geistr <version> · engine <commit> · <OS, CPU, GPU> · terminal <name> · load <avg>
| ID | result | note / evidence |
|---|---|---|
| A1 | pass | |
| D2 | fail | table misaligned at 48 columns, screenshot e2e/D2.png |
```

A failure becomes an issue with the case ID, the steps, what happened and
the evidence (screenshot, `capture-pane` text, stderr). A case that cannot
run here (no GPU, no Apple hardware) is recorded as `skipped`, with the
reason.

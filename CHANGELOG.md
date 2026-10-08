# Changelog

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

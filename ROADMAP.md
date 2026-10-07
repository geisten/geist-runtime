# Roadmap

Where geistr is heading: from a fast local chat to a local AI you can build
on. This is a snapshot of current thinking, not a commitment; priorities
change as we learn. Ideas and feedback are welcome in
[issues](https://github.com/geisten/geist-runtime/issues).

| | Theme | Goal |
|---|---|---|
| **Now** | Everyday chat | The chat remembers what you typed and shows the models that suit your computer first. |
| **Next** | Tools and skills | The local model can act: call tools over MCP and run reusable skills. |
| **Later** | A local AI platform | Build your own local AI on geistr, including fast, typed decisions instead of generated text. |

## Now: everyday chat

**Input history across sessions.** ↑ and ↓ already browse what you typed in
the current chat. The history will persist between sessions, as in a shell,
so a question from yesterday is one keystroke away.

**Models sorted by fit.** `geistr catalog` already rates every model for this
computer (RAM, CPU, GPU, measured speed). The model picker (`/model`) will use
the same ranking, so the best model for your machine comes first.

## Next: tools and skills

**MCP.** geistr becomes a [Model Context Protocol](https://modelcontextprotocol.io)
host. Two paths, possibly both:
- a built-in MCP server with a small, safe set of actions (read files, run a
  command with confirmation, search the web);
- connecting existing third-party MCP servers.

Both need tool calling in the runtime and the OpenAI-compatible API, which
does not support it yet.

**Skills.** Reusable, shareable instructions with optional tools ("review
this diff", "summarize my notes"), loaded by name in the chat and through the
API.

## Later: a local AI platform

**A framework for building local AI.** Today's pieces (C runtime, Python
package, OpenAI/Ollama API, model catalog with device fit) become a toolkit
for building your own local assistant or agent, with documented building
blocks and examples.

**JEV-style decision models.** [Jev](https://innfactory.ai/de/blog/jev-system-one-modell-klassifizierer-statt-llm/)
(TypeSafe AI's "System One Model") does not write text: it returns a typed,
calibrated decision. Which option applies (with a probability per option),
a score on a scale, or yes/no, in well under a second. That is what
automation needs: intent routing before an expensive LLM, checking another
model's answer, classifying documents in bulk. Jev itself is a cloud API.
geistr can bring the idea local:
- a `decide` call in the C API, the Python package and the HTTP API, with
  JSON in and typed JSON out;
- "JEV-ize" existing models: answer from a single prefill by reading the
  probabilities of the options' tokens, without generating anything. That
  is fast, never yields a malformed answer, and can be calibrated per model
  against a labelled set;
- support for dedicated local decision models when open weights appear.

## Under consideration

Not scheduled yet, but likely next candidates:

- **First run without a manual.** `geistr chat` with no model installed
  suggests the best model for this computer and offers to download it.
- **Windows and winget.** Releases exist for Linux and macOS only (Homebrew
  covers both).
- **Chat with your files.** An embeddings endpoint and local search over
  documents and notes.
- **Tool calling and images in the API.** Tool calling is also the
  foundation for MCP and skills.
- **Reliability.** Fix small models failing on GPUs with plenty of memory
  free (Vulkan reports "out of device memory" for an 80 MiB KV cache).

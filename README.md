# geist-runtime

An embeddable model runner on top of [geistlib](https://github.com/geisten/geistlib):
open a model, hold a chat, read the answer as text. Templates, streaming,
thinking output, context limits, KV reuse and cancellation are handled here,
behind one C API: [`include/geistr.h`](include/geistr.h).

Users:
- the `geistr` CLI (#11),
- the Python package `geistr` (#9),
- [geist-serve](https://github.com/geisten/geist-serve) (geist-serve#148),
- apps that embed models directly.

Status: the API (#1), templates (#2), streaming text (#3) and the runtime on
geistlib with context management (#4) and the model catalog with SHA-256
verification (#5, [`include/geistr_catalog.h`](include/geistr_catalog.h)) are in;
device fit (#6) is next. `src/stub.c` implements the API without an engine, for the fast
conformance tests.

```sh
make test        # conformance tests (C and C++) and the example chat, against the stub
make sanitize    # the same under ASan + UBSan
printf 'Hello\n' | build/chat stub:echo

make runtime     # the real libgeistr.a on the pinned geistlib
make fetch-model && make test-real   # the real runtime against SmolLM2
make chat-real && build/chat-real model.gguf
```

- [docs/API.md](docs/API.md): usage, design decisions, thread and ABI rules,
  and the mapping of every geist-serve use.
- Plan: [geist-serve#149](https://github.com/geisten/geist-serve/issues/149).

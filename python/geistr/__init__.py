"""geistr — run language models in your Python process (geist-runtime#9).

    import geistr

    for m in geistr.catalog(installed=True):
        print(m.id, m.name)

    with geistr.chat("smollm2-360m", system="Answer briefly.") as chat:
        for piece in chat.send("What is the capital of France?"):
            print(piece, end="", flush=True)
        print(chat.ask("And of Italy?"))

A chat holds its conversation: send() takes only the new message, and the
runtime processes only that. Models are catalog ids (verified files in the
geisten model folder, shared with the app and the geistr CLI) or paths to
GGUF files. ctypes over libgeistr; no compiled extension.
"""
from __future__ import annotations

import ctypes as C
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path

__all__ = ["catalog", "chat", "open", "models_dir", "Model", "Chat", "Piece", "Entry", "GeistrError"]
__version__ = "0.1.2"

_HERE = Path(__file__).resolve().parent
_lib = C.CDLL(os.environ.get("GEISTR_LIBRARY") or str(_HERE / ("libgeistr.dylib" if sys.platform == "darwin" else "libgeistr.so")))

# ---- C declarations (include/geistr.h, include/geistr_catalog.h) -------------

_p = C.c_void_p
_str = C.c_char_p
_status = C.c_int


class _ModelOpts(C.Structure):
    _fields_ = [("size", C.c_size_t), ("processor", C.c_int), ("threads", C.c_uint32), ("context", C.c_uint32),
                ("chat_format", _str)]


class _ModelInfo(C.Structure):
    _fields_ = [("size", C.c_size_t), ("arch", _str), ("chat_format", _str), ("backend", _str), ("context", C.c_uint32)]


class _ChatOpts(C.Structure):
    _fields_ = [("size", C.c_size_t), ("temperature", C.c_float), ("top_p", C.c_float), ("max_tokens", C.c_uint32),
                ("reasoning", C.c_int), ("overflow", C.c_int), ("thinking", C.c_int),
                ("stop", C.POINTER(_str)), ("n_stop", C.c_size_t)]


class _Message(C.Structure):
    _fields_ = [("role", _str), ("content", _str)]


class _Piece(C.Structure):
    _fields_ = [("size", C.c_size_t), ("part", C.c_int), ("text", C.POINTER(C.c_char)), ("len", C.c_size_t)]


class _Stats(C.Structure):
    _fields_ = [("size", C.c_size_t), ("finish", C.c_int), ("input_tokens", C.c_uint32),
                ("context_tokens", C.c_uint32), ("dropped_messages", C.c_uint32), ("output_tokens", C.c_uint32),
                ("prefill_ms", C.c_double), ("first_answer_ms", C.c_double), ("generation_ms", C.c_double),
                ("total_ms", C.c_double)]


class _Entry(C.Structure):
    _fields_ = [(name, _str) for name in ("id", "name", "file", "url", "sha256", "group_id", "group_name",
                                          "quantization", "reasoning_format", "unsupported_format", "quality",
                                          "reference")] + \
               [("bytes", C.c_uint64), ("working_mib", C.c_uint32), ("recommended_ram_gib", C.c_uint32),
                ("backends", C.c_uint32), ("quality_passed", C.c_uint32), ("quality_total", C.c_uint32)]


class _Device(C.Structure):
    _fields_ = [("size", C.c_size_t), ("name", C.c_char * 160), ("arch", C.c_char * 32), ("os", C.c_char * 128),
                ("kind", C.c_int), ("ram", C.c_uint64), ("available", C.c_uint64), ("disk", C.c_uint64),
                ("cores", C.c_uint32), ("logical_cpus", C.c_uint32), ("gpu", C.c_uint32), ("supported", C.c_bool),
                ("available_known", C.c_bool), ("disk_known", C.c_bool)]


class _Fit(C.Structure):
    _fields_ = [("entry", C.POINTER(_Entry)), ("resource", C.c_int), ("resource_reason", _str), ("verdict", C.c_int),
                ("reason", _str), ("basis", C.c_int), ("processor", C.c_int), ("seconds_cpu", C.c_double),
                ("seconds_gpu", C.c_double), ("estimated_from", C.c_uint32), ("passed", C.c_uint32),
                ("total", C.c_uint32), ("installed", C.c_bool)]


def _fn(name, res, *args):
    f = getattr(_lib, name)
    f.restype, f.argtypes = res, list(args)
    return f


_status_text = _fn("geistr_status_text", _str, _status)
_model_open = _fn("geistr_model_open", _status, _str, C.POINTER(_ModelOpts), C.POINTER(_p), C.c_char_p, C.c_size_t)
_model_close = _fn("geistr_model_close", None, _p)
_model_info = _fn("geistr_model_info_get", _status, _p, C.POINTER(_ModelInfo))
_chat_open = _fn("geistr_chat_open", _status, _p, C.POINTER(_ChatOpts), C.POINTER(_p))
_chat_close = _fn("geistr_chat_close", None, _p)
_chat_send = _fn("geistr_chat_send", _status, _p, C.c_size_t, C.POINTER(_Message))
_chat_next = _fn("geistr_chat_next", _status, _p, C.POINTER(_Piece))
_chat_cancel = _fn("geistr_chat_cancel", _status, _p)
_chat_rewind = _fn("geistr_chat_rewind", _status, _p, C.c_size_t)
_chat_length = _fn("geistr_chat_length", C.c_size_t, _p)
_chat_stats = _fn("geistr_chat_stats", _status, _p, C.POINTER(_Stats))
_chat_error = _fn("geistr_chat_error", _str, _p)
_catalog_parse = _fn("geistr_catalog_parse", _status, C.c_char_p, C.c_size_t, C.POINTER(_p), C.c_char_p, C.c_size_t)
_catalog_free = _fn("geistr_catalog_free", None, _p)
_catalog_count = _fn("geistr_catalog_count", C.c_size_t, _p)
_catalog_get = _fn("geistr_catalog_get", C.POINTER(_Entry), _p, C.c_size_t)
_catalog_check = _fn("geistr_catalog_check", _status, C.POINTER(_Entry), _str, C.c_bool, C.POINTER(C.c_int))
_models_dir = _fn("geistr_models_dir", _status, C.c_char_p, C.c_size_t)
_device_probe = _fn("geistr_device_probe", _status, _str, C.POINTER(_Device))
_rank = _fn("geistr_rank", _status, _p, C.POINTER(_Device), _p, _p, C.POINTER(_p))
_ranking_free = _fn("geistr_ranking_free", None, _p)
_ranking_count = _fn("geistr_ranking_count", C.c_size_t, _p)
_ranking_get = _fn("geistr_ranking_get", C.POINTER(_Fit), _p, C.c_size_t)

_STATUS = ["ok", "invalid", "no_memory", "io", "format", "context", "backend", "cancelled"]
_FINISH = [None, "stop", "length", "context", "cancelled", "error", "repetition"]
_INSTALL = ["available", "unverified", "installed", "mismatch"]
_RESOURCE = ["fits", "limited", "unavailable"]


class GeistrError(Exception):
    """A runtime failure. status is the code name: "context", "io", "cancelled", …"""

    def __init__(self, status: int, detail: str = ""):
        self.status = _STATUS[status] if 0 <= status < len(_STATUS) else str(status)
        text = (_status_text(status) or b"").decode()
        super().__init__(f"{text}: {detail}" if detail and detail != text else text)


def _check(status: int, detail: str = ""):
    if status:
        raise GeistrError(status, detail)


def _b(s):
    return None if s is None else s.encode()


def _s(b):
    return None if b is None else b.decode()


# ---- catalog -------------------------------------------------------------------

def models_dir() -> str:
    """The model folder shared with the geisten app and the geistr CLI."""
    buf = C.create_string_buffer(4096)
    _check(_models_dir(buf, len(buf)), "set HOME or GEISTEN_HOME")
    return buf.value.decode()


@dataclass(frozen=True)
class Entry:
    """A catalog model and its state on this computer (as `geistr catalog --json`)."""
    id: str
    name: str
    quantization: str | None
    file: str
    url: str
    sha256: str
    bytes: int
    recommended_ram_gib: int
    state: str            # available, unverified, installed, mismatch
    resource: str         # fits, limited, unavailable
    resource_reason: str
    path: str             # where the file is (or would be) in the model folder
    reasoning_format: str | None = None

    @property
    def installed(self) -> bool:
        return self.state == "installed"


class _Catalog:
    """A parsed catalog: the app's catalog.json next to the default model
    folder if present (the app may hold a newer one), else the packaged copy."""

    def __init__(self, folder: str, catalog_file: str | None):
        self.handle = _p()
        app = Path(folder).parent / "catalog.json"
        if catalog_file:
            self._parse(catalog_file, strict=True)
        elif not (folder == models_dir() and app.exists() and self._parse(app, strict=False)):
            self._parse(_HERE / "catalog.json", strict=True)

    def _parse(self, path, strict: bool) -> bool:
        text, error = Path(path).read_bytes(), C.create_string_buffer(256)
        status = _catalog_parse(text, len(text), C.byref(self.handle), error, len(error))
        if status and strict:
            raise GeistrError(status, f"{path}: {error.value.decode()}")
        return status == 0

    def __del__(self):
        if getattr(self, "handle", None):
            _catalog_free(self.handle)

    def entries(self):
        return [_catalog_get(self.handle, i).contents for i in range(_catalog_count(self.handle))]


def _state(entry: _Entry, folder: str) -> str:
    """Installed state; hashes a new or changed file once (then a receipt)."""
    out = C.c_int()
    _check(_catalog_check(C.byref(entry), folder.encode(), False, C.byref(out)))
    if out.value == 1:
        _check(_catalog_check(C.byref(entry), folder.encode(), True, C.byref(out)))
    return _INSTALL[out.value]


def catalog(installed: bool = False, available: bool = False, *, folder: str | None = None,
            catalog_file: str | None = None) -> list[Entry]:
    """The catalog models and their state here, installed first. Offline.
    The first listing hashes each model file once (seconds per GB)."""
    folder = folder or models_dir()
    cat = _Catalog(folder, catalog_file)
    device = _Device(size=C.sizeof(_Device))
    _check(_device_probe(folder.encode(), C.byref(device)), "cannot read this computer's memory")
    ranking = _p()
    _check(_rank(cat.handle, C.byref(device), None, None, C.byref(ranking)))
    try:
        fits = {}
        for i in range(_ranking_count(ranking)):
            f = _ranking_get(ranking, i).contents
            fits[f.entry.contents.id] = (_RESOURCE[f.resource], _s(f.resource_reason))
        out = []
        for e in cat.entries():
            resource, reason = fits[e.id]
            out.append(Entry(_s(e.id), _s(e.name), _s(e.quantization), _s(e.file), _s(e.url), _s(e.sha256),
                             e.bytes, e.recommended_ram_gib, _state(e, folder), resource, reason,
                             os.path.join(folder, _s(e.file)), _s(e.reasoning_format)))
    finally:
        _ranking_free(ranking)
    out.sort(key=lambda m: not m.installed)
    return [m for m in out if not (installed and not m.installed) and not (available and m.installed)]


def _resolve(model: str, folder: str | None, catalog_file: str | None) -> tuple[str, str | None]:
    """A catalog id → its verified file; a path stays a path. Also the
    reasoning format the catalog knows for that file."""
    folder = folder or models_dir()
    is_path = os.sep in model or model.endswith(".gguf") or model.startswith("stub:")
    cat = _Catalog(folder, catalog_file)
    for e in cat.entries():
        if (_s(e.file) == os.path.basename(model)) if is_path else (_s(e.id) == model):
            if is_path:
                return model, _s(e.reasoning_format)
            state = _state(e, folder)
            if state != "installed":
                raise GeistrError(3, f"{model} is not installed (geistr pull {model})" if state == "available"
                                  else f"{model} does not match the catalog (geistr pull {model})")
            return os.path.join(folder, _s(e.file)), _s(e.reasoning_format)
    if is_path:
        return model, None
    raise GeistrError(1, f"unknown model {model} (see geistr.catalog())")


# ---- model and chat --------------------------------------------------------------

class Model:
    """An open model. Chats keep it alive, so closing order does not matter."""

    def __init__(self, model: str, *, processor: str = "auto", context: int = 0, threads: int = 0,
                 chat_format: str | None = None, folder: str | None = None, catalog_file: str | None = None):
        path, self._reasoning = _resolve(model, folder, catalog_file)
        opts = _ModelOpts(C.sizeof(_ModelOpts), ["auto", "cpu", "gpu"].index(processor), threads, context,
                          _b(chat_format))
        self._h, error = _p(), C.create_string_buffer(256)
        _check(_model_open(path.encode(), C.byref(opts), C.byref(self._h), error, len(error)), error.value.decode())
        self.path = path

    @property
    def info(self) -> dict:
        i = _ModelInfo(size=C.sizeof(_ModelInfo))
        _check(_model_info(self._handle(), C.byref(i)))
        return {"arch": _s(i.arch), "chat_format": _s(i.chat_format), "backend": _s(i.backend), "context": i.context}

    def chat(self, system: str | None = None, *, temperature: float = 0.0, top_p: float = 1.0, max_tokens: int = 0,
             overflow: str = "refuse", thinking: bool = False, stop: tuple[str, ...] = ()) -> "Chat":
        return Chat(self, system, temperature=temperature, top_p=top_p, max_tokens=max_tokens, overflow=overflow,
                    thinking=thinking, stop=stop)

    def _handle(self):
        if not self._h:
            raise ValueError("model is closed")
        return self._h

    def close(self):
        if getattr(self, "_h", None):
            _model_close(self._h)
            self._h = None

    __del__ = close

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class Piece(str):
    """A piece of the answer: complete UTF-8 text. thinking is True for
    reasoning text (only with thinking=True)."""
    thinking = False


class Chat:
    """A conversation. send() takes only the new message(s); the answer
    becomes part of the conversation by itself."""

    def __init__(self, model: Model, system: str | None = None, *, temperature: float = 0.0, top_p: float = 1.0,
                 max_tokens: int = 0, overflow: str = "refuse", thinking: bool = False, stop: tuple[str, ...] = ()):
        stops = (_str * max(len(stop), 1))(*[s.encode() for s in stop])
        opts = _ChatOpts(C.sizeof(_ChatOpts), temperature, top_p, max_tokens,
                         1 if model._reasoning == "think_tags" else 0, ["refuse", "drop_oldest"].index(overflow),
                         int(thinking), stops if stop else None, len(stop))
        self._h = _p()
        _check(_chat_open(model._handle(), C.byref(opts), C.byref(self._h)))
        self._system = system
        self._turn = 0  # a newer send ends an older answer's iterator

    def send(self, message, role: str = "user"):
        """Stream the answer to message (a str, or a list of (role, content)
        pairs) as Pieces. Ctrl-C cancels the answer and raises
        KeyboardInterrupt; the chat stays usable."""
        turns = [(role, message)] if isinstance(message, str) else list(message)
        if self._system is not None and _chat_length(self._handle()) == 0:
            turns.insert(0, ("system", self._system))
        keep = [(r.encode(), c.encode()) for r, c in turns]
        msgs = (_Message * len(keep))(*[_Message(r, c) for r, c in keep])
        status = _chat_send(self._handle(), len(keep), msgs)
        if status:
            raise GeistrError(status, _s(_chat_error(self._h)))
        self._turn += 1
        return self._pieces(self._turn)

    def _pieces(self, turn: int):
        piece = _Piece(size=C.sizeof(_Piece))
        try:
            while self._h and turn == self._turn:
                status = _chat_next(self._h, C.byref(piece))
                if status == 7:  # cancelled
                    return
                if status:
                    raise GeistrError(status, _s(_chat_error(self._h)))
                if piece.part == 2:  # END
                    return
                out = Piece(C.string_at(piece.text, piece.len).decode())
                out.thinking = piece.part == 1
                yield out
        except (KeyboardInterrupt, GeneratorExit):
            if self._h and turn == self._turn:  # stop generating; the chat stays usable
                _chat_cancel(self._h)
                while _chat_next(self._h, C.byref(piece)) == 0 and piece.part != 2:
                    pass
            raise

    def ask(self, message, role: str = "user") -> str:
        """The whole answer (thinking left out)."""
        return "".join(p for p in self.send(message, role) if not p.thinking)

    def cancel(self):
        """Stop the running answer as soon as possible. Any thread."""
        if self._h:
            _chat_cancel(self._h)

    def rewind(self, keep: int):
        """Go back to the first keep messages (0: a new conversation)."""
        self._turn += 1
        _check(_chat_rewind(self._handle(), keep), _s(_chat_error(self._h)))

    def __len__(self):
        return _chat_length(self._handle())

    @property
    def stats(self) -> dict:
        s = _Stats(size=C.sizeof(_Stats))
        _check(_chat_stats(self._handle(), C.byref(s)))
        return {name: (_FINISH[s.finish] if name == "finish" else getattr(s, name)) for name, _ in _Stats._fields_[1:]}

    def _handle(self):
        if not self._h:
            raise ValueError("chat is closed")
        return self._h

    def close(self):
        if getattr(self, "_h", None):
            _chat_close(self._h)
            self._h = None

    __del__ = close

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def open(model: str, **opts) -> Model:  # noqa: A001 — geistr.open, like the C API
    """Open a model: a catalog id or a GGUF path. Options: processor
    ("auto", "cpu", "gpu"), context, threads, chat_format, folder, catalog_file."""
    return Model(model, **opts)


def chat(model: str, system: str | None = None, *, processor: str = "auto", context: int = 0,
         folder: str | None = None, catalog_file: str | None = None, **chat_opts) -> Chat:
    """Open a model and a chat on it in one step; the chat owns the model."""
    with Model(model, processor=processor, context=context, folder=folder, catalog_file=catalog_file) as m:
        return m.chat(system, **chat_opts)

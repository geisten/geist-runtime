"""Fixed-option decisions. All rendering, validation and scoring live in C.
Results own copies; selection probabilities are conditional, not confidence.
"""
from __future__ import annotations

import ctypes as C
from dataclasses import dataclass
from pathlib import Path
import threading

from . import _lib, _fn, _p, _str, _check, _s, GeistrError

_HAS_API = hasattr(_lib, "geistr_decision_open")


class _Policy(C.Structure):
    _fields_ = [("sha256", C.c_char * 65), ("enabled", C.c_bool), ("profile", C.c_int), ("mode", C.c_int)]


class _Opts(C.Structure):
    _fields_ = [("size", C.c_size_t), ("mode", C.c_int), ("max_prompt_tokens", C.c_size_t)]


class _Option(C.Structure):
    _fields_ = [("id_len", C.c_size_t), ("description_len", C.c_size_t), ("id", _str), ("description", _str)]


class _Request(C.Structure):
    _fields_ = [("size", C.c_size_t), ("operation", C.c_int), ("question_len", C.c_size_t),
                ("context_len", C.c_size_t), ("n_options", C.c_size_t), ("question", _str),
                ("context", _str), ("options", C.POINTER(_Option))]


class _Result(C.Structure):
    _fields_ = [("size", C.c_size_t), ("n_options", C.c_size_t), ("best_index", C.c_size_t),
                ("prompt_tokens", C.c_size_t), ("mode", C.c_int), ("profile", C.c_int),
                ("external_id", C.c_char * 129), ("logits", C.POINTER(C.c_float)),
                ("selection_probabilities", C.POINTER(C.c_double)), ("model_calls", C.c_size_t),
                ("preparation_ms", C.c_double), ("scoring_ms", C.c_double)]


class _Capability(C.Structure):
    _fields_ = [("size", C.c_size_t), ("configured", C.c_bool), ("available", C.c_bool),
                ("dense", C.c_bool), ("selected_rows", C.c_bool), ("profile", C.c_int), ("backend", _str), ("engine_revision", _str), ("engine_version", _str)]


class _Resources(C.Structure):
    _fields_ = [("size", C.c_size_t), ("live_allocations", C.c_size_t), ("live_bytes", C.c_size_t),
                ("provider_known", C.c_bool), ("unified_memory", C.c_bool),
                ("provider_allocated_bytes", C.c_uint64)]


class _Plan(C.Structure):
    _fields_ = [("size", C.c_size_t), ("n_prompt", C.c_size_t), ("n_candidates", C.c_size_t),
                ("prompt", _str), ("prompt_ids", C.POINTER(C.c_int32)),
                ("candidate_ids", C.POINTER(C.c_int32)), ("template_sha256", _str)]


if _HAS_API:
    _parse = _fn("geistr_decision_config_parse", C.c_int, C.c_size_t, C.c_size_t, _str, C.POINTER(_p), _str)
    _free = _fn("geistr_decision_config_free", None, _p)
    _find = _fn("geistr_decision_config_find", C.POINTER(_Policy), _str, _p)
    _sha = _fn("geistr_sha256_file", C.c_int, _str, _str)
    _open = _fn("geistr_decision_open", C.c_int, _p, C.c_size_t, C.POINTER(_Opts), C.POINTER(_p), _str)
    _close = _fn("geistr_decision_close", None, _p)
    _score = _fn("geistr_decision_score", C.c_int, _p, C.POINTER(_Request), C.POINTER(_Result))
    _reset = _fn("geistr_decision_reset", C.c_int, _p)
    _cancel = _fn("geistr_decision_cancel", C.c_int, _p)
    _error = _fn("geistr_decision_error", _str, _p)
    _plan = _fn("geistr_decision_plan_get", C.c_int, _p, C.POINTER(_Plan))
    _capability = _fn("geistr_decision_capability_get", C.c_int, _p, C.POINTER(_Capability))
    _resources = _fn("geistr_decision_resources_get", C.c_int, _p, C.POINTER(_Resources))
    _profile_name = _fn("geistr_decision_profile_name", _str, C.c_int)
    _available = _fn("geistr_decision_available", C.c_bool)


def _require_api():
    if not _HAS_API:
        raise GeistrError(4, "linked runtime has no decision API")


def decision_available() -> bool:
    """Linked build feature only; loaded model/backend permission is separate."""
    return bool(_HAS_API and _available())


class DecisionConfig:
    """Immutable strict schema-1 policy. Caller supplies JSON bytes or uses from_file.
    Models copy the selected policy; closing this config does not change them.
    """
    def __init__(self, data: bytes):
        _require_api()
        self._h = _p()
        self._guard = threading.RLock()
        if not isinstance(data, bytes) or len(data) > 65536:
            raise GeistrError(1, "configuration must be at most 65536 bytes")
        error = C.create_string_buffer(256)
        status = _parse(len(data), len(error), data, C.byref(self._h), error)
        _check(status, error.value.decode())

    @classmethod
    def from_file(cls, path) -> "DecisionConfig":
        with Path(path).open("rb") as source:
            data = source.read(65537)
        return cls(data)

    def _policy(self, path: str) -> _Policy | None:
        with self._guard:
            if not self._h:
                raise ValueError("decision config is closed")
            sha = C.create_string_buffer(65)
            _check(_sha(path.encode(), sha), "cannot verify model artifact")
            pointer = _find(sha.value, self._h)
            # Copy immutable policy before releasing the config's lifetime lock.
            return _Policy.from_buffer_copy(pointer.contents) if pointer else None

    def close(self):
        with getattr(self, "_guard", threading.RLock()):
            if getattr(self, "_h", None):
                _free(self._h)
                self._h = None

    __del__ = close

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def model_capability(model) -> dict:
    _require_api()
    with model._guard:
        cap = _Capability(size=C.sizeof(_Capability))
        _check(_capability(model._handle(), C.byref(cap)))
        return {"configured": cap.configured, "available": cap.available, "dense": cap.dense,
                "selected_rows": cap.selected_rows, "profile": _s(_profile_name(cap.profile)),
                "backend": _s(cap.backend), "engine_revision": _s(cap.engine_revision), "engine_version": _s(cap.engine_version)}


def model_resources(model) -> dict:
    _require_api()
    with model._guard:
        stats = _Resources(size=C.sizeof(_Resources))
        _check(_resources(model._handle(), C.byref(stats)))
        return {name: getattr(stats, name) for name, _ in _Resources._fields_[1:]}


@dataclass(frozen=True)
class DecisionResult:
    external_id: str
    best_index: int
    logits: tuple[float, ...]
    selection_probabilities: tuple[float, ...]
    prompt_tokens: int
    mode: str
    profile: str
    model_calls: int
    preparation_ms: float
    scoring_ms: float
    prompt_ids: tuple[int, ...]
    candidate_ids: tuple[int, ...]
    template_sha256: str


def _utf8(value: str, limit: int) -> bytes:
    if not isinstance(value, str) or len(value) > limit:
        raise GeistrError(1, "invalid or oversized decision field")
    try:
        encoded = value.encode("utf-8", errors="strict")
    except UnicodeError as exc:
        raise GeistrError(1, "invalid UTF-8 decision field") from exc
    if len(encoded) > limit:
        raise GeistrError(1, "oversized UTF-8 decision field")
    return encoded


class Decision:
    """Independent decision state sharing the model's immutable weights.
    score/reset/close are serialized; cancel may be called from another thread.
    Cancellation discards the result after a synchronous engine call completes.
    """
    def __init__(self, model, *, mode: str = "default", max_prompt_tokens: int = 0):
        _require_api()
        self._h = _p()
        self._calls = threading.RLock()
        self._lifetime = threading.RLock()
        if not isinstance(max_prompt_tokens, int) or not 0 <= max_prompt_tokens <= (1 << 32) - 1:
            raise GeistrError(1, "invalid prompt token limit")
        try:
            numeric = ["default", "dense", "selected_rows"].index(mode)
        except ValueError as exc:
            raise GeistrError(1, "unknown decision mode") from exc
        opts = _Opts(C.sizeof(_Opts), numeric, max_prompt_tokens)
        error = C.create_string_buffer(256)
        with model._guard:
            status = _open(model._handle(), len(error), C.byref(opts), C.byref(self._h), error)
        _check(status, error.value.decode())

    def _handle(self):
        if not self._h:
            raise ValueError("decision is closed")
        return self._h

    def score(self, question: str, options, *, context: str | None = None) -> DecisionResult:
        """Options are ordered (external_id, description) pairs; exactly one wins.
        Returned scores/IDs own copies and survive later score/reset/close calls.
        """
        q = _utf8(question, 65536)
        ctx = _utf8(context, 65536) if context is not None else b""
        encoded = []
        total = len(q) + len(ctx)
        for pair in options:
            if len(encoded) == 26:
                raise GeistrError(1, "too many options")
            try:
                identity, description = pair
            except (TypeError, ValueError) as exc:
                raise GeistrError(1, "options must be ID/description pairs") from exc
            identity, description = _utf8(identity, 128), _utf8(description, 16384)
            total += len(identity) + len(description)
            if total > 262144:
                raise GeistrError(1, "aggregate request too large")
            encoded.append((identity, description))
        values = (_Option * len(encoded))(*[_Option(len(i), len(d), i, d) for i, d in encoded])
        request = _Request(C.sizeof(_Request), 1, len(q), len(ctx), len(encoded), q, ctx or None, values)
        with self._calls:
            with self._lifetime:
                handle = self._handle()
            out = _Result(size=C.sizeof(_Result))
            status = _score(handle, C.byref(request), C.byref(out))
            _check(status, _s(_error(handle)))
            plan = _Plan(size=C.sizeof(_Plan))
            _check(_plan(handle, C.byref(plan)))
            return DecisionResult(out.external_id.decode(), out.best_index,
                tuple(out.logits[i] for i in range(out.n_options)),
                tuple(out.selection_probabilities[i] for i in range(out.n_options)),
                out.prompt_tokens, ["default", "dense", "selected_rows"][out.mode],
                _s(_profile_name(out.profile)), out.model_calls, out.preparation_ms, out.scoring_ms,
                tuple(plan.prompt_ids[i] for i in range(plan.n_prompt)),
                tuple(plan.candidate_ids[i] for i in range(plan.n_candidates)), _s(plan.template_sha256))

    def reset(self):
        with self._calls, self._lifetime:
            _check(_reset(self._handle()), _s(_error(self._h)))

    def cancel(self):
        with self._lifetime:
            if self._h:
                _check(_cancel(self._h))

    def close(self):
        with getattr(self, "_calls", threading.RLock()), getattr(self, "_lifetime", threading.RLock()):
            if getattr(self, "_h", None):
                _close(self._h)
                self._h = None

    __del__ = close

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

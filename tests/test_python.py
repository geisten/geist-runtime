#!/usr/bin/env python3
"""The geistr Python package (#9), installed from the wheel, against the
reference model. Usage: test_python.py <model.gguf> <abi_sizes binary>"""
import ctypes, hashlib, json, os, subprocess, sys, tempfile, threading

import geistr

model, abi = sys.argv[1], sys.argv[2]
assert "site-packages" in geistr.__file__, "test the installed wheel, not the source tree"

# ---- the ctypes layout matches the C headers -----------------------------------
sizes = dict(line.split() for line in subprocess.run([abi], capture_output=True, text=True, check=True).stdout.splitlines())
for c_name, py in [("geistr_model_opts", geistr._ModelOpts), ("geistr_model_info", geistr._ModelInfo),
                   ("geistr_chat_opts", geistr._ChatOpts), ("geistr_message", geistr._Message),
                   ("geistr_piece", geistr._Piece), ("geistr_stats", geistr._Stats),
                   ("geistr_catalog_entry", geistr._Entry), ("geistr_device", geistr._Device),
                   ("geistr_fit", geistr._Fit)]:
    assert int(sizes[c_name]) == ctypes.sizeof(py), (c_name, sizes[c_name], ctypes.sizeof(py))
from geistr import decision as decision_bindings
for c_name, py in [("geistr_decision_policy", decision_bindings._Policy),
                   ("geistr_decision_opts", decision_bindings._Opts),
                   ("geistr_decision_option", decision_bindings._Option),
                   ("geistr_decision_request", decision_bindings._Request),
                   ("geistr_decision_result", decision_bindings._Result),
                   ("geistr_decision_capability", decision_bindings._Capability),
                   ("geistr_decision_resources", decision_bindings._Resources),
                   ("geistr_decision_plan", decision_bindings._Plan)]:
    assert int(sizes[c_name]) == ctypes.sizeof(py), (c_name, sizes[c_name], ctypes.sizeof(py))
with geistr.DecisionConfig(b'{"schema":1,"models":[]}') as cfg:
    with geistr.open(model, processor="cpu", context=512, decision_config=cfg) as disabled:
        assert not disabled.decision_capability["configured"]
        try:
            disabled.decision()
            raise AssertionError("default-off model allowed a decision")
        except geistr.GeistrError as exc:
            assert exc.status == "format"
print("python: ctypes layout = C headers; decision configuration and default-off")

# ---- catalog: packaged copy, a folder of our own, installed vs available ------------
assert len(geistr.catalog(folder=tempfile.mkdtemp())) >= 1  # the packaged catalog
folder = tempfile.mkdtemp(prefix="geistr-py-")
digest = hashlib.sha256(open(model, "rb").read()).hexdigest()
entry = {"id": "ref", "name": "Reference", "file": os.path.basename(model),
         "url": "https://huggingface.co/x/y/resolve/main/ref.gguf", "sha256": digest, "bytes": os.path.getsize(model),
         "working_mib": 64, "recommended_ram_gib": 1, "backends": ["cpu"], "group_id": "ref", "group_name": "Reference",
         "quantization": "Q8_0"}
catalog_file = os.path.join(folder, "test-catalog.json")
json.dump({"schema": 2, "revision": 1, "models": [entry, {**entry, "id": "other", "file": "other.gguf",
                                                           "group_id": "other"}]}, open(catalog_file, "w"))
listing = geistr.catalog(folder=folder, catalog_file=catalog_file)
assert [m.state for m in listing] == ["available", "available"] and listing[0].resource == "fits"
os.link(model, os.path.join(folder, os.path.basename(model)))
listing = geistr.catalog(folder=folder, catalog_file=catalog_file)
assert listing[0].id == "ref" and listing[0].installed and listing[0].path.endswith(".gguf"), listing
assert [m.id for m in geistr.catalog(installed=True, folder=folder, catalog_file=catalog_file)] == ["ref"]
assert [m.id for m in geistr.catalog(available=True, folder=folder, catalog_file=catalog_file)] == ["other"]
try:
    geistr.chat("other", folder=folder, catalog_file=catalog_file)
    raise AssertionError("opened a model that is not installed")
except geistr.GeistrError as e:
    assert e.status == "io" and "geistr pull other" in str(e)
print("python: catalog (states, filters, ids resolve to verified files)")

# ---- chat: streaming, only the new message, stats, rewind -----------------------
with geistr.chat("ref", folder=folder, catalog_file=catalog_file, system="Answer in one word.") as chat:
    pieces = list(chat.send("What is the capital of France?"))
    assert all(isinstance(p, geistr.Piece) and not p.thinking for p in pieces) and "Paris" in "".join(pieces)
    first = chat.stats
    assert first["finish"] == "stop" and len(chat) == 3, (first, len(chat))
    answer = chat.ask("And of Italy?")
    assert "Rome" in answer, answer
    assert chat.stats["input_tokens"] < first["input_tokens"], "the second turn processes only the new message"
    chat.rewind(3)
    assert len(chat) == 3 and "Rome" in chat.ask("And of Italy?")
print("python: streaming, stateful turns, stats, rewind")

with geistr.open(model) as m:
    info = m.info
    assert info["context"] > 0 and info["backend"] in ("cpu", "metal", "vulkan"), info
    # cancel from another thread, then the chat goes on
    chat = m.chat(system="You are a storyteller.")
    stream = chat.send("Write a very long story about a lighthouse keeper, at least 2000 words.")
    got = [next(stream) for _ in range(3)]
    threading.Thread(target=chat.cancel).start()
    rest = list(stream)
    assert chat.stats["finish"] == "cancelled", chat.stats
    assert "Paris" in chat.ask("Now only this: what is the capital of France? One word.")
    # leaving an answer early stops it; the next send works
    for piece in chat.send("Count from one to one thousand in words."):
        break
    assert chat.stats["finish"] == "cancelled"
    assert chat.ask("Say yes.")
    # stop strings and max_tokens
    with m.chat(stop=("\n",)) as short:
        assert "\n" not in short.ask("List three fruits, one per line.")
    with m.chat(max_tokens=5) as tiny:
        tiny.ask("Tell me about the sea.")
        assert tiny.stats["finish"] == "length" and tiny.stats["output_tokens"] == 5
    chat.close()
    chat.close()  # idempotent
    try:
        chat.ask("closed?")
        raise AssertionError("a closed chat answered")
    except ValueError:
        pass
print("python: cancel (thread, leaving the loop), stop strings, max_tokens, close")

try:
    geistr.open("/nonexistent/model.gguf")
    raise AssertionError("opened a missing file")
except geistr.GeistrError as e:
    assert e.status == "io", e.status
try:
    with geistr.open(model, context=512) as m, m.chat() as small:
        small.ask("word " * 2000)
    raise AssertionError("an oversized message fit")
except geistr.GeistrError as e:
    assert e.status == "context", e.status
print("python: errors (missing file, context) — all passed")

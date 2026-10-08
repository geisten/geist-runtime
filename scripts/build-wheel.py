#!/usr/bin/env python3
"""build-wheel.py <libgeistr.{dylib,so}> <outdir> — the geistr wheel (#9).

A platform wheel without an extension module: the Python package (ctypes),
the shared library and the catalog. Written with zipfile, so the build needs
no packaging tools. On Linux the tag is linux_<arch>; `auditwheel repair`
turns it into a manylinux wheel and bundles libgomp.
"""
import base64, hashlib, platform, re, subprocess, sys, zipfile
from pathlib import Path

lib, out = Path(sys.argv[1]), Path(sys.argv[2])
root = Path(__file__).resolve().parent.parent
version = re.search(r'__version__ = "([^"]+)"', (root / "python/geistr/__init__.py").read_text()).group(1)
machine = platform.machine().lower()
if sys.platform == "darwin":
    # The oldest macOS the library loads on: its LC_BUILD_VERSION minos.
    minos = re.search(r"minos (\d+)\.(\d+)", subprocess.run(["otool", "-l", str(lib)], capture_output=True,
                                                             text=True, check=True).stdout)
    tag = f"macosx_{minos.group(1)}_{minos.group(2)}_{machine}"
else:
    tag = f"linux_{machine}"
name = f"geistr-{version}-py3-none-{tag}.whl"
info = f"geistr-{version}.dist-info"

files = {
    "geistr/__init__.py": (root / "python/geistr/__init__.py").read_bytes(),
    "geistr/decision.py": (root / "python/geistr/decision.py").read_bytes(),
    "geistr/catalog.json": (root / "models/catalog.json").read_bytes(),
    f"geistr/{lib.name}": lib.read_bytes(),
    f"{info}/METADATA": f"""Metadata-Version: 2.1
Name: geistr
Version: {version}
Summary: Run language models in your Python process (geist-runtime)
Home-page: https://github.com/geisten/geist-runtime
License: see LICENSE
Requires-Python: >=3.9

{(root / "README.md").read_text()}""".encode(),
    f"{info}/WHEEL": f"Wheel-Version: 1.0\nGenerator: geist-runtime build-wheel.py\nRoot-Is-Purelib: false\nTag: py3-none-{tag}\n".encode(),
    f"{info}/LICENSE": (root / "LICENSE").read_bytes(),
}


def digest(data):
    return "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()


record = "".join(f"{path},{digest(data)},{len(data)}\n" for path, data in files.items()) + f"{info}/RECORD,,\n"
files[f"{info}/RECORD"] = record.encode()
out.mkdir(parents=True, exist_ok=True)
for old in out.glob("geistr-*.whl"):
    old.unlink()
with zipfile.ZipFile(out / name, "w", zipfile.ZIP_DEFLATED) as whl:
    for path, data in files.items():
        entry = zipfile.ZipInfo(path, date_time=(2026, 1, 1, 0, 0, 0))  # reproducible
        entry.external_attr = (0o755 if path.endswith((".so", ".dylib")) else 0o644) << 16
        entry.compress_type = zipfile.ZIP_DEFLATED
        whl.writestr(entry, data)
print(out / name)

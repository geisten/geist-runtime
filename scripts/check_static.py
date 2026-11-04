#!/usr/bin/env python3
"""check_static.py - clang-tidy over the runtime, with a findings ratchet.

Ported from geistlib (#342). No models, no GPU: the compile commands come
from `make -n` (the engine is synced, not built), clang-tidy reads the
sources. Only the runtime's own code is reported, not the pinned geistlib. A finding is (file, check, message) with a count; line numbers
stay out, so moving code does not disturb the baseline.

  check_static.py           fail on a finding the baseline lacks, and on a
                            baseline entry that is gone: it only shrinks
  check_static.py --update  rewrite the baseline
  check_static.py --selftest
                            the seeded defects in tests/static/ must be found

A NOLINT needs its reason on the same line: NOLINT(check): why. So does an
analyzer finding in the baseline, in static-reasons.txt.

CLANG_TIDY names the binary; JOBS the parallelism.
"""
import argparse
import collections
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "scripts", "static-baseline.txt")
REASONS = os.path.join(ROOT, "scripts", "static-reasons.txt")
FINDING = re.compile(r"^(.+?):(\d+):(\d+): (warning|error): (.*) \[([^\]]+)\]$")
NOLINT = re.compile(r"NOLINT(?:NEXTLINE|BEGIN|END)?(\([^)]*\))?(.*)")
REPORTED = ("src/", "include/", "tools/", "tests/static/")
COMPILERS = ("cc", "clang", "gcc")
GOALS = ("all", "runtime", "geistr")
ENTER = re.compile(r"^\S*make(?:\[\d+\])?: (Entering|Leaving) directory [`'\"](.+)['\"]$")


def flags_of(args):
    """The arguments of a compiler call that shape parsing: -std, -I, -D, -W, -f, -isystem."""
    out, i = [], 1
    while i < len(args):
        a = args[i]
        if a in ("-framework", "-o") and i + 1 < len(args):
            i += 1  # a link flag and its argument: not for clang-tidy
        elif a == "-isystem" and i + 1 < len(args):
            out += [a, args[i + 1]]
            i += 1
        elif a.startswith(("-std=", "-I", "-D", "-W", "-f", "-isystem")):
            out.append(a)
        i += 1
    return out


def compile_commands():
    """Each runtime source with the flags its make rule compiles it with.

    `make -n -w` prints the engine's sub-make too; its paths are relative to
    geistlib/ and are skipped. A tool and its test compile and link in one
    call, so every runtime .c of a call counts, not only the one after -c."""
    out = subprocess.run(
        ["make", "-n", "-B", "-w", *GOALS], cwd=ROOT, check=True, capture_output=True, text=True
    ).stdout
    dirs, by_src = [ROOT], {}
    for line in out.replace("\\\n", " ").splitlines():
        m = ENTER.match(line)
        if m:
            if m[1] == "Entering":
                dirs.append(os.path.realpath(m[2]))
            elif len(dirs) > 1:
                dirs.pop()
            continue
        if dirs[-1] != os.path.realpath(ROOT):
            continue
        try:
            args = shlex.split(line)
        except ValueError:
            continue
        if not args or os.path.basename(args[0]) not in COMPILERS:
            continue
        flags = flags_of(args)
        for src in args[1:]:
            if src.endswith(".c") and src.startswith(("src/", "tools/")) and src not in by_src:
                by_src[src] = {"directory": ROOT, "arguments": [args[0], *flags, "-c", src], "file": src}
    return [by_src[k] for k in sorted(by_src)]


def extra_args():
    if sys.platform != "darwin":
        return []
    sdk = subprocess.run(
        ["xcrun", "--show-sdk-path"], check=True, capture_output=True, text=True
    ).stdout.strip()
    return ["--extra-arg=-isysroot" + sdk]


def tidy(tool, db, src):
    r = subprocess.run(
        [tool, "-p", db, "--quiet", *extra_args(), src],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode not in (0, 1) or "[clang-diagnostic-error]" in r.stdout:
        # A file clang-tidy could not compile has no findings: never pass it.
        raise RuntimeError(f"{tool} on {src}: exit {r.returncode}\n{(r.stdout + r.stderr)[-2000:]}")
    return r.stdout


def findings(tool, cmds):
    """{(file, check, message): count}; a header's finding counts once."""
    with tempfile.TemporaryDirectory() as db:
        with open(os.path.join(db, "compile_commands.json"), "w") as f:
            json.dump(cmds, f)
        jobs = int(os.environ.get("JOBS", os.cpu_count() or 1))
        with ThreadPoolExecutor(jobs) as pool:
            outputs = list(pool.map(lambda c: tidy(tool, db, c["file"]), cmds))
    seen = set()
    for text in outputs:
        for line in text.splitlines():
            m = FINDING.match(line)
            if m is None:
                continue
            path = os.path.relpath(os.path.normpath(os.path.join(ROOT, m[1])), ROOT)
            if path.startswith(REPORTED):
                seen.add((path, int(m[2]), int(m[3]), m[6], m[5]))
    counts = collections.Counter((p, check, msg) for p, _, _, check, msg in seen)
    return counts


def read_baseline():
    counts = collections.Counter()
    if os.path.exists(BASELINE):
        with open(BASELINE) as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                n, path, check, msg = line.rstrip("\n").split("\t", 3)
                counts[(path, check, msg)] = int(n)
    return counts


def unexplained(base):
    """Analyzer findings in the baseline without a line in static-reasons.txt."""
    with open(REASONS) as f:
        known = {tuple(l.split("\t")[:2]) for l in f if not l.startswith("#")}
    keys = {(p, c) for p, c, _ in base if c.startswith("clang-analyzer-")}
    return sorted(keys - known)


def ratchet(now, base):
    """(findings above the baseline, baseline entries no longer found)."""
    return now - base, base - now


def write_baseline(counts):
    with open(BASELINE, "w") as f:
        f.write("# scripts/check_static.py: known findings; this list only shrinks.\n")
        f.write("# count, file, check, message. Regenerate with --update.\n")
        for (path, check, msg), n in sorted(counts.items()):
            f.write(f"{n}\t{path}\t{check}\t{msg}\n")


def nolint_ok(line):
    m = NOLINT.search(line)
    return m is None or re.match(r"\s*:\s*\w", m[2]) is not None


def nolint_without_reason():
    tracked = subprocess.run(
        ["git", "ls-files", "src", "include", "tests", "tools"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.split()
    bad = []
    for path in tracked:
        if not path.endswith((".c", ".h", ".m")):
            continue
        with open(os.path.join(ROOT, path), errors="replace") as f:
            for no, line in enumerate(f, 1):
                if not nolint_ok(line):
                    bad.append(f"{path}:{no}: NOLINT without a reason")
    return bad


def selftest(tool):
    """Each `// expect: <check>` in tests/static/ must be reported."""
    cmds = compile_commands()
    flags = [a for a in cmds[0]["arguments"] if a.startswith(("-std=", "-I", "-D"))]
    fixtures = sorted(
        os.path.join("tests/static", n)
        for n in os.listdir(os.path.join(ROOT, "tests/static"))
        if n.endswith(".c")
    )
    want = set()
    for path in fixtures:
        with open(os.path.join(ROOT, path)) as f:
            want |= {(path, c) for c in re.findall(r"// expect: (\S+)", f.read())}
    seeded = [
        {"directory": ROOT, "arguments": ["cc", *flags, "-c", p, "-o", "/dev/null"], "file": p}
        for p in fixtures
    ]
    got = {(p, check) for p, check, _ in findings(tool, seeded)}
    missed = sorted(want - got)
    for path, check in missed:
        print(f"selftest: {path}: {check} was not reported")
    print(f"selftest: {len(want) - len(missed)} of {len(want)} seeded defects found")
    return 1 if missed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    tool = os.environ.get("CLANG_TIDY", "clang-tidy")
    if a.selftest:
        return selftest(tool)

    bad = nolint_without_reason()
    bad += [f"{p}: {c}: in the baseline without a reason" for p, c in unexplained(read_baseline())]
    for line in bad:
        print(line)
    now = findings(tool, compile_commands())
    if a.update:
        write_baseline(now)
        print(f"baseline: {sum(now.values())} findings")
        return 1 if bad else 0
    new, gone = ratchet(now, read_baseline())
    for (path, check, msg), n in sorted(new.items()):
        print(f"new: {path}: {check}: {msg} (+{n})")
    for (path, check, msg), n in sorted(gone.items()):
        print(f"gone, drop it from the baseline: {path}: {check}: {msg} (-{n})")
    print(f"check_static: {sum(now.values())} findings, {sum(new.values())} new, "
          f"{sum(gone.values())} gone")
    return 1 if bad or new or gone else 0


if __name__ == "__main__":
    sys.exit(main())

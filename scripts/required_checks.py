#!/usr/bin/env python3
"""required_checks.py — keep main's required status checks in step with CI.

main's branch protection on git.geisten.net requires the fast per-PR jobs of
.gitea/workflows/ci.yml by their exact status names (without any, Gitea's
auto-merge merged PR #250 20 s after CI started). The list lives in
.gitea/required-checks.txt and:

    check    derive every job's status name from .gitea/workflows/ci.yml
             (matrix include entries expanded the way Gitea names them) and
             check the file against it: every listed name exists, and a job
             with one listed leg has all its legs listed (a new or renamed
             matrix leg is caught). Exit 1 on any difference. Runs in CI,
             needs no token.
    print    print every derived status name (to pick from).
    verify   compare the file with the live branch protection.
    sync     write the file's list into the live branch protection.

verify/sync read GITEA_TOKEN (repository admin) and talk to
https://git.geisten.net/api/v1/repos/geisten/geist-runtime/branch_protections/main.

Ported from geistlib's scripts/required_checks.py. Stdlib only: the parser
needs just the job names and the scalar fields of matrix include entries.
"""
from __future__ import annotations

import json
import os
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".gitea" / "workflows" / "ci.yml"
LIST = ROOT / ".gitea" / "required-checks.txt"
PROTECTION = "https://git.geisten.net/api/v1/repos/geisten/geist-runtime/branch_protections/main"
EVENT = "pull_request"


def scalar(value: str) -> str:
    value = value.split(" #", 1)[0].strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
        value = value[1:-1]
    return value


def parse(text: str) -> tuple[str, list[dict]]:
    """The workflow name, and per job in file order: key, name, entries
    (matrix include, each a dict of its scalar fields)."""
    lines = text.splitlines()
    wf = next((scalar(l[5:]) for l in lines if l.startswith("name:")), WORKFLOW.stem)
    start = lines.index("jobs:")
    out: list[dict] = []
    i = start + 1
    while i < len(lines):
        if re.match(r"^\S", lines[i]):
            break
        m = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", lines[i])
        if not m:
            i += 1
            continue
        key, name, entries = m.group(1), None, []
        i += 1
        while i < len(lines) and not re.match(r"^  \S", lines[i]) and not re.match(r"^\S", lines[i]):
            line = lines[i]
            nm = re.match(r"^    name:\s*(.+)$", line)
            if nm:
                name = scalar(nm.group(1))
            inc = re.match(r"^( +)include:\s*$", line)
            if inc:
                base = len(inc.group(1))
                i += 1
                item_indent = None
                while i < len(lines):
                    l2 = lines[i]
                    if l2.strip() == "" or l2.lstrip().startswith("#"):
                        i += 1
                        continue
                    ind = len(l2) - len(l2.lstrip())
                    if ind <= base:
                        break
                    im = re.match(r"^( *)- ([A-Za-z0-9_-]+):\s*(.*)$", l2)
                    if im and (item_indent is None or len(im.group(1)) == item_indent):
                        item_indent = len(im.group(1))
                        entries.append({im.group(2): scalar(im.group(3))})
                    elif item_indent is not None and ind == item_indent + 2:
                        km = re.match(r"^ *([A-Za-z0-9_-]+):\s*(.*)$", l2)
                        if km and entries:
                            entries[-1][km.group(1)] = scalar(km.group(2))
                    i += 1
                continue
            i += 1
        out.append({"key": key, "name": name, "entries": entries})
    return wf, out


def derived(text: str) -> dict[str, list[str]]:
    """Status names per job key. Gitea names a matrix leg of a job without a
    templated name "<name> (<values in sorted key order>)"."""
    wf, jobs = parse(text)
    out: dict[str, list[str]] = {}
    for job in jobs:
        template, entries = job["name"] or job["key"], job["entries"]
        fields = re.findall(r"\$\{\{\s*matrix\.([A-Za-z0-9_-]+)\s*\}\}", template)
        if not entries:
            names = [template]
        elif fields:
            names = []
            for entry in entries:
                name = template
                for field in fields:
                    if field not in entry:
                        raise SystemExit(f"matrix entry {entry} has no {field!r} for {template!r}")
                    name = re.sub(r"\$\{\{\s*matrix\." + field + r"\s*\}\}", entry[field], name)
                names.append(name)
        else:
            names = [f"{template} ({', '.join(e[k] for k in sorted(e))})" for e in entries]
        out[job["key"]] = sorted(f"{wf} / {n} ({EVENT})" for n in names)
    return out


def listed() -> list[str]:
    return sorted(l.strip() for l in LIST.read_text().splitlines()
                  if l.strip() and not l.lstrip().startswith("#"))


def api(method: str, body: dict | None = None) -> dict:
    token = os.environ.get("GITEA_TOKEN")
    if not token:
        raise SystemExit("GITEA_TOKEN (repository admin) is not set")
    req = urllib.request.Request(PROTECTION, method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Authorization": f"token {token}",
                                          "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read())


def check(per_job: dict[str, list[str]], want: list[str]) -> int:
    rc = 0
    every = {n for names in per_job.values() for n in names}
    for n in want:
        if n not in every:
            print(f"  not a job of {WORKFLOW.relative_to(ROOT).as_posix()}: {n}")
            rc = 1
    for key, names in per_job.items():
        if any(n in want for n in names):
            for n in names:
                if n not in want:
                    print(f"  job {key} is required, but this leg is not listed: {n}")
                    rc = 1
    if not want:
        print("  the list is empty")
        rc = 1
    return rc


def main() -> int:
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    per_job = derived(WORKFLOW.read_text())
    want = listed()
    if cmd == "print":
        print("\n".join(n for names in per_job.values() for n in names))
        return 0
    if cmd == "check":
        rc = check(per_job, want)
        print(f"required checks: {len(want)} listed, "
              + ("all current" if rc == 0 else
                 "list out of date: fix .gitea/required-checks.txt (names: "
                 "`scripts/required_checks.py print`), then "
                 "`GITEA_TOKEN=... scripts/required_checks.py sync` after the merge"))
        return rc
    if cmd == "verify":
        live = sorted(api("GET").get("status_check_contexts") or [])
        for n in sorted(set(live) - set(want)):
            print(f"  only in branch protection: {n}")
        for n in sorted(set(want) - set(live)):
            print(f"  only in {LIST.relative_to(ROOT).as_posix()}: {n}")
        rc = 0 if live == want else 1
        print(f"branch protection: {len(live)} required, " + ("matches the file" if rc == 0 else "differs"))
        return rc
    if cmd == "sync":
        if check(per_job, want):
            raise SystemExit("the file does not match ci.yml; fix it first (check)")
        got = api("PATCH", {"enable_status_check": True, "status_check_contexts": want})
        print(f"branch protection now requires {len(got['status_check_contexts'])} checks")
        return 0
    raise SystemExit(f"unknown command {cmd!r}: check | print | verify | sync")


if __name__ == "__main__":
    sys.exit(main())

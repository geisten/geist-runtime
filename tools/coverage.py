"""Line coverage of the runtime and the CLI from a `make coverage` build.

  coverage.py BUILD_DIR [--gcov "xcrun llvm-cov gcov"] [--out report.md]

Runs gcov on every .gcda under BUILD_DIR. A source compiled into several
binaries (the library, the test build, the no-network build) counts a line as
covered when any of them ran it. Reports src/ and tools/geistr/ per file,
sorted by uncovered lines, as Markdown. Standard library only."""
import argparse
import collections
import os
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCOPE = ('src/', 'tools/geistr/')


def gcov_lines(gcov, gcda):
    """{source: {line: hits}} for one .gcda, from gcov's text output. gcov runs
    in the repository (sources were compiled by relative path) and writes its
    .gcov files there; they are read and removed."""
    before = set(ROOT.glob('*.gcov'))
    subprocess.run([*gcov, '-p', '-o', str(gcda.parent), str(gcda)], cwd=ROOT, capture_output=True, text=True)
    out = {}
    for f in set(ROOT.glob('*.gcov')) - before:
        lines, source = {}, None
        for row in f.read_text(errors='replace').splitlines():
            count, _, rest = row.partition(':')
            number, _, text = rest.partition(':')
            number = number.strip()
            if number == '0' and text.startswith('Source:'):
                source = os.path.normpath(os.path.join(ROOT, text[len('Source:'):]))
            elif number.isdigit() and number != '0':
                count = count.strip()
                if count not in ('-', ''):
                    lines[int(number)] = 0 if count.startswith('#') or count.startswith('=') else 1
        if source:
            out[source] = lines
        f.unlink()
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('build', type=Path)
    p.add_argument('--gcov', default='gcov', help='the gcov command, e.g. "xcrun llvm-cov gcov" or gcov-14')
    p.add_argument('--out', type=Path)
    a = p.parse_args()
    gcov = shlex.split(a.gcov)
    merged = collections.defaultdict(dict)  # source → line → covered
    for gcda in sorted(a.build.rglob('*.gcda')):
        for source, lines in gcov_lines(gcov, gcda.resolve()).items():
            for n, hit in lines.items():
                merged[source][n] = merged[source].get(n, 0) or hit
    rows = []
    for source, lines in merged.items():
        rel = os.path.relpath(source, ROOT)
        if rel.startswith(SCOPE) and rel.endswith('.c'):
            covered = sum(lines.values())
            rows.append((rel, covered, len(lines)))
    rows.sort(key=lambda r: r[2] - r[1], reverse=True)
    total = sum(r[1] for r in rows), sum(r[2] for r in rows)
    report = [f'# Line coverage: {100 * total[0] / max(total[1], 1):.1f} % ({total[0]} of {total[1]} lines)', '',
              'make coverage: `test`, `test-real`, `test-embed`, `test-geistr`; src/ and tools/geistr/, most uncovered first.', '',
              '| file | covered | lines | % | uncovered |', '|---|---|---|---|---|']
    report += [f'| {rel} | {c} | {n} | {100 * c / max(n, 1):.1f} | {n - c} |' for rel, c, n in rows]
    text = '\n'.join(report) + '\n'
    if a.out:
        a.out.write_text(text)
    print(text)


if __name__ == '__main__':
    main()

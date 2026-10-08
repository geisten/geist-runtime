"""Offline frontend contract; real success/cancel cases use the same driver.
The driver is a test dispatcher, not a second product CLI.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def call(binary, args, expected, stdin=None):
    result = subprocess.run([str(binary), *args], input=stdin, capture_output=True, timeout=20)
    assert result.returncode == expected, (args, result.returncode, result.stderr)
    assert result.stdout == b'', (args, result.stdout)
    assert result.stderr, args


def main():
    binary = Path(sys.argv[1]).resolve()
    base = ['stub:echo', '--question', 'Q?', '--config', 'missing.json',
            '--option', 'yes', 'yes', '--option', 'no', 'no']
    call(binary, [], 2)
    call(binary, base, 1)  # actual linked stub feature is off, no generation
    call(binary, base + ['--unknown', 'x'], 2)
    call(binary, base + ['--question', 'second'], 2)
    call(binary, base + ['--mode', 'auto'], 2)
    call(binary, base + ['--processor', 'ternary'], 2)
    call(binary, base + ['--option', 'yes', 'duplicate'], 2)
    call(binary, base + ['--option', '', 'empty'], 2)
    for bad in ['bad\nID', 'bad\rID', '\x1b[31m', '\x7f', '\u0085', '\u2028', '\u2029', 'x' * 129]:
        call(binary, base + ['--option', bad, 'invalid'], 2)
    with tempfile.TemporaryDirectory() as directory:
        question = Path(directory) / 'question.txt'
        args = ['stub:echo', '--question-file', str(question), '--config', 'missing.json', '--option', 'x', 'yes']
        question.write_bytes(b'q' * 65536)
        call(binary, args, 1)
        question.write_bytes(b'q' * 65537)
        call(binary, args, 2)
        question.write_bytes(b'q\0q')
        call(binary, args, 2)
        question.write_bytes(b'\xc0\xaf')
        call(binary, args, 2)
        args[2] = '-'
        call(binary, args, 2, b'q' * 65537)
        call(binary, args, 2, b'q\0q')
    print('decide CLI: usage, feature-off, IDs, duplicate options and bounded file/stdin input passed')


if __name__ == '__main__':
    main()

"""Offline frontend contract; real success/cancel cases use the same driver.
The driver is a test dispatcher, not a second product CLI.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def call(binary, args, expected, stdin=None):
    result = subprocess.run([str(binary), *args], input=stdin, capture_output=True, timeout=20)
    assert result.returncode == expected, (args, result.returncode, result.stderr)
    assert result.stdout == b'', (args, result.stdout)
    assert result.stderr, args
    return result.stderr


def request(questions=None, context='Shared context.'):
    if questions is None:
        questions = [{'id': 'q1', 'question': 'Q?', 'options': [{'id': 'yes', 'description': 'Yes'},
                                                               {'id': 'no', 'description': 'No'}]}]
    return json.dumps({'context': context, 'questions': questions}, ensure_ascii=False).encode()


def esc(text):
    """JSON text with ~ for its backslashes, as UTF-8."""
    return text.replace('~', chr(92)).encode()


def ctx(text='', raw=None):
    """A valid one-question request whose context is the JSON string text (~ for
    its backslashes) or the raw JSON value."""
    value = raw if raw is not None else '"' + text + '"'
    return esc('{"context":' + value + ',"questions":[{"id":"q","question":"Q",'
               '"options":[{"id":"a","description":"A"}]}]}')


def batch(binary):
    """decide-batch (#164) on the stub: the request passes the parser and
    validation (exit 1: the linked engine is off, nothing guessed) or is
    refused as input (exit 2) before any model work."""
    off = b'DECISION_CONTEXT=1; no fallback'
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'request.json'
        base = ['decide-batch', 'stub:echo', '--config', 'missing.json', '--shared-context',
                '--request-file', str(path)]

        def accepted(body, extra=()):
            path.write_bytes(body)
            assert off in call(binary, base + list(extra), 1), body[:200]

        def refused(body, extra=()):
            path.write_bytes(body)
            assert off not in call(binary, base + list(extra), 2), body[:200]

        accepted(request())
        assert off in call(binary, base[:-1] + ['-'], 1, request())  # stdin
        accepted(request(context=''))  # an empty common context
        accepted(ctx('plain ~u00e4 ~ud83d~ude00'))  # the control for the refused ctx() cases below
        accepted(esc('{"con~u0074ext":"",' + ctx()[1:].decode()[13:]))  # an escaped key is the same key
        # Unicode IDs, escapes, reordered keys and whitespace between all tokens.
        accepted(esc(' \n{ "questions" : [ { "options":[{"description":"r~u00f6t","id":"r\u00f6t"},'
                     '{"id":"~ud83d~ude00","description":"~"q~" ~~ ~/ ~b~f~n~r~t"}],'
                     '"question":"Q?", "id":"frage-\u00fc"} ] ,\t"context":"" }\r\n'))
        accepted(request([{'id': f'q{i}', 'question': 'Q', 'options': [{'id': 'a', 'description': 'A'}]}
                          for i in range(32)]))
        accepted(request([{'id': 'q', 'question': 'Q',
                           'options': [{'id': chr(65 + i), 'description': 'x'} for i in range(26)]}]))
        accepted(request([{'id': 'x' * 128, 'question': 'Q' * 65536,
                           'options': [{'id': 'a', 'description': 'A'}]}], context=''))
        padded = request()
        accepted(padded + b' ' * (1048576 - len(padded)))  # exactly the 1 MiB bound
        refused(padded + b' ' * (1048577 - len(padded)))
        assert off not in call(binary, base[:-1] + ['-'], 2, padded + b' ' * (1048577 - len(padded)))
        one = {'id': 'q1', 'question': 'Q?', 'options': [{'id': 'yes', 'description': 'Yes'}]}
        for body in [b'', b'[]', b'{}', b'{"context":""}', b'{"questions":[]}', request([]),
                     request() + b'x', request() + b'{}', request()[:-1],
                     b'{"context":"","context":"","questions":[]}',
                     b'{"context":"","questions":[{"id":"q","id":"r","question":"Q",'
                     b'"options":[{"id":"a","description":"A"}]}]}',
                     b'{"context":"","questions":[{"id":"q","question":"Q",'
                     b'"options":[{"id":"a","description":"A","id":"b"}]}]}',
                     b'{"context":"","questions":[{"id":"q","question":"Q",'
                     b'"options":[{"id":"a","description":"A"}],"x":1}]}',
                     request() [:-1] + b',"extra":1}',
                     ctx(raw='1'), ctx(raw='null'), ctx(raw='["x"]'),
                     b'{"context":"","questions":[{"id":"q","question":"Q","options":[]}]}',
                     b'{"context":"","questions":[{"id":"q","question":"Q",'
                     b'"options":[{"id":"a","description":"A"},]}]}',
                     b'{"context":"","questions":[{"id":"q","question":"Q"}]}',
                     ctx('a\nb'), ctx('a\x01'),  # raw controls inside a string
                     ctx('~u0000'), ctx('~ud800'), ctx('~udc00'), ctx('~ud800~u0041'), ctx('~ud800x'),
                     ctx('~x41'), ctx('~U0041'), ctx('~u12G4'), ctx('~u12'), ctx('~'), ctx('~a'),
                     request([one] * 33), request([one, one]),
                     request([{**one, 'options': [{'id': chr(65 + i), 'description': 'x'} for i in range(27)]}]),
                     request([{**one, 'options': [{'id': 'a', 'description': 'x'}] * 2}]),
                     request([{**one, 'id': ''}]), request([{**one, 'id': 'x' * 129}]),
                     request([{**one, 'question': ''}]), request([{**one, 'question': 'Q' * 65537}], context=''),
                     request(context='C' * 65537),
                     request([{**one, 'options': [{'id': 'a', 'description': ''}]}]),
                     ctx('\xc0\xaf').replace(b'\xc3\x80\xc2\xaf', b'\xc0\xaf'),
                     request(context='\xff').replace(b'\xc3\xbf', b'\xff')]:
            refused(body)
        for bad in ['bad\nID', 'bad\tID', '\x1b[31m', '\x7f', '\u0085', '\u2028', '\u2029']:
            refused(request([{**one, 'id': bad}]))
            refused(request([{**one, 'options': [{'id': bad, 'description': 'x'}]}]))
        path.write_bytes(request())
        assert off in call(binary, base[:2] + base[4:], 1)  # without --config: the verified profiles are allowed
        for args in [[], ['decide-batch'], base[:5], base + ['--shared-context'],
                     base + ['--config', 'x'], base + ['--unknown', 'x'], base + ['--mode', 'dense'],
                     base + ['--processor', 'ternary'], base + ['--request-file', str(path)],
                     base + ['--context-tokens'], base[:-1] + [str(Path(directory) / 'absent.json')],
                     base[:4] + base[5:]]:  # without --shared-context
            call(binary, args, 2)
        for value in ['0', '-1', '+5', '', ' 5', '5x', '0x10', '4294967296', '99999999999999999999999']:
            call(binary, base + ['--context-tokens', value], 2)
        for value in ['0', '-1', '', '1.5', '17592186044416', '18446744073709551616']:
            call(binary, base + ['--snapshot-budget-mib', value], 2)
        accepted(request(), ['--context-tokens', '4294967295', '--snapshot-budget-mib', '17592186044415',
                             '--processor', 'gpu', '--profile', 'gemma4-e2b-q4-v1'])


BONSAI2 = '3907dc1658db1f78a9826bf8d5bcb8dc65db0d466388937af57f2294fae62ec1'  # bonsai2-27b-pq2-v1's artifact


def stamp(path, sha):
    """The receipt geistr pull / a catalog check leaves for path (src/catalog.c stamp)."""
    s = os.lstat(path)
    return (f'v1 {sha} {s.st_dev} {s.st_ino} {s.st_size} {s.st_mode} {s.st_uid} {s.st_gid} {s.st_nlink} '
            f'{s.st_mtime_ns // 10**9} {s.st_mtime_ns % 10**9} {s.st_ctime_ns // 10**9} {s.st_ctime_ns % 10**9}\n')


def receipts(binary):
    """#285: a model given by path is identified by its receipt, never hashed at
    decide time. With the receipt of the Bonsai 2 profile's artifact it is that
    profile (early exit by default); without one, or once the file changed, it
    is unverified (no profile, no early-exit default). With --config (a list
    of SHA-256s) a file without a receipt is hashed, so the list matches it.
    Thinking is the chat template's, the same for a catalog id and a path (the
    real-model test)."""
    def resolve(path, configured=False):
        out = subprocess.run([str(binary), 'receipt-config' if configured else 'receipt', str(path)],
                             capture_output=True, timeout=20, check=True)
        return out.stdout.decode().split()
    unverified = ['-', 'none', '0']
    with tempfile.TemporaryDirectory() as directory:
        models = Path(directory) / 'models'
        (models / '.verified').mkdir(parents=True)
        model = models / 'Ternary-Bonsai-2-27B-PQ2_0.gguf'
        model.write_bytes(b'GGUF' + bytes(60))
        assert resolve(model) == unverified
        assert resolve(models / 'missing.gguf') == unverified
        assert resolve(model, True) == [hashlib.sha256(model.read_bytes()).hexdigest(), 'none', '0']  # hashed
        assert resolve(models / 'missing.gguf', True) == unverified
        (models / '.verified' / BONSAI2).write_text(stamp(model, BONSAI2))
        assert resolve(model) == [BONSAI2, 'bonsai2-27b-pq2-v1', '1']
        assert resolve(model, True) == [BONSAI2, 'bonsai2-27b-pq2-v1', '1']  # the receipt, not a hash
        other = 'ab' * 32  # a receipt of another artifact: no profile
        (models / '.verified' / BONSAI2).unlink()
        (models / '.verified' / other).write_text(stamp(model, other))
        assert resolve(model) == [other, 'none', '0']
        (models / '.verified' / other).write_text(stamp(model, BONSAI2))  # names another SHA than its own
        assert resolve(model) == unverified
        (models / '.verified' / other).unlink()
        (models / '.verified' / BONSAI2).write_text(stamp(model, BONSAI2))
        copy = Path(directory) / model.name  # the same bytes elsewhere: that file has no receipt
        copy.write_bytes(model.read_bytes())
        assert resolve(copy) == unverified
        link = models / 'link.gguf'  # a link is not the file the receipt was written for
        link.symlink_to(model)
        assert resolve(link) == unverified
        cwd = os.getcwd()
        os.chdir(models)
        try:
            assert resolve(model.name) == [BONSAI2, 'bonsai2-27b-pq2-v1', '1']  # a relative path
        finally:
            os.chdir(cwd)
        with open(model, 'r+b') as f:  # changed after its receipt: unverified until checked again
            f.write(b'GGUX')
        assert resolve(model) == unverified
        assert resolve(model, True)[0] == hashlib.sha256(model.read_bytes()).hexdigest()


def options(binary):
    """#288: what decide does without options, and the removed options as
    aliases. The driver parses as decide does and applies what a profile
    brings (threshold, early exit), as decide does after loading."""
    def parsed(profile, *argv, warns=None):
        out = subprocess.run([str(binary), 'args', profile, 'model.gguf', *argv], capture_output=True, timeout=20)
        assert out.returncode == 0, (argv, out.stderr)
        if warns is None:
            assert out.stderr == b'', (argv, out.stderr)  # the current options warn about nothing
        else:
            assert b'deprecated and goes away in the next release' in out.stderr and warns.encode() in out.stderr, \
                (argv, out.stderr)
        return json.loads(out.stdout)
    short = ['Q?', 'yes', 'no']
    # Without options, what the verified profile carries (src/decision_profile.c): Bonsai 2 27B escalates to chat
    # below 0.78 (geistlib#204) with a cap of 8192 and early exit at 0.8 every 128 tokens (geistlib#286). Every
    # other model, verified (Gemma 4) or not, has threshold 0: the direct answer, never an escalation; its chat
    # cap, should --threshold ask for one, is 4096.
    for profile, threshold, cap in [('none', 0, 4096), ('gemma4-e2b-q4-v1', 0, 4096),
                                    ('bonsai2-27b-pq2-v1', 0.78, 8192)]:
        assert parsed(profile, *short) == {
            'threshold': threshold, 'escalate': 'chat', 'early_exit': profile == 'bonsai2-27b-pq2-v1',
            'probe_interval': 128, 'exit_threshold': 0.8, 'max_tokens': cap, 'question': 'Q?',
            'question_file': '', 'options': 2}, profile
    bonsai = 'bonsai2-27b-pq2-v1'
    assert parsed(bonsai, *short, '--escalate', 'chat') == parsed(bonsai, *short)  # the default, spelled out
    # --no-escalate: below the threshold exit 3 (what no --escalate did); no chat, so no early exit
    assert parsed(bonsai, *short, '--no-escalate')['escalate'] is None
    assert parsed(bonsai, *short, '--no-escalate')['early_exit'] is False
    other = parsed(bonsai, *short, '--escalate', 'other.gguf')
    assert other['escalate'] == 'other.gguf' and other['early_exit'] is False and other['threshold'] == 0.78
    # --threshold and --max-tokens override the profile's, in both directions
    assert parsed('gemma4-e2b-q4-v1', *short, '--threshold', '0.9')['threshold'] == 0.9
    assert parsed(bonsai, *short, '--threshold', '0')['threshold'] == 0
    assert parsed(bonsai, *short, '--max-tokens', '2048')['max_tokens'] == 2048
    assert parsed('none', *short, '--max-tokens', '8192')['max_tokens'] == 8192
    # --threshold overrides the default; 0 never escalates (every answer direct, the old default)
    for value, expected in [('0.9', 0.9), ('1', 1), ('0', 0), ('0.5', 0.5)]:
        assert parsed('none', *short, '--threshold', value)['threshold'] == expected
        assert parsed(bonsai, *short, '--threshold', value, '--no-escalate')['threshold'] == expected
    # Early exit: --early-exit[=P] and --no-early-exit only
    assert parsed('none', *short, '--early-exit')['early_exit'] is True
    assert parsed('none', *short, '--early-exit=0.9')['exit_threshold'] == 0.9
    assert parsed('none', *short, '--early-exit=0.9')['early_exit'] is True
    assert parsed(bonsai, *short, '--early-exit=0.65')['exit_threshold'] == 0.65
    assert parsed(bonsai, *short, '--no-early-exit')['early_exit'] is False
    assert parsed('gemma4-e2b-q4-v1', *short, '--max-tokens', '1536')['max_tokens'] == 1536
    # --question TEXT|@FILE|-
    assert parsed('none', '--question', 'text', '--option', 'a', 'A')['question'] == 'text'
    at = parsed('none', '--question', '@q.txt', '--option', 'a', 'A')
    assert at['question'] == '' and at['question_file'] == 'q.txt'
    assert parsed('none', '--question', '-', '--option', 'a', 'A')['question_file'] == '-'
    assert parsed('none', '@literal', 'a')['question'] == '@literal'  # the short form's question is text
    # The removed options, for one release: the same settings, with a warning on stderr
    alias = parsed('none', '--question-file', 'q.txt', '--option', 'a', 'A', warns='--question @FILE')
    assert alias == at, (alias, at)
    assert parsed('none', '--question-file', '-', '--option', 'a', 'A', warns='--question @FILE') == \
        parsed('none', '--question', '-', '--option', 'a', 'A')
    assert parsed('none', *short, '--exit-threshold', '0.9', warns='--early-exit=P') == \
        parsed('none', *short, '--early-exit=0.9')
    assert parsed('none', *short, '--early-exit', '--exit-threshold', '0.9', warns='--early-exit=P') == \
        parsed('none', *short, '--early-exit=0.9')
    interval = parsed('none', *short, '--probe-interval', '64', warns='fixed at the validated 128')
    assert interval == {**parsed('none', *short, '--early-exit'), 'probe_interval': 64}, interval
    # Removed and conflicting: usage errors, as for any option
    for argv in [['--no-escalate', '--early-exit'], ['--escalate', 'm.gguf', '--early-exit=0.9'],
                 ['--no-escalate', '--max-tokens', '64'], ['--no-escalate', '--exit-threshold', '0.9'],
                 ['--question', 'a', '--question-file', 'q.txt'], ['--early-exit=', '--threshold', '0.5'],
                 ['--early-exit=1'], ['--threshold', '1.01'], ['--no-early-exit', '--exit-threshold', '0.9']]:
        out = subprocess.run([str(binary), 'args', 'none', 'model.gguf', *short, *argv], capture_output=True,
                             timeout=20)
        assert out.returncode == 2 and out.stdout == b'' and out.stderr, (argv, out)


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
    # Short form: the question, then option IDs (each its own description); --config is optional.
    call(binary, ['stub:echo', 'Q?', 'yes', 'no'], 1)
    call(binary, ['stub:echo', 'Q?', 'yes', 'no', '--processor', 'cpu'], 1)
    call(binary, ['stub:echo', 'Q?', 'yes', 'yes'], 2)  # duplicate ID
    call(binary, ['stub:echo', 'Q?'], 2)  # no option
    call(binary, ['stub:echo', '--question', 'Q?', 'second?', 'yes'], 1)  # positional after --question: options
    call(binary, ['stub:echo', 'Q?', '--question', 'again'], 2)  # two questions
    call(binary, ['stub:echo', 'Q?'] + [chr(65 + i) for i in range(27)], 2)  # 27 options
    # --threshold, --escalate, --no-escalate: checked before any model work (#288: chat is the default).
    for value in ['-0.1', '1.5', 'abc', '', '0.5x', 'nan']:
        call(binary, ['stub:echo', 'Q?', 'yes', 'no', '--threshold', value], 2)
    call(binary, ['stub:echo', 'Q?', 'yes', 'no', '--threshold', '0.9', '--threshold', '0.8'], 2)
    for extra in [['--threshold', '1'], ['--threshold', '0'], ['--escalate', 'chat'], ['--no-escalate'],
                  ['--threshold', '0.9', '--escalate', 'other.gguf'], ['--threshold', '0.9', '--no-escalate']]:
        call(binary, ['stub:echo', 'Q?', 'yes', 'no', *extra], 1)
    call(binary, ['stub:echo', 'Q?', 'yes', 'no', '--escalate', 'chat', '--no-escalate'], 2)
    call(binary, ['stub:echo', 'Q?', 'yes', 'no', '--no-escalate', '--escalate', 'other.gguf'], 2)
    call(binary, ['stub:echo', 'Q?', 'yes', 'no', '--no-escalate', '--no-escalate'], 2)
    # --temperature (calibration) and --max-tokens (the chat escalation's cap, #210).
    for value in ['0', '0.001', '101', '-1', 'abc', '', '1x', 'nan', 'inf']:
        call(binary, ['stub:echo', 'Q?', 'yes', 'no', '--temperature', value], 2)
    call(binary, ['stub:echo', 'Q?', 'yes', 'no', '--temperature', '1.0546'], 1)
    call(binary, ['stub:echo', 'Q?', 'yes', 'no', '--temperature', '2', '--temperature', '3'], 2)
    chat = ['stub:echo', 'Q?', 'yes', 'no']  # the chat escalation is the default
    for value in ['0', '-1', '+5', ' 5', '28673', '1e3', '12x', '', '99999999999999999999999']:
        call(binary, chat + ['--max-tokens', value], 2)
    call(binary, chat + ['--max-tokens', '1536'], 1)
    call(binary, chat + ['--max-tokens', '1', '--temperature', '1.0546'], 1)
    call(binary, chat + ['--max-tokens', '28672', '--escalate', 'chat'], 1)
    call(binary, chat + ['--max-tokens', '512', '--max-tokens', '256'], 2)
    call(binary, chat + ['--max-tokens', '512', '--no-escalate'], 2)  # the chat's cap
    call(binary, chat + ['--escalate', 'other.gguf', '--max-tokens', '512'], 2)
    # Early exit (geistlib#236): --early-exit[=P] asks for it, --no-early-exit turns it off, for every model;
    # both belong to the chat escalation. The default is on only for the profile geistlib#286 validated
    # (Bonsai 2 27B); every other model, verified or not, reasons to the end unless asked.
    defaults = subprocess.run([str(binary), 'early-exit-default'], capture_output=True, timeout=20)
    assert defaults.returncode == 0, defaults
    rule = dict(line.split() for line in defaults.stdout.decode().splitlines())
    assert rule.pop('bonsai2-27b-pq2-v1') == '1' and rule and set(rule.values()) == {'0'}, defaults.stdout
    for extra in [['--early-exit'], ['--no-early-exit'], ['--early-exit=0.9'], ['--early-exit', '--exit-threshold', '0.9'],
                  ['--probe-interval', '64'], ['--exit-threshold', '0.9'], ['--early-exit=0.5', '--probe-interval', '64']]:
        call(binary, chat + extra, 1)
    for extra in [['--early-exit', '--early-exit'], ['--no-early-exit', '--no-early-exit'],
                  ['--early-exit', '--no-early-exit'], ['--no-early-exit', '--early-exit=0.9'],
                  ['--early-exit=0.9', '--early-exit'], ['--early-exit=0.9', '--exit-threshold', '0.8'],
                  ['--no-early-exit', '--probe-interval', '64'], ['--exit-threshold', '0.9', '--no-early-exit']]:
        call(binary, chat + extra, 2)
    for flag in ['--early-exit', '--early-exit=0.9', '--no-early-exit']:
        call(binary, chat + ['--no-escalate', flag], 2)  # the chat escalation's
        call(binary, chat + ['--escalate', 'other.gguf', flag], 2)
    for flag in [['--probe-interval', '64'], ['--exit-threshold', '0.9']]:
        call(binary, chat + ['--no-escalate'] + flag, 2)
    for value in ['0', '-1', '28673', 'x', '']:
        call(binary, chat + ['--early-exit', '--probe-interval', value], 2)
        call(binary, chat + ['--probe-interval', value], 2)
    for value in ['0', '1', '-0.5', '1.5', 'x', '']:
        call(binary, chat + ['--early-exit', '--exit-threshold', value], 2)
        call(binary, chat + ['--exit-threshold', value], 2)
        call(binary, chat + ['--early-exit=' + value], 2)
    options(binary)
    for bad in ['bad\nID', 'bad\rID', '\x1b[31m', '\x7f', '\u0085', '\u2028', '\u2029', 'x' * 129]:
        call(binary, base + ['--option', bad, 'invalid'], 2)
    with tempfile.TemporaryDirectory() as directory:
        question = Path(directory) / 'question.txt'
        # --question @FILE and - (stdin); --question-file, removed (#288), still reads them for one release
        for form in [['--question', '@' + str(question)], ['--question-file', str(question)]]:
            args = ['stub:echo', *form, '--config', 'missing.json', '--option', 'x', 'yes']
            question.write_bytes(b'q' * 65536)
            call(binary, args, 1)
            question.write_bytes(b'q' * 65537)
            call(binary, args, 2)
            question.write_bytes(b'q\0q')
            call(binary, args, 2)
            question.write_bytes(b'\xc0\xaf')
            call(binary, args, 2)
            args[2] = '-'
            call(binary, args, 1, b'q' * 65536)
            call(binary, args, 2, b'q' * 65537)
            call(binary, args, 2, b'q\0q')
        call(binary, ['stub:echo', '--question', '@' + str(Path(directory) / 'absent.txt'), 'x'], 2)
        call(binary, ['stub:echo', '--question', '@', 'x'], 2)
    receipts(binary)
    print('decide CLI: usage, feature-off, IDs, duplicate options, escalation, threshold and early-exit defaults and the aliases of the removed options (#288), receipts of a path (#285) and bounded file/stdin input passed')
    batch(binary)
    print('decide-batch CLI: flags, numeric bounds, request shape, IDs, limits, file/stdin and feature-off passed')


if __name__ == '__main__':
    main()

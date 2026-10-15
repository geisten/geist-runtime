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
    batch(binary)
    print('decide-batch CLI: flags, numeric bounds, request shape, IDs, limits, file/stdin and feature-off passed')


if __name__ == '__main__':
    main()

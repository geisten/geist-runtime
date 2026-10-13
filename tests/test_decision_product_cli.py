"""Single product binary E2E on one exact profile. No network or downloads."""
import json
import os
import re
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

binary, model, profile, processor = sys.argv[1:]
fixture = json.loads((Path(__file__).parent / 'fixtures/decisions' / (profile + '.json')).read_text())
profile_name = {'gemma4': 'gemma4-e2b-q4-v1', 'bonsai2': 'bonsai2-27b-pq2-v1'}[profile]
with tempfile.TemporaryDirectory() as directory:
    config = Path(directory) / 'decisions.json'
    config.write_text(json.dumps({'schema': 1, 'models': [{'sha256': fixture['artifact_sha256'],
        'enabled': True, 'profile': profile_name}]}))
    base = [binary, 'decide', model, '--config', str(config), '--processor', processor]
    options = ['--option', 'röt', '4', '--option', 'blau', '5']
    success = subprocess.run(base + ['--question', 'Which option equals 4?'] + options,
                             capture_output=True, timeout=120)
    assert success.returncode == 0, success.stderr
    assert success.stdout in [b'r\xc3\xb6t\n', b'blau\n'], success.stdout
    diagnostics = [json.loads(line) for line in success.stderr.splitlines() if line.startswith(b'{')]
    assert len(diagnostics) == 1, success.stderr
    d = diagnostics[0]
    assert d['schema'] == 1 and d['artifact_sha256'] == fixture['artifact_sha256']
    assert d['profile'] == profile_name and d['template_sha256'] == fixture['template_sha256']
    assert d['operation'] == 'decision' and d['model_calls'] == 1 and d['resolved_mode'] == 1
    pinned = re.search(r'^GEIST_REF\s*\?=\s*([0-9a-f]{40})', (Path(__file__).parents[1] / 'Makefile').read_text(), re.M)
    assert pinned and d['engine_revision'] == pinned.group(1), (d['engine_revision'], pinned)  # the Makefile's pin
    assert d['backend'] == 'metal' if processor == 'gpu' else d['backend'].startswith('cpu'), d['backend']
    assert d['until_result_ms'] >= d['setup_ms'] and d['preparation_ms'] >= 0
    for args, status in [(['--option', 'röt', 'duplicate'], 2),
                         (['--profile', 'unknown'], 1),
                         (['--mode', 'auto'], 2)]:
        bad = subprocess.run(base + ['--question', 'Q'] + options + args, capture_output=True, timeout=120)
        assert bad.returncode == status and not bad.stdout, bad
    disabled = {'schema': 1, 'models': [{'sha256': fixture['artifact_sha256'], 'enabled': False}]}
    config.write_text(json.dumps(disabled))
    denied = subprocess.run(base + ['--question', 'Q'] + options, capture_output=True, timeout=120)
    assert denied.returncode == 1 and not denied.stdout and b'disabled' in denied.stderr, denied
    config.write_text(json.dumps({'schema': 1, 'models': [{'sha256': fixture['artifact_sha256'],
        'enabled': True, 'profile': profile_name}]}))
    cancelled = subprocess.Popen(base + ['--question', 'Q'] + options, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # Install handler before scoring; SIGINT is remembered during artifact load.
    time.sleep(0.25)
    cancelled.send_signal(signal.SIGINT)
    stdout, stderr = cancelled.communicate(timeout=120)
    assert cancelled.returncode == 130 and not stdout and b'cancelled' in stderr, (cancelled.returncode, stdout, stderr)
    listing = subprocess.run([binary, 'catalog', '--models', directory, '--decision-config', str(config), '--json'],
                             capture_output=True, timeout=30, check=True)
    data = json.loads(listing.stdout)
    target = [m for m in data['models'] if m['sha256'] == fixture['artifact_sha256']]
    assert target and target[0]['decision'] == {'configured': True, 'profile': profile_name,
                                               'support': 'not_loaded', 'verified': False}

# decide-batch (#164): one process per case, timed from outside (process
# start to exit) against the command's own total_ms.
batch_on = b'shared contexts on' in subprocess.run([binary, 'decide-batch', '--help'], capture_output=True,
                                                   check=True).stdout
# longer than Metal's prefill chunk (256 tokens): a shorter one caches nothing there
context = 'You answer questions about simple facts. Pick exactly one option. ' * 24
questions = [
    {'id': 'frage-ü', 'question': 'Which option equals 4?',
     'options': [{'id': 'röt', 'description': '4'}, {'id': 'blau', 'description': '5'}]},
    {'id': 'q2', 'question': 'Is the sky usually green?',
     'options': [{'id': 'yes', 'description': 'Yes'}, {'id': 'no', 'description': 'No'}]},
    {'id': '✓ q3', 'question': 'Which one is a colour?',
     'options': [{'id': 'c', 'description': 'seven'}, {'id': 'd', 'description': 'red'},
                 {'id': 'e', 'description': 'fast'}]}]
with tempfile.TemporaryDirectory() as directory:
    config = Path(directory) / 'decisions.json'
    request = Path(directory) / 'request.json'
    policy = {'sha256': fixture['artifact_sha256'], 'enabled': True, 'profile': profile_name}
    config.write_text(json.dumps({'schema': 1, 'models': [policy]}))
    base = [binary, 'decide-batch', model, '--config', str(config), '--shared-context', '--processor', processor]

    def run(body, extra=(), stdin=False, **kwargs):
        request.write_bytes(body)
        args = base + ['--request-file', '-' if stdin else str(request), *extra]
        started = time.monotonic()
        result = subprocess.run(args, input=body if stdin else None, capture_output='preexec_fn' not in kwargs,
                                stderr=subprocess.PIPE if 'preexec_fn' in kwargs else None, timeout=300, **kwargs)
        wall_ms = (time.monotonic() - started) * 1000
        records = [json.loads(line) for line in result.stderr.splitlines() if line.startswith(b'{')]
        assert len(records) <= 1, result.stderr
        assert context.encode() not in result.stderr and b'Which option' not in result.stderr, result.stderr
        if records:
            r = records[0]
            assert r['exit'] == result.returncode and r['total_ms'] <= wall_ms, (r, wall_ms)
            assert r['total_ms'] >= r['until_result_ms'] >= 0 and r['total_ms'] >= r['load_ms'] >= 0, r
        return result, records[0] if records else None

    def body(qs, ctx=context, **dumps):
        return json.dumps({'questions': qs, 'context': ctx}, ensure_ascii=False, **dumps).encode()

    if not batch_on:
        off, _ = run(body(questions))
        assert off.returncode == 1 and not off.stdout and b'DECISION_CONTEXT=1' in off.stderr, off
    else:
        ok, r = run(body(questions))
        assert ok.returncode == 0, ok.stderr
        lines = ok.stdout.decode().split('\n')
        assert lines[-1] == '' and len(lines) == 4, ok.stdout
        selected = dict(line.split('\t') for line in lines[:-1])
        assert [line.split('\t')[0] for line in lines[:-1]] == [q['id'] for q in questions], ok.stdout
        for q in questions:
            assert selected[q['id']] in [o['id'] for o in q['options']], selected
        assert r['profile'] == profile_name and r['artifact_sha256'] == fixture['artifact_sha256']
        assert r['template_sha256'] == fixture['template_sha256'] and re.fullmatch('[0-9a-f]{64}', r['context_sha256'])
        assert r['engine_revision'] == pinned.group(1) and r['kv_cache'] == 'fp32' and r['activation'] is None
        assert r['backend'] == 'metal' if processor == 'gpu' else r['backend'].startswith('cpu'), r['backend']
        assert r['mode'] == 'dense' and r['max_prompt_tokens'] == 512 and r['max_snapshot_bytes'] == 256 << 20
        assert r['questions'] == r['prepared_questions'] == r['restores'] == 3 and r['failed_index'] is None
        assert r['cached_tokens'] > 0 and r['snapshot_bytes'] > 0 and r['setup_model_calls'] == 1, r
        assert r['cached_tokens'] % r['prefill_chunk'] == 0, r
        assert [p - u for p, u in zip(r['prompt_tokens'], r['uncached_tokens'])] == [r['cached_tokens']] * 3, r
        assert r['output_ms'] >= 0 and r['teardown_ms'] >= 0 and r['context_ms'] > 0, r
        # stdin, other key order and whitespace: the same answers.
        same, _ = run(body(questions, indent=3, sort_keys=True), stdin=True)
        assert same.returncode == 0 and same.stdout == ok.stdout, (same.stdout, ok.stdout)
        # Reversed questions and options: each answer stays with its question.
        flipped = [{**q, 'options': q['options'][::-1]} for q in questions[::-1]]
        rev, _ = run(body(flipped))
        assert rev.returncode == 0 and dict(line.split('\t') for line in rev.stdout.decode().split('\n')[:-1]) == \
            selected and rev.stdout.decode().split('\t')[0] == questions[-1]['id'], (rev.stdout, selected)
        # An explicit larger capacity within the loaded context; one beyond it.
        wide, r = run(body(questions), ['--context-tokens', '1024'])
        assert wide.returncode == 0 and r['max_prompt_tokens'] == 1024 and wide.stdout == ok.stdout, wide.stderr
        beyond, r = run(body(questions), ['--context-tokens', '4294967295'])
        assert beyond.returncode == 1 and not beyond.stdout and b'loaded model context' in beyond.stderr, beyond
        assert r['restores'] == 0 and 'cached_tokens' not in r, r
        # A middle question over capacity: refused in preflight, nothing scored, nothing printed.
        long = [questions[0], {**questions[1], 'question': 'word ' * 700}, questions[2]]
        over, r = run(body(long))
        assert over.returncode == 1 and not over.stdout and b'question 1 (q2)' in over.stderr, over.stderr
        assert r['failed_index'] == 1 and r['failed_id'] == 'q2' and r['restores'] == 0, r
        assert r['prepared_questions'] == 1 and r['prompt_tokens'] == [], r
        # Snapshot budget: the cached context does not fit in 1 MiB.
        tight, r = run(body(questions), ['--snapshot-budget-mib', '1'])
        assert tight.returncode == 1 and not tight.stdout and 'cached_tokens' not in r, tight.stderr
        # Output failure: stdout closed, the selections cannot be written.
        closed, r = run(body(questions), preexec_fn=lambda: os.close(1))
        assert closed.returncode == 1 and b'selection output failed' in closed.stderr and r['restores'] == 3, closed
        for policy_change, extra, why in [({'enabled': False}, [], b'disabled'),
                                          ({'mode': 'selected_rows'}, [], b'DENSE only'),
                                          ({}, ['--profile', 'unknown'], b'profile override')]:
            config.write_text(json.dumps({'schema': 1, 'models': [{**policy, **policy_change}]}))
            refused, _ = run(body(questions), extra)
            assert refused.returncode == 1 and not refused.stdout and why in refused.stderr, refused.stderr
        config.write_text(json.dumps({'schema': 1, 'models': [policy]}))
        request.write_bytes(body(questions))
        cancelled = subprocess.Popen(base + ['--request-file', str(request)], stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
        time.sleep(0.25)
        cancelled.send_signal(signal.SIGINT)
        stdout, stderr = cancelled.communicate(timeout=300)
        assert cancelled.returncode == 130 and not stdout and b'cancelled' in stderr, (cancelled.returncode, stderr)
        assert json.loads(stderr.splitlines()[-1])['exit'] == 130, stderr
print(json.dumps({'cli_profile': profile, 'backend': processor, 'stdout_clean': True,
                  'cancel_exit_130': True, 'permission_errors': True, 'offline_listing': True,
                  'decide_batch': 'passed' if batch_on else 'feature_off'}))

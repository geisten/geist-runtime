"""Single product binary E2E on one exact profile. No network or downloads."""
import json
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
print(json.dumps({'cli_profile': profile, 'backend': processor, 'stdout_clean': True,
                  'cancel_exit_130': True, 'permission_errors': True, 'offline_listing': True}))

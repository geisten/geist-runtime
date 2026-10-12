#!/usr/bin/env python3
"""test_upgrade.py <old geistr> <new geistr> <models dir> [embedding model dir] (#174)

A data folder written by the previous release still works with this build:
the old geistr writes settings, a verified model (receipt), a chat with input
history, speeds and (with an embedding model) an index; the new one must keep
the settings, trust the receipt, resume the chat and use the index.
"""
import os, pty, select, subprocess, sys, tempfile, time

old, new, models = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3])
embed = os.path.abspath(sys.argv[4]) if len(sys.argv) > 4 else None
home = tempfile.mkdtemp(prefix='gr-up-')
env = {**os.environ, 'GEISTEN_HOME': home, 'TERM': 'xterm'}
model = 'smollm2-360m'


def run(binary, *args, **kw):
    return subprocess.run([binary, *args, '--models', models], capture_output=True, text=True, env=env, timeout=600, **kw)


def chat(binary, line):
    """One line in a real terminal (pty): only there is a chat kept."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary, 'chat', model, '--models', models], env)
    seen, deadline, sent = b'', time.time() + 300, False
    while time.time() < deadline:
        if select.select([fd], [], [], 0.5)[0]:
            try: seen += os.read(fd, 65536)
            except OSError: break
        if not sent and b'> ' in seen:
            time.sleep(.3); os.write(fd, line.encode() + b'\r'); sent = True
        if sent and b'tok/s' in seen:
            break
    os.write(fd, b'\x04')  # Ctrl-D: the chat ends
    while not os.waitpid(pid, os.WNOHANG)[0]:
        try:
            if select.select([fd], [], [], 0.2)[0]: os.read(fd, 4096)
        except OSError: pass
    os.close(fd)
    return seen.decode(errors='replace')


version = lambda b: subprocess.run([b, '--version'], capture_output=True, text=True).stdout.strip()
print('old:', version(old), '\nnew:', version(new))

# the old release writes the data folder
assert run(old, 'config', 'temperature', '0.4').returncode == 0
assert run(old, 'config', 'system', 'Answer briefly.').returncode == 0
r = run(old, 'catalog', '--installed')  # hashes the model once, leaves the receipt
assert r.returncode == 0 and model in r.stdout, r
assert 'tok/s' in chat(old, 'My name is Ada.')
if embed:
    notes = os.path.join(home, 'notes'); os.makedirs(notes)
    with open(os.path.join(notes, 'shed.md'), 'w') as f:
        f.write('The lock on the garden shed opens with the code 4711.\n')
    r = subprocess.run([old, 'index', notes, '--models', embed], capture_output=True, text=True, env=env, timeout=600)
    assert r.returncode == 0, r.stderr

# the new build on that folder
r = run(new, 'config')
assert r.returncode == 0 and '0.4' in r.stdout and 'Answer briefly.' in r.stdout, r.stdout  # settings kept
r = run(new, 'catalog', '--installed')
assert r.returncode == 0 and model in r.stdout and 'verifying' not in r.stderr, (r.stdout, r.stderr)  # receipt trusted
out = chat(new, 'What is my name?')
assert '↻' in out, out[-600:]  # the old chat resumes
assert len(os.listdir(os.path.join(home, 'chats'))) == 1, os.listdir(os.path.join(home, 'chats'))
history = open(os.path.join(home, 'history')).read()
assert 'My name is Ada.' in history and 'What is my name?' in history, history  # history read and extended
if embed:
    r = subprocess.run([new, 'index', notes, '--models', embed], capture_output=True, text=True, env=env, timeout=600)
    assert r.returncode == 0 and '(0 new)' in r.stderr, r.stderr  # the old index is used, nothing re-embedded
print('upgrade: settings, receipts, chat and history' + (', index' if embed else '') + ' from the previous release passed')

#!/usr/bin/env python3
"""geistr CLI (#11) against the reference model: catalog states and --json
schema, run, chat, Ctrl-C, exit codes, pull from a local server, and a
build without the download module.
Usage: test_geistr.py <geistr (GEISTR_TESTING)> <geistr PULL=0> <model.gguf> <pull 0|1>"""
import faulthandler, functools, hashlib, http.server, json, os, signal, subprocess, sys, tempfile, threading, time
faulthandler.dump_traceback_later(900, exit=True)  # a hang shows where, instead of the CI timeout

geistr, nonet, model_path, pull = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3]), sys.argv[4] == '1'
tmp = tempfile.mkdtemp(prefix='geistr-test-')
models = os.path.join(tmp, 'models')
os.makedirs(models)
www = os.path.join(tmp, 'www', 'x', 'y', 'resolve', 'main')
os.makedirs(www)

def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()

def entry(id, file, digest, size, **extra):
    return {'id': id, 'name': id.upper(), 'file': file, 'url': f'https://huggingface.co/x/y/resolve/main/{file}',
            'sha256': digest, 'bytes': size, 'working_mib': 64, 'recommended_ram_gib': 1, 'backends': ['cpu'],
            'group_id': id, 'group_name': id.upper(), 'quantization': 'Q8_0', **extra}

tiny = b'GGUF tiny, never loaded' * 1000
with open(os.path.join(www, 'tiny.gguf'), 'wb') as f:
    f.write(tiny)
ref_file = os.path.basename(model_path)
catalog = {'schema': 2, 'revision': 1, 'models': [
    entry('ref', ref_file, sha(model_path), os.path.getsize(model_path)),
    entry('tiny', 'tiny.gguf', hashlib.sha256(tiny).hexdigest(), len(tiny)),
    entry('wrong', 'wrong.gguf', '0' * 64, len(tiny)),
    entry('huge', 'huge.gguf', '1' * 64, 200 * 2**30)]}
catalog_path = os.path.join(tmp, 'catalog.json')
with open(catalog_path, 'w') as f:
    json.dump(catalog, f)
base = ['--models', models, '--catalog', catalog_path]
env = {**os.environ, 'GEISTEN_HOME': os.path.join(tmp, 'home')}

def geistr_run(*args, binary=geistr, **kw):
    return subprocess.run([binary, *args, *base], capture_output=True, text=True, env=env, timeout=600, **kw)

def listing():
    r = geistr_run('catalog', '--json')
    assert r.returncode == 0, r.stderr
    doc = json.loads(r.stdout)
    # schema 1: the fields scripts rely on, with their types
    assert set(doc) == {'schema', 'models_dir', 'models'} and doc['schema'] == 1 and doc['models_dir'] == models
    types = {'id': str, 'name': str, 'quantization': (str, type(None)), 'file': str, 'url': str, 'sha256': str,
             'bytes': int, 'recommended_ram_gib': int, 'state': str, 'resource': str, 'resource_reason': str,
             'tokens_per_s': dict}
    for m in doc['models']:
        assert set(m) == set(types), m
        assert all(isinstance(m[k], t) for k, t in types.items()), m
        assert m['state'] in ('available', 'unverified', 'installed', 'mismatch')
        assert m['resource'] in ('fits', 'limited', 'unavailable')
        assert set(m['tokens_per_s']) == {'cpu', 'gpu'} and all(v is None or v > 0 for v in m['tokens_per_s'].values())
    return {m['id']: m for m in doc['models']}

# ---- catalog: available → installed (verified) → mismatch ----------------------
states = listing()
assert [m['state'] for m in states.values()] == ['available'] * 4, states
assert states['huge']['resource'] == 'unavailable' and states['huge']['resource_reason'] in ('ram', 'disk')
try:
    os.link(model_path, os.path.join(models, ref_file))
except OSError:
    subprocess.run(['cp', model_path, models], check=True)
with open(os.path.join(models, 'wrong.gguf'), 'wb') as f:
    f.write(tiny)  # right size, wrong bytes
states = listing()
assert states['ref']['state'] == 'installed' and states['wrong']['state'] == 'mismatch', states
assert list(states)[0] == 'ref'  # installed first
text = geistr_run('catalog').stdout
assert '✓ ref' in text and '↓ tiny' in text and '⟳ wrong' in text and '✗' in text.split('huge')[1], text
r = geistr_run('pull', binary=nonet)  # update: wrong is not this catalog's file, so it would be downloaded again
assert r.returncode == 1 and 'no download module' in r.stderr, r.stderr
r = geistr_run('--version')
assert r.returncode == 0 and r.stdout.startswith('geistr ') and 'catalog revision' in r.stdout and 'engine ' in r.stdout, r.stdout
assert geistr_run('catalog', '--installed').stdout.count('\n') == 1
assert 'ref' not in geistr_run('catalog', '--available').stdout
print('geistr catalog: states, receipts, --json schema, filters passed')

# ---- exit codes ------------------------------------------------------------
assert geistr_run().returncode == 2 and geistr_run('fly').returncode == 2 and geistr_run('catalog', '--bogus').returncode == 2
assert geistr_run('run', 'nope', 'hi').returncode == 1
r = geistr_run('run', 'tiny', 'hi')
assert r.returncode == 1 and 'geistr pull tiny' in r.stderr, r.stderr
r = geistr_run('run', 'wrong', 'hi')
assert r.returncode == 1 and 'does not match' in r.stderr, r.stderr

# ---- run: by id, by path, prompt from stdin ------------------------------------
r = geistr_run('run', 'ref', 'What is the capital of France? Answer in one word.')
assert r.returncode == 0 and 'Paris' in r.stdout, (r.stdout, r.stderr)
r = geistr_run('run', model_path, input='What is the capital of France? Answer in one word.')
assert r.returncode == 0 and 'Paris' in r.stdout, (r.stdout, r.stderr)

# ---- Ctrl-C: run ends with 130, chat stops the answer and goes on ----------------
p = subprocess.Popen([geistr, 'run', 'ref', 'Write a very long story about a lighthouse keeper, at least 3000 words.', *base],
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
p.stdout.read(1)  # the answer has started
p.send_signal(signal.SIGINT)
out, err = p.communicate(timeout=30)
assert p.returncode == 130, (p.returncode, err)

p = subprocess.Popen([geistr, 'chat', 'ref', *base], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, text=True, env=env, bufsize=0)
p.stdin.write('Write a very long story about a lighthouse keeper, at least 3000 words.\n')
p.stdin.flush()
seen = ''
while len(seen) < 40:
    seen += p.stdout.read(1)
p.send_signal(signal.SIGINT)
time.sleep(.5)
out, err = p.communicate('What is the capital of France? Answer in one word.\n', timeout=120)
assert p.returncode == 0 and '[stopped]' in out and 'Paris' in out.split('[stopped]')[1], (out, err)
assert ('⚙ >' in out or '⚡ >' in out) and 'tok/s' in out, out  # processor symbol, speed per answer

# ---- settings: geistr.conf, the last model remembered -------------------------
# where without GEISTEN_HOME: Application Support on macOS; XDG config on Linux, moved there once
user = os.path.join(tmp, 'user')
plain = {k: v for k, v in env.items() if k not in ('GEISTEN_HOME', 'GEIST_HOME', 'XDG_DATA_HOME')}
plain.update(HOME=user, XDG_CONFIG_HOME=os.path.join(user, 'cfg'))
if sys.platform == 'darwin':
    where = os.path.join(user, 'Library', 'Application Support', 'geisten', 'geistr.conf')
else:
    where = os.path.join(user, 'cfg', 'geisten', 'geistr.conf')
    before = os.path.join(user, '.local', 'share', 'geisten', 'geistr.conf')
    os.makedirs(os.path.dirname(before))
    with open(before, 'w') as f:
        f.write('temperature = 0.25\n')
r = subprocess.run([geistr, 'config'], capture_output=True, text=True, env=plain)
assert r.returncode == 0 and r.stdout.startswith('# ' + where), r.stdout
if sys.platform != 'darwin':
    assert not os.path.exists(before) and '0.25' in r.stdout, r.stdout
assert subprocess.run([geistr, 'config', 'stats', 'off'], env=plain).returncode == 0 and os.path.exists(where)
conf = os.path.join(tmp, 'home', 'geistr.conf')
assert os.path.exists(conf) and 'model = ref' in open(conf).read(), 'chat remembers its model'
r = geistr_run('chat', input='What is the capital of France? Answer in one word.\n')
assert r.returncode == 0 and 'Paris' in r.stdout, (r.stdout, r.stderr)  # no model given: the last one
assert geistr_run('config', 'processor', 'turbo').returncode == 2
assert geistr_run('config', 'temperature', '3').returncode == 2
assert geistr_run('config', 'colour', 'blue').returncode == 2
assert geistr_run('config', 'system', 'Answer', 'in', 'one', 'word.').returncode == 0
assert geistr_run('config', 'system').stdout.strip() == 'Answer in one word.'
assert geistr_run('config', 'stats', 'off').returncode == 0
settings = geistr_run('config').stdout
assert conf in settings and 'stats        off' in settings and 'processor    auto' in settings, settings
r = geistr_run('chat', 'ref', input='What is the capital of Italy?\n/clear\n/help\n/exit\n')
assert r.returncode == 0 and 'Rome' in r.stdout and 'tok/s' not in r.stdout and '/clear' in r.stdout, r.stdout
assert geistr_run('config', 'stats', 'on').returncode == 0 and geistr_run('config', 'system', '').returncode == 0
# runtime switches: the session changes, the conversation moves along
script = ('Remember the word lighthouse.\n/cpu\n/info\n/temp 9\n/temp 0.3\n/model nope\n'
          f'/model {model_path}\n/system Answer briefly.\nWhich word did I ask you to remember?\n/save\n/exit\n')
r = geistr_run('chat', 'ref', input=script)
assert r.returncode == 0, r.stderr
assert '⚙ cpu · ref · the conversation moves along' in r.stdout, r.stdout   # /cpu with history
assert 'chat format' in r.stdout and '/temp 0 … 2' in r.stdout and 'temperature 0.3' in r.stdout, r.stdout
assert 'unknown model nope' in r.stderr, r.stderr                                 # a failed switch keeps the session
assert f'· {model_path} · the conversation moves along' in r.stdout and 'system prompt set' in r.stdout, r.stdout
assert 'saved for the next chat' in r.stdout, r.stdout
saved = geistr_run('config').stdout
assert 'temperature  0.3' in saved and 'processor    cpu' in saved and 'Answer briefly.' in saved, saved
assert geistr_run('config', 'processor', 'auto').returncode == 0 and geistr_run('config', 'system', '').returncode == 0
assert geistr_run('config', 'temperature', '0').returncode == 0 and geistr_run('config', 'model', 'ref').returncode == 0
# Tab completion in a real terminal (pty): commands, model ids, the list, history
import pty, select
def until(fd, text, seconds=60):
    seen, deadline = b'', time.time() + seconds
    while text.encode() not in seen and time.time() < deadline:
        if select.select([fd], [], [], 1)[0]:
            try: seen += os.read(fd, 4096)
            except OSError: break
    assert text.encode() in seen, (text, seen[-400:])
    return seen
pid, fd = pty.fork()
if pid == 0:
    os.execve(geistr, [geistr, 'chat', 'ref', *base], {**env, 'TERM': 'xterm'})
until(fd, 'Ctrl-C twice exits')                                  # the intro (geistr config intro)
os.write(fd, b'/'); until(fd, 'another model')                     # / opens the list with what each does
os.write(fd, b'\x15/he\t'); until(fd, '/help')                    # Tab takes the chosen entry
os.write(fd, b'\r'); until(fd, 'Tab take')                  # and it runs
os.write(fd, b'/model r\t'); until(fd, '/model ref')             # installed model ids
os.write(fd, b'\x15/c\t'); out = until(fd, '/clear')             # several: listed with help
assert b'/cpu' in out and b'a new conversation' in out, out[-400:]
os.write(fd, b'\x15\x1b[A'); until(fd, '/help')                  # history: the last line
os.write(fd, b'\x15?'); until(fd, 'Ctrl-L clear screen')          # ? on an empty line: the shortcuts
time.sleep(.5)                                                      # the editor reads keys again (raw mode)
os.write(fd, b'\x03'); until(fd, 'Ctrl-C again to exit')           # once: a hint
os.write(fd, b'\x03')                                               # twice: the chat ends
until_exit = time.time() + 30   # keep reading: a full pty would block the child
while time.time() < until_exit:
    try:
        if select.select([fd], [], [], 1)[0] and not os.read(fd, 4096): break
    except OSError: break
_, status = os.waitpid(pid, 0)
assert os.WEXITSTATUS(status) == 0, status
os.close(fd)
print('geistr run/chat: answers, prompt from stdin, Ctrl-C (130 / stopped answer), exit codes, settings, Tab completion in a terminal passed')

# ---- the conversation across runs (in a terminal only) ------------------------
chats = os.path.join(tmp, 'home', 'chats')
def terminal_chat(message, *flags):
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(geistr, [geistr, 'chat', 'ref', *flags, *base], {**env, 'TERM': 'xterm'})
    seen = until(fd, 'Ctrl-C twice exits')
    time.sleep(.3)
    os.write(fd, message.encode() + b'\r')
    seen += until(fd, 'tok/s', 180)  # a whole answer: slow on a busy machine
    os.write(fd, b'\x03'); until(fd, 'Ctrl-C again to exit')   # right after the answer: still counts
    os.write(fd, b'\x03')
    deadline, status, tail = time.time() + 30, None, b''
    while status is None and time.time() < deadline:
        try:
            if select.select([fd], [], [], 0.2)[0]: tail += os.read(fd, 4096)
        except OSError: pass
        done, code = os.waitpid(pid, os.WNOHANG)
        status = code if done else None
    if status is None:
        os.kill(pid, signal.SIGKILL)
    os.close(fd)
    assert status is not None and os.WEXITSTATUS(status) == 0, (status, message, seen[-300:], tail[-300:])
    return seen.decode(errors='replace')
def stored():
    return sorted(os.listdir(chats)) if os.path.isdir(chats) else []
def lines(name):
    return [json.loads(l) for l in open(os.path.join(chats, name))]
out = terminal_chat('My name is Ada.')
assert '↻' not in out and len(stored()) == 1, (out, stored())
first = stored()[0]
assert os.stat(os.path.join(chats, first)).st_mode & 0o777 == 0o600
assert [m['role'] for m in lines(first)] == ['user', 'assistant'] and lines(first)[0]['content'] == 'My name is Ada.'
out = terminal_chat('What is my name?')
assert '↻ 2 · „My name is Ada.“' in out, out                 # continued where it was
assert len(stored()) == 1 and stored()[0] != first, stored()  # moved into this chat's file
assert [m['content'] for m in lines(stored()[0])][::2] == ['My name is Ada.', 'What is my name?']
out = terminal_chat('Hello.', '--new')
assert '↻' not in out and len(stored()) == 2, (out, stored())  # a new one; the old one stays
assert geistr_run('chat', 'ref', input='Hi.\n').returncode == 0 and len(stored()) == 2  # piped: nothing kept
print('geistr chat: continues the last conversation in a terminal, --new, private files, piped chats keep nothing passed')

# ---- serve and chat --socket --------------------------------------------------
import socket as unix
sock = f'/tmp/geistr-test-{os.getpid()}.sock'  # short: sun_path holds ~100 bytes
service = subprocess.Popen([geistr, 'serve', 'ref', '--socket=' + sock, '--chats', '2', '--cpu', *base],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, env=env)
deadline = time.time() + 120
while not os.path.exists(sock):
    assert service.poll() is None and time.time() < deadline, service.stderr.read()
    time.sleep(0.1)
assert os.stat(sock).st_mode & 0o777 == 0o600

def request(o, keep=None):
    s = unix.socket(unix.AF_UNIX)
    s.connect(sock)
    s.sendall((json.dumps(o) + '\n').encode())
    lines = []
    for line in s.makefile(encoding='utf-8'):
        lines.append(json.loads(line))
        if keep and len(lines) == keep:
            break
    s.close()
    return lines

def ask(messages, **kw):
    lines = request({'op': 'chat', 'messages': messages, 'max': 12, **kw})
    assert lines and lines[-1].get('done'), lines
    return ''.join(l['text'] for l in lines[:-1]), lines[-1]

info = request({'op': 'info'})[0]
assert info['model'] == 'ref' and info['backend'] == 'cpu' and info['chats'] == 2 and info['context'] > 0, info
a1 = [{'role': 'user', 'content': 'Name a color.'}]
text, first = ask(a1)
assert text and first['finish'] in ('stop', 'length'), first
a2 = a1 + [{'role': 'assistant', 'content': text}, {'role': 'user', 'content': 'Another one?'}]
_, hit = ask(a2)  # continues the held conversation: only the new messages are processed
assert hit['input_tokens'] < hit['context_tokens'] - hit['output_tokens'], hit
b = [{'role': 'user', 'content': 'Count to three, ünïcode “quoted” \\ \n line.'}]
ask(b)  # a second client between two turns of the first
_, again = ask(a2 + [{'role': 'assistant', 'content': 'x'}, {'role': 'user', 'content': 'And?'}])
assert again['input_tokens'] < again['context_tokens'] - again['output_tokens'], again  # A's is still held
edited = [{'role': 'user', 'content': 'Name a fruit.'}] + a2[1:]
_, rewound = ask(edited)  # differs at the first message: processed in full
assert rewound['input_tokens'] == rewound['context_tokens'] - rewound['output_tokens'], rewound
assert len(request({'op': 'chat', 'messages': a1, 'max': 400}, keep=2)) == 2  # leave mid-answer
t0 = time.time()
assert ask(a1)[1]['finish'] in ('stop', 'length') and time.time() - t0 < 60  # served after the cancel
big = request({'op': 'chat', 'messages': [{'role': 'user', 'content': 'word ' * (info['context'] * 2)}]})
assert big[-1].get('status') == 'context', big[-1]
assert request({'op': 'nope'})[0]['status'] == 'invalid'
r = subprocess.run([geistr, 'chat', '--socket=' + sock, *base], input='Say hi.\n/model ref\n/temp 0.5\n/info\n',
                   capture_output=True, text=True, env=env, timeout=120)
assert r.returncode == 0 and 'the service has its model' in r.stdout and 'temperature 0.5' in r.stdout, r
assert '· service' in r.stdout and 'chatml' in r.stdout, r.stdout
service.send_signal(signal.SIGTERM)
assert service.wait(30) == 0 and not os.path.exists(sock)
r = geistr_run('chat', '--socket=' + sock)
assert r.returncode == 1 and 'no service' in r.stderr, r.stderr
print('geistr serve / chat --socket: protocol, 0600 socket, cache hit, rewind, two clients, disconnect, context, SIGTERM passed')

# ---- bench and the speed chart --------------------------------------------------
speeds = os.path.join(tmp, 'home', 'speed.tsv')
before = open(speeds).read().count('\n') if os.path.exists(speeds) else 0  # chats above recorded theirs
r = geistr_run('bench', 'ref')
assert r.returncode == 0 and 'tok/s' in r.stdout and 'ref' in r.stdout, (r.stdout, r.stderr)
rows = [l.split('\t') for l in open(speeds).read().splitlines()]
assert len(rows) > before and rows[-1][0] == 'ref' and rows[-1][1] in ('cpu', 'gpu') and float(rows[-1][2]) > 0, rows
engine = rows[-1][5]
assert len(engine) == 40 and rows[-1][6] == 'bench', rows[-1]  # the geistlib commit, the source
assert all(r[6] == 'answer' for r in rows[:before]), rows[:before]  # chats and runs above
with open(speeds, 'a') as f:  # a model recorded by path counts for its catalog entry
    f.write(f'{model_path}\tcpu\t1000.0\t0.1\t0\t{engine}\n' * 11)
    f.write(f'ref\tcpu\t5.0\t0.1\t0\tanother-engine\n' * 11)  # not this engine's: ignored
measured = listing()['ref']['tokens_per_s']
assert measured['cpu'] == 1000.0, measured  # the median of the last ten with this engine
assert listing()['tiny']['tokens_per_s'] == {'cpu': None, 'gpu': None}
r = geistr_run('bench', '--compare')  # one engine so far
assert r.returncode == 0 and f'only one engine measured so far ({engine[:7]})' in r.stdout, r.stdout
with open(speeds, 'a') as f:  # two engines' bench rows; an answer row does not compare
    f.write('cmp\tcpu\t100.0\t0.1\t10\taaaa111\tbench\n' 'cmp\tcpu\t150.0\t0.1\t4000000000\tbbbb222\tbench\n'
            'cmp\tcpu\t999.0\t0.1\t4000000001\tbbbb222\tanswer\n')
r = geistr_run('bench', '--compare')  # the engine measured last against the one before
header = r.stdout.split('\n')[0].split()
assert r.returncode == 0 and header == [engine[:7], 'bbbb222'], r.stdout  # this engine (bench ref), then bbbb
r = geistr_run('bench', '--compare', 'aaaa', 'bbbb')
assert r.returncode == 0 and '▲ 50.0 %' in r.stdout and '999' not in r.stdout, r.stdout
r = geistr_run('bench', '--compare', 'bbbb', 'aaaa')
assert '▼ 33.3 %' in r.stdout, r.stdout
assert geistr_run('bench', '--compare', 'zzzz').returncode == 1
print('geistr bench / catalog speeds: recorded per answer with the engine, median of the last ten, by id or path, --compare passed')

# ---- pull -------------------------------------------------------------------
r = geistr_run('pull', 'tiny', binary=nonet)
assert r.returncode == 1 and 'no download module' in r.stderr, r.stderr
symbols = subprocess.run(['nm', nonet], capture_output=True, text=True).stdout
assert 'curl_' not in symbols, 'the PULL=0 build links network code'
if pull:
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args):
            pass
    handler = functools.partial(Quiet, directory=os.path.join(tmp, 'www'))
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env['GEISTR_TEST_URL_BASE'] = f'http://127.0.0.1:{server.server_port}'
    with open(os.path.join(models, 'tiny.gguf.part'), 'wb') as f:
        f.write(b'garbage')  # this server ignores ranges: sent again from 0
    r = geistr_run('pull', 'tiny')
    assert r.returncode == 0 and '✓ tiny installed' in r.stdout, (r.stdout, r.stderr)
    assert open(os.path.join(models, 'tiny.gguf'), 'rb').read() == tiny and not os.path.exists(os.path.join(models, 'tiny.gguf.part'))
    assert listing()['tiny']['state'] == 'installed'
    assert geistr_run('pull', 'tiny').stdout.startswith('✓ tiny is installed')
    os.unlink(os.path.join(models, 'wrong.gguf'))
    with open(os.path.join(tmp, 'www', 'x', 'y', 'resolve', 'main', 'wrong.gguf'), 'wb') as f:
        f.write(tiny)
    r = geistr_run('pull', 'wrong')  # served bytes do not match the catalog
    assert r.returncode == 1 and 'does not match' in r.stderr and not os.path.exists(os.path.join(models, 'wrong.gguf')), r.stderr
    r = geistr_run('pull')  # nothing left to update
    assert r.returncode == 0 and 'installed models are current' in r.stdout, (r.stdout, r.stderr)
    server.shutdown()
    print('geistr pull: download, restart after an ignored range, verify, refuse a mismatch; PULL=0 has no network code passed')
else:
    print('geistr pull: PULL=0 has no network code passed (no libcurl: download not tested)')
# ---- make install: DESTDIR and PREFIX -------------------------------------------
stage = os.path.join(tmp, 'stage')
r = subprocess.run(['make', '-s', 'install', 'BUILD=' + os.path.dirname(geistr), 'DESTDIR=' + stage, 'PREFIX=/opt/g'],
                   capture_output=True, text=True)
installed = os.path.join(stage, 'opt', 'g', 'bin', 'geistr')
assert r.returncode == 0 and os.access(installed, os.X_OK), r.stderr
assert subprocess.run([installed, '--version'], capture_output=True, text=True).stdout.startswith('geistr ')
assert subprocess.run(['make', '-s', 'uninstall', 'DESTDIR=' + stage, 'PREFIX=/opt/g']).returncode == 0
assert not os.path.exists(installed)
print('make install / uninstall: DESTDIR, PREFIX passed')
# ---- install.sh: the archive for this computer, checked against SHA256SUMS ----------
import tarfile, platform
release = os.path.join(tmp, 'release')
os.makedirs(release)
names = ['geistr-linux-amd64', 'geistr-linux-arm64', 'geistr-macos-arm64']
with open(os.path.join(release, 'SHA256SUMS'), 'w') as sums:
    for name in names:
        with tarfile.open(os.path.join(release, name + '.tar.gz'), 'w:gz') as t:
            t.add(geistr, arcname='geistr')
        sums.write(f'{sha(os.path.join(release, name + ".tar.gz"))}  {name}.tar.gz\n')
handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=release)
handler.log_message = lambda *a: None
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
url = f'http://127.0.0.1:{server.server_port}'
prefix = os.path.join(tmp, 'prefix')
here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'install.sh')
r = subprocess.run(['sh', here], capture_output=True, text=True, env={**env, 'GEISTR_BASE_URL': url, 'PREFIX': prefix})
assert r.returncode == 0 and 'geistr ' in r.stdout and os.access(os.path.join(prefix, 'bin', 'geistr'), os.X_OK), (r.stdout, r.stderr)
with open(os.path.join(release, 'SHA256SUMS'), 'w') as sums:  # a download that does not match: nothing installed
    sums.write(''.join(f'{"0" * 64}  {n}.tar.gz\n' for n in names))
os.unlink(os.path.join(prefix, 'bin', 'geistr'))
r = subprocess.run(['sh', here], capture_output=True, text=True, env={**env, 'GEISTR_BASE_URL': url, 'PREFIX': prefix})
assert r.returncode == 1 and 'does not match' in r.stderr and not os.path.exists(os.path.join(prefix, 'bin', 'geistr')), r.stderr
server.shutdown()
print('install.sh: the archive for this computer, SHA256SUMS checked, a mismatch installs nothing passed')
subprocess.run(['rm', '-rf', tmp])

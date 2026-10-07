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


def install_models():  # ref as itself, wrong with the right size and wrong bytes; once
    if not os.path.exists(os.path.join(models, ref_file)):
        try:
            os.link(model_path, os.path.join(models, ref_file))
        except OSError:
            subprocess.run(['cp', model_path, models], check=True)
        with open(os.path.join(models, 'wrong.gguf'), 'wb') as f:
            f.write(tiny)

# ---- helpers for the terminal (pty) sections ---------------------------------
import pty, select
def until(fd, text, seconds=60):
    seen, deadline = b'', time.time() + seconds
    while text.encode() not in seen and time.time() < deadline:
        if select.select([fd], [], [], 1)[0]:
            try: seen += os.read(fd, 4096)
            except OSError: break
    assert text.encode() in seen, (text, seen[-400:])
    return seen

# ---- catalog: available → installed (verified) → mismatch ----------------------
def section_catalog():
    states = listing()
    assert [m['state'] for m in states.values()] == ['available'] * 4, states
    assert states['huge']['resource'] == 'unavailable' and states['huge']['resource_reason'] in ('ram', 'disk')
    install_models()
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
def section_run():
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

    # with a temperature, each run samples anew (geistlib's seed 0 is one fixed seed)
    assert geistr_run('config', 'temperature', '1.5').returncode == 0
    runs = {geistr_run('run', 'ref', 'Write one sentence about the sea.').stdout for _ in range(2)}
    assert len(runs) == 2, runs
    assert geistr_run('config', 'temperature', '0').returncode == 0
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
    conf = os.path.join(env['GEISTEN_HOME'], 'geistr.conf')
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
def section_resume():
    chats = os.path.join(env['GEISTEN_HOME'], 'chats')
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
def section_serve():
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

# ---- serve --http: the OpenAI and Ollama APIs --------------------------------
def section_http():
    import http.client, socket as unix
    probe = unix.socket(); probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]; probe.close()
    sock = f'/tmp/geistr-http-{os.getpid()}.sock'
    service = subprocess.Popen([geistr, 'serve', 'ref', f'--socket={sock}', f'--http=127.0.0.1:{port}', '--cpu', *base],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, env=env)
    def call(method, path, body=None, headers={}):
        c = http.client.HTTPConnection('127.0.0.1', port, timeout=120)
        c.request(method, path, body=json.dumps(body) if isinstance(body, dict) else body, headers=headers)
        r = c.getresponse()
        data = r.read().decode()
        c.close()
        return r.status, r.getheader('Content-Type'), data
    deadline = time.time() + 120
    while True:
        try:
            if call('GET', '/')[0] == 200: break
        except OSError:
            pass
        assert service.poll() is None and time.time() < deadline, service.stderr.read()
        time.sleep(0.2)
    assert call('GET', '/api/version')[2] == '{"version":"0.1.0"}'
    models = json.loads(call('GET', '/v1/models')[2])
    assert models['data'][0]['id'] == 'ref', models
    assert json.loads(call('GET', '/api/tags')[2])['models'][0]['name'] == 'ref'
    # OpenAI, one answer: content, finish, usage; developer role and text parts accepted
    ask = [{'role': 'developer', 'content': 'Answer briefly.'},
           {'role': 'user', 'content': [{'type': 'text', 'text': 'Name a color.'}]}]
    status, kind, body = call('POST', '/v1/chat/completions', {'model': 'ref', 'messages': ask, 'temperature': 0})
    one = json.loads(body)
    answer = one['choices'][0]['message']['content']
    assert status == 200 and kind == 'application/json' and answer and one['object'] == 'chat.completion', body
    assert one['choices'][0]['finish_reason'] in ('stop', 'length') and one['usage']['completion_tokens'] > 0, body
    # the next turn over the socket continues the same conversation: only the new part is processed
    s = unix.socket(unix.AF_UNIX); s.connect(sock)
    turn = [{'role': 'system', 'content': 'Answer briefly.'}, {'role': 'user', 'content': 'Name a color.'},
            {'role': 'assistant', 'content': answer}, {'role': 'user', 'content': 'Another?'}]
    s.sendall((json.dumps({'op': 'chat', 'messages': turn, 'max': 8, 'temperature': 0}) + '\n').encode())
    done = [json.loads(l) for l in s.makefile(encoding='utf-8')][-1]
    s.close()
    assert done.get('done') and done['input_tokens'] < done['context_tokens'] - done['output_tokens'], done
    # a conversation full of the model's own loops: the next answer loops too, and the runtime ends it
    loop = '"Hello, I\'m here to help you with your questions and ideas. I\'m here to listen and provide guidance. ' + \
           "I'm here to help you with your questions and ideas. " * 30
    poisoned = [m for _ in range(7) for m in ({'role': 'user', 'content': 'Say hello in five words.'},
                                              {'role': 'assistant', 'content': loop})]
    s = unix.socket(unix.AF_UNIX); s.connect(sock)
    s.sendall((json.dumps({'op': 'chat', 'messages': poisoned + [{'role': 'user', 'content': 'Say hello in five words'}],
                           'max': 300, 'temperature': 0}) + '\n').encode())
    done = [json.loads(l) for l in s.makefile(encoding='utf-8')][-1]
    s.close()
    assert done.get('finish') != 'length' and done['output_tokens'] < 300, done  # ended by the guard (or the model)
    # OpenAI stream: role first, content, the finish, usage, [DONE]; max_tokens → length
    status, kind, body = call('POST', '/v1/chat/completions', {'messages': [{'role': 'user', 'content': 'Count to ten.'}],
                              'stream': True, 'max_tokens': 3, 'temperature': 0, 'stream_options': {'include_usage': True}})
    events = [l[len('data: '):] for l in body.split('\n') if l.startswith('data: ')]
    assert status == 200 and kind == 'text/event-stream' and events[-1] == '[DONE]', body
    chunks = [json.loads(e) for e in events[:-1]]
    assert chunks[0]['choices'][0]['delta']['role'] == 'assistant'
    assert ''.join(c['choices'][0]['delta'].get('content', '') for c in chunks if c['choices'])
    assert [c['choices'][0]['finish_reason'] for c in chunks if c['choices']][-1] == 'length', chunks[-3:]
    assert chunks[-1]['usage']['completion_tokens'] == 3, chunks[-1]
    # stop strings end the answer before them
    status, _, body = call('POST', '/v1/chat/completions', {'messages': [{'role': 'user', 'content': 'Count from 1 to 5 with commas.'}],
                           'stop': [','], 'temperature': 0, 'max_tokens': 30})
    assert ',' not in json.loads(body)['choices'][0]['message']['content'], body
    # Ollama, one object and a stream that ends with done
    status, _, body = call('POST', '/api/chat', {'model': 'ref', 'messages': [{'role': 'user', 'content': 'Count from 1 to 50 with commas.'}],
                           'stream': False, 'options': {'temperature': 0, 'num_predict': 4}})  # longer than 4 tokens: length
    one = json.loads(body)
    assert status == 200 and one['done'] and one['message']['content'] and one['done_reason'] == 'length', body
    status, kind, body = call('POST', '/api/chat', {'messages': [{'role': 'user', 'content': 'Count from 1 to 50 with commas.'}],
                              'options': {'num_predict': 3}})
    lines = [json.loads(l) for l in body.splitlines()]
    assert kind == 'application/x-ndjson' and not lines[0]['done'] and lines[-1]['done'] and lines[-1]['eval_count'] == 3, body
    # refused: another Host (DNS rebinding), unknown path, wrong method, too long, chunked, the context
    assert call('GET', '/v1/models', headers={'Host': 'evil.example'})[0] == 403
    assert call('GET', '/nope')[0] == 404 and call('GET', '/v1/chat/completions')[0] == 405
    c = unix.create_connection(('127.0.0.1', port))
    c.sendall(b'POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\nContent-Length: 99999999\r\n\r\n')
    assert c.recv(100).startswith(b'HTTP/1.1 413'); c.close()
    c = unix.create_connection(('127.0.0.1', port))
    c.sendall(b'POST /api/chat HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n')
    assert c.recv(100).startswith(b'HTTP/1.1 411'); c.close()
    status, _, body = call('POST', '/v1/chat/completions', {'messages': [{'role': 'user', 'content': 'word ' * 20000}]})
    assert status == 400 and json.loads(body)['error']['code'] == 'context_length_exceeded', body
    # a client that leaves mid-stream stops its answer; the next request is served
    c = unix.create_connection(('127.0.0.1', port))
    long = json.dumps({'messages': [{'role': 'user', 'content': 'Write a long story.'}], 'stream': True, 'max_tokens': 400})
    c.sendall(f'POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\nContent-Length: {len(long)}\r\n\r\n{long}'.encode())
    assert b'data:' in c.recv(4096); c.close()
    t0 = time.time()
    assert call('POST', '/api/chat', {'messages': [{'role': 'user', 'content': 'Hi'}], 'stream': False,
                'options': {'num_predict': 2}})[0] == 200 and time.time() - t0 < 60
    service.send_signal(signal.SIGTERM)
    assert service.wait(30) == 0
    print('geistr serve --http: OpenAI (one answer, stream, usage, stop, length) and Ollama APIs, shared cache, Host check, 403/404/405/411/413, context, disconnect passed')

# ---- bench and the speed chart --------------------------------------------------
def section_bench():
    speeds = os.path.join(env['GEISTEN_HOME'], 'speed.tsv')
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
    assert '×1 ' in r.stdout and '⚠' not in r.stdout, r.stdout  # one run each: no spread
    with open(speeds, 'a') as f:  # runs far apart: marked
        f.write('cmp\tcpu\t300.0\t0.1\t11\taaaa111\tbench\n')
    r = geistr_run('bench', '--compare', 'aaaa', 'bbbb')
    assert '×2  ⚠' in r.stdout and 'busy machine' in r.stdout, r.stdout
    assert geistr_run('bench', '--compare', 'zzzz').returncode == 1
    print('geistr bench / catalog speeds: recorded per answer with the engine, median of the last ten, by id or path, --compare passed')

# ---- pull -------------------------------------------------------------------
def section_pull():
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
def section_install():
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
def section_install_sh():
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

# ---- run: every section, or those in GEISTR_TEST_ONLY (comma separated) -------
# Each gets its own GEISTEN_HOME (settings, chats, speed.tsv); they share the
# model folder. ponytail: one after another, not in parallel: parallel model
# runs would load the machine the terminal sections time against.
SECTIONS = {name[len('section_'):]: f for name, f in list(globals().items()) if name.startswith('section_')}
only = [s for s in os.environ.get('GEISTR_TEST_ONLY', '').split(',') if s]
assert all(s in SECTIONS for s in only), f'GEISTR_TEST_ONLY: unknown section in {only}; known: {", ".join(SECTIONS)}'
for name, section in SECTIONS.items():
    if not only or name in only:
        env['GEISTEN_HOME'] = os.path.join(tmp, 'home-' + name)
        if name != 'catalog':  # it starts from an empty model folder
            install_models()
        section()
subprocess.run(['rm', '-rf', tmp])

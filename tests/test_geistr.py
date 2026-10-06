#!/usr/bin/env python3
"""geistr CLI (#11) against the reference model: catalog states and --json
schema, run, chat, Ctrl-C, exit codes, pull from a local server, and a
build without the download module.
Usage: test_geistr.py <geistr (GEISTR_TESTING)> <geistr PULL=0> <model.gguf> <pull 0|1>"""
import functools, hashlib, http.server, json, os, signal, subprocess, sys, tempfile, threading, time

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
             'bytes': int, 'recommended_ram_gib': int, 'state': str, 'resource': str, 'resource_reason': str}
    for m in doc['models']:
        assert set(m) == set(types), m
        assert all(isinstance(m[k], t) for k, t in types.items()), m
        assert m['state'] in ('available', 'unverified', 'installed', 'mismatch')
        assert m['resource'] in ('fits', 'limited', 'unavailable')
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
assert '✓ ref' in text and '↓ tiny' in text and '✗ wrong' in text and '✗' in text.split('huge')[1], text
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
print('geistr run/chat: answers, prompt from stdin, Ctrl-C (130 / stopped answer), exit codes, settings passed')

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
    server.shutdown()
    print('geistr pull: download, restart after an ignored range, verify, refuse a mismatch; PULL=0 has no network code passed')
else:
    print('geistr pull: PULL=0 has no network code passed (no libcurl: download not tested)')
subprocess.run(['rm', '-rf', tmp])

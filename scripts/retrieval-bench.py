#!/usr/bin/env python3
"""retrieval-bench.py — how often the embedding model finds the right file (#157).

The passages are the paragraphs of tests/fixtures/retrieval/files/ (English and
German); tests/fixtures/retrieval/questions.tsv has one question per passage
with the file it should find. Each question and passage is embedded through
`geistr serve --http` (/v1/embeddings), passages ranked by cosine; a hit is the
expected file among the files of the nearest 1 (top-1) or 4 (top-4, what
--files puts before a question) passages. Also printed: the best score of the
right file and of any other file, the numbers a relevance threshold (#150)
would have to separate.

  scripts/retrieval-bench.py GEISTR MODEL.gguf [--query PREFIX] [--passage PREFIX]
  scripts/retrieval-bench.py GEISTR MODEL.gguf --compare   # the prefixes of #157

PREFIX (default: nothing, as --files) takes \\n for a newline. On the shared
Mac, wrap it in ~/bin/with-mac-lock.
"""
import argparse, http.client, json, os, signal, socket, subprocess, sys, tempfile, time

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests', 'fixtures', 'retrieval')
INSTRUCT = "Instruct: Given a question, retrieve passages from the user's files that answer it\nQuery: "
SETTINGS = [('Instruct: …\\nQuery: ', INSTRUCT, ''), ('Instruct: …\\nQuery: ', INSTRUCT, 'passage: '),
            ('none', '', ''), ('none', '', 'passage: '),
            ('query: ', 'query: ', ''), ('query: ', 'query: ', 'passage: ')]


def passages():
    out = []
    root = os.path.join(HERE, 'files')
    for d, _, names in sorted(os.walk(root)):
        for name in sorted(names):
            path = os.path.join(d, name)
            text = open(path, encoding='utf-8').read()
            for para in text.split('\n\n'):
                if para.strip() and not para.lstrip().startswith('#'):
                    out.append((os.path.relpath(path, root), para.strip()))
    return out


def questions():
    out = []
    for line in open(os.path.join(HERE, 'questions.tsv'), encoding='utf-8'):
        if line.strip() and not line.startswith('#'):
            path, q = line.rstrip('\n').split('\t')
            out.append((path, q))
    return out


class Service:
    def __init__(self, geistr, model):
        probe = socket.socket(); probe.bind(('127.0.0.1', 0)); self.port = probe.getsockname()[1]; probe.close()
        self.home = tempfile.mkdtemp(prefix='geistr-bench-')
        sock = os.path.join(self.home, 's')
        self.p = subprocess.Popen([geistr, 'serve', model, f'--socket={sock}', f'--http=127.0.0.1:{self.port}', '--cpu'],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                                  env={**os.environ, 'GEISTEN_HOME': self.home})
        deadline = time.time() + 120
        while True:
            try:
                c = http.client.HTTPConnection('127.0.0.1', self.port, timeout=5); c.request('GET', '/'); c.getresponse(); c.close()
                return
            except OSError:
                if time.time() > deadline or self.p.poll() is not None:
                    sys.exit('geistr serve did not start: ' + self.p.stderr.read())
                time.sleep(.3)

    def embed(self, texts):
        out = []
        for at in range(0, len(texts), 16):
            c = http.client.HTTPConnection('127.0.0.1', self.port, timeout=600)
            c.request('POST', '/v1/embeddings', body=json.dumps({'input': texts[at:at + 16], 'encoding_format': 'float'}))
            r = c.getresponse(); data = json.loads(r.read()); c.close()
            if r.status != 200:
                sys.exit(f'/v1/embeddings: {r.status} {data}')
            out += [d['embedding'] for d in data['data']]
        return out

    def close(self):
        self.p.send_signal(signal.SIGTERM); self.p.wait(30)


def run(service, query, passage, ps, qs, vecs_cache):
    if passage not in vecs_cache:
        vecs_cache[passage] = service.embed([passage + t for _, t in ps])
    pv = vecs_cache[passage]
    qv = service.embed([query + q for _, q in qs])
    top1 = top4 = 0
    right, wrong, misses = [], [], []
    for (want, q), v in zip(qs, qv):
        scores = sorted(((sum(a * b for a, b in zip(v, p)), f) for (f, _), p in zip(ps, pv)), reverse=True)
        files = [f for _, f in scores[:4]]
        top1 += files[0] == want
        top4 += want in files
        if files[0] != want:
            misses.append(f'{q} -> {", ".join(dict.fromkeys(files))}{"" if want in files else "  (not in top-4)"}')
        right.append(max(s for s, f in scores if f == want))
        wrong.append(max(s for s, f in scores if f != want))
    n = len(qs)
    return {'top1': top1 / n, 'top4': top4 / n, 'n': n, 'misses': misses,
            'right': (min(right), sum(right) / n), 'wrong': (max(wrong), sum(wrong) / n),
            'apart': sum(r > max(wrong) for r in right)}


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('geistr'); a.add_argument('model')
    a.add_argument('--query', default=''); a.add_argument('--passage', default='')  # what --files uses (files.h)
    a.add_argument('--compare', action='store_true')
    o = a.parse_args()
    ps, qs = passages(), questions()
    files = {f for f, _ in ps}
    assert len(ps) == len(qs) and {f for f, _ in qs} == files, (len(ps), len(qs))
    print(f'{len(ps)} passages in {len(files)} files, {len(qs)} questions')
    service, cache = Service(o.geistr, o.model), {}
    try:
        settings = SETTINGS if o.compare else [(o.query or 'none', o.query.replace('\\n', '\n'), o.passage.replace('\\n', '\n'))]
        print('| query prefix | passage prefix | top-1 | top-4 | right file, best score min / mean | other files, best score max / mean | right above every wrong |')
        print('|---|---|---|---|---|---|---|')
        for name, query, passage in settings:
            r = run(service, query, passage, ps, qs, cache)
            print(f"| `{name}` | `{passage or 'none'}` | {r['top1']:.0%} | {r['top4']:.0%} | "
                  f"{r['right'][0]:.2f} / {r['right'][1]:.2f} | {r['wrong'][0]:.2f} / {r['wrong'][1]:.2f} | {r['apart']}/{r['n']} |")
            for m in r['misses'] if not o.compare else []:
                print('  missed:', m)
    finally:
        service.close()


if __name__ == '__main__':
    main()

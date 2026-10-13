#!/usr/bin/env python3
"""retrieval-bench.py — how often the embedding model finds the right file (#157),
and which files sources: should name (#150).

The passages are the paragraphs of tests/fixtures/retrieval/files/ (English and
German); tests/fixtures/retrieval/questions.tsv has one question per passage
with the file it should find, then questions that need two files and
questions no file answers. Each question and passage is embedded through
`geistr serve --http` (/v1/embeddings), passages ranked by cosine; a hit is the
expected file among the files of the nearest 1 (top-1) or 4 (top-4, what
--files puts before a question) passages. Also printed: the best score of the
right file and of any other file, the numbers a relevance threshold (#150)
would have to separate.

--sources: the files cut into chunks as --files does, every question (also the
two-file and off-topic ones) ranked against them; for rules that pick files
from the top 4 chunks: precision and recall against the files that answer,
and how often no file is picked (wanted for off-topic questions). --rerank
HF_MODEL adds a cross-encoder's judgement of the top 4 (needs torch and
transformers; an estimate for #150, geistr cannot run one).

  scripts/retrieval-bench.py GEISTR MODEL.gguf [--query PREFIX] [--passage PREFIX]
  scripts/retrieval-bench.py GEISTR MODEL.gguf --compare   # the prefixes of #157
  scripts/retrieval-bench.py GEISTR MODEL.gguf --sources [--rerank BAAI/bge-reranker-v2-m3]

PREFIX (default: nothing, as --files) takes \\n for a newline. On the shared
Mac, wrap it in ~/bin/with-mac-lock.
"""
import argparse, http.client, json, os, signal, socket, subprocess, sys, tempfile, time

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests', 'fixtures', 'retrieval')
INSTRUCT = "Instruct: Given a question, retrieve passages from the user's files that answer it\nQuery: "
CHUNK = 1200  # files.c
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


def chunks():
    """The files cut as files.c does (next_chunk): paragraphs gathered to about
    CHUNK bytes, a longer paragraph cut at a space."""
    out, root = [], os.path.join(HERE, 'files')
    for d, _, names in sorted(os.walk(root)):
        for name in sorted(names):
            path = os.path.join(d, name)
            s = open(path, 'rb').read()
            n, at = len(s), 0
            while True:
                while at < n and s[at] in b' \t\r\n':
                    at += 1
                if at >= n:
                    break
                e = at
                while True:
                    gap = s.find(b'\n\n', e)
                    nxt = gap + 2 if gap >= 0 else n
                    if nxt - at > CHUNK and e > at:
                        break
                    e = nxt
                    if e - at >= CHUNK or e >= n:
                        break
                if e - at > 2 * CHUNK:
                    e = at + CHUNK
                    while e > at + CHUNK // 2 and s[e] not in b' \n':
                        e -= 1
                    while e < n and s[e] & 0xC0 == 0x80:
                        e += 1
                out.append((os.path.relpath(path, root), s[at:e].decode()))
                at = e
    return out


def questions():
    """(relevant files, question): one file for the first questions (one per
    passage), two for some, none for the off-topic ones."""
    out = []
    for line in open(os.path.join(HERE, 'questions.tsv'), encoding='utf-8'):
        if line.strip() and not line.startswith('#'):
            files, q = line.rstrip('\n').split('\t')
            out.append((frozenset() if files == '-' else frozenset(files.split(',')), q))
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


def score(rel, picked):
    """Over the questions: precision and recall of the picked files against the
    relevant ones (answerable questions), the share of off-topic questions
    with nothing picked, and of answerable ones with nothing picked."""
    tp = n_picked = n_rel = off = n_off = empty = 0
    for r, p in zip(rel, picked):
        if r:
            tp, n_picked, n_rel, empty = tp + len(p & r), n_picked + len(p), n_rel + len(r), empty + (not p)
        else:
            off, n_off = off + (not p), n_off + 1
    n_ans = len(rel) - n_off
    return tp / max(n_picked, 1), tp / n_rel, off / n_off, empty / n_ans


def rerank(name, qs, top, cs):
    """A cross-encoder (Hugging Face transformers, e.g. BAAI/bge-reranker-v2-m3):
    the relevance of each of a question's top-4 chunks, 0 to 1."""
    import torch
    from transformers import AutoModelForSequenceClassification, AutoTokenizer
    tok = AutoTokenizer.from_pretrained(name)
    model = AutoModelForSequenceClassification.from_pretrained(name).eval()
    out, took = [], []
    with torch.no_grad():
        for (_, q), t in zip(qs, top):
            start = time.time()
            enc = tok([q] * len(t), [cs[j][1] for j in t], padding=True, truncation=True, max_length=512,
                      return_tensors='pt')
            out.append(torch.sigmoid(model(**enc).logits.view(-1).float()).tolist())
            took.append(time.time() - start)
    return out, sorted(took)[len(took) // 2]


def sources(service, o):
    """What sources: would name (#150): the files of the top-4 chunks a rule
    keeps, against the files that answer the question."""
    cs, qs = chunks(), questions()
    rel = [r for r, _ in qs]
    cv = service.embed([o.passage + t for _, t in cs])
    start = time.time()
    qv = [service.embed([o.query + q])[0] for _, q in qs]
    per_q = (time.time() - start) / len(qs)
    scores = [[sum(a * b for a, b in zip(v, c)) for c in cv] for v in qv]
    top = [sorted(range(len(cs)), key=lambda j: -s[j])[:4] for s in scores]
    print(f'{len(cs)} chunks (as --files cuts them) in {len({f for f, _ in cs})} files; {len(qs)} questions: '
          f'{sum(len(r) == 1 for r in rel)} with one file, {sum(len(r) > 1 for r in rel)} with two, '
          f'{sum(not r for r in rel)} no file answers; a question embeds in {per_q * 1000:.0f} ms')

    def kept(keep):
        out = []
        for s, t in zip(scores, top):
            mean = sum(s) / len(s)
            sd = (sum((x - mean) ** 2 for x in s) / len(s)) ** .5
            out.append({cs[j][0] for j in t if keep(s[j], s[t[0]], (s[j] - mean) / sd)})
        return out
    rules = [('top 4 (now)', kept(lambda x, top, z: True))]
    rules += [(f'cosine ≥ {t:.2f}', kept(lambda x, top, z, t=t: x >= t)) for t in (0.62, 0.64, 0.66, 0.68, 0.70)]
    rules += [(f'within {d} of the top', kept(lambda x, top, z, d=d: top - x <= d)) for d in (0.01, 0.02, 0.05)]
    rules += [(f'z-score ≥ {k}', kept(lambda x, top, z, k=k: z >= k)) for k in (1.5, 2.0, 2.5)]
    if o.rerank:
        p, took = rerank(o.rerank, qs, top, cs)
        name = o.rerank.split('/')[-1]
        rules += [(f'{name} ≥ {t}', [{cs[j][0] for j, x in zip(tj, pj) if x >= t} for tj, pj in zip(top, p)])
                  for t in (0.001, 0.003, 0.01, 0.1, 0.5)]
        print(f'{name}: {took:.2f} s per question (4 pairs, median, CPU)')
    print('| rule | precision | recall | off-topic: no file | answerable: no file |')
    print('|---|---|---|---|---|')
    for name, picked in rules:
        print('| %s | %.2f | %.2f | %.2f | %.2f |' % (name, *score(rel, picked)))


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('geistr'); a.add_argument('model')
    a.add_argument('--query', default=''); a.add_argument('--passage', default='')  # what --files uses (files.h)
    a.add_argument('--compare', action='store_true')
    a.add_argument('--sources', action='store_true')
    a.add_argument('--rerank', metavar='HF_MODEL')
    o = a.parse_args()
    o.query, o.passage = o.query.replace('\\n', '\n'), o.passage.replace('\\n', '\n')
    ps = passages()
    qs = [(next(iter(f)), q) for f, q in questions() if len(f) == 1]  # one per passage
    files = {f for f, _ in ps}
    assert len(ps) == len(qs) and {f for f, _ in qs} == files, (len(ps), len(qs))
    service, cache = Service(o.geistr, o.model), {}
    try:
        if o.sources:
            return sources(service, o)
        print(f'{len(ps)} passages in {len(files)} files, {len(qs)} questions')
        settings = SETTINGS if o.compare else [(o.query.replace('\n', '\\n') or 'none', o.query, o.passage)]
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

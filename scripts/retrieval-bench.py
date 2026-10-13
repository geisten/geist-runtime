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


GERMAN = set('der die das und ist nicht mit für wir wie wann wo was wer bei auf den dem ein eine ich im zu von'.split())
ENGLISH = set('the and is not with for we how when where what who which at on a i to of does do our is are'.split())


def language(text):
    """'de' or 'en' by function words: enough for the fixtures."""
    words = [w.strip('.,?!:;()"').lower() for w in text.split()]
    de = sum(w in GERMAN for w in words) + any(c in text for c in 'äöüßÄÖÜ')
    return 'de' if de > sum(w in ENGLISH for w in words) else 'en'


def by_language(qs, picked):
    """Recall and answerable-with-no-file for the answerable questions in the
    language of their files and for those across the language (#150)."""
    root, out = os.path.join(HERE, 'files'), []
    lang = {}
    for (r, q), p in zip(qs, picked):
        if not r:
            continue
        for f in r:
            lang.setdefault(f, language(open(os.path.join(root, f), encoding='utf-8').read()))
        cross = any(lang[f] != language(q) for f in r)
        out.append((cross, len(p & r), len(r), not p))
    res = []
    for cross in (False, True):
        g = [x for x in out if x[0] == cross]
        res += [sum(x[1] for x in g) / max(sum(x[2] for x in g), 1), sum(x[3] for x in g) / max(len(g), 1), len(g)]
    return res


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


QWEN3_RERANK_PREFIX = ('<|im_start|>system\nJudge whether the Document meets the requirements based on the Query and '
                       'the Instruct provided. Note that the answer can only be "yes" or "no".<|im_end|>\n<|im_start|>user\n')
QWEN3_RERANK_SUFFIX = '<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'
QWEN3_RERANK_INSTRUCT = 'Given a web search query, retrieve relevant passages that answer the query'  # the model card's default


def qwen3_prompt(instruct, q, doc):
    """The Qwen3-Reranker prompt of the model card, exactly."""
    return QWEN3_RERANK_PREFIX + f'<Instruct>: {instruct}\n<Query>: {q}\n<Document>: {doc}' + QWEN3_RERANK_SUFFIX


PIECE = 400  # bytes a paragraph piece may have (--pieces)


def paragraphs(text):
    """A chunk in paragraphs (blank-line separated); a longer one cut after
    sentences into pieces of at most about PIECE bytes; a short one (a
    heading) joined to the next."""
    import re
    out, carry = [], ''
    for para in (p.strip() for p in text.split('\n\n')):
        if not para:
            continue
        para = (carry + '\n\n' + para) if carry else para
        carry = ''
        if len(para.encode()) < 80:
            carry = para
            continue
        piece = ''
        for sentence in re.split(r'(?<=[.!?])\s+|\n', para):
            if piece and len((piece + ' ' + sentence).encode()) > PIECE:
                out.append(piece); piece = ''
            piece = (piece + ' ' + sentence).strip()
        if piece:
            out.append(piece)
    if carry and out:
        out[-1] += '\n\n' + carry
    elif carry:
        out.append(carry)
    return out or [text]


def judge(ask, qs, top, cs, instruct, split):
    """P(yes) per question and chunk (the best of its pieces), and seconds."""
    ask(qwen3_prompt(instruct, 'warm up', 'warm up'))
    out, took = [], []
    for (_, q), t in zip(qs, top):
        ps, ts = [], []
        for j in t:
            start = time.time()
            ps.append(max(ask(qwen3_prompt(instruct, q, d)) for d in split(cs[j][1])))
            ts.append(time.time() - start)
        out.append(ps), took.append(ts)
    return out, took


def rerank_gguf(path, qs, top, cs, ngl, instruct, split=lambda t: [t]):
    """Qwen3-Reranker GGUF through llama-server: P(yes) = softmax(logit yes, logit no)
    of the token after the prompt, for each of a question's top chunks; with
    split, the best of the chunk's pieces. ponytail: yes/no from the top 50
    tokens; one missing takes the 50th's log-prob (both missing: 0.5)."""
    import math
    probe = socket.socket(); probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]; probe.close()
    p = subprocess.Popen(['llama-server', '-m', path, '--port', str(port), '-ngl', str(ngl), '-c', '4096', '-np', '1',
                          '--no-webui'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + 120
        while True:
            try:
                c = http.client.HTTPConnection('127.0.0.1', port, timeout=5); c.request('GET', '/health')
                ok = c.getresponse().status == 200; c.close()
                if ok:
                    break
            except OSError:
                pass
            if time.time() > deadline or p.poll() is not None:
                sys.exit('llama-server did not start')
            time.sleep(.3)

        def ask(prompt):
            c = http.client.HTTPConnection('127.0.0.1', port, timeout=600)
            c.request('POST', '/completion', body=json.dumps({'prompt': prompt, 'n_predict': 1, 'n_probs': 50,
                                                              'temperature': 0, 'cache_prompt': True}))
            r = json.loads(c.getresponse().read()); c.close()
            lp = {t['token']: t['logprob'] for t in r['completion_probabilities'][0]['top_logprobs']}
            floor = min(lp.values())
            y, n = lp.get('yes', floor), lp.get('no', floor)
            return 1 / (1 + math.exp(n - y))
        return judge(ask, qs, top, cs, instruct, split)
    finally:
        p.send_signal(signal.SIGTERM); p.wait(30)


def rerank_geistr(lib, path, qs, top, cs, gpu, instruct, split=lambda t: [t]):
    """The same through geistr itself: geistr_logits of libgeistr (make
    shared), exact yes/no logits, as --files runs it."""
    import ctypes as C, math
    g = C.CDLL(lib)

    class Opts(C.Structure):
        _fields_ = [('size', C.c_size_t), ('processor', C.c_int), ('threads', C.c_uint32), ('context', C.c_uint32),
                    ('chat_format', C.c_char_p), ('decision', C.c_void_p)]
    g.geistr_model_open.argtypes = [C.c_char_p, C.POINTER(Opts), C.POINTER(C.c_void_p), C.c_char_p, C.c_size_t]
    g.geistr_logits.argtypes = [C.c_void_p, C.c_char_p, C.c_size_t, C.POINTER(C.c_char_p), C.POINTER(C.c_float)]
    g.geistr_model_error.restype = C.c_char_p
    model, error = C.c_void_p(), C.create_string_buffer(256)
    opts = Opts(C.sizeof(Opts), 2 if gpu else 1, 0, 2048, None, None)
    if g.geistr_model_open(path.encode(), C.byref(opts), C.byref(model), error, 256) != 0:
        sys.exit(f'geistr_model_open: {error.value.decode()}')
    words, logit = (C.c_char_p * 2)(b'yes', b'no'), (C.c_float * 2)()

    def ask(prompt):
        if g.geistr_logits(model, prompt.encode(), 2, words, logit) != 0:
            sys.exit(f'geistr_logits: {g.geistr_model_error(model).decode()}')
        return 1 / (1 + math.exp(logit[1] - logit[0]))
    try:
        return judge(ask, qs, top, cs, instruct, split)
    finally:
        g.geistr_model_close(model)


def loo(rel, top, p, cs):
    """The threshold chosen leave-one-question-out: for each question the
    threshold that maximises precision + recall + off-topic share on the
    other questions, applied to this one."""
    cands = sorted({x for pj in p for x in pj})
    picked = []
    for i in range(len(rel)):
        others = [k for k in range(len(rel)) if k != i]

        def picks(t, ks):
            return [{cs[j][0] for j, x in zip(top[k], p[k]) if x >= t} for k in ks]
        best = max(cands, key=lambda t: sum(score([rel[k] for k in others], picks(t, others))[:3]))
        picked.append(picks(best, [i])[0])
    return picked


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
        rules.append((f'{name}, threshold leave-one-out', loo(rel, top, p, cs)))
    if o.rerank_gguf:
        top8 = [sorted(range(len(cs)), key=lambda j: -s[j])[:8] for s in scores]
        split = paragraphs if o.pieces else lambda t: [t]
        p8, took = (rerank_geistr(o.lib, o.rerank_gguf, qs, top8, cs, o.ngl > 0, o.instruct, split) if o.lib else
                    rerank_gguf(o.rerank_gguf, qs, top8, cs, o.ngl, o.instruct, split))
        name = os.path.basename(o.rerank_gguf) + (', paragraphs' if o.pieces else '')
        if o.pieces:
            n = [len(paragraphs(t)) for _, t in cs]
            print(f'paragraph pieces: {sum(n)} in {len(cs)} chunks (max {max(n)} per chunk)')
        for k in (4, 8):
            tk, pk = [t[:k] for t in top8], [x[:k] for x in p8]
            rules += [(f'{name} top {k}, P(yes) ≥ {t}', [{cs[j][0] for j, x in zip(tj, pj) if x >= t}
                                                         for tj, pj in zip(tk, pk)])
                      for t in (0.01, 0.05, 0.1, 0.2, 0.3, 0.5, 0.7)]
            rules.append((f'{name} top {k}, threshold leave-one-out', loo(rel, tk, pk, cs)))
            med = sorted(sum(x[:k]) for x in took)[len(took) // 2]
            print(f'{name}: {med:.2f} s per question ({k} chunks, median, '
                  f'{"geistr_logits, " + ("GPU" if o.ngl else "CPU") if o.lib else f"llama-server -ngl {o.ngl}"})')
        if o.dump:
            json.dump({'questions': [q for _, q in qs], 'rel': [sorted(r) for r in rel],
                       'top': [[cs[j][0] for j in t] for t in top8], 'p': p8, 'seconds': took},
                      open(o.dump, 'w'), ensure_ascii=False, indent=0)
    same, cross = (sum(1 for (r, q) in qs if r and (any(language(open(os.path.join(HERE, 'files', f), encoding='utf-8')
                   .read()) != language(q) for f in r)) == c) for c in (False, True))
    print(f'| rule | precision | recall | off-topic: no file | answerable: no file | recall same / cross language '
          f'({same} / {cross} questions) | answerable: no file, same / cross |')
    print('|---|---|---|---|---|---|---|')
    for name, picked in rules:
        rs, es, _, rc, ec, _ = by_language(qs, picked)
        print('| %s | %.2f | %.2f | %.2f | %.2f | %.2f / %.2f | %.2f / %.2f |' % (name, *score(rel, picked), rs, rc, es, ec))


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('geistr'); a.add_argument('model')
    a.add_argument('--query', default=''); a.add_argument('--passage', default='')  # what --files uses (files.h)
    a.add_argument('--compare', action='store_true')
    a.add_argument('--sources', action='store_true')
    a.add_argument('--rerank', metavar='HF_MODEL')
    a.add_argument('--rerank-gguf', metavar='GGUF', help='a Qwen3-Reranker GGUF, scored through llama-server')
    a.add_argument('--ngl', type=int, default=0, help='llama-server -ngl for --rerank-gguf (0: CPU)')
    a.add_argument('--instruct', default=QWEN3_RERANK_INSTRUCT, help='the <Instruct> line for --rerank-gguf')
    a.add_argument('--dump', metavar='JSON', help='write the --rerank-gguf scores to this file')
    a.add_argument('--lib', metavar='LIBGEISTR', help='--rerank-gguf through geistr_logits of this libgeistr '
                   '(make shared) instead of llama-server; --ngl > 0: the GPU')
    a.add_argument('--pieces', action='store_true',
                   help='--rerank-gguf judges each chunk in paragraphs; a chunk scores its best piece')
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

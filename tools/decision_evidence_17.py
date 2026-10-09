"""Run and analyse #17's Apple measurements (docs/DECISION_EVIDENCE_PLAN_17.json).

  decision_evidence_17.py run STEP --out DIR [--old-bench BIN --old-geistr BIN]
      STEP: p1 (engine 0707c3b vs 4afbfcd), p2 (kv AUTO vs FP32), overhead, scalar
  decision_evidence_17.py analyse --out DIR

Raw output goes to DIR, one file per process run, never overwritten; the
analysis reads only those files. Standard library only."""
import argparse
import json
import os
import random
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLAN = ROOT / 'docs/DECISION_EVIDENCE_PLAN_17.json'
MODELS = Path(os.environ.get('GEISTEN_MODELS', Path.home() / 'Library/Application Support/geisten/models'))
FILES = {'gemma4': 'gemma-4-E2B-it-Q4_K_M.gguf', 'bonsai2': 'Ternary-Bonsai-2-27B-PQ2_0.gguf'}
NEW_BENCH, NEW_GEISTR = ROOT / 'build/dec/bench_decision', ROOT / 'build/geistr'
CATALOG_ID = {'gemma4': 'gemma4-e2b', 'bonsai2': 'bonsai2-27b-pq2'}


def mains():
    """The plan's power policy: on battery the clocks differ; nothing is measured."""
    if sys.platform == 'darwin':
        power = subprocess.run(['pmset', '-g', 'batt'], capture_output=True, text=True).stdout
        if "'AC Power'" not in power:
            raise SystemExit('on battery: the plan measures on mains power only')


def quiet(limit=4.0, wait=1800):
    """The plan's thermal policy: 1-minute load below the limit before a block."""
    mains()
    deadline = time.time() + wait
    while os.getloadavg()[0] >= limit:
        if time.time() > deadline:
            raise SystemExit(f'load stayed at {os.getloadavg()[0]:.1f} for {wait} s; not measuring')
        time.sleep(15)
    return os.getloadavg()[0]


def model(profile):
    path = MODELS / FILES[profile]
    if not path.exists():  # the catalog's file name
        found = [p for p in MODELS.glob('*.gguf') if profile[:5].lower() in p.name.lower()]
        if len(found) != 1:
            raise SystemExit(f'{profile}: no single model file in {MODELS}')
        path = found[0]
    return str(path)


def record(out, name, argv, env=None):
    target = out / name
    if target.exists():
        raise SystemExit(f'{target} exists; evidence is never overwritten')
    load = quiet()
    start = time.time()
    r = subprocess.run(argv, capture_output=True, text=True, env=env)
    target.write_text(r.stdout)
    meta = {'name': name, 'argv': [str(a) for a in argv], 'returncode': r.returncode, 'load1_before': load,
            'started': start, 'seconds': time.time() - start, 'stderr_tail': r.stderr[-2000:]}
    with open(out / 'runs.jsonl', 'a') as f:
        f.write(json.dumps(meta) + '\n')
    if r.returncode:
        raise SystemExit(f'{name} failed: {r.stderr[-500:]}')
    print(f'{name}: {meta["seconds"]:.0f} s (load {load:.1f})', flush=True)


def run(step, out, old_bench, old_geistr):
    out.mkdir(parents=True, exist_ok=True)
    if step == 'p1':
        for profile in ('gemma4', 'bonsai2'):
            for block in range(3):  # interleaved; the order alternates per block
                pair = [('old', old_bench), ('new', NEW_BENCH)]
                for tag, binary in pair if block % 2 == 0 else pair[::-1]:
                    record(out, f'p1-direct-{profile}-cpu-{tag}-{block}.jsonl',
                           [binary, 'direct', model(profile), profile, 'cpu', '30'])
            for tag, binary in (('old', old_bench), ('new', NEW_BENCH)):  # Metal: information only
                record(out, f'p1-direct-{profile}-gpu-{tag}-0.jsonl',
                       [binary, 'direct', model(profile), profile, 'gpu', '30'])
        home = out / 'p1-bench-home'
        for block in range(3):
            pair = [('old', old_geistr), ('new', NEW_GEISTR)]
            for tag, binary in pair if block % 2 == 0 else pair[::-1]:
                record(out, f'p1-bench-{tag}-{block}.txt',
                       [binary, 'bench', CATALOG_ID['gemma4'], CATALOG_ID['bonsai2'], '--models', MODELS],
                       env={**os.environ, 'GEISTEN_HOME': str(home), 'NO_COLOR': '1'})
    elif step == 'p2':
        for profile in ('gemma4', 'bonsai2'):
            for proc in ('cpu', 'gpu'):
                record(out, f'p2-kvpair-{profile}-{proc}.jsonl', [NEW_BENCH, 'kvpair', model(profile), profile, proc])
    elif step in ('overhead', 'overhead_fp32'):  # overhead_fp32: after p2 adopted FP32 KV (both sides)
        for profile in ('gemma4', 'bonsai2'):
            for proc in ('cpu', 'gpu'):
                record(out, f'{step}-{profile}-{proc}.jsonl', [NEW_BENCH, 'overhead', model(profile), profile, proc])
    elif step == 'records':  # wrapper vs direct engine, native IDs, logits (test_decision_real)
        for profile in ('gemma4', 'bonsai2'):
            for proc in ('cpu', 'gpu'):
                record(out, f'records-{profile}-{proc}.jsonl',
                       [ROOT / 'build/dec/test_decision_real', model(profile), profile, proc])
    elif step == 'scalar':
        for profile in ('gemma4', 'bonsai2'):
            record(out, f'scalar-{profile}.jsonl', [NEW_BENCH, 'scalar', model(profile), profile])
    else:
        raise SystemExit(f'unknown step {step}')


def rows(path):
    return [json.loads(l) for l in path.read_text().splitlines() if l.startswith('{')]


def mean(xs):
    return sum(xs) / len(xs)


def paired_ratio(a, b, resamples=10000, seed=14917):
    """Ratio of paired means A/B and its two-sided 95 % paired bootstrap interval."""
    rng = random.Random(seed)
    n = len(a)
    ratios = []
    for _ in range(resamples):
        idx = [rng.randrange(n) for _ in range(n)]
        ratios.append(mean([a[i] for i in idx]) / mean([b[i] for i in idx]))
    ratios.sort()
    return mean(a) / mean(b), ratios[int(0.025 * resamples)], ratios[int(0.975 * resamples) - 1]


def gate(lo, hi, bound=1.02):
    return 'PASS' if hi <= bound else 'FAIL' if lo > bound else 'INCONCLUSIVE'


def analyse(out):
    plan = json.loads(PLAN.read_text())
    report = {'plan_sha256': __import__('hashlib').sha256(PLAN.read_bytes()).hexdigest()}
    # p1: decision scoring, engine new/old, cpu (gating) and gpu (information)
    p1 = {}
    for path in sorted(out.glob('p1-direct-*.jsonl')):
        _, _, profile, proc, tag, _ = path.stem.split('-')
        times = [r['scoring_ms'] for r in rows(path) if 'scoring_ms' in r and not r['warmup']]
        p1.setdefault(f'{profile}-{proc}', {}).setdefault(tag, []).extend(times)
    scoring = {k: {'old_ms': mean(v['old']), 'new_ms': mean(v['new']), 'ratio_new_old': mean(v['new']) / mean(v['old']),
                   'n': [len(v['old']), len(v['new'])]} for k, v in p1.items() if 'old' in v and 'new' in v}
    # p1: decode tokens/s from the bench rows each engine recorded
    speeds = {}
    tsv = out / 'p1-bench-home/speed.tsv'
    if tsv.exists():
        for line in tsv.read_text().splitlines():
            f = line.split('\t')
            if len(f) >= 7 and f[6] == 'bench':
                speeds.setdefault((f[0], f[1], f[5][:7]), []).append(float(f[2]))
    decode = {}
    for (m, proc, engine), v in speeds.items():
        decode.setdefault(f'{m}-{proc}', {})[engine] = mean(v)
    for k, v in decode.items():
        if '0707c3b' in v and '4afbfcd' in v:
            v['ratio_new_old'] = v['4afbfcd'] / v['0707c3b']
    significant = [k for k, v in scoring.items() if k.endswith('-cpu') and v['ratio_new_old'] > 1.05] + \
                  [k for k, v in decode.items() if k.endswith('-cpu') and v.get('ratio_new_old', 1) < 0.95]
    report['p1'] = {'scoring': scoring, 'decode_tokens_per_s': decode, 'significant': significant,
                    'complete': len(scoring) == 4 and all('ratio_new_old' in v for k, v in decode.items() if k.endswith('-cpu'))}
    # p2: FP32 KV cost
    p2 = {}
    for path in sorted(out.glob('p2-kvpair-*.jsonl')):
        r = [x for x in rows(path) if 'pair' in x and not x['warmup']]
        ratio, lo, hi = paired_ratio([x['b_scoring_ms'] for x in r], [x['a_scoring_ms'] for x in r])
        p2[path.stem[len('p2-kvpair-'):]] = {'ratio_fp32_auto': ratio, 'ci95': [lo, hi], 'n': len(r)}
    report['p2'] = {'combinations': p2, 'adopt_fp32_kv': bool(p2) and all(v['ratio_fp32_auto'] <= 1.05 for v in p2.values())}
    report['contract'] = ('pending' if not report['p1']['complete'] else
                          'original' if significant else '94')
    # overhead gate
    for prefix in ('overhead', 'overhead_fp32'):
        report[prefix] = overhead(out, prefix)
    print(json.dumps(report, indent=2))


def overhead(out, prefix):
    gates = {}
    for path in sorted(out.glob(f'{prefix}-*.jsonl')):
        data = rows(path)
        r = [x for x in data if 'pair' in x and not x['warmup']]
        ratio, lo, hi = paired_ratio([x['a_scoring_ms'] for x in r], [x['b_scoring_ms'] for x in r])
        prep = sorted(x['a_preparation_ms'] for x in r)
        total = sorted(x['a_total_ms'] for x in r)
        gates[path.stem[len(prefix) + 1:]] = {
            'ratio_runtime_direct': ratio, 'ci95': [lo, hi], 'gate': gate(lo, hi), 'n': len(r),
            'preparation_ms_p50': prep[len(prep) // 2], 'total_ms_p50': total[len(total) // 2],
            'total_ms_p95': total[min(len(total) - 1, int(0.95 * len(total)))],
            'setup': next((x['setup'] for x in data if 'setup' in x), None),
            'selected_rows': next((x['selected_rows'] for x in data if 'selected_rows' in x), None)}
    return gates


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action', choices=['run', 'analyse'])
    p.add_argument('step', nargs='?')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--old-bench', default=str(ROOT / 'build/dec-0707/bench_decision'))
    p.add_argument('--old-geistr', default=str(ROOT / 'build/g-0707/geistr'))
    a = p.parse_args()
    if a.action == 'run':
        run(a.step, a.out, a.old_bench, a.old_geistr)
    else:
        analyse(a.out)


if __name__ == '__main__':
    main()

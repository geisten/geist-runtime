"""Run, analyse and verify #165's Apple prefix-reuse evidence (docs/DECISION_EVIDENCE_PLAN_165.json).

  decision_evidence_165.py schedule                 the sections and their estimate (for the plan)
  decision_evidence_165.py fixtures                 oracle fixtures from the frozen prepare records
  decision_evidence_165.py run [--only PREFIX]      every section not yet recorded, each under the machine lock
  decision_evidence_165.py exec NAME                one section (called by run inside ~/bin/with-mac-lock)
  decision_evidence_165.py analyse                  report.json from the raw records
  decision_evidence_165.py verify                   the raw records against the plan (exit 1 on a violation)

Raw output goes to tests/fixtures/decisions/validation/apple-165/, one file per section, never
overwritten; the analysis reads only those files. Standard library only."""
import argparse
import hashlib
import json
import math
import os
import random
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLAN = ROOT / 'docs/DECISION_EVIDENCE_PLAN_165.json'
OUT = ROOT / 'tests/fixtures/decisions/validation/apple-165'
BENCH = ROOT / 'build/ctx/bench_decision_context'
LOCK = Path.home() / 'bin/with-mac-lock'
MODELS = Path(os.environ.get('GEISTEN_MODELS', Path.home() / 'geist-models'))
FILES = {'gemma4': 'gemma4-e2b-Q4_K_M.gguf', 'bonsai2': 'Ternary-Bonsai-2-27B-PQ2_0.gguf'}
BACKEND = {'cpu': 'cpu_neon', 'gpu': 'metal'}
SIZES, QUESTIONS = (64, 256, 1024), (1, 4, 8, 16)
WARMUP, PAIRS, SECTION_S = 5, 30, 1800
INVALID, CANCELLED = 1, 7


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def rows(path):
    return [json.loads(l) for l in Path(path).read_text().splitlines() if l.startswith('{')]


def ids_sha(ids):
    return hashlib.sha256(struct.pack(f'<{len(ids)}i', *ids)).hexdigest()


# ---- the schedule -----------------------------------------------------------------

def prepared(profile, proc):
    """The frozen prompts: per size the context row and the question rows."""
    data = rows(OUT / f'prepare-{profile}-{proc}.jsonl')
    out = {}
    for r in data[1:]:
        entry = out.setdefault(r['t'], {'questions': {}})
        if 'question' in r:
            entry['questions'][r['question']] = r
        else:
            entry['context'] = r
    return data[0], out


def cost_ms(plan, profile, proc, n):
    c = plan['calibration']['cost_ms'][f'{profile}-{proc}']
    return c['fixed'] + c['per_token'] * n if n else 0.0


def pair_ms(plan, profile, proc, t, q):
    """Estimated time of one reuse/independent pair (both sides)."""
    _, prep = prepared(profile, proc)
    ctx, qs = prep[t]['context'], [prep[t]['questions'][f'q{i + 1:02d}'] for i in range(q)]
    independent = sum(cost_ms(plan, profile, proc, x['prompt_tokens']) for x in qs)
    reuse = cost_ms(plan, profile, proc, ctx['cached_tokens']) + 2 * sum(
        cost_ms(plan, profile, proc, x['suffix_tokens']) for x in qs)
    return independent + reuse


def schedule(plan):
    """Sections of at most SECTION_S (estimated), each with its own five warm-up pairs."""
    sections = []
    for group in plan['matrix']['run_order']:
        profile, proc = group.split('-')
        model = FILES[profile]
        for t in SIZES:
            for q in QUESTIONS:
                cell = f'{profile}-{proc}-{t}-{q}'
                if cell in plan['matrix']['not_run']:
                    continue
                p = pair_ms(plan, profile, proc, t, q) / 1e3
                per = max(1, min(PAIRS, int((SECTION_S - 30) / p) - WARMUP))
                first = 0
                while first < PAIRS:
                    n = min(per, PAIRS - first)
                    sections.append({'name': f'cell-{cell}-p{first:02d}', 'argv': ['cell', model, profile, proc, str(t),
                                                                                  str(q), str(n), str(first)],
                                     'estimate_s': round((WARMUP + n) * p + 30)})
                    first += n
        overhead = 2 * (WARMUP + 30) * cost_ms(plan, profile, proc, 90) / 1e3
        sections.append({'name': f'overhead-{profile}-{proc}', 'argv': ['overhead', model, profile, proc],
                         'estimate_s': round(overhead + 30)})
        _, prep = prepared(profile, proc)
        suffix = max(x['suffix_tokens'] for x in prep[1024]['questions'].values())
        cycle = lambda n: (3 * cost_ms(plan, profile, proc, prep[1024]['context']['cached_tokens']) +
                           (16 + 1 + n) * cost_ms(plan, profile, proc, suffix) + 10 * cost_ms(plan, profile, proc, 20)) / 1e3
        if 3 * cycle(100) + 30 > SECTION_S:  # one process per cycle; cross-cycle growth is then not observable
            n = 100 if cycle(100) + 30 <= SECTION_S else -(-100 // 3)  # at least 100 measured batches in all
            for c in range(3):
                sections.append({'name': f'lifecycle-{profile}-{proc}-c{c}',
                                 'argv': ['lifecycle', model, profile, proc, '1', str(n)], 'estimate_s': round(cycle(n) + 30)})
        else:
            sections.append({'name': f'lifecycle-{profile}-{proc}', 'argv': ['lifecycle', model, profile, proc, '3'],
                             'estimate_s': round(3 * cycle(100) + 30)})
    for profile in plan['numerics']['oracle_profiles']:
        sections.append({'name': f'oracle-{profile}', 'argv': ['oracle', FILES[profile], profile, 'cpu'],
                         'estimate_s': 600})
    return sections


# ---- running -----------------------------------------------------------------------

def host_state():
    def out(argv):
        try:
            return subprocess.run(argv, capture_output=True, text=True, timeout=30).stdout.strip()
        except (OSError, subprocess.SubprocessError) as exc:
            return f'unavailable: {exc}'
    return {'pmset_batt': out(['pmset', '-g', 'batt']), 'pmset_therm': out(['pmset', '-g', 'therm']),
            'load': os.getloadavg(), 'time': time.time()}


def section(plan, name):
    found = [s for s in plan['sections'] if s['name'] == name]
    if len(found) != 1:
        raise SystemExit(f'{name}: not a section of the plan')
    return found[0]


def execute(plan, name):
    """One section, inside the lock: power check, the binary's hash, the run, its record."""
    s = section(plan, name)
    target = OUT / f'{name}.jsonl'
    if target.exists():
        raise SystemExit(f'{target} exists; evidence is never overwritten')
    before = host_state()
    if "'AC Power'" not in before['pmset_batt']:
        raise SystemExit('on battery: the plan measures on mains power only')
    binary = sha(BENCH)
    if binary != plan['freeze']['binary_sha256']['bench_decision_context']:
        raise SystemExit('the harness binary is not the frozen one')
    argv = list(s['argv'])
    if argv[0] == 'oracle':
        fixture = OUT / f'oracle-fixture-{argv[2]}.json'
        cmd = [sys.executable, ROOT / 'tools/llama_oracle.py', '--model', MODELS / argv[1], '--fixture', fixture,
               '--out', OUT / f'oracle-{argv[2]}.json', '--tag', plan['numerics']['oracle']['tag']]
    else:
        argv[1] = str(MODELS / argv[1])
        cmd = [BENCH, *argv]
    start = time.time()
    r = subprocess.run([str(a) for a in cmd], capture_output=True, text=True)
    target.write_text(r.stdout)
    meta = {'name': name, 'argv': [str(a) for a in cmd], 'returncode': r.returncode, 'started': start,
            'seconds': time.time() - start, 'binary_sha256': binary, 'before': before, 'after': host_state(),
            'stderr_tail': r.stderr[-2000:]}
    with open(OUT / 'runs.jsonl', 'a') as f:
        f.write(json.dumps(meta) + '\n')
    print(f'{name}: {meta["seconds"]:.0f} s, rc {r.returncode}, load {before["load"][0]:.1f}', flush=True)
    return r.returncode


def run(plan, only, gap):
    todo = [s for s in plan['sections'] if s['name'].startswith(only or '') and not (OUT / f'{s["name"]}.jsonl').exists()]
    total = sum(s['estimate_s'] for s in todo)
    print(f'{len(todo)} sections, estimated {total / 3600:.1f} h of lock time', flush=True)
    for i, s in enumerate(todo):
        status = subprocess.run([LOCK, 'status'], capture_output=True, text=True).stdout.strip()
        print(f'[{i + 1}/{len(todo)}] {s["name"]} (est. {s["estimate_s"]} s), lock: {status}', flush=True)
        subprocess.run([LOCK, sys.executable, __file__, 'exec', s['name']])
        if i + 1 < len(todo):
            time.sleep(gap)  # the mac CI runner may take queued jobs


def fixtures(plan):
    """Oracle fixtures: every frozen full prompt (IDs are backend-independent; checked)."""
    for profile in plan['numerics']['oracle_profiles']:
        cases = []
        _, cpu = prepared(profile, 'cpu')
        _, gpu = prepared(profile, 'gpu')
        for t in SIZES:
            for qid, q in sorted(cpu[t]['questions'].items()):
                other = gpu[t]['questions'][qid]
                if (q['ids'], q['candidate_ids']) != (other['ids'], other['candidate_ids']):
                    raise SystemExit(f'{profile} {t} {qid}: prompt IDs differ between backends')
                cases.append({'id': f'{t}-{qid}', 'ids': q['ids'], 'candidate_ids': q['candidate_ids']})
        fixture = {'schema': 1, 'artifact_sha256': plan['freeze']['models'][profile]['sha256'], 'cases': cases}
        (OUT / f'oracle-fixture-{profile}.json').write_text(json.dumps(fixture) + '\n')


# ---- analysis ----------------------------------------------------------------------

def mean(xs):
    return sum(xs) / len(xs)


def quantile(xs, p):
    s = sorted(xs)
    return s[min(len(s) - 1, int(p * len(s)))]


def paired_ratio(a, b, resamples=10000, seed=14917):
    """Ratio of paired means A/B and its two-sided 95 % paired bootstrap interval (#17)."""
    rng = random.Random(seed)
    n = len(a)
    ratios = []
    for _ in range(resamples):
        idx = [rng.randrange(n) for _ in range(n)]
        ratios.append(mean([a[i] for i in idx]) / mean([b[i] for i in idx]))
    ratios.sort()
    return mean(a) / mean(b), ratios[int(0.025 * resamples)], ratios[int(0.975 * resamples) - 1]


def speed_gate(lo, hi):
    return 'PASS' if hi < 1.0 else 'FAIL' if lo > 1.0 else 'INCONCLUSIVE'


def overhead_gate(lo, hi, bound=1.02):
    return 'PASS' if hi <= bound else 'FAIL' if lo > bound else 'INCONCLUSIVE'


class Violations(list):
    def check(self, condition, message):
        if not condition:
            self.append(message)
        return condition


def header_ok(v, plan, name, head, profile, proc, task):
    v.check(head.get('task') == task, f'{name}: task {head.get("task")}')
    v.check(head.get('profile') == profile, f'{name}: wrong profile {head.get("profile")}')
    v.check(head.get('backend') == BACKEND[proc], f'{name}: wrong backend {head.get("backend")}')
    v.check(head.get('engine') == plan['freeze']['engine_revision'], f'{name}: wrong engine')
    v.check(head.get('model_sha256') == plan['freeze']['models'][profile]['sha256'], f'{name}: wrong model')
    v.check(head.get('threads') == plan['freeze']['threads'] and head.get('model_context') == plan['freeze']['model_context'],
            f'{name}: wrong threads/context')


def analyse_cell(plan, v, profile, proc, t, q, names):
    cell = f'{profile}-{proc}-{t}-{q}'
    _, prep = prepared(profile, proc)
    ctx = prep[t]['context']
    pairs, controls, failures, sections = {}, [], [], []
    for name in names:
        data = rows(OUT / f'{name}.jsonl')
        if not v.check(len(data) >= 3, f'{name}: no records'):
            continue
        header_ok(v, plan, name, data[0], profile, proc, 'cell')
        geo = data[1].get('cell', {})
        v.check(geo.get('t') == t and geo.get('q') == q and geo.get('cap') == t + plan['freeze']['capacity_margin'] and
                geo.get('context_tokens') == ctx['context_tokens'] and geo.get('snapshot_budget') == plan['freeze']['max_snapshot_bytes'],
                f'{name}: wrong geometry {geo}')
        opened = [r for r in data if r.get('control') == 'open']
        if v.check(len(opened) == 1 and opened[0]['ok'], f'{name}: control context did not open'):
            info = opened[0]['info']
            v.check(info['cached_tokens'] == ctx['cached_tokens'] and info['prefill_chunk'] == ctx['prefill_chunk'] and
                    info['max_prompt_tokens'] == ctx['cap'] and info['kv_cache'] == 'fp32' and
                    info['context_sha256'] == ctx['context_sha256'], f'{name}: context geometry differs from the frozen cut')
            sections.append({'name': name, 'load1': data[0]['load1'], 'snapshot_bytes': info['snapshot_bytes'],
                             'setup_ms': info['setup_ms']})
        abc = [r for r in data if r.get('control') == 'ABC']
        v.check(len(abc) == q, f'{name}: {len(abc)} control rows for {q} questions')
        for r in abc:
            frozen = prep[t]['questions'][r['question']]
            v.check(r['ids_sha256'] == ids_sha(frozen['ids']) and r['candidate_ids'] == frozen['candidate_ids'],
                    f'{name}: {r["question"]} prompt IDs are not the frozen ones')
            controls.append(r)
        for r in data:
            if 'pair' in r and not r['warmup']:
                v.check(r['pair'] not in pairs, f'{name}: pair {r["pair"]} twice')
                pairs[r['pair']] = r
    v.check(sorted(pairs) == list(range(PAIRS)), f'{cell}: measured pairs {sorted(pairs)}')
    ordered = [pairs[i] for i in sorted(pairs)]
    for r in ordered:
        v.check(r['order'] == ('RI' if r['pair'] % 2 == 0 else 'IR'), f'{cell}: pair {r["pair"]} order')
        if r['reuse']['failed'] or r['independent']['failed']:
            failures.append(r['pair'])
    first = [r['reuse']['first_package_ms'] for r in ordered]
    repeat = [r['reuse'].get('repeat_package_ms', math.nan) for r in ordered]
    independent = [r['independent']['package_ms'] for r in ordered]
    out = {'t': t, 'q': q, 'sections': sections, 'n_pairs': len(ordered), 'failed_pairs': failures,
           'context_tokens': ctx['context_tokens'], 'prefix_tokens': ctx['prefix_tokens'],
           'cached_tokens': ctx['cached_tokens'], 'prefill_chunk': ctx['prefill_chunk'], 'capacity': ctx['cap']}
    if len(ordered) == PAIRS and not any(math.isnan(x) for x in repeat):
        for key, xs in (('first', first), ('repeat', repeat)):
            ratio, lo, hi = paired_ratio(xs, independent)
            verdict = speed_gate(lo, hi) if not failures else 'FAIL'
            out[key] = {'ratio': ratio, 'ci95': [lo, hi], 'verdict': verdict, 'p50_ms': quantile(xs, 0.5),
                        'p95_ms': quantile(xs, 0.95), 'mean_ms': mean(xs)}
        out['independent'] = {'p50_ms': quantile(independent, 0.5), 'p95_ms': quantile(independent, 0.95),
                              'mean_ms': mean(independent)}
        ok = [r for r in ordered if not r['reuse']['failed']]
        comp = lambda f: quantile([f(r) for r in ok], 0.5) if ok else None
        out['components_p50_ms'] = {
            'open_setup': comp(lambda r: r['reuse']['open']['setup_ms']),
            'first_preparation': comp(lambda r: r['reuse']['first']['preparation_ms']),
            'first_scoring': comp(lambda r: r['reuse']['first']['scoring_ms']),
            'repeat_preparation': comp(lambda r: r['reuse']['repeat']['preparation_ms']),
            'repeat_scoring': comp(lambda r: r['reuse']['repeat']['scoring_ms']),
            'close': comp(lambda r: r['reuse']['close_ms']),
            'independent_preparation': quantile([r['independent']['preparation_ms'] for r in ordered], 0.5),
            'independent_scoring': quantile([r['independent']['scoring_ms'] for r in ordered], 0.5)}
        out['snapshot_bytes'] = sections[0]['snapshot_bytes'] if sections else None
        out['rss_bytes_max'] = max(r['rss_bytes'] for r in ordered)
        out['model_calls'] = {'reuse_per_pair': sum(r['reuse']['first']['model_calls'] + r['reuse']['repeat']['model_calls']
                                                    for r in ok) / max(1, len(ok)),
                              'setup_per_pair': 1 if ctx['cached_tokens'] else 0}
        out['outputs_stable'] = all(r['reuse']['first']['same_as_ref'] and r['reuse']['repeat']['same_as_ref'] for r in ok)
        out['load1_mean'] = mean([r['load1'] for r in ordered])
    else:
        out['first'] = out['repeat'] = {'verdict': 'MISSING'}
    out['controls'] = {
        'c_vs_b_bit_identical': all(r['b_ok'] and r['c_vs_b_bit_identical'] for r in controls) and bool(controls),
        'a_same_ids': all(r['a_same_ids'] for r in controls) and bool(controls),
        'c_vs_a_bit_identical': all(r['c_vs_a_bit_identical'] for r in controls) and bool(controls),
        'c_vs_a_max_abs': max((r['c_vs_a_max_abs'] for r in controls), default=None),
        'c_vs_a_selection_agree': all(r['best_c'] == r['best_a'] for r in controls) and bool(controls),
        'b_ms_p50': quantile([r['b_ms'] for r in controls], 0.5) if controls else None}
    return out


def analyse_overhead(plan, v, profile, proc):
    name = f'overhead-{profile}-{proc}'
    path = OUT / f'{name}.jsonl'
    if not v.check(path.exists(), f'{name}: missing'):
        return {'verdict': 'MISSING'}
    data = rows(path)
    header_ok(v, plan, name, data[0], profile, proc, 'overhead')
    setup = data[1].get('setup', {})
    if not v.check(setup.get('ok'), f'{name}: setup failed'):
        return {'verdict': 'FAIL', 'setup': setup}
    r = [x for x in data if 'pair' in x and not x['warmup']]
    v.check(len(r) == PAIRS, f'{name}: {len(r)} measured pairs')
    failed = [x['pair'] for x in r if not (x['a_ok'] and x['b_ok'])]
    ratio, lo, hi = paired_ratio([x['a_scoring_ms'] for x in r], [x['b_scoring_ms'] for x in r])
    return {'ratio_runtime_direct': ratio, 'ci95': [lo, hi], 'verdict': overhead_gate(lo, hi) if not failed else 'FAIL',
            'failed_pairs': failed, 'n': len(r), 'setup': setup,
            'bit_identical': all(x['a_bit_identical'] and x['b_bit_identical'] for x in r),
            'preparation_ms_p50': quantile([x['a_preparation_ms'] for x in r], 0.5),
            'a_scoring_ms_p50': quantile([x['a_scoring_ms'] for x in r], 0.5),
            'b_scoring_ms_p50': quantile([x['b_scoring_ms'] for x in r], 0.5)}


def analyse_lifecycle(plan, v, profile, proc):
    names = sorted(s['name'] for s in plan['sections'] if s['name'].startswith(f'lifecycle-{profile}-{proc}'))
    budget = plan['lifecycle']['budgets'][profile]
    checks, records = {}, []
    for k, name in enumerate(names):
        path = OUT / f'{name}.jsonl'
        if not v.check(path.exists(), f'{name}: missing'):
            return {'verdict': 'MISSING'}
        data = rows(path)
        for r in data:
            if 'cycle' in r and len(names) > 1:
                r['cycle'] = k  # one process per cycle
            if 'task' in r:
                header_ok(v, plan, name, r, profile, proc, 'lifecycle')
        records += [r for r in data if 'cycle' in r]

    def check(key, ok, observed):
        prior = checks.get(key)
        checks[key] = {'ok': ok and (prior is None or prior['ok']), 'observed': observed if prior is None or not ok else prior['observed']}

    argv = [s['argv'] for s in plan['sections'] if s['name'] in names]
    per_cycle = int(argv[0][6]) if argv and len(argv[0]) > 6 else plan['lifecycle']['requests']
    cycles = sorted({r['cycle'] for r in records})
    check('at_least_100_measured_batches', per_cycle * len(cycles) >= 100, per_cycle * len(cycles))
    check('three_cycles', cycles == [0, 1, 2], cycles)
    gpu = proc == 'gpu'
    for c in cycles:
        rs = [r for r in records if r['cycle'] == c]
        phase = lambda p: [r for r in rs if r.get('phase') == p]
        batches = phase('batch')
        check('measured_batches', len(batches) == per_cycle, len(batches))
        check('batches_ok_and_identical', all(r['ok'] and r['same_as_warm'] and r['model_calls'] == 1 for r in batches),
              [r['index'] for r in batches if not (r['ok'] and r['same_as_warm'])])
        opened = phase('context_open')
        check('context_open', len(opened) == 1 and opened[0]['ok'], opened[0].get('status') if opened else None)
        warm = phase('warm')
        check('warm_largest_geometry', len(warm) == 1 and warm[0]['ok'] and warm[0]['model_calls'] == 16, warm[:1])
        mix = phase('mix')
        check('mix_invalid_cancel_chat', len(mix) == -(-per_cycle // 10) and all(
            r['chat_ok'] and r['invalid_status'] == INVALID and r['invalid_answers'] == 0 and r['cancel_status'] == CANCELLED
            and r['cancel_answers'] == 0 for r in mix), len(mix))
        reset = phase('context_reset_open')
        check('reset_recovery', len(reset) == 1 and reset[0]['ok'], len(reset))
        second = phase('second_context')
        check('weights_resident_once', len(second) == 1 and second[0]['ok'] and
              second[0]['rss_delta_bytes'] < plan['freeze']['models'][profile]['bytes'] // 2,
              second[0]['rss_delta_bytes'] if second else None)
        live = {(r['live_allocations'], r['live_bytes']) for r in batches + mix}
        check('wrapper_constant_per_request', len(live) == 1, sorted(live))
        for p in ('context_reset_closed', 'context_closed', 'before_model_close'):
            z = phase(p)
            check('wrapper_ownership_zero', len(z) == 1 and z[0]['live_allocations'] == 0 and z[0]['live_bytes'] == 0,
                  [(x['live_allocations'], x['live_bytes']) for x in z])
        loaded = [r for r in rs if r.get('resources_ok')]
        rss_max = max(r['rss_bytes'] for r in loaded)
        check('rss_max', rss_max <= budget['rss_max_bytes'], rss_max)
        if gpu:
            check('metal_known', all(r['provider_known'] for r in loaded), None)
            metal_max = max(r['provider_allocated_bytes'] for r in loaded)
            check('metal_max', metal_max <= budget['metal_max_bytes'], metal_max)
            before = phase('before_context')[0]['provider_allocated_bytes']
            after = phase('context_closed')[0]['provider_allocated_bytes']
            check('metal_released_after_context', after - before <= plan['lifecycle']['metal_context_retention_bytes'],
                  after - before)
        reset_closed = phase('context_reset_closed')[0]['rss_bytes']
        final_closed = phase('context_closed')[0]['rss_bytes']
        check('rss_no_growth_across_contexts', final_closed - reset_closed <= plan['lifecycle']['growth_bytes'],
              final_closed - reset_closed)
        closed = phase('model_closed')
        check('rss_after_model_close', len(closed) == 1 and closed[0]['rss_bytes'] <= budget['rss_cleanup_max_bytes'],
              closed[0]['rss_bytes'] if closed else None)
    closed = [r['rss_bytes'] for r in records if r.get('phase') == 'model_closed']
    if len(names) == 1:
        check('rss_no_growth_across_cycles', len(closed) == 3 and closed[-1] - closed[0] <= plan['lifecycle']['growth_bytes'],
              closed[-1] - closed[0] if closed else None)
    else:
        checks['rss_no_growth_across_cycles'] = {'ok': None, 'observed': 'not observable: one process per cycle'}
    failed = [k for k, x in checks.items() if x['ok'] is False]
    summary = {'rss_max_bytes': max((r['rss_bytes'] for r in records if r.get('resources_ok')), default=None),
               'rss_model_closed_bytes': closed,
               'metal_max_bytes': max((r['provider_allocated_bytes'] for r in records if r.get('provider_known')), default=None),
               'snapshot_bytes': next((r['info']['snapshot_bytes'] for r in records if r.get('phase') == 'context_open'
                                       and 'info' in r), None),
               'context_live_bytes': next((r['live_bytes'] for r in records if r.get('phase') == 'warm'), None),
               'second_context_rss_delta_bytes': [r['rss_delta_bytes'] for r in records if r.get('phase') == 'second_context']}
    return {'verdict': 'FAIL' if failed else 'PASS', 'failed_checks': failed, 'checks': checks, 'observed': summary}


def numerics(plan, v, cells, runs):
    """#94 contract (in force via #125) against the pinned oracle, and the original contract, per backend."""
    contract = plan['numerics']['contract_94']
    out = {}
    for profile in plan['numerics']['oracle_profiles']:
        path = OUT / f'oracle-{profile}.json'
        if not path.exists():
            run = [r for r in runs if r['name'] == f'oracle-r2-{profile}']
            out[profile] = {'verdict': 'NOT_APPLICABLE' if run else 'MISSING',
                            'reason': run[0]['stderr_tail'][-600:] if run else 'not run'}
            continue
        oracle = json.loads(path.read_text())
        v.check(oracle['reference']['tag'] == plan['numerics']['oracle']['tag'], f'oracle-{profile}: wrong tag')
        v.check(oracle['token_fixture_sha256'] == sha(OUT / f'oracle-fixture-{profile}.json'), f'oracle-{profile}: fixture')
        ref = dict(zip(oracle['cases'], oracle['outputs']))
        for proc in ('cpu', 'gpu'):
            observed = {}
            for t in SIZES:
                name = next((s['name'] for s in plan['sections'] if s['name'].startswith(f'cell-{profile}-{proc}-{t}-16-p00')), None)
                if name is None or not (OUT / f'{name}.jsonl').exists():
                    continue
                for r in rows(OUT / f'{name}.jsonl'):
                    if r.get('control') == 'ABC':
                        observed[f'{t}-{r["question"]}'] = r
            cases = []
            for cid, o in ref.items():
                if cid not in observed:
                    continue
                c = observed[cid]['c']
                err = [abs(x - y) for x, y in zip(o['logits'], c)]
                top = sorted(o['logits'], reverse=True)
                gap = top[0] - top[1]
                best = max(range(len(c)), key=lambda k: c[k])
                allowance = [contract['original_atol'] + contract['original_rtol'] * abs(x) for x in o['logits']]
                near = gap <= 2 * max(allowance)
                cases.append({'case': cid, 'reference_gap': gap, 'max_abs': max(err), 'best': best, 'oracle_best': o['best_index'],
                              'selection_94': ('PASS' if best == o['best_index'] else 'FAIL') if gap >= contract['winner_gap'] else 'NOT_GATED',
                              'envelope_94': 'PASS' if max(err) <= contract['envelope'] else 'FAIL',
                              'original_logits': 'PASS' if all(e <= a for e, a in zip(err, allowance)) else 'FAIL',
                              'original_selection': 'INCONCLUSIVE' if near else 'PASS' if best == o['best_index'] else 'FAIL'})
            complete = len(cases) == len(ref)
            v.check(complete, f'numerics {profile}-{proc}: {len(cases)} of {len(ref)} cases')
            v94 = 'FAIL' if any(c['selection_94'] == 'FAIL' or c['envelope_94'] == 'FAIL' for c in cases) else 'PASS'
            orig = 'FAIL' if any(c['original_logits'] == 'FAIL' or c['original_selection'] == 'FAIL' for c in cases) else \
                'INCONCLUSIVE' if any(c['original_selection'] == 'INCONCLUSIVE' for c in cases) else 'PASS'
            out[f'{profile}-{proc}'] = {'oracle': oracle['reference'], 'n': len(cases), 'complete': complete,
                                         'contract_94': v94 if complete else 'MISSING', 'original': orig if complete else 'MISSING',
                                         'max_abs': max((c['max_abs'] for c in cases), default=None),
                                         'selection_agree': sum(c['best'] == c['oracle_best'] for c in cases),
                                         'gated': sum(c['selection_94'] != 'NOT_GATED' for c in cases), 'cases': cases}
    # within the engine: C vs B is the gate (bit-identical); C vs A against #94's within-engine bound
    within = {}
    for key, cell in cells.items():
        ctl = cell['controls']
        within[key] = {'c_vs_b': 'PASS' if ctl['c_vs_b_bit_identical'] else 'FAIL',
                       'c_vs_a': 'PASS' if ctl['c_vs_a_bit_identical'] else
                       'PASS_WITHIN_BOUND' if ctl['c_vs_a_max_abs'] is not None and ctl['c_vs_a_max_abs'] <= 0.10 else 'FAIL',
                       'c_vs_a_max_abs': ctl['c_vs_a_max_abs'], 'selection_agree': ctl['c_vs_a_selection_agree']}
    out['within_engine'] = within
    return out


def analyse(plan):
    v = Violations()
    runs = [json.loads(l) for l in (OUT / 'runs.jsonl').read_text().splitlines()] if (OUT / 'runs.jsonl').exists() else []
    superseded = {n for a in plan.get('amendments', []) for n in a.get('superseded_sections', [])}
    for s in plan['sections']:
        rec = [r for r in runs if r['name'] == s['name']]
        if v.check(len(rec) == 1, f'{s["name"]}: {len(rec)} run records'):
            v.check(rec[0]['returncode'] == 0 or s['name'] in superseded or s['name'] == 'oracle-r2-bonsai2',
                    f'{s["name"]}: exit {rec[0]["returncode"]}')
            v.check(rec[0]['binary_sha256'] == plan['freeze']['binary_sha256']['bench_decision_context'], f'{s["name"]}: binary')
    report = {'plan_sha256': sha(PLAN), 'cells': {}, 'overhead': {}, 'lifecycle': {}}
    for group in plan['matrix']['run_order']:
        profile, proc = group.split('-')
        for t in SIZES:
            for q in QUESTIONS:
                key = f'{profile}-{proc}-{t}-{q}'
                names = [s['name'] for s in plan['sections'] if s['name'].startswith(f'cell-{key}-p')]
                if key in plan['matrix']['not_run']:
                    report['cells'][key] = {'t': t, 'q': q, 'first': {'verdict': 'NOT_RUN'}, 'repeat': {'verdict': 'NOT_RUN'},
                                            'reason': plan['matrix']['not_run'][key]}
                    continue
                if not all((OUT / f'{n}.jsonl').exists() for n in names):
                    v.append(f'{key}: missing sections')
                    report['cells'][key] = {'t': t, 'q': q, 'first': {'verdict': 'MISSING'}, 'repeat': {'verdict': 'MISSING'}}
                    continue
                report['cells'][key] = analyse_cell(plan, v, profile, proc, t, q, names)
        report['overhead'][group] = analyse_overhead(plan, v, profile, proc)
        report['lifecycle'][group] = analyse_lifecycle(plan, v, profile, proc)
    run_cells = {k: c for k, c in report['cells'].items() if 'controls' in c}
    report['numerics'] = numerics(plan, v, run_cells, runs)
    report['configuration'] = {
        'verdict': 'PASS' if not v and all(c['controls']['c_vs_b_bit_identical'] and c['controls']['a_same_ids']
                                           for c in run_cells.values()) else 'FAIL',
        'violations': list(v)}
    report['superseded'] = [{'name': r['name'], 'returncode': r['returncode'], 'seconds': r['seconds']}
                            for r in runs if r['name'] in superseded]
    report['lock_seconds'] = sum(r['seconds'] for r in runs)
    report['runs'] = len(runs)
    return report, v


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action', choices=['schedule', 'fixtures', 'run', 'exec', 'analyse', 'verify'])
    p.add_argument('name', nargs='?')
    p.add_argument('--only')
    p.add_argument('--gap', type=int, default=180)
    a = p.parse_args()
    plan = json.loads(PLAN.read_text())
    if a.action == 'schedule':
        s = schedule(plan)
        print(json.dumps(s, indent=1))
        print(f'{len(s)} sections, {sum(x["estimate_s"] for x in s) / 3600:.1f} h', file=sys.stderr)
    elif a.action == 'fixtures':
        fixtures(plan)
    elif a.action == 'run':
        run(plan, a.only, a.gap)
    elif a.action == 'exec':
        raise SystemExit(execute(plan, a.name))
    else:
        report, v = analyse(plan)
        if a.action == 'analyse':
            (OUT / 'report.json').write_text(json.dumps(report, indent=1) + '\n')
            print(json.dumps({k: report[k] for k in ('configuration', 'lock_seconds', 'runs')}, indent=1))
        else:
            print('\n'.join(v) or 'verified')
            raise SystemExit(1 if v else 0)


if __name__ == '__main__':
    main()

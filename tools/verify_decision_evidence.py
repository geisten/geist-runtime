"""Verify checksum-bound numerical development evidence; no quality inference.

early_exit() is the exception: it recomputes geistlib#286's declared quality gate
(docs/DECISION_EVIDENCE_PLAN_286.json) from the committed raw probe records."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import math

def require(condition, message):
    if not condition:
        raise ValueError(message)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def numerical(plan, fixture, reference, actual):
    contract = plan['independent_numeric']
    require(fixture['artifact_sha256'] == contract['artifact_sha256'], 'evidence invariant failed')
    require(reference['artifact_sha256'] == contract['artifact_sha256'], 'evidence invariant failed')
    require(reference['reference_revision'] == contract['reference_revision'], 'evidence invariant failed')
    require(reference['token_fixture_sha256'] == hashlib.sha256(json.dumps(fixture, ensure_ascii=False, indent=2).encode() + b'\n').hexdigest(), 'evidence invariant failed')
    expected = [row for row in actual if row.get('mode') == 1 and 'logits' in row]
    require(len(expected) == len(reference['outputs']) == len(fixture['cases']) == 3, 'evidence invariant failed')
    reports = []
    for index, (case, ref, observed) in enumerate(zip(fixture['cases'], reference['outputs'], expected)):
        require(case['id'] == contract['cases'][index] and observed['fixture'] == index, 'evidence invariant failed')
        require(len(ref['logits']) == len(observed['logits']) == len(case['candidate_ids']), 'evidence invariant failed')
        require(all((math.isfinite(x) for x in ref['logits'] + observed['logits'])), 'evidence invariant failed')
        require(ref['best_index'] == max(range(len(ref['logits'])), key=lambda k: ref['logits'][k]), 'evidence invariant failed')
        allowance = [contract['absolute_tolerance'] + contract['relative_tolerance'] * abs(x) for x in ref['logits']]
        error = [abs(x - y) for x, y in zip(ref['logits'], observed['logits'])]
        sorted_logits = sorted(ref['logits'], reverse=True)
        near = len(sorted_logits) > 1 and sorted_logits[0] - sorted_logits[1] <= 2 * max(allowance)
        logits_pass = all((e <= a for e, a in zip(error, allowance)))
        selection = 'INCONCLUSIVE' if near else 'PASS' if ref['best_index'] == observed['best_index'] else 'FAIL'
        reports.append({'fixture': case['id'], 'max_absolute_error': max(error), 'logits': 'PASS' if logits_pass else 'FAIL', 'selection': selection})
    verdict = 'FAIL' if any((r['logits'] == 'FAIL' or r['selection'] == 'FAIL' for r in reports)) else 'INCONCLUSIVE' if any((r['selection'] == 'INCONCLUSIVE' for r in reports)) else 'PASS'
    return {'scope': 'three independent numerical development fixtures; not MMLU quality', 'verdict': verdict, 'cases': reports}

def numerical_94(plan17, fixture, reference, actual):
    """#17's contract (#94): the winner where the oracle's top-two gap is wide
    enough, and an envelope per candidate; thresholds from the frozen plan."""
    contract = plan17['independent_numeric_94']
    envelope = float(contract['envelope'].split('<=')[1].split()[0])
    min_gap = float(contract['winner_gate'].split('>=')[1].split()[0])
    expected = [row for row in actual if row.get('mode') == 1 and 'logits' in row]
    require(len(expected) == len(reference['outputs']) == len(fixture['cases']) == 3, 'evidence invariant failed')
    reports = []
    for index, (case, ref, observed) in enumerate(zip(fixture['cases'], reference['outputs'], expected)):
        require(observed['fixture'] == index and len(ref['logits']) == len(observed['logits']), 'evidence invariant failed')
        require(all(math.isfinite(x) for x in ref['logits'] + observed['logits']), 'evidence invariant failed')
        error = [abs(x - y) for x, y in zip(ref['logits'], observed['logits'])]
        top = sorted(ref['logits'], reverse=True)
        gap = top[0] - top[1]
        gated = gap >= min_gap
        selection = ('PASS' if ref['best_index'] == observed['best_index'] else 'FAIL') if gated else 'NOT_GATED'
        reports.append({'fixture': case['id'], 'reference_gap': gap, 'max_absolute_error': max(error),
                        'envelope': 'PASS' if max(error) <= envelope else 'FAIL', 'selection': selection})
    verdict = 'FAIL' if any(r['envelope'] == 'FAIL' or r['selection'] == 'FAIL' for r in reports) else 'PASS'
    return {'scope': 'three independent numerical development fixtures under the #94 contract; not MMLU quality',
            'contract': '94', 'verdict': verdict, 'cases': reports}


def against_oracle(plan, plan17, fixture, oracle, actual):
    """Both contracts against an oracle recorded by tools/llama_oracle.py (the
    latest llama.cpp release, decision 2026-10-09): its own provenance replaces
    the original plan's pinned revision; thresholds are unchanged."""
    require(oracle['artifact_sha256'] == fixture['artifact_sha256'] == plan['independent_numeric']['artifact_sha256'],
            'evidence invariant failed')
    require(oracle['cases'] == [case['id'] for case in fixture['cases']], 'evidence invariant failed')
    pinned = dict(oracle, reference_revision=plan['independent_numeric']['reference_revision'],
                  token_fixture_sha256=hashlib.sha256(json.dumps(fixture, ensure_ascii=False, indent=2).encode() + b'\n').hexdigest())
    return {'oracle': oracle['reference'], 'original': numerical(plan, fixture, pinned, actual),
            '94': numerical_94(plan17, fixture, oracle, actual)}


def jsonl_gz(path):
    return [json.loads(line) for line in gzip.decompress(path.read_bytes()).decode().split('\n') if line]


def _binomial_cdf(n, k, p):
    """P(X <= k), X ~ Binomial(n, p), summed in log space (no underflow at n in the thousands)."""
    if k < 0:
        return 0.
    if k >= n or p <= 0:
        return 1.
    if p >= 1:
        return 0.
    log_p, log_q = math.log(p), math.log1p(-p)
    return min(1., math.fsum(math.exp(math.lgamma(n + 1) - math.lgamma(i + 1) - math.lgamma(n - i + 1)
                                      + i * log_p + (n - i) * log_q) for i in range(k + 1)))


def _clopper_pearson(n, k, alpha):
    """Exact two-sided (1 - alpha) interval of k successes in n, by bisection."""
    def solve(predicate):
        lo, hi = 0., 1.
        for _ in range(60):
            mid = (lo + hi) / 2
            lo, hi = (mid, hi) if predicate(mid) else (lo, mid)
        return (lo + hi) / 2
    lower = 0. if k == 0 else solve(lambda p: _binomial_cdf(n, k - 1, p) > 1 - alpha / 2)
    upper = 1. if k == n else solve(lambda p: _binomial_cdf(n, k, p) > alpha / 2)
    return lower, upper


def _percentile(values, q):
    xs = sorted(values)
    i = (len(xs) - 1) * q
    lo, hi = math.floor(i), math.ceil(i)
    return xs[lo] + (xs[hi] - xs[lo]) * (i - lo)


def early_exit(plan, records, live):
    """geistlib#286: both arms from one recorded generation, the paired gate, and the live check.

    The early-exit arm is the frozen rule applied to the recorded probes; the
    full arm is the recorded answer (a capped answer is wrong). Non-inferior iff
    the lower bound of the conservative paired interval (Clopper-Pearson for
    gains and for losses, Bonferroni) is >= -margin."""
    c = plan['contract']
    ids = [r['question_id'] for r in records]
    require(len(records) == c['questions'] == len(set(ids)), 'evidence invariant failed')
    require(hashlib.sha256('\n'.join(ids).encode()).hexdigest() == c['question_ids_sha256'], 'not the declared sample')
    full, early, answers, tokens = {}, {}, {}, {'full': 0, 'early_exit': 0}
    exits = at_first = capped = still_capped = 0
    for r in records:
        require(r['stop'] in ('eos', 'limit') and r['generated_tokens'] <= c['cap'], 'evidence invariant failed')
        require((r['stop'] == 'eos') == r['valid'] == (r['predicted_index'] is not None), 'evidence invariant failed')
        positions = [p[0] for p in r['probes'] if p[0] > 0]
        require(positions == list(range(c['probe_interval'], r['generated_tokens'], c['probe_interval'])),
                'probes missing or out of order')
        full[r['question_id']] = r['valid'] and r['predicted_index'] == r['target_index']
        capped += r['stop'] == 'limit'
        answer, used = (r['predicted_index'] if r['valid'] else None), r['generated_tokens']
        for pos, *logits in r['probes']:
            require(len(logits) >= 2 and all(math.isfinite(x) for x in logits), 'non-finite probe')
            if pos < c['probe_interval']:
                continue
            best = max(range(len(logits)), key=lambda i: logits[i])  # first maximum, as the engine
            top = 1 / math.fsum(math.exp(x - logits[best]) for x in logits)
            if top >= c['tau']:
                answer, used = best, pos
                exits += 1
                at_first += pos == c['probe_interval']
                break
        else:
            still_capped += r['stop'] == 'limit'
        early[r['question_id']] = answer == r['target_index']
        answers[r['question_id']] = (answer, used)
        tokens['full'] += r['generated_tokens']
        tokens['early_exit'] += used
    n = len(records)
    gains = sum(early[k] and not full[k] for k in full)
    losses = sum(full[k] and not early[k] for k in full)
    alpha = (1 - c['confidence']) / 2
    gain, loss = _clopper_pearson(n, gains, alpha), _clopper_pearson(n, losses, alpha)
    interval = [100 * (gain[0] - loss[1]), 100 * (gain[1] - loss[0])]
    noninferior = interval[0] >= -c['margin_pp']
    # the live check: the same 300, both arms run live; agreement with the offline early-exit arm
    arms = {arm: {r['question_id']: r for r in live if r['arm'] == arm} for arm in ('early_exit', 'full')}
    lat_ids = sorted(arms['early_exit'])
    require(len(lat_ids) == c['latency_questions'] and sorted(arms['full']) == lat_ids
            and set(lat_ids) <= set(full), 'not the declared latency questions')
    by_id = {r['question_id']: r for r in records}
    agree = sum(arms['early_exit'][k]['predicted_index'] == answers[k][0] for k in lat_ids)
    identical = sum(arms['early_exit'][k]['predicted_index'] == answers[k][0]
                    and arms['early_exit'][k]['generated_tokens'] == answers[k][1] for k in lat_ids)
    full_identical = sum(arms['full'][k]['predicted_index'] == (by_id[k]['predicted_index'])
                         and arms['full'][k]['generated_tokens'] == by_id[k]['generated_tokens'] for k in lat_ids)
    latency = {arm: {'accuracy': sum(rows[k]['predicted_index'] == by_id[k]['target_index'] for k in lat_ids) / len(lat_ids),
                     'generated_tokens': sum(rows[k]['generated_tokens'] for k in lat_ids),
                     'p50_s': _percentile([rows[k]['latency_ms'] / 1000 for k in lat_ids], .5),
                     'p95_s': _percentile([rows[k]['latency_ms'] / 1000 for k in lat_ids], .95)}
               for arm, rows in arms.items()}
    return {'scope': 'geistlib#286: Bonsai 2 27B, 5-shot classic MMLU (geistlib prompts), cap 8192, M1 Max Metal',
            'verdict': 'PASS' if noninferior else 'FAIL',
            'n': n, 'accuracy': {'full': sum(full.values()) / n, 'early_exit': sum(early.values()) / n},
            'gains': gains, 'losses': losses, 'interval_pp': interval, 'margin_pp': c['margin_pp'],
            'noninferior': noninferior, 'early_exits': exits, 'exits_at_first_probe': at_first,
            'capped': {'full': capped, 'early_exit': still_capped}, 'generated_tokens': tokens,
            'live': {'questions': len(lat_ids), 'answers_agree_with_offline': agree,
                     'identical_with_offline': identical, 'full_identical_with_main_run': full_identical,
                     **latency}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--actual', type=Path, required=True)
    parser.add_argument('--plan-17', type=Path, help="#17's plan: check under its #94 contract instead")
    parser.add_argument('--oracle', action='store_true',
                        help='--reference is a tools/llama_oracle.py file: report both contracts (needs --plan-17)')
    args = parser.parse_args()
    if args.oracle:
        rows = [json.loads(line) for line in args.actual.read_text().splitlines() if line.startswith('{')]
        rows = [dict(r, mode=r.get('mode', 1)) for r in rows]
        report = against_oracle(json.loads(args.plan.read_text()), json.loads(args.plan_17.read_text()),
                                json.loads(args.fixture.read_text()), json.loads(args.reference.read_text()), rows)
        print(json.dumps(report, indent=2))
        return 0 if report['original']['verdict'] == 'PASS' else 1
    reference = json.loads(args.reference.read_text())
    require(reference['plan_sha256'] == sha(args.plan), 'changed numerical contract')
    rows = [json.loads(line) for line in args.actual.read_text().splitlines() if line.startswith('{')]
    if args.plan_17:  # the oracle is unchanged; the new plan names the original one it extends
        plan17 = json.loads(args.plan_17.read_text())
        require(plan17['extends']['plan_sha256'] == sha(args.plan), 'changed numerical contract')
        report = numerical_94(plan17, json.loads(args.fixture.read_text()), reference, rows)
        report['actual_sha256'] = sha(args.actual)
        report['plan_17_sha256'] = sha(args.plan_17)
    else:
        require(reference['actual_sha256'] == sha(args.actual), 'changed actual raw outputs')
        report = numerical(json.loads(args.plan.read_text()), json.loads(args.fixture.read_text()), reference, rows)
    print(json.dumps(report, indent=2))
    return 0 if report['verdict'] == 'PASS' else 1
if __name__ == '__main__':
    raise SystemExit(main())

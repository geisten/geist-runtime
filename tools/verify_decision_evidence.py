"""Verify checksum-bound numerical development evidence; no quality inference."""
import argparse
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

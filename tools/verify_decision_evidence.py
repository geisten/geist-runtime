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

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--actual', type=Path, required=True)
    args = parser.parse_args()
    reference = json.loads(args.reference.read_text())
    require(reference['plan_sha256'] == sha(args.plan), 'changed numerical contract')
    require(reference['actual_sha256'] == sha(args.actual), 'changed actual raw outputs')
    rows = [json.loads(line) for line in args.actual.read_text().splitlines() if line.startswith('{')]
    report = numerical(json.loads(args.plan.read_text()), json.loads(args.fixture.read_text()), reference, rows)
    print(json.dumps(report, indent=2))
    return 0 if report['verdict'] == 'PASS' else 1
if __name__ == '__main__':
    raise SystemExit(main())

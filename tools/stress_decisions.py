"""Repeatable lifecycle fixture. Requires frozen numeric budgets before running.
No hidden default budget or PASS with missing Apple observations. Invocation:
  <installed-wheel-python> tools/stress_decisions.py --model <gguf> --plan <json>
Plan fields: fixture, fixture_sha256, artifact_sha256, profile, processor, mode,
requests (>=100), cycles (>=3), rss_max_bytes, rss_cleanup_max_bytes,
metal_max_bytes, metal_cleanup_max_bytes (required for GPU), estimate_seconds.
RSS (ps) and scoped provider bytes are reported separately; no unique-residency
or quality inference is made. Run a single model with no concurrent benchmark.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

import geistr


def rss():
    result = subprocess.run(['ps', '-o', 'rss=', '-p', str(os.getpid())],
                            capture_output=True, text=True, check=True)
    return int(result.stdout.strip()) * 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--model', required=True)
    ap.add_argument('--plan', type=Path, required=True)
    args = ap.parse_args()
    raw_plan = args.plan.read_bytes()
    plan = json.loads(raw_plan)
    assert plan['requests'] >= 100 and plan['cycles'] >= 3 and plan['estimate_seconds'] > 0
    for key in ['rss_max_bytes', 'rss_cleanup_max_bytes'] + (
        ['metal_max_bytes', 'metal_cleanup_max_bytes'] if plan['processor'] == 'gpu' else []):
        assert isinstance(plan[key], int) and plan[key] > 0, 'numeric budget required: ' + key
    raw_fixture = Path(plan['fixture']).read_bytes()
    assert hashlib.sha256(raw_fixture).hexdigest() == plan['fixture_sha256']
    fixture = json.loads(raw_fixture)
    assert fixture['artifact_sha256'] == plan['artifact_sha256']
    largest = max(fixture['cases'], key=lambda case: len(case['ids']))
    config = json.dumps({'schema': 1, 'models': [{'sha256': plan['artifact_sha256'],
        'enabled': True, 'profile': plan['profile'], 'mode': plan['mode']}]}).encode()
    records, started = [], time.monotonic()
    for cycle in range(plan['cycles']):
        with geistr.DecisionConfig(config) as cfg:
            with geistr.open(args.model, decision_config=cfg, processor=plan['processor'],
                             threads=6, context=512) as model:
                with model.chat(max_tokens=1) as chat:
                    with model.decision() as primary, model.decision() as secondary:
                        # Largest frozen geometry, both private states, chat and failure paths.
                        expected = primary.score(largest['question'], largest['options'], context=largest['context'])
                        assert secondary.score(largest['question'], largest['options'], context=largest['context']).logits == expected.logits
                        assert chat.ask('Reply OK.'); chat.rewind(0)
                        primary.cancel()
                        try:
                            primary.score('Q', [('x', 'x')]); raise AssertionError('cancellation failed')
                        except geistr.GeistrError as exc:
                            assert exc.status == 'cancelled'
                        primary.reset()
                        baseline = model.decision_resources
                        assert baseline['live_allocations'] == 8
                        for index in range(plan['requests']):
                            if index % 10 == 0:
                                assert chat.ask('Reply OK.'); chat.rewind(0)
                                try:
                                    primary.score('Q', [('same', 'a'), ('same', 'b')])
                                    raise AssertionError('invalid request succeeded')
                                except geistr.GeistrError as exc:
                                    assert exc.status == 'invalid'
                                primary.cancel()
                                try:
                                    primary.score('Q', [('x', 'a')]); raise AssertionError('cancelled request succeeded')
                                except geistr.GeistrError as exc:
                                    assert exc.status == 'cancelled'
                            primary.reset()
                            case = fixture['cases'][index % len(fixture['cases'])]
                            result = primary.score(case['question'], case['options'], context=case['context'])
                            assert result.model_calls == 1 and list(result.prompt_ids) == case['ids']
                            if case is largest:
                                assert result.logits == expected.logits
                            observed = model.decision_resources
                            assert observed['live_allocations'] == baseline['live_allocations']
                            assert observed['live_bytes'] == baseline['live_bytes']
                            resident = rss()
                            assert resident <= plan['rss_max_bytes']
                            if plan['processor'] == 'gpu':
                                assert observed['provider_known']
                                assert observed['provider_allocated_bytes'] <= plan['metal_max_bytes']
                            records.append({'cycle': cycle, 'index': index, 'rss_bytes': resident,
                                            **observed, 'model_calls': result.model_calls})
                    cleanup = model.decision_resources
                    assert cleanup['live_allocations'] == cleanup['live_bytes'] == 0
                    resident = rss()
                    assert resident <= plan['rss_cleanup_max_bytes']
                    if plan['processor'] == 'gpu':
                        assert cleanup['provider_known']
                        assert cleanup['provider_allocated_bytes'] <= plan['metal_cleanup_max_bytes']
                    records.append({'cycle': cycle, 'phase': 'decision_cleanup', 'rss_bytes': resident, **cleanup})
    print(json.dumps({'schema': 1, 'scope': 'lifecycle only; no numeric/quality eligibility',
        'plan_sha256': hashlib.sha256(raw_plan).hexdigest(), 'elapsed_seconds': time.monotonic() - started,
        'verdict': 'PASS', 'records': records}, indent=2))


if __name__ == '__main__':
    main()

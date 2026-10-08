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

def require(condition, message):
    if not condition:
        raise ValueError(message)

def rss():
    result = subprocess.run(['ps', '-o', 'rss=', '-p', str(os.getpid())], capture_output=True, text=True, check=True)
    return int(result.stdout.strip()) * 1024

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--model', required=True)
    ap.add_argument('--plan', type=Path, required=True)
    args = ap.parse_args()
    raw_plan = args.plan.read_bytes()
    plan = json.loads(raw_plan)
    require(plan['requests'] >= 100 and plan['cycles'] >= 3 and (plan['estimate_seconds'] > 0), 'evidence invariant failed')
    for key in ['rss_max_bytes', 'rss_cleanup_max_bytes'] + (['metal_max_bytes', 'metal_cleanup_max_bytes'] if plan['processor'] == 'gpu' else []):
        require(isinstance(plan[key], int) and plan[key] > 0, 'numeric budget required: ' + key)
    raw_fixture = Path(plan['fixture']).read_bytes()
    require(hashlib.sha256(raw_fixture).hexdigest() == plan['fixture_sha256'], 'evidence invariant failed')
    fixture = json.loads(raw_fixture)
    require(fixture['artifact_sha256'] == plan['artifact_sha256'], 'evidence invariant failed')
    largest = max(fixture['cases'], key=lambda case: len(case['ids']))
    config = json.dumps({'schema': 1, 'models': [{'sha256': plan['artifact_sha256'], 'enabled': True, 'profile': plan['profile'], 'mode': plan['mode']}]}).encode()
    records, started = ([], time.monotonic())
    for cycle in range(plan['cycles']):
        with geistr.DecisionConfig(config) as cfg:
            with geistr.open(args.model, decision_config=cfg, processor=plan['processor'], threads=6, context=512) as model:
                with model.chat(max_tokens=1) as chat:
                    with model.decision() as primary, model.decision() as secondary:
                        expected = primary.score(largest['question'], largest['options'], context=largest['context'])
                        require(secondary.score(largest['question'], largest['options'], context=largest['context']).logits == expected.logits, 'evidence invariant failed')
                        require(chat.ask('Reply OK.'), 'evidence invariant failed')
                        chat.rewind(0)
                        primary.cancel()
                        try:
                            primary.score('Q', [('x', 'x')])
                            raise AssertionError('cancellation failed')
                        except geistr.GeistrError as exc:
                            require(exc.status == 'cancelled', 'evidence invariant failed')
                        primary.reset()
                        baseline = model.decision_resources
                        require(baseline['live_allocations'] == 8, 'evidence invariant failed')
                        for index in range(plan['requests']):
                            if index % 10 == 0:
                                require(chat.ask('Reply OK.'), 'evidence invariant failed')
                                chat.rewind(0)
                                try:
                                    primary.score('Q', [('same', 'a'), ('same', 'b')])
                                    raise AssertionError('invalid request succeeded')
                                except geistr.GeistrError as exc:
                                    require(exc.status == 'invalid', 'evidence invariant failed')
                                primary.cancel()
                                try:
                                    primary.score('Q', [('x', 'a')])
                                    raise AssertionError('cancelled request succeeded')
                                except geistr.GeistrError as exc:
                                    require(exc.status == 'cancelled', 'evidence invariant failed')
                            primary.reset()
                            case = fixture['cases'][index % len(fixture['cases'])]
                            result = primary.score(case['question'], case['options'], context=case['context'])
                            require(result.model_calls == 1 and list(result.prompt_ids) == case['ids'], 'evidence invariant failed')
                            if case is largest:
                                require(result.logits == expected.logits, 'evidence invariant failed')
                            observed = model.decision_resources
                            require(observed['live_allocations'] == baseline['live_allocations'], 'evidence invariant failed')
                            require(observed['live_bytes'] == baseline['live_bytes'], 'evidence invariant failed')
                            resident = rss()
                            require(resident <= plan['rss_max_bytes'], 'evidence invariant failed')
                            if plan['processor'] == 'gpu':
                                require(observed['provider_known'], 'evidence invariant failed')
                                require(observed['provider_allocated_bytes'] <= plan['metal_max_bytes'], 'evidence invariant failed')
                            records.append({'cycle': cycle, 'index': index, 'rss_bytes': resident, **observed, 'model_calls': result.model_calls})
                    cleanup = model.decision_resources
                    require(cleanup['live_allocations'] == cleanup['live_bytes'] == 0, 'evidence invariant failed')
                    resident = rss()
                    require(resident <= plan['rss_cleanup_max_bytes'], 'evidence invariant failed')
                    if plan['processor'] == 'gpu':
                        require(cleanup['provider_known'], 'evidence invariant failed')
                        require(cleanup['provider_allocated_bytes'] <= plan['metal_cleanup_max_bytes'], 'evidence invariant failed')
                    records.append({'cycle': cycle, 'phase': 'decision_cleanup', 'rss_bytes': resident, **cleanup})
    print(json.dumps({'schema': 1, 'scope': 'lifecycle only; no numeric/quality eligibility', 'plan_sha256': hashlib.sha256(raw_plan).hexdigest(), 'elapsed_seconds': time.monotonic() - started, 'verdict': 'PASS', 'records': records}, indent=2))
if __name__ == '__main__':
    main()

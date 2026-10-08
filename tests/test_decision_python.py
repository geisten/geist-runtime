"""Installed/source bindings against one exact profile; no quality claim."""
import ctypes
import json
from pathlib import Path
import subprocess
import sys
import threading

import geistr
from geistr import decision as bindings

model, profile, processor, abi = sys.argv[1:]
fixture = json.loads((Path(__file__).parent / 'fixtures/decisions' / (profile + '.json')).read_text())
profile_name = {'gemma4': 'gemma4-e2b-q4-v1', 'bonsai2': 'bonsai2-27b-pq2-v1'}[profile]
config = {'schema': 1, 'models': [{'sha256': fixture['artifact_sha256'], 'enabled': True, 'profile': profile_name}]}
sizes = dict(line.split() for line in subprocess.run([abi], text=True, capture_output=True, check=True).stdout.splitlines())
for name, cls in [('policy', bindings._Policy), ('opts', bindings._Opts), ('option', bindings._Option),
                  ('request', bindings._Request), ('result', bindings._Result), ('plan', bindings._Plan),
                  ('capability', bindings._Capability), ('resources', bindings._Resources)]:
    assert int(sizes['geistr_decision_' + name]) == ctypes.sizeof(cls), name
with geistr.DecisionConfig(json.dumps(config).encode()) as cfg:
    m = geistr.open(model, processor=processor, context=512, threads=6, decision_config=cfg)
cfg.close()  # permission copied, not borrowed from config
assert m.decision_capability['configured'] and m.decision_capability['available']
assert m.decision_resources['live_allocations'] == 0
with m.decision() as d, m.decision() as other:
    baseline = m.decision_resources
    assert baseline['live_allocations'] == 8 and baseline['live_bytes'] > 0
    results = []
    for f in fixture['cases']:
        out = d.score(f['question'], f['options'], context=f['context'])
        assert out.external_id == f['options'][out.best_index][0]
        assert list(out.prompt_ids) == f['ids'] and list(out.candidate_ids) == f['candidate_ids']
        assert out.model_calls == 1 and len(out.logits) == len(f['options'])
        results.append(out)
    f = fixture['cases'][0]
    assert other.score(f['question'], f['options']).logits == results[0].logits
    for opts in [[('x', 'a'), ('x', 'b')], [('bad\nID', 'a')], [('x', '')], [('x' * 129, 'a')]]:
        try:
            d.score('Q', opts)
            raise AssertionError('invalid input succeeded')
        except geistr.GeistrError as exc:
            assert exc.status == 'invalid', exc
    thread = threading.Thread(target=d.cancel)
    thread.start(); thread.join()
    try:
        d.score(f['question'], f['options'])
        raise AssertionError('cancelled request succeeded')
    except geistr.GeistrError as exc:
        assert exc.status == 'cancelled'
    assert d.score(f['question'], f['options']).logits == results[0].logits
    d.reset()
    assert results[0].external_id and results[0].logits  # owned copies survived all operations
    assert m.decision_resources['live_allocations'] == baseline['live_allocations']
assert m.decision_resources['live_allocations'] == m.decision_resources['live_bytes'] == 0
child = m.decision()
m.close()  # engine retains the model for its independent decision state
assert child.score(f['question'], f['options']).logits == results[0].logits
child.close()
print(json.dumps({'python_profile': profile, 'backend': processor, 'owned_results': True,
                  'lifetime_cancel_recovery': True, 'bindings_layout': True}))

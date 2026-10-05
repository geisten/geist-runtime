"""The evidence verifier must reject known drift and tampered raw outputs."""
import copy
import importlib.util
import json
from pathlib import Path

root = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('evidence', root / 'tools/verify_decision_evidence.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
plan = json.loads((root / 'docs/DECISION_EVIDENCE_PLAN.json').read_text())
fixture = json.loads((root / 'tests/fixtures/decisions/gemma4.json').read_text())
reference = json.loads((root / 'tests/fixtures/decisions/gemma4_numeric.json').read_text())
rows = [json.loads(line) for line in (root / 'tests/fixtures/decisions/gemma4_runtime_cpu.jsonl').read_text().splitlines()
        if line.startswith('{')]
assert module.numerical(plan, fixture, reference, rows)['verdict'] == 'FAIL', 'known independent native drift hidden'
correct = [{'mode': 1, 'fixture': index, **record} for index, record in enumerate(reference['outputs'])]
assert module.numerical(plan, fixture, reference, correct)['verdict'] == 'PASS'
corrupt = copy.deepcopy(correct)
corrupt[0]['logits'][0] = float('nan')
try:
    module.numerical(plan, fixture, reference, corrupt)
    raise RuntimeError('accepted non-finite score')
except AssertionError:
    pass
truncated = correct[:-1]
try:
    module.numerical(plan, fixture, reference, truncated)
    raise RuntimeError('accepted incomplete population')
except AssertionError:
    pass
print('evidence verifier: independent Gemma drift FAIL preserved; finite complete output required')

"""The evidence verifier must reject known drift and tampered raw outputs."""
import copy
import hashlib
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
except ValueError:
    pass
truncated = correct[:-1]
try:
    module.numerical(plan, fixture, reference, truncated)
    raise RuntimeError('accepted incomplete population')
except ValueError:
    pass
# #94: the x86 record on the current pin: bound to its reference copy, still FAIL (a linked cause), and the
# manifest's hashes are those of the files
validation = root / 'tests/fixtures/decisions/validation'
x86 = json.loads((root / 'tests/fixtures/decisions/gemma4_numeric_x86.json').read_text())
x86_rows = [json.loads(line) for line in (validation / 'gemma4-cpu-x86.jsonl').read_text().splitlines() if line.startswith('{')]
assert x86['outputs'] == reference['outputs'] and x86['actual_sha256'] == hashlib.sha256((validation / 'gemma4-cpu-x86.jsonl').read_bytes()).hexdigest()
report = module.numerical(plan, fixture, x86, x86_rows)
manifest = json.loads((validation / 'manifest.json').read_text())
assert report['verdict'] == manifest['x86_cpu']['independent_gemma_numeric_verdict'] == 'FAIL'
assert [c['selection'] for c in report['cases']] == list(manifest['x86_cpu']['independent_gemma_numeric']['selection'].values())
# #17: the Apple records on the current pin: every raw file's hash, and the numeric FAIL under both contracts
plan17 = json.loads((root / 'docs/DECISION_EVIDENCE_PLAN_17.json').read_text())
assert manifest['apple_17']['plan_sha256'] == hashlib.sha256((root / 'docs/DECISION_EVIDENCE_PLAN_17.json').read_bytes()).hexdigest()
for name in ('records-gemma4-cpu', 'records-gemma4-gpu'):
    apple = [json.loads(line) for line in (validation / 'apple-17' / (name + '.jsonl')).read_text().splitlines() if line.startswith('{')]
    assert module.numerical(plan, fixture, reference, apple)['verdict'] == 'FAIL', name
    assert module.numerical_94(plan17, fixture, reference, apple)['verdict'] == 'FAIL', name
# The oracle is the latest llama.cpp release (decision 2026-10-09): bound to the fixture and artifact,
# and its verdicts are what the manifest says
oracle_path = root / 'tests/fixtures/decisions/gemma4_oracle_llamacpp.json'
oracle = json.loads(oracle_path.read_text())
latest = manifest['apple_17']['independent_gemma_numeric_latest_llamacpp']
assert latest['oracle_sha256'] == hashlib.sha256(oracle_path.read_bytes()).hexdigest()
assert oracle['token_fixture_sha256'] == hashlib.sha256((root / 'tests/fixtures/decisions/gemma4.json').read_bytes()).hexdigest()
for name in ('records-gemma4-cpu', 'records-gemma4-gpu'):
    apple = [json.loads(line) for line in (validation / 'apple-17' / (name + '.jsonl')).read_text().splitlines() if line.startswith('{')]
    both = module.against_oracle(plan, plan17, fixture, oracle, apple)
    assert both['original']['verdict'] == latest['verdicts'][name]['original'], name
    assert both['94']['verdict'] == latest['verdicts'][name]['contract_94'], name
# The contract in force is #94 (decision 2026-10-09): Gemma on engine 12f77e8 against the latest oracle
assert manifest['apple_17']['contract_in_force'] == '94'
for proc in ('cpu', 'gpu'):
    current = [json.loads(line) for line in (validation / 'apple-17/contract-94-12f77e8' / f'records-gemma4-{proc}.jsonl').read_text().splitlines()
               if line.startswith('{')]
    assert module.against_oracle(plan, plan17, fixture, oracle, current)['94']['verdict'] == 'PASS', proc
# #94 on the current pin d53560d: x86 (the self-hosted runner) and Apple, #94 contract, latest oracle
for name in ('x86-d53560d/records-gemma4-cpu-x86.jsonl', 'apple-17/contract-94-d53560d/records-gemma4-cpu.jsonl',
             'apple-17/contract-94-d53560d/records-gemma4-gpu.jsonl'):
    rows94 = [json.loads(line) for line in (validation / name).read_text().splitlines() if line.startswith('{')]
    assert module.against_oracle(plan, plan17, fixture, oracle, rows94)['94']['verdict'] == 'PASS', name
for section in (manifest, manifest['x86_cpu'], manifest['apple_17'], manifest['x86_cpu_d53560d']):
    for name, record in section['records'].items():
        assert hashlib.sha256((validation / name).read_bytes()).hexdigest() == record['sha256'], name
print('evidence verifier: independent Gemma drift FAIL preserved (Apple NEON, Metal and x86 records, both contracts); finite complete output required; manifest hashes match')

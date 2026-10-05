#!/usr/bin/env python3
"""The catalog refuses what geist-serve refuses: its invalid-input cases
(geist-serve tests/app/catalog_test.py), run through test_catalog --validate."""
import copy, json, subprocess, sys

tool, catalog = sys.argv[1], sys.argv[2]
base = json.load(open(catalog))

def accepts(doc):
    text = doc if isinstance(doc, str) else json.dumps(doc)
    return subprocess.run([tool, '--validate'], input=text.encode(), capture_output=True).returncode == 0

assert accepts(base), 'shipped catalog refused'
good = copy.deepcopy(base); good['revision'] += 1
good['models'].append({**good['models'][1], 'id': 'another-small-model', 'name': 'Another small model', 'file': 'another.gguf', 'group_id': 'another-small-model', 'group_name': 'Another small model'})
evidence = {'suite': '0123456789ab', 'date': '2026-10-03', 'engine': '33db79d7764b', 'evidence': 'a'*64, 'tasks': {'classify': {'de': [19, 20], 'en': [0, 20]}, 'context': {'de': [20, 20], 'en': [18, 20]}}}
ref = {'platform': 'Apple M1 Max', 'backend': 'gpu', 'answer_ms': 3200, 'tokens_per_s': 72, 'memory_mib': 1200, 'date': '2026-10-04', 'engine': '33db79d7764b'}
good['models'][-1]['quality'] = evidence
good['models'][-1]['reference'] = [ref]
assert accepts(good), 'extended catalog refused'

invalid = []
for key, value in [('schema', 3), ('revision', 0), ('models', []), ('extra', True)]:
    bad = copy.deepcopy(good); bad[key] = value; invalid.append(bad)
for key, value in [('id', '../escape'), ('file', '../escape.gguf'), ('file', 'model.sh'), ('url', 'https://evil.example/model.gguf'), ('sha256', '0'*63), ('backends', ['metal']), ('backends', []), ('unsupported_format', 'unknown'), ('unsupported_format', 'pq2_0'), ('backends', ['cpu', 'cpu']), ('backends', ['cpu\x00suffix']), ('backends', ['cpu', 'metal\x00suffix']), ('bytes', -1), ('bytes', 1.5), ('name', 'bad\nname'), ('group_id', '../bad'), ('group_id', 'custom'), ('group_name', ''), ('quantization', ''), ('quantization', 'Q4/0'), ('reasoning_format', 'guess'), ('reasoning_format', None), ('reasoning_format', 'think_tags\x00suffix'), ('id', 'custom'), ('url', 'https://huggingface.co/a/b/resolve/x/../y.gguf'), ('url', 'https://huggingface.co/a/b/resolve/main/m.gguf?x=1'), ('sha256', 'A'*64)]:
    bad = copy.deepcopy(good); bad['models'][0][key] = value; invalid.append(bad)
bad = copy.deepcopy(good); bad['models'].append(bad['models'][0]); invalid.append(bad)
for change in [lambda q: q.update(suite='0123'), lambda q: q.update(suite='0123456789AB'), lambda q: q.update(date='03.10.2026'), lambda q: q.update(engine='a b'),
               lambda q: q.update(evidence='a'*63), lambda q: q.update(extra=1), lambda q: q.pop('tasks'), lambda q: q.update(tasks={}), lambda q: q.update(tasks=[]),
               lambda q: q['tasks'].update(classify={'de': [21, 20], 'en': [0, 20]}), lambda q: q['tasks'].update(classify={'de': [1, 0], 'en': [0, 20]}),
               lambda q: q['tasks'].update(classify={'de': [-1, 20], 'en': [0, 20]}), lambda q: q['tasks'].update(classify={'de': [1.5, 20], 'en': [0, 20]}),
               lambda q: q['tasks'].update(classify={'de': [1, 20, 3], 'en': [0, 20]}), lambda q: q['tasks'].update(classify={'de': [1, 20]}),
               lambda q: q['tasks'].update(classify={'de': ['1', 20], 'en': [0, 20]}), lambda q: q['tasks'].update({'../x': {'de': [1, 20], 'en': [0, 20]}})]:
    bad = copy.deepcopy(good); change(bad['models'][-1]['quality']); invalid.append(bad)
bad = copy.deepcopy(good); bad['models'][-1]['quality'] = '146/160'; invalid.append(bad)
for change in [lambda r: r.update(backend='metal'), lambda r: r.update(answer_ms=0), lambda r: r.update(answer_ms=1.5), lambda r: r.update(tokens_per_s=-1),
               lambda r: r.update(memory_mib='1200'), lambda r: r.update(date='4.10.2026'), lambda r: r.update(engine='a b'), lambda r: r.update(platform=''),
               lambda r: r.update(platform='x'*65), lambda r: r.pop('engine'), lambda r: r.update(extra=1)]:
    bad = copy.deepcopy(good); change(bad['models'][-1]['reference'][0]); invalid.append(bad)
for value in [[], [ref]*5, ref, 'fast']:
    bad = copy.deepcopy(good); bad['models'][-1]['reference'] = value; invalid.append(bad)
for field in ['group_id', 'group_name', 'quantization']:
    bad = copy.deepcopy(good); del bad['models'][0][field]; invalid.append(bad)
for field, value in [('group_name', 'Conflicting name'), ('quantization', 'Q4_0')]:
    bad = copy.deepcopy(good); bad['models'][-2][field] = value; invalid.append(bad)
# Schema 1 carries no grouping metadata.
bad = copy.deepcopy(good); bad['schema'] = 1; invalid.append(bad)
legacy = copy.deepcopy(good); legacy['schema'] = 1
for m in legacy['models']:
    for field in ['group_id', 'group_name', 'quantization']:
        m.pop(field, None)
assert accepts(legacy), 'schema 1 refused'

for bad in invalid:
    assert not accepts(bad), bad
text = json.dumps(good)
for bad in [text + 'x', text + '{}', text[:-1], '[' + text + ']', '', text.replace('"schema"', '"sch\\u0065ma"')]:
    assert not accepts(bad), bad[-40:]
assert accepts(text + '\n')
print(f'test_catalog.py: {len(invalid) + 6} invalid catalogs refused')

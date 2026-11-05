"""Extract geistlib#286's raw records for the evidence verifier (docs/DECISION_EVIDENCE_PLAN_286.json).

  decision_evidence_286.py CAMPAIGN_DIR OUT_DIR

CAMPAIGN_DIR is the finished campaign (main/, latency/, latency-full/ with
report.json, cases-test/test.jsonl, latency_ids.json). OUT_DIR receives
records.jsonl.gz (the main run: per question its target, its full-reasoning
outcome and every recorded probe) and live.jsonl.gz (the two live latency
arms on the 300). Nothing is computed here; tools/verify_decision_evidence.py
early_exit() reads only these files. Standard library only."""
import gzip
import json
import sys
from pathlib import Path


def write(path, rows):
    # mtime 0: the same records give the same bytes, so the manifest hash is reproducible
    with open(path, 'wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', mtime=0, filename='') as out:
        for row in rows:
            out.write((json.dumps(row, separators=(',', ':')) + '\n').encode())


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    campaign, out = Path(sys.argv[1]), Path(sys.argv[2])
    cases = [json.loads(line) for line in (campaign / 'cases-test/test.jsonl').read_text().split('\n') if line]
    # split on newline only: questions contain Unicode line breaks (geistlib 4da9fdb2)
    main_run = json.loads((campaign / 'main/report.json').read_text())['summary']['per_question']
    if len(cases) != len(main_run) or {c['question_id'] for c in cases} != set(main_run):
        raise SystemExit('main run and cases differ')
    out.mkdir(parents=True, exist_ok=True)
    write(out / 'records.jsonl.gz', (
        {'question_id': c['question_id'], 'subject': c['subject'], 'target_index': c['target_index'],
         'stop': q['stop'], 'valid': q['valid'], 'predicted_index': q['predicted_index'] if q['valid'] else None,
         'generated_tokens': q['generated_tokens'], 'probes': [[p['pos'], *p['logits']] for p in q['probes']]}
        for c in cases for q in [main_run[c['question_id']]]))
    ids = json.loads((campaign / 'latency_ids.json').read_text())
    live = []
    for arm, name in (('early_exit', 'latency'), ('full', 'latency-full')):
        report = json.loads((campaign / name / 'report.json').read_text())['summary']['per_question']
        if set(report) != set(ids):
            raise SystemExit(f'{name}: not the 300')
        live += [{'arm': arm, 'question_id': k, 'stop': report[k]['stop'],
                  'predicted_index': report[k]['predicted_index'] if report[k]['valid'] else None,
                  'generated_tokens': report[k]['generated_tokens'], 'latency_ms': report[k]['latency_ms'],
                  'probe_ms': round(sum(report[k]['probe_ms']), 3)} for k in ids]
    write(out / 'live.jsonl.gz', live)


if __name__ == '__main__':
    main()

"""Offline native template/tokenizer fixtures; no inference/quality claim.

Requires installed gguf/transformers and a pinned independently built
reference_tokenize. The metadata-only GGUF reader skips tensor construction
so Prism's PQ2_0 tag is not mistaken for a supported stock dequantizer.
"""
import argparse
import hashlib
from importlib.metadata import version
import json
from pathlib import Path
import subprocess

from gguf import GGUFReader
from transformers.utils.chat_template_utils import _compile_jinja_template


class MetadataReader(GGUFReader):
    def _build_tensors(self, *_args):
        pass  # metadata only; never decodes or claims support for tensors


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as src:
        for block in iter(lambda: src.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def literal(text):
    return '"' + ''.join(chr(b) if 32 <= b < 127 and b not in (34, 92)
                         else f'\\{b:03o}' for b in text.encode('utf-8')) + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--model', type=Path, required=True)
    ap.add_argument('--sha256', required=True)
    ap.add_argument('--profile', choices=['bonsai2', 'gemma4'], required=True)
    ap.add_argument('--reference', type=Path, required=True)
    ap.add_argument('--reference-revision', required=True)
    ap.add_argument('--reference-root', type=Path, required=True)
    ap.add_argument('--reference-libraries', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    if digest(args.model) != args.sha256:
        raise ValueError('model checksum mismatch')
    if len(args.reference_revision) != 40 or any(c not in '0123456789abcdef' for c in args.reference_revision):
        raise ValueError('reference revision must be an exact commit')
    actual_revision = subprocess.run(['git', '-C', str(args.reference_root), 'rev-parse', 'HEAD'],
        capture_output=True, text=True, check=True).stdout.strip()
    clean = subprocess.run(['git', '-C', str(args.reference_root), 'diff', '--quiet'], check=False)
    if actual_revision != args.reference_revision or clean.returncode:
        raise ValueError('reference source is not the pinned clean checkout')
    linked = sorted({p.resolve() for p in args.reference_libraries.glob('*.dylib')})
    if not linked:
        linked = sorted({p.resolve() for p in args.reference_libraries.glob('*.so*')})
    if not linked:
        raise ValueError('missing independent reference libraries')
    native_version = subprocess.run([str(args.reference_libraries / 'llama-cli'), '--version'],
        capture_output=True, text=True, check=True)
    native_build = (native_version.stdout + native_version.stderr).strip()
    if args.reference_revision[:9] not in native_build:
        raise ValueError('reference build identity does not match pinned source')
    r = MetadataReader(str(args.model))
    template = r.fields['tokenizer.chat_template'].contents()
    compiled = _compile_jinja_template(template)
    system = 'Select exactly one supplied option. Answer only with its key. Do not explain.'
    cases = [dict(id='plain', question='Which option equals 4?', context='',
                  options=[['four', '4'], ['five', '5']]),
             dict(id='unicode', question='  Welche Farbe?\n🙂  ', context='Zeile 1\nZeile 2',
                  options=[['röt', ' rot  '], ['blau', 'blau\nhell']]),
             dict(id='order', question='Choose the even number.', context='',
                  options=[['five', '5'], ['two', '2'], ['three', '3'], ['four', '4']])]
    prompts = []
    for case in cases:
        body = ('Context:\n' + case['context'] + '\n') if case['context'] else ''
        body += 'Question:\n' + case['question'] + '\nOptions:\n'
        body += ''.join(f'{chr(65+i)} [{oid}]: {description}\n'
                        for i, (oid, description) in enumerate(case['options']))
        body += 'Choose one key.'
        messages = [{'role': 'system', 'content': system}, {'role': 'user', 'content': body}]
        case['prompt'] = compiled.render(messages=messages, bos_token='<bos>' if args.profile == 'gemma4' else '',
            add_generation_prompt=True, enable_thinking=False, tools=None)
        thinking = compiled.render(messages=messages, bos_token='<bos>' if args.profile == 'gemma4' else '',
            add_generation_prompt=True, enable_thinking=True, tools=None)
        if thinking == case['prompt']:
            raise ValueError('thinking positive/negative control did not change native rendering')
        case['thinking_control_sha256'] = hashlib.sha256(thinking.encode()).hexdigest()
        prompts += [case['prompt']] + [case['prompt'] + chr(65+i) for i in range(len(case['options']))]
    response = subprocess.run([str(args.reference), str(args.model)],
        input=''.join(p.encode().hex() + '\n' for p in prompts), text=True, capture_output=True, timeout=120)
    if response.returncode:
        raise RuntimeError(response.stderr[-2000:])
    answers = [json.loads(line) for line in response.stdout.splitlines()]
    if len(answers) != len(prompts):
        raise ValueError('incomplete native tokenizer answers')
    offset = 0
    for case in cases:
        case['ids'] = answers[offset]
        labels = answers[offset + 1:offset + 1 + len(case['options'])]
        if any(label[:-1] != case['ids'] or len(label) != len(case['ids']) + 1 for label in labels):
            raise ValueError('native complete-prefix/single-token condition failed')
        case['candidate_ids'] = [label[-1] for label in labels]
        if len(set(case['candidate_ids'])) != len(labels):
            raise ValueError('duplicate native candidate IDs')
        offset += 1 + len(case['options'])
    manifest = {'schema': 1, 'artifact_sha256': args.sha256, 'profile': args.profile,
                'template_sha256': hashlib.sha256(template.encode()).hexdigest(),
                'reference_revision': args.reference_revision, 'reference_binary_sha256': digest(args.reference),
                'reference_source_sha256': digest(Path(__file__).with_name('reference_tokenize.cpp')),
                'reference_build_version': native_build,
                'reference_library_sha256': {p.name: digest(p) for p in linked},
                'command': ['python3', 'tools/prepare_profile_oracles.py', '--model', '<artifact>',
                    '--sha256', args.sha256, '--profile', args.profile, '--reference', '<reference_tokenize>',
                    '--reference-root', '<pinned-checkout>', '--reference-libraries', '<pinned-build/bin>',
                    '--reference-revision', args.reference_revision, '--out', 'tests/fixtures/decisions'],
                'libraries': {n: version(n) for n in ['gguf', 'transformers', 'jinja2']},
                'tokenization': {'add_special': False, 'parse_special': True},
                'scope': 'independent native template/tokenizer fixtures; no numerical or quality evidence',
                'cases': cases}
    args.out.mkdir(parents=True, exist_ok=True)
    target = args.out / (args.profile + '.json')
    target.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    header = ['/* Generated ONLY from the independent native oracle JSON. */']
    for index, case in enumerate(cases):
        stem = f'{args.profile}_{index}'
        header += [f'static const int32_t {stem}_ids[] = {{{",".join(map(str,case["ids"]))}}};',
                   f'static const int32_t {stem}_candidates[] = {{{",".join(map(str,case["candidate_ids"]))}}};',
                   f'static const char {stem}_prompt[] = {literal(case["prompt"])};']
    header += [f'static const struct native_case {args.profile}_cases[] = {{']
    for index, case in enumerate(cases):
        stem = f'{args.profile}_{index}'
        header += ['{'+','.join([literal(case['question']), literal(case['context']),
            f'{len(case["options"])}', '{'+','.join('{'+literal(x)+','+literal(y)+'}' for x,y in case['options'])+'}',
            stem+'_prompt', stem+'_ids', f'sizeof {stem}_ids / sizeof {stem}_ids[0]', stem+'_candidates'])+'},']
    header += ['};']
    (args.out / (args.profile + '.h')).write_text('\n'.join(header) + '\n')
    print(json.dumps({'oracle': str(target), 'cases': len(cases), 'sha256': digest(target)}))


if __name__ == '__main__':
    main()

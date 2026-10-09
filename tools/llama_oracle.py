"""The independent numeric oracle for decisions: the latest upstream llama.cpp release.

  llama_oracle.py --model GGUF --fixture tests/fixtures/decisions/gemma4.json \
                  --out tests/fixtures/decisions/gemma4_oracle_llamacpp.json [--tag vX.Y.Z]

Resolves the latest release of ggml-org/llama.cpp (or --tag), builds it CPU-only
from source in build/llama.cpp/<tag>, compiles tools/reference_score.cpp against
it and scores the fixture's native prompt/candidate IDs (CPU, 6 threads, F32 KV,
no flash attention, last prompt position). The output records the release, its
commit, the binary's hash and the outputs. Decision 2026-10-09: the oracle is
always the latest release, re-pinned before numeric evidence is recorded.
Standard library only; needs git, cmake and a C++ compiler."""
import argparse
import hashlib
import json
import os
import platform
import subprocess
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = 'ggml-org/llama.cpp'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def latest_tag():
    request = urllib.request.Request(f'https://api.github.com/repos/{REPO}/releases/latest',
                                     headers={'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(request, timeout=30) as r:
        return json.load(r)['tag_name']


def run(argv, **kw):
    return subprocess.run([str(a) for a in argv], check=True, **kw)


def build(tag, cache):
    src = cache / tag
    if not (src / '.git').exists():
        run(['git', 'clone', '-q', '--depth', '1', '--branch', tag, f'https://github.com/{REPO}.git', src])
    commit = run(['git', '-C', src, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    out = src / 'build-oracle'
    run(['cmake', '-S', src, '-B', out, '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=OFF',
         '-DGGML_METAL=OFF', '-DGGML_BLAS=OFF', '-DGGML_VULKAN=OFF', '-DGGML_CUDA=OFF', '-DLLAMA_CURL=OFF',
         '-DLLAMA_BUILD_TESTS=OFF', '-DLLAMA_BUILD_EXAMPLES=OFF', '-DLLAMA_BUILD_SERVER=OFF', '-DLLAMA_BUILD_TOOLS=OFF'],
        stdout=subprocess.DEVNULL)
    run(['cmake', '--build', out, '--target', 'llama', '-j', str(os.cpu_count() or 4)], stdout=subprocess.DEVNULL)
    libs = sorted(str(p) for p in out.rglob('*.a'))
    binary = out / 'reference_score'
    omp = ['-L/opt/homebrew/opt/libomp/lib', '-lomp'] if platform.system() == 'Darwin' else ['-fopenmp']
    frameworks = ['-framework', 'Accelerate', '-framework', 'Foundation'] if platform.system() == 'Darwin' else ['-lpthread']
    run(['c++', '-std=c++17', '-O2', '-I', src / 'include', '-I', src / 'ggml/include', ROOT / 'tools/reference_score.cpp',
         '-Wl,-all_load' if platform.system() == 'Darwin' else '-Wl,--whole-archive', *libs,
         *([] if platform.system() == 'Darwin' else ['-Wl,--no-whole-archive']), *omp, *frameworks, '-o', binary])
    return src, commit, binary


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--model', type=Path, required=True)
    p.add_argument('--fixture', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--tag', help='a release tag; default: the latest release')
    p.add_argument('--cache', type=Path, default=ROOT / 'build/llama.cpp')
    a = p.parse_args()
    tag = a.tag or latest_tag()
    fixture = json.loads(a.fixture.read_text())
    if sha(a.model) != fixture['artifact_sha256']:
        raise SystemExit('the model is not the fixture\'s artifact')
    src, commit, binary = build(tag, a.cache)
    cases = ''.join(f"{len(c['ids'])} {len(c['candidate_ids'])} {' '.join(map(str, c['ids'] + c['candidate_ids']))}\n"
                    for c in fixture['cases'])
    r = run([binary, a.model], input=cases, capture_output=True, text=True)
    outputs = [json.loads(l) for l in r.stdout.splitlines() if l.startswith('{')]
    if len(outputs) != len(fixture['cases']):
        raise SystemExit(f'expected {len(fixture["cases"])} outputs, got {len(outputs)}: {r.stderr[-500:]}')
    result = {
        'schema': 1,
        'scope': 'independent numerical oracle for the decision fixtures; not MMLU quality',
        'policy': 'the latest upstream llama.cpp release at recording time (decision 2026-10-09)',
        'artifact_sha256': fixture['artifact_sha256'],
        'token_fixture_sha256': sha(a.fixture),
        'reference': {'project': REPO, 'tag': tag, 'commit': commit, 'binary_sha256': sha(binary),
                      'source_sha256': sha(ROOT / 'tools/reference_score.cpp'),
                      'host': f'{platform.system()} {platform.machine()}'},
        'execution': 'CPU only (no offload devices, n_gpu_layers=0, offload_kqv=false, op_offload=false), F32 KV, '
                     'flash attention disabled, six threads, last prompt logits only',
        'cases': [c['id'] for c in fixture['cases']],
        'outputs': outputs,
    }
    a.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'tag': tag, 'commit': commit[:9], 'outputs': outputs}))


if __name__ == '__main__':
    main()

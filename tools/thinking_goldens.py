"""Goldens for thinking on/off (#266): the chat templates of the hybrid
reasoning models, rendered as transformers' apply_chat_template does.

  python3 tools/thinking_goldens.py [--llama-server]   > tests/fixtures/templates/thinking.h

Each template in tests/fixtures/templates/*.jinja (tokenizer.chat_template of
the catalog's GGUF: qwen3-0.6b, qwen35-0.8b, qwen38-27b, bonsai2-27b-pq2,
gemma4-e2b) renders a few conversations with enable_thinking unset (the
template's default), true and false, and where the template has
reasoning_effort also with low, medium and xhigh. jinja2's sandbox with
trim_blocks and lstrip_blocks, as transformers renders; bos_token is empty
(the runtime adds BOS apart). tests/test_template.c checks that the runtime
renders the same bytes.

--llama-server: also render the explicit modes through llama.cpp's Jinja
engine (llama-server's /apply-template, on PATH; one small GGUF per template
from GEISTR_MODELS) and fail on any difference. llama.cpp passes
enable_thinking=true unless told otherwise, so the template's default is
compared with transformers only. Needs jinja2 (pip install jinja2)."""
import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

from jinja2 import ext
from jinja2.sandbox import ImmutableSandboxedEnvironment

ROOT = Path(__file__).resolve().parent.parent
TEMPLATES = ROOT / 'tests/fixtures/templates'
NAMES = ['qwen3', 'qwen35', 'qwen38', 'bonsai2', 'gemma4']
VOCAB = {'qwen3': 'Qwen3-0.6B-Q8_0.gguf', 'gemma4': 'gemma-4-E2B-it-Q4_K_M.gguf'}  # others: Qwen3.5 0.8B
CONVS = {
    'user': [{'role': 'user', 'content': 'Hi'}],
    'system': [{'role': 'system', 'content': 'Be brief.'}, {'role': 'user', 'content': 'Hi'}],
    'turns': [{'role': 'system', 'content': 'Be brief.'}, {'role': 'user', 'content': 'Hi'},
              {'role': 'assistant', 'content': 'Hello!'}, {'role': 'user', 'content': 'Bye'}],
    'turns_nosys': [{'role': 'user', 'content': 'Hi'}, {'role': 'assistant', 'content': 'Hello!'},
                    {'role': 'user', 'content': 'Bye'}],
}
MODES = {'default': {}, 'on': {'enable_thinking': True}, 'off': {'enable_thinking': False}}
EFFORTS = {'low': 'low', 'medium': 'medium', 'high': 'xhigh'}


def raise_exception(message):
    raise ValueError(message)


def render_hf(text, messages, kwargs):
    env = ImmutableSandboxedEnvironment(trim_blocks=True, lstrip_blocks=True, extensions=[ext.loopcontrols])
    env.filters['tojson'] = lambda x, **k: json.dumps(x, ensure_ascii=False)
    env.globals['raise_exception'] = raise_exception
    return env.from_string(text).render(messages=messages, add_generation_prompt=True, bos_token='', eos_token='',
                                        **kwargs)


def cases():
    for name in NAMES:
        text = (TEMPLATES / f'{name}.jinja').read_text()
        modes = dict(MODES)
        if 'reasoning_effort' in text:
            modes.update({e: {'enable_thinking': True, 'reasoning_effort': v} for e, v in EFFORTS.items()})
        for conv, messages in CONVS.items():
            for mode, kwargs in modes.items():
                yield name, conv, mode, messages, kwargs, render_hf(text, messages, kwargs)


def llama_check(all_cases):
    models = Path(os.environ.get('GEISTR_MODELS', Path.home() / 'Library/Application Support/geisten/models'))
    bad = 0
    for name in NAMES:
        server = subprocess.Popen(['llama-server', '-m', models / VOCAB.get(name, 'Qwen3.5-0.8B-Q8_0.gguf'),
                                   '--chat-template-file', TEMPLATES / f'{name}.jinja', '--jinja', '-ngl', '0', '-t',
                                   '1', '-c', '512', '--no-warmup', '--port', '18799'],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(240):
                try:
                    urllib.request.urlopen('http://127.0.0.1:18799/health', timeout=1)
                    break
                except Exception:
                    time.sleep(0.5)
            for n, conv, mode, messages, kwargs, want in all_cases:
                if n != name or mode == 'default':
                    continue
                body = json.dumps({'messages': messages, 'chat_template_kwargs': kwargs}).encode()
                req = urllib.request.Request('http://127.0.0.1:18799/apply-template', body)
                got = json.load(urllib.request.urlopen(req, timeout=30))['prompt']
                if got != want:
                    bad += 1
                    print(f'llama.cpp differs: {name} {conv} {mode}\n  llama.cpp: {got!r}\n  hf:        {want!r}',
                          file=sys.stderr)
        finally:
            server.terminate()
            server.wait()
    print(f'llama.cpp /apply-template: {bad} differences', file=sys.stderr)
    return bad == 0


def c_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def main():
    all_cases = list(cases())
    if '--llama-server' in sys.argv and not llama_check(all_cases):
        sys.exit(1)
    print('/* Generated by tools/thinking_goldens.py (transformers-style Jinja rendering): do not edit. */')
    print('static const struct thinking_golden {\n    const char *template, *conv, *mode, *text;\n} THINKING_GOLDENS[] = {')
    for name, conv, mode, _, _, text in all_cases:
        print(f'    {{"{name}", "{conv}", "{mode}",\n     {c_string(text)}}},')
    print('};')


if __name__ == '__main__':
    main()

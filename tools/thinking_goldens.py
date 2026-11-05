"""Goldens for thinking on/off (#266): the chat templates of the hybrid
reasoning models, rendered as transformers' apply_chat_template does.

  python3 tools/thinking_goldens.py [--llama-server]   > tests/fixtures/templates/thinking.h

Each template in tests/fixtures/templates/*.jinja (tokenizer.chat_template of
the catalog's GGUF: qwen3-0.6b, qwen35-0.8b, qwen38-27b, bonsai2-27b-pq2,
gemma4-e2b) renders a few conversations with enable_thinking unset (the
template's default), true and false, and where the template has
reasoning_effort also with low, medium and xhigh. Since #269 also
whitespace-padded messages (the templates' |trim, also Unicode whitespace),
a blank system message, and tool loops: tool results as role "tool" messages,
an assistant's reasoning in its content (<think>...</think>, which Qwen3.5
and Qwen3 (#276) keep after the last user query and drop before it). Since
#282 also an assistant's reasoning as reasoning_content, as a client sends
an earlier answer back with it (Qwen3 and Qwen3.5 render it in a tool loop,
Qwen3.6+ in every answer, Gemma 4 only after the last user message). TEMPLATES_OF says which
template renders which conversation. jinja2's sandbox with
trim_blocks and lstrip_blocks, as transformers renders; bos_token is empty
(the runtime adds BOS apart). tests/test_template.c checks that the runtime
renders the same bytes.

--llama-server: also render the explicit modes through llama.cpp's Jinja
engine (llama-server's /apply-template, on PATH; one small GGUF per template
from GEISTR_MODELS) and fail on any difference. llama.cpp passes
enable_thinking=true unless told otherwise, so the template's default is
compared with transformers only. llama.cpp's trim removes ASCII whitespace
only (Python's str.strip all of Unicode's), so it differs on 'padded_unicode':
reported, not failed; the runtime trims as transformers does (#269). Needs
jinja2 (pip install jinja2)."""
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
CALL = '<tool_call>\n<function=get_weather>\n<parameter=city>\nParis\n</parameter>\n</function>\n</tool_call>'
CALL2 = CALL.replace('Paris', 'Lyon')
THINK = '<think>\nThe user wants the weather in Paris.\n</think>\n\n'
Q = {'role': 'user', 'content': 'What is the weather in Paris?'}
CONVS = {
    'user': [{'role': 'user', 'content': 'Hi'}],
    'system': [{'role': 'system', 'content': 'Be brief.'}, {'role': 'user', 'content': 'Hi'}],
    'turns': [{'role': 'system', 'content': 'Be brief.'}, {'role': 'user', 'content': 'Hi'},
              {'role': 'assistant', 'content': 'Hello!'}, {'role': 'user', 'content': 'Bye'}],
    'turns_nosys': [{'role': 'user', 'content': 'Hi'}, {'role': 'assistant', 'content': 'Hello!'},
                    {'role': 'user', 'content': 'Bye'}],
    # #269: |trim, Unicode whitespace too (Python's str.strip)
    'padded': [{'role': 'system', 'content': '  Be brief.\n'}, {'role': 'user', 'content': '\n Hi \t'},
               {'role': 'assistant', 'content': ' Hello! \n\n'}, {'role': 'user', 'content': 'Bye\n'}],
    'padded_unicode': [{'role': 'user', 'content': '\u00a0Hi\u3000'},
                       {'role': 'assistant', 'content': '\u2003Hello!\r\n'}, {'role': 'user', 'content': ' Bye\u2028'}],
    'blank_system': [{'role': 'system', 'content': ' \n '}, {'role': 'user', 'content': 'Hi'}],
    # #269: tool loops (Qwen3.x: results as <tool_response> in a user turn)
    'tool': [Q, {'role': 'assistant', 'content': CALL}, {'role': 'tool', 'content': '{"temperature": 21}'}],
    'tool_two': [Q, {'role': 'assistant', 'content': CALL + '\n' + CALL2},
                 {'role': 'tool', 'content': '{"temperature": 21}'}, {'role': 'tool', 'content': '{"temperature": 24}'}],
    'tool_padded': [{'role': 'user', 'content': ' What is the weather in Paris? '},
                    {'role': 'assistant', 'content': '\n' + CALL + ' '}, {'role': 'tool', 'content': ' 21 \n'}],
    'tool_think': [Q, {'role': 'assistant', 'content': THINK + CALL}, {'role': 'tool', 'content': '{"temperature": 21}'}],
    'tool_steps': [Q, {'role': 'assistant', 'content': THINK + CALL}, {'role': 'tool', 'content': '{"temperature": 21}'},
                   {'role': 'assistant', 'content': '<think>\nNow Lyon.\n</think>\n\n' + CALL2},
                   {'role': 'tool', 'content': '{"temperature": 24}'}],
    'tool_done': [Q, {'role': 'assistant', 'content': THINK + CALL}, {'role': 'tool', 'content': '{"temperature": 21}'},
                  {'role': 'assistant', 'content': '<think>\nWarm.\n</think>\n\nIt is 21 degrees.'},
                  {'role': 'user', 'content': 'Thanks!'}],
    # #276: Qwen3's loop: no block for an answer without reasoning (thinking off sends the empty one back),
    # reasoning only stripped of line breaks, a user message quoting both tags is no query
    'tool_empty': [Q, {'role': 'assistant', 'content': '<think>\n\n</think>\n\n' + CALL},
                   {'role': 'tool', 'content': '{"temperature": 21}'},
                   {'role': 'assistant', 'content': '<think>\nNow Lyon.\n</think>\n\n' + CALL2},
                   {'role': 'tool', 'content': '{"temperature": 24}'}],
    'tool_think_padded': [Q, {'role': 'assistant', 'content': '<think>\n\n Paris, \t\n\n</think>\n\n\n' + CALL + ' \n'},
                          {'role': 'tool', 'content': '{"temperature": 21}'}],
    'tool_quoted': [Q, {'role': 'assistant', 'content': THINK + CALL}, {'role': 'tool', 'content': '{"temperature": 21}'},
                    {'role': 'assistant', 'content': '<think>\nWarm.\n</think>\n\nIt is 21 degrees.'},
                    {'role': 'user', 'content': 'What do <tool_response> and </tool_response> mean?'}],
    # #282: the reasoning as reasoning_content, as a client sends an earlier answer back (the content not split)
    'turns_rc': [{'role': 'user', 'content': 'Hi'},
                 {'role': 'assistant', 'content': 'Hello!', 'reasoning_content': 'A greeting.'},
                 {'role': 'user', 'content': 'Bye'}],
    'tool_rc': [Q, {'role': 'assistant', 'content': CALL, 'reasoning_content': 'The user wants the weather in Paris.'},
                {'role': 'tool', 'content': '{"temperature": 21}'}],
    'tool_rc_steps': [Q, {'role': 'assistant', 'content': CALL, 'reasoning_content': 'The user wants the weather.'},
                      {'role': 'tool', 'content': '{"temperature": 21}'},
                      {'role': 'assistant', 'content': CALL2, 'reasoning_content': 'Now Lyon.'},
                      {'role': 'tool', 'content': '{"temperature": 24}'}],
    'tool_rc_done': [Q, {'role': 'assistant', 'content': CALL, 'reasoning_content': 'The user wants the weather.'},
                     {'role': 'tool', 'content': '{"temperature": 21}'},
                     {'role': 'assistant', 'content': 'It is 21 degrees.', 'reasoning_content': 'Warm.'},
                     {'role': 'user', 'content': 'Thanks!'}],
    # padded: Qwen3 strips line breaks only (and lstrips the content in the loop), the others trim
    'tool_rc_padded': [{'role': 'user', 'content': 'Hi'},
                       {'role': 'assistant', 'content': '\nHello! ', 'reasoning_content': '\n\n A greeting. \t\n'},
                       Q, {'role': 'assistant', 'content': '\n' + CALL + ' \n', 'reasoning_content': '\n\n Paris, \t\n\n'},
                       {'role': 'tool', 'content': '{"temperature": 21}'}],
    # given reasoning, even empty or only line breaks, is the reasoning: </think> in the content stays text
    'tool_rc_empty': [Q, {'role': 'assistant', 'content': THINK + CALL, 'reasoning_content': ''},
                      {'role': 'tool', 'content': '{"temperature": 21}'},
                      {'role': 'assistant', 'content': CALL2, 'reasoning_content': '\n\n'},
                      {'role': 'tool', 'content': '{"temperature": 24}'}],
    # a null reasoning_content is none: the content is split as before
    'tool_rc_null': [Q, {'role': 'assistant', 'content': THINK + CALL, 'reasoning_content': None},
                     {'role': 'tool', 'content': '{"temperature": 21}'}],
}
BASE = ['user', 'system', 'turns', 'turns_nosys', 'padded', 'padded_unicode', 'blank_system']
TOOLS = ['tool', 'tool_two', 'tool_padded', 'tool_think', 'tool_steps', 'tool_done', 'tool_empty',
         'tool_think_padded', 'tool_quoted']
RC = ['tool_rc', 'tool_rc_steps', 'tool_rc_done', 'tool_rc_padded', 'tool_rc_empty', 'tool_rc_null']
# Gemma 4's tool format is not rendered: its reasoning only before the last user message (not rendered).
TEMPLATES_OF = {'qwen3': BASE + TOOLS + ['turns_rc'] + RC, 'qwen35': BASE + TOOLS + ['turns_rc'] + RC,
                'qwen38': BASE + TOOLS + ['turns_rc'] + RC, 'bonsai2': BASE + TOOLS + ['turns_rc'] + RC,
                'gemma4': BASE + ['turns_rc']}
ASCII_TRIM = ['padded_unicode']  # llama.cpp's |trim keeps Unicode whitespace
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
        for conv in TEMPLATES_OF[name]:
            messages = CONVS[conv]
            for mode, kwargs in modes.items():
                yield name, conv, mode, messages, kwargs, render_hf(text, messages, kwargs)


def llama_check(all_cases):
    models = Path(os.environ.get('GEISTR_MODELS', Path.home() / 'Library/Application Support/geisten/models'))
    bad = known = 0
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
                if got != want and conv in ASCII_TRIM:
                    known += 1
                elif got != want:
                    bad += 1
                    print(f'llama.cpp differs: {name} {conv} {mode}\n  llama.cpp: {got!r}\n  hf:        {want!r}',
                          file=sys.stderr)
        finally:
            server.terminate()
            server.wait()
    print(f'llama.cpp /apply-template: {bad} differences; {known} known (its trim is ASCII only: '
          f'{", ".join(ASCII_TRIM)})', file=sys.stderr)
    return bad == 0


def c_string(s):
    """A C string literal: escapes for quotes, backslash and control bytes, octal for every non-ASCII byte."""
    out = []
    for b in s.encode():
        c = chr(b)
        out.append({'\\': '\\\\', '"': '\\"', '\n': '\\n', '\t': '\\t', '\r': '\\r'}.get(c) or
                   (c if 0x20 <= b < 0x7f else f'\\{b:03o}'))
    return '"' + ''.join(out) + '"'


def main():
    all_cases = list(cases())
    if '--llama-server' in sys.argv and not llama_check(all_cases):
        sys.exit(1)
    print('/* Generated by tools/thinking_goldens.py (transformers-style Jinja rendering): do not edit. */')
    print('static const struct thinking_conv {\n    const char *name;\n    size_t n;\n    geistr_message m[6];\n'
          '    const char *r[6]; /* reasoning_content (#282), nullptr: none */\n} THINKING_CONVS[] = {')
    for conv, messages in CONVS.items():
        ms = ', '.join(f'{{{c_string(m["role"])}, {c_string(m["content"])}}}' for m in messages)
        rs = ', '.join('nullptr' if m.get('reasoning_content') is None else c_string(m['reasoning_content'])
                       for m in messages)
        print(f'    {{"{conv}", {len(messages)}, {{{ms}}}, {{{rs}}}}},')
    print('};')
    print('static const struct thinking_golden {\n    const char *template, *conv, *mode, *text;\n} THINKING_GOLDENS[] = {')
    for name, conv, mode, _, _, text in all_cases:
        print(f'    {{"{name}", "{conv}", "{mode}",\n     {c_string(text)}}},')
    print('};')


if __name__ == '__main__':
    main()

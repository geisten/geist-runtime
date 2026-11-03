#!/usr/bin/env python3
"""geistr serve --http against real clients (#132): the OpenAI Python SDK and
the Ollama Python library (pinned in the Makefile, make test-clients), not
hand-written HTTP. Every response goes through the library's own types; the
OpenAI SDK builds them without validation, so each is validated again with
its pydantic model (strict()).
Usage: test_clients.py <geistr> <smollm2.gguf> <qwen3.gguf> <bitnet-embed.gguf>"""
import contextlib, json, os, signal, socket, subprocess, sys, tempfile, time
import ollama, openai
from openai.types import CreateEmbeddingResponse
from openai.types.chat import ChatCompletion, ChatCompletionChunk

geistr, chat_model, tools_model, embed_model = sys.argv[1], *map(os.path.abspath, sys.argv[2:5])
tmp = tempfile.mkdtemp(prefix='geistr-clients-')
env = {**os.environ, 'GEISTEN_HOME': os.path.join(tmp, 'home')}

def strict(obj):
    """The SDK's pydantic model, validated (required fields, types)."""
    return type(obj).model_validate(obj.model_dump(mode='json', exclude_unset=True))

@contextlib.contextmanager
def serve(path):
    probe = socket.socket(); probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]; probe.close()
    service = subprocess.Popen([geistr, 'serve', path, f'--socket={tmp}/geistr-{port}.sock', f'--http=127.0.0.1:{port}',
                                '--cpu', '--models', os.path.join(tmp, 'models')],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, env=env)
    base = f'http://127.0.0.1:{port}'
    deadline = time.time() + 120
    while True:
        with contextlib.suppress(OSError):
            socket.create_connection(('127.0.0.1', port), timeout=5).close(); break
        assert time.time() < deadline and service.poll() is None, service.stderr.read()
        time.sleep(.5)
    try:
        yield openai.OpenAI(base_url=base + '/v1', api_key='geistr', timeout=300, max_retries=0), \
              ollama.Client(host=base, timeout=300)
    finally:
        service.send_signal(signal.SIGTERM)
        assert service.wait(60) == 0, service.stderr.read()

# ---- SmolLM2: one answer, a stream, max_tokens, stop, usage ---------------------
with serve(chat_model) as (oa, ol):
    model = oa.models.list().data[0].id
    ask = [{'role': 'system', 'content': 'Answer briefly.'}, {'role': 'user', 'content': 'Name a color.'}]
    one = strict(oa.chat.completions.create(model=model, messages=ask, temperature=0))
    assert isinstance(one, ChatCompletion) and one.choices[0].message.content, one
    assert one.choices[0].finish_reason in ('stop', 'length') and one.usage.completion_tokens > 0, one
    assert one.usage.total_tokens == one.usage.prompt_tokens + one.usage.completion_tokens, one.usage
    chunks = [strict(c) for c in oa.chat.completions.create(
        model=model, messages=[{'role': 'user', 'content': 'Count to ten.'}], stream=True, max_tokens=3,
        temperature=0, stream_options={'include_usage': True})]
    assert all(isinstance(c, ChatCompletionChunk) for c in chunks)
    assert ''.join(c.choices[0].delta.content or '' for c in chunks if c.choices), chunks
    assert [c.choices[0].finish_reason for c in chunks if c.choices][-1] == 'length', chunks[-3:]
    assert chunks[-1].usage.completion_tokens == 3, chunks[-1]
    cut = strict(oa.chat.completions.create(model=model, messages=[{'role': 'user', 'content': 'Count from 1 to 5 with commas.'}],
                                            stop=[','], temperature=0, max_tokens=30))
    assert ',' not in cut.choices[0].message.content, cut
    short = strict(oa.chat.completions.create(model=model, messages=[{'role': 'user', 'content': 'Count from 1 to 50 with commas.'}],
                                              max_tokens=2, temperature=0))
    assert short.choices[0].finish_reason == 'length' and short.usage.completion_tokens == 2, short
    # Ollama: the model listed, one answer, a stream that ends with done
    assert [m.model for m in ol.list().models] == [model], ol.list()
    one = ol.chat(model=model, messages=[{'role': 'user', 'content': 'Count from 1 to 50 with commas.'}],
                  options={'temperature': 0, 'num_predict': 4})
    assert isinstance(one, ollama.ChatResponse) and one.done and one.message.content and one.done_reason == 'length', one
    parts = list(ol.chat(model=model, messages=[{'role': 'user', 'content': 'Name a color.'}], stream=True,
                         options={'temperature': 0, 'num_predict': 3}))
    assert not parts[0].done and parts[-1].done and parts[-1].eval_count == 3, parts
    # a model that does not think: Ollama says so for think true (#266); off and OpenAI's effort are ignored
    try:
        ol.chat(model=model, messages=[{'role': 'user', 'content': 'Hi'}], think=True)
        raise AssertionError('think true on a model that does not think')
    except ollama.ResponseError as e:
        assert e.status_code == 400 and 'thinking' in str(e), e
    assert ol.chat(model=model, messages=[{'role': 'user', 'content': 'Name a color.'}], think=False,
                   options={'temperature': 0, 'num_predict': 3}).message.content
    assert strict(oa.chat.completions.create(model=model, messages=ask, temperature=0, max_tokens=3,
                                             reasoning_effort='high')).choices[0].message.content
print('openai and ollama clients, SmolLM2: one answer, stream, max_tokens, stop, usage passed')

# ---- Qwen3: the OpenAI SDK's function-calling round-trip ------------------------
with serve(tools_model) as (oa, ol):
    model = oa.models.list().data[0].id
    tools = [{'type': 'function', 'function': {'name': 'get_weather', 'description': 'Get the current weather in a city',
              'parameters': {'type': 'object', 'properties': {'city': {'type': 'string'}}, 'required': ['city']}}}]
    messages = [{'role': 'user', 'content': 'What is the weather in Paris? Use the tool.'}]
    done = strict(oa.chat.completions.create(model=model, messages=messages, tools=tools, temperature=0))
    choice = done.choices[0]
    assert choice.finish_reason == 'tool_calls' and choice.message.tool_calls, done
    call = choice.message.tool_calls[0]
    assert call.function.name == 'get_weather' and json.loads(call.function.arguments)['city'] == 'Paris', call
    messages += [choice.message.model_dump(exclude_none=True),
                 {'role': 'tool', 'tool_call_id': call.id, 'content': json.dumps({'temperature_c': 21, 'sky': 'sunny'})}]
    done = strict(oa.chat.completions.create(model=model, messages=messages, tools=tools, temperature=0))
    assert done.choices[0].finish_reason == 'stop' and '21' in done.choices[0].message.content, done
    # the same call streamed: the SDK's chunk types carry the tool-call deltas
    chunks = [strict(c) for c in oa.chat.completions.create(model=model, messages=messages[:1], tools=tools,
                                                           temperature=0, stream=True)]
    deltas = [d for c in chunks if c.choices for d in c.choices[0].delta.tool_calls or []]
    assert deltas and deltas[0].function.name == 'get_weather', chunks
    assert [c.choices[0].finish_reason for c in chunks if c.choices][-1] == 'tool_calls', chunks[-1]
    # Ollama: the call (arguments an object) and the result back
    ask = [{'role': 'user', 'content': 'What is the weather in Paris? Use the tool.'}]
    one = ol.chat(model=model, messages=ask, tools=tools, options={'temperature': 0})
    calls = one.message.tool_calls
    assert calls and calls[0].function.name == 'get_weather' and calls[0].function.arguments['city'] == 'Paris', one
    one = ol.chat(model=model, messages=ask + [one.message, {'role': 'tool', 'content': json.dumps({'temperature_c': 4, 'sky': 'snow'})}],
                  tools=tools, options={'temperature': 0})
    assert '4' in one.message.content and not one.message.tool_calls, one
    # thinking (#266), as the clients switch it: the OpenAI SDK through extra_body (vLLM's and
    # llama.cpp's chat_template_kwargs) or reasoning_effort, Ollama's think; the reasoning apart
    q = [{'role': 'user', 'content': 'What is 2+3? One word.'}]
    one = strict(oa.chat.completions.create(model=model, messages=q, temperature=0))
    assert one.choices[0].message.model_extra.get('reasoning_content') and 'think>' not in one.choices[0].message.content, one
    for off in ({'extra_body': {'chat_template_kwargs': {'enable_thinking': False}}}, {'reasoning_effort': 'none'}):
        one = strict(oa.chat.completions.create(model=model, messages=q, temperature=0, **off))
        assert 'reasoning_content' not in one.choices[0].message.model_extra, (off, one)
        assert 'think>' not in one.choices[0].message.content and one.usage.completion_tokens < 40, (off, one)
    chunks = [strict(c) for c in oa.chat.completions.create(model=model, messages=q, temperature=0, stream=True)]
    assert any((c.choices[0].delta.model_extra or {}).get('reasoning_content') for c in chunks if c.choices), chunks[:3]
    assert 'think>' not in ''.join(c.choices[0].delta.content or '' for c in chunks if c.choices)
    off = ol.chat(model=model, messages=q, think=False, options={'temperature': 0})
    assert not off.message.thinking and 'think>' not in off.message.content, off
    on = ol.chat(model=model, messages=q, think=True, options={'temperature': 0})
    assert on.message.thinking and 'think>' not in on.message.content, on
    parts = list(ol.chat(model=model, messages=q, think=True, stream=True, options={'temperature': 0}))
    assert any(p.message.thinking for p in parts) and 'think>' not in ''.join(p.message.content or '' for p in parts)
print('openai and ollama clients, Qwen3: tool call round-trip (plain, streamed), thinking on/off and apart passed')

# ---- bitnet-embed: embeddings.create (base64, the SDK default) and ollama embed --
with serve(embed_model) as (oa, ol):
    model = oa.models.list().data[0].id
    docs = ['The cat sleeps on the red sofa.', 'Revenue grew by twelve percent.']
    out = strict(oa.embeddings.create(model=model, input=docs))
    assert isinstance(out, CreateEmbeddingResponse) and [d.index for d in out.data] == [0, 1], out
    assert all(len(d.embedding) == 1024 for d in out.data) and out.usage.prompt_tokens > 0, out.usage
    floats = strict(oa.embeddings.create(model=model, input=docs[0], encoding_format='float'))
    assert max(abs(a - b) for a, b in zip(floats.data[0].embedding, out.data[0].embedding)) < 1e-6
    out = ol.embed(model=model, input=docs)
    assert isinstance(out, ollama.EmbedResponse) and len(out.embeddings) == 2 and len(out.embeddings[0]) == 1024, out
    assert max(abs(a - b) for a, b in zip(out.embeddings[0], floats.data[0].embedding)) < 1e-4
print('openai and ollama clients, bitnet-embed: embeddings.create (base64, float), embed passed')

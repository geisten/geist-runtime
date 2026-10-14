"""retrieval-bench.py --rerank-gguf: P(yes) from llama-server's top log-probs
must use the exact log-probs of "yes" and "no", also when one of them is not
among the first 50 (#225)."""
import importlib.util
import math
from pathlib import Path

root = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('retrieval_bench', root / 'scripts/retrieval-bench.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

# The first piece of #225 (Qwen3-Reranker-0.6B, llama-server): "no" second
# most likely, "yes" behind 300 other tokens.
vocab = {'no': -2.6123, 'yes': -11.2540, 'Yes': -0.4570}
vocab.update({f't{i}': -3.0 - 8.0 * i / 300 for i in range(300)})
asked = []


# byte tokens that print alike: llama-server lists each, the text repeats
fffd = [('\ufffd', -4.0 - i / 100) for i in range(100)]


def top(n):
    asked.append(n)
    return sorted(list(vocab.items()) + fffd, key=lambda kv: -kv[1])[:n]


p = module.llama_p_yes(top)
exact = 1 / (1 + math.exp(vocab['no'] - vocab['yes']))
assert abs(p - exact) < 1e-9, f'P(yes) {p:.5f}, exact {exact:.5f} (the 50th log-prob stood in for "yes"?)'
assert asked[0] == 50 and len(asked) > 1, asked

# both among the first 50: one request
asked.clear()
vocab['yes'] = -1.0
assert abs(module.llama_p_yes(top) - 1 / (1 + math.exp(vocab['no'] - vocab['yes']))) < 1e-9 and asked == [50]

# a model without the token: an error, not a guess
del vocab['yes']
try:
    module.llama_p_yes(top)
    raise RuntimeError('P(yes) without a "yes" token')
except SystemExit:
    pass
print('retrieval-bench P(yes): passed')

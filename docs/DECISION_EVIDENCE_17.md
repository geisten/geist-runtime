# Apple decision evidence (#17)

Engine `4afbfcd`, Apple M1 Max (10 cores, 64 GiB), macOS 27.0, on mains power,
6 threads, context 512. The plan, [DECISION_EVIDENCE_PLAN_17.json](DECISION_EVIDENCE_PLAN_17.json),
was committed before any measurement (`6c774d0`, 07:09); one amendment to the
lifecycle budgets was committed before any lifecycle run (`ebfbd81`). Raw data,
hashes and the analysis are in `tests/fixtures/decisions/validation/apple-17/` and
`manifest.json` → `apple_17`; `make test` checks the hashes.

## Verdict

| Part | Result |
|---|---|
| Configuration correctness: native IDs, wrapper = direct engine (bit-identical), CLI, Python, error/cancel/recovery, feature-off | **PASS**, both models, NEON and Metal |
| Runtime overhead, scoring phase, paired 2 % gate | **PASS**, all four model × backend combinations |
| Gemma numerics against the independent llama.cpp oracle | **FAIL** under both contracts: fixture `order` (geistlib#728) |
| Lifecycle, 100 requests × 3 cycles, frozen memory budgets | Metal **PASS** (both models); CPU **FAIL** (geistlib#729) |
| Quality | development pilot only; **not eligible** |

The feature stays EXPERIMENTAL and default-off. Nothing here claims quality
equivalence, a speed-up over another model, Jev equivalence or calibration.

## Contract choice

The #94 contract was to apply only if performance did not get significantly
worse (more than 5 %). Engine `0707c3b` against `4afbfcd` (which carries the
cpu_neon fix geistlib#698), interleaved:

| | 0707c3b | 4afbfcd | ratio |
|---|---|---|---|
| decision scoring, Gemma CPU | 703.6 ms | 715.5 ms | 1.017 |
| decision scoring, Bonsai CPU | 11.92 s | 11.64 s | 0.977 |
| decode, Gemma CPU | 33.3 tok/s | 34.7 tok/s | 1.041 |
| decode, Bonsai CPU | 9.0 tok/s | 8.3 tok/s | **0.922** |

The Bonsai decode mean falls by 7.8 %, from one run (6.5 against 9.1 and 9.3; the old
engine had 8.9, 8.9, 9.2). The frozen rule uses the mean and allows no rerun, so the
**original contract** is the gate. The verdict is the same under both (below).

FP32 KV for decisions cost nothing measurable (FP32/AUTO 0.995–1.006), so, as the plan
said, the runtime now passes `GEIST_KV_FP32` for decisions, like the oracle.

## Overhead gate

Runtime `geistr_decision_score` (scoring phase: lock, score, copy, unlock) against the
direct engine with the same IDs, FP32 KV on both sides; 5 warm-up and 30 AB/BA pairs,
ratio of paired means, 95 % paired bootstrap (10000, seed 14917):

| | ratio | 95 % interval | gate |
|---|---|---|---|
| Gemma, NEON | 1.0033 | 0.9959 – 1.0106 | PASS |
| Gemma, Metal | 1.0003 | 0.9955 – 1.0047 | PASS |
| Bonsai, NEON | 0.9914 | 0.9715 – 1.0102 | PASS |
| Bonsai, Metal | 0.9956 | 0.9898 – 1.0014 | PASS |

Preparation (rendering, tokenization) takes 0.1–0.2 ms and is reported separately. End-to-end
per decision (p50 / p95): Gemma NEON 812 / 924 ms, Metal 181 / 186 ms; Bonsai NEON 13.1 /
15.1 s, Metal 1.36 / 1.45 s. Decode is not applicable (no generation). The first run with
AUTO KV (before FP32 was adopted) gave PASS twice and INCONCLUSIVE twice (intervals wider
than 2 %, means 0.99–1.01); it is kept in `overhead-*.jsonl`. SELECTED_ROWS opens on Metal
and for Bonsai on NEON, and is an explicit error for Gemma on NEON; the gate covers DENSE.

## Gemma numerics

Largest difference per fixture against the oracle (llama.cpp `01ae597e`, CPU, F32 KV):

| | plain | unicode | order |
|---|---|---|---|
| NEON | 1.89 | 1.00 | 7.76, other option |
| Metal | 1.54 | 0.80 | 7.84, other option |
| cpu_scalar + FP32 KV (full precision) | 1.53 | 0.80 | 7.88, other option |

`plain` and `unicode` pick the oracle's option on every backend (at engine 5dd7e17 the
differences were 6.49 and 3.52). `order` fails on every backend, full precision
included: geist picks D (4), the oracle B (2); both are even. So the remaining
difference is between the two Gemma 4 implementations, not int8 activations
(geistlib#728). Original contract: FAIL; #94 contract: FAIL (envelope 4 logits and
winner gate on `order`).

## Lifecycle

`tools/stress_decisions.py`, 100 requests (every tenth with chat, an invalid request and a
cancel) × 3 model create-use-destroy cycles:

| | result | observed |
|---|---|---|
| Gemma, Metal | PASS | RSS ≤ 0.82 GB, Metal ≤ 3.69 GB, after close 0.15–0.21 GB |
| Bonsai, Metal | PASS | RSS ≤ 2.98 GB, Metal ≤ 10.12 GB, after close 0.08 GB |
| Gemma, NEON | FAIL | 1.43 GB RSS after the model closed (budget 1 GiB); four cycles: 1.38, 1.40, 1.40, 1.40, so retained once, not growing |
| Bonsai, NEON | FAIL | 13.7 GB RSS after load, 14.8 GB after a decision (budget 11.9 GB): the weights are held about twice |

**Follow-up on engine `12f77e8`** (geistlib#731 fixed the double residency, #729):
Bonsai on NEON now completes all 100 requests within the budget (RSS at most 9.75 GB,
was 13.7 GB after load), and Gemma stays at 4.46 GB. Both still fail only the check after
the model closes: 1.64 GB and 1.41 GB against 1 GiB. That is macOS malloc's large-block
cache (`vmmap`: "Malloc Large (empty)"; 0.04 GB with `MallocLargeCache=0`), flat across
cycles, not a leak. Records in `apple-17/followup-12f77e8/`.

Runtime allocations were constant per request and zero after every decision closed
in all runs that got that far. Two reruns are declared: the first runs' chat check
required visible text, which a thinking model (Bonsai) hides; it now checks generated
tokens. The Bonsai NEON rerun named no check; checks now name their line. All runs are kept.

## Quality pilot (development only)

Gemma 4 on Metal through geistlib#587's evaluator, unchanged: classic MMLU, 8 development
questions from 8 subjects (label-independent selection), 5 shots, all four cyclic rotations.

| | canonical accuracy | rotations 1 / 2 / 3 | decision change across rotations |
|---|---|---|---|
| chat_direct (DENSE) | 4/8 | 4/8, 2/8, 4/8 | 46 % |
| cloze (DENSE) | 1/8 | 4/8, 4/8, 4/8 | 83 % |
| reasoning baseline, cap 512 | 0/8 | 0 | – |

The reasoning baseline answered nothing valid within 512 tokens, so it is not a
quality-matched reference, and non-inferiority is not established (paired interval
−29.3 to +87.1 pp). The large decision changes under rotation show a strong position
bias; eight questions are far too few to estimate it. Held-out non-inferiority stays
open with geistlib#587.

## Open

- geistlib#728: the `order` difference between geist's and llama.cpp's Gemma 4.
- geistlib#729: cpu_neon memory (Bonsai twice; ~1.4 GB kept after close).
- Held-out quality campaign (geistlib#587).
- If the P1 outlier should be re-measured under a new, frozen protocol (more runs,
  median), that is a new decision; this run keeps the original contract.

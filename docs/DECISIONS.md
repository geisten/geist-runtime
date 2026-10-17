# Fixed-option decisions (V1)

The EXPERIMENTAL contract in `geistr_decision.h` keeps prompt policy in the
runtime and numerical execution in geistlib. Chat remains the model default.
A decision chooses exactly one supplied option; it does not produce free text,
JSON, a continuous rating or calibrated correctness/truth confidence.

## Permission and identity

In the C API, decision permission is absent/disabled by default. The `geistr`
CLI enables every verified profile for its exact artifact when no `--config`
is given (see CLI below); a `--config` file replaces that list. A model's options can point
at a policy returned by the immutable configuration parser. Model open copies
that value: later edits or freeing the config cannot change a loaded model.
An enabled policy requires a known profile bound to the exact model SHA-256.
The runtime hashes file or borrowed-memory contents before loading an enabled
model. A mismatch is an error; filename and family are not an identity.
Borrowed model bytes must remain unchanged for the model's lifetime.

Configuration is a **separate schema-1 JSON file**, leaving the schema-2 model
catalog unchanged:

```json
{
  "schema": 1,
  "models": [
    {
      "sha256": "3907dc1658db1f78a9826bf8d5bcb8dc65db0d466388937af57f2294fae62ec1",
      "enabled": false,
      "profile": "bonsai2-27b-pq2-v1",
      "mode": "dense"
    },
    {
      "sha256": "740185b21d22ceb83a11c3aa62ad5842ef32c70f6096d756bbee85a1e4ec34b8",
      "enabled": false,
      "profile": "gemma4-e2b-q4-v1",
      "mode": "dense"
    }
  ]
}
```

Both top-level keys are required. Policies require `sha256`; `enabled` defaults
to false, `mode` to dense, and a profile may be absent only while disabled.
`selected_rows` is an explicit alternative, never a fallback. Each SHA occurs
once. Unknown/duplicate keys, unknown schema/profile/mode, malformed or trailing
JSON and escaped ASCII keys/values are refused. At most 64 policies and 65,536
configuration bytes are accepted. Parsing owns its data; readers may share a
parsed immutable config until it is freed.

An embedder selects the policy for the verified artifact hash with
`geistr_decision_config_find`, sets `geistr_model_opts.decision`, then opens
the model. `nullptr` leaves decisions disabled. Processor choice is separate.
A config declaration is permission, not proof that the linked engine/backend
can execute it. Capability probing checks the actual linked engine/loaded backend; it does not certify numerical parity. The stub refuses
enabled pretrained profiles instead of faking their support.

Precedence: global hard bounds, artifact/profile integrity and model permission
cannot be overridden. V1 has no preprompt/template override; the model policy
fixes its verified profile and thinking-off rendering contract. An instance
may request a numeric mode explicitly; that mode must be supported. Persisted
policy changes apply only to subsequently opened models.

## Input contract and limits

Requests explicitly select `GEISTR_OPERATION_DECISION` and borrow their input
only for the synchronous call. The request struct requires its known V1 size;
later versions must preserve the same required prefix. Options stay in caller
order. IDs compare byte-for-byte; descriptions do not become IDs.

| Field | Byte/count limit |
| --- | ---: |
| External option ID | 1–128 UTF-8 bytes |
| Question | 1–65,536 UTF-8 bytes |
| Optional context | 0–65,536 UTF-8 bytes |
| Each option description | 1–16,384 UTF-8 bytes |
| Sum of question/context/ID/description bytes | 262,144 |
| Options | 1–26 |
| Serialized CLI request | 1,048,576 bytes |

Lengths exclude terminators. Embedded NUL, invalid/overlong UTF-8, surrogates
and incomplete sequences fail. IDs additionally exclude U+0000–001F,
U+007F–009F and U+2028/2029 so selection can be written as exactly one line.
Unicode IDs are preserved without normalization. Duplicate/empty/oversized IDs
fail; they are never escaped, shortened or sanitized into another identity.

Consumers must bound ingestion before accumulating file/stdin data and check
aggregate lengths before allocations. Validation checks lengths before reading
bytes. A loaded model's token context and complete-prefix single-token checks
may impose tighter limits. No failure truncates a question or substitutes
generation. Profile preparation checks reserved template controls.

## API and verification

New declarations are EXPERIMENTAL. Status codes must be checked and outputs
are defined on failure. Config parsing returns nullptr on failure; diagnostic
strings alone may be shortened to caller capacity. Previous model option sizes
remain supported: the appended decision pointer defaults to nullptr. Partial
appended pointers are refused. C23/C++20 linkage and the original catalog/chat
tests remain gates.

Integration order: #13 contract/configuration → #14 verified native profiles →
#15 isolated engine bridge → #16 CLI → #17 evidence. Successful contract tests
establish neither numerical parity nor quality-matched performance.

## Reusable contexts: profiles and token preparation (#162)

Preparation for several questions over one immutable context (epic #161);
the C API that opens such a context follows below (#163). A verified profile
(`src/decision_profile.c`) is one registry entry: name/version, exact artifact
SHA-256, embedded template hash, native BOS and role markers and the
thinking-off generation prompt. The configuration resolves names and
artifacts from it; a new model adds an entry, never a family or filename
guess, and V1 has no pre-prompt/template override.

The common prefix is the BOS, system pre-prompt, user role and the context
block; question, options and any batch question ID never enter it. Every
complete question prompt is byte-identical to the single-question profile
above. Preparation tokenizes the common prefix natively and caches a cut
strictly before its last token (text after it may merge with that token),
aligned down to the engine's resolved prefill chunk; a short context may
cache zero tokens. Batch preflight checks that the cached IDs are an exact
prefix of each full prompt's own native IDs; the rest, including the
uncached context tail, is the question's suffix. Keys map A..Z as before:
full prompt plus key must add exactly one distinct token. A mismatch fails
explicitly; nothing is guessed, truncated or re-templated.

`tests/fixtures/decisions/{bonsai2,gemma4}_context.json` (with generated
`.h`, `tools/prepare_profile_oracles.py --contexts`) are the native fixtures:
common prefixes, cuts for explicit prefill chunks 16 and 64, full prompt IDs
and key candidates for empty, short, Unicode/whitespace/trailing-newline,
template-lookalike and long contexts with reordered options. Each manifest
binds the artifact, profile version, template hash, oracle revision and
binary/source/library hashes, and the chunks. A cut applies only where the
engine resolves exactly that chunk. The test refuses fixtures whose profile
name or template hash differs from the registry: a changed profile gets a
new version and new fixtures; old manifests are then marked `historical`.

## Reusable contexts and atomic question batches (#163)

EXPERIMENTAL, default-off. It needs the build flag (`make runtime DECISION=1
DECISION_CONTEXT=1`; `DECISION=0 DECISION_CONTEXT=1` is refused), the model's
decision permission and an explicit `geistr_decision_context_open`. The engine
is geistlib with geistlib#5 (`3841fa6` or later; pinned: `e78f9ee`). Off builds and the stub keep every
symbol: `geistr_decision_context_available()` is false and open returns
`GEISTR_FORMAT`.

```c
geistr_decision_context *ctx = nullptr;
char error[256];
if (geistr_decision_context_open(model, context_len, sizeof error, context, nullptr, &ctx, error) != GEISTR_OK)
    ...;
const geistr_decision_question questions[] = {
    {2, question_len, n_options, "q1", question, options}, ...};
const geistr_decision_batch batch = {sizeof batch, GEISTR_OPERATION_DECISION, 2, questions};
geistr_decision_batch_result result = {.size = sizeof result};
if (geistr_decision_context_score_batch(ctx, &batch, &result) == GEISTR_OK)
    for (size_t i = 0; i < result.n_answers; ++i)
        printf("%s %s\n", result.answers[i].question_id, result.answers[i].external_id);
/* The same handle answers the next batch over the same context: the prefix
 * is not prefilled again, and result's previous answers are now invalid. */
const geistr_decision_batch next = {sizeof next, GEISTR_OPERATION_DECISION, 1, other_questions};
if (geistr_decision_context_score_batch(ctx, &next, &result) != GEISTR_OK)
    fprintf(stderr, "%s\n", geistr_decision_context_error(ctx)); /* no answers; ctx stays usable */
geistr_decision_context_close(ctx);
```

A long-lived caller keeps one handle per context and reuses it for every
batch over that context. `geistr decide-batch` (below) is the one-shot form:
one process opens the model, opens one context, scores one batch, closes both.

**Open** copies the context and holds a model reference. It creates one
private scoring handle (DENSE, FP32 KV, `max_prompt_tokens`) and one tokenizer.
It preallocates the workspace for 32 complete prompts and reads the engine's
resolved prefill chunk. It cuts the common prefix there (#162), then prefills
and snapshots the cached tokens once (`geist_decision_prefix_prepare`). The
snapshot holds every KV row, the recurrent state, positions and pending logits.
A short context may cache zero tokens; the info says so. Open never changes
the model's policy, its chats or its weights. Weights are shared with chat and
with other decisions; KV, recurrent state and results are not. Another
context needs another handle. Nothing is cached globally or kept on disk.

| Option | Default | Refused |
| --- | --- | --- |
| `mode` | the policy's; must be DENSE | SELECTED_ROWS, explicit or via the policy: `GEISTR_FORMAT` |
| `max_prompt_tokens` (each full prompt) | min(model context, 512) | above the loaded context: `GEISTR_CONTEXT` |
| `max_snapshot_bytes` | 256 MiB | a larger snapshot: `GEISTR_NO_MEMORY`, nothing allocated |

The KV cache is always FP32; there is no cache override to conflict with. A
context, model or chunk change needs a new handle.

**A batch** has 1–32 questions; each has 1–26 options. Question IDs follow
the option ID rules (byte-exact, no controls) and are unique per batch. Each
question with the context obeys the single-question limits. The whole batch
is preflighted before anything is scored: lengths, UTF-8, IDs, rendering,
native tokenization, the exact cached-prefix match, the one-token keys and the
full prefix+suffix capacity. Then each question restores the snapshot,
prefills only its suffix (the uncached context tail included) and reads its
options from the ordinary DENSE logits. Questions run serially; none sees
another's state. The answers are staged and published in input order only
when every question succeeded.

**Failure** of any kind (validation, tokenizer, engine, restore, a non-finite
logit or cancellation) returns zero answers and `answers == nullptr`. Nothing
staged survives. `failed_index` and `failed_id` name the question where there
is one. `prepared_questions`, `model_calls` and the timings stay defined; a
rejection in preflight has `model_calls == 0`. The context stays usable, and
the next valid batch succeeds. Answers are borrowed until the next batch
(including a failed one) or close; copy them to keep them.

**Cancellation** (`geistr_decision_context_cancel`, any thread) is atomic and
idempotent. It is checked before preflight, before each question, between
tokenizer calls, and before and after each restore+score. It cannot interrupt
a running kernel. The cancelled batch's results are discarded and the
cancellation is consumed; the next batch runs.

**Concurrency.** One caller uses a context at a time, except cancel. Close must
not race scoring, cancel or getters. Setup and teardown take the model's
setup lock; every engine call takes the operation lock, and Metal/Vulkan calls
stay serialized per model. The caller may close the model first: the context
keeps it alive. Distinct contexts share only the weights.

**Diagnostics.** `geistr_decision_context_info_get` reports the resolved mode,
`cached_tokens`, `snapshot_bytes` (engine-owned), the budget, the prefill chunk,
the prompt capacity, the prefix prefills and `setup_ms` at open. It also
reports the backend, engine revision/version, `kv_cache` "fp32" and the
model, template and context hashes. The context hash is the SHA-256 of the
rendered common prefix. `activation` is nullptr: the engine does not report its
activation arithmetic (int8 on the quantized CPU paths), so nothing claims
FP32 compute. Each answer has `prompt_tokens` and `uncached_tokens`; each
batch has `model_calls` (one restore each) and preparation/scoring/batch
milliseconds. The context's wrapper allocations (the handle and one workspace)
count in `geistr_decision_resources_get`. Snapshot bytes are separate, and
RSS and the Metal counter keep their own scopes. Per batch nothing is
allocated.

Tests: `tests/test_decision_batch.c` (in `make test` and `make sanitize`)
runs the batch core on the native context fixtures. An injected engine fails,
returns NaN or cancels at the first, middle and last question. It also covers
zero/33 questions, duplicate and invalid IDs, invalid UTF-8, reserved
controls, a mismatched cached prefix, prompts over capacity, recovery,
reorder/addition identity, independent handles and the off stubs.
`make test-decision-real DECISION=1 DECISION_CONTEXT=1` adds the real model, for every
fixture context. It checks the native cut and the batch, reversed and per
question bit for bit. It compares against the full-prompt single-question
API, interleaves with chat, closes the model first, and checks the budget,
mode and capacity refusals. No speed or quality claim follows from these;
that evidence is #165/#166.

## Execution, ownership and concurrency

Build the optional engine with `make runtime DECISION=1` (add
`DECISION_CONTEXT=1` for reusable contexts, #163) (or `make geistr
DECISION=1 PULL=0`, `make wheel DECISION=1`). `DECISION=1` is the default, so
release binaries and wheels include the engine; with `DECISION=0` all decision
symbols stay linkable and return explicit unsupported errors.
The engine is the Makefile's pin (`GEIST_REF`). The recorded validation evidence
(`tests/fixtures/decisions/validation`): the Apple records (NEON, Metal) were
produced on engine `5dd7e1747df86092a320e638c66993afd409e3b6` and are kept as
recorded; the x86 CPU records (`*-x86.jsonl`, `manifest.json` → `x86_cpu`) on
engine `0707c3b1c9e547c909d200331a0750b4ff8e8cbe` (#94). Evidence is
not re-established for later pins until it is recorded again. The Apple records of #17
(`validation/apple-17`, `manifest.json` → `apple_17`) are on engine `4afbfcd`, with Gemma's
numeric records again on `12f77e8` and `d53560d`; x86 on `d53560d` (`validation/x86-d53560d`,
`manifest.json` → `x86_cpu_d53560d`, the self-hosted runner, the `decision-evidence` workflow).
On `d53560d` Gemma passes the #94 contract against llama.cpp v0.6.0 on cpu_x86, cpu_neon and Metal.
Required EXPERIMENTAL engine symbols are availability/support/mode probes,
create/score/destroy, error access, tokenization and resource observation
(`geistr_decision_reset` clears only the wrapper's state: every score starts
from an empty engine state).
Future stability promotion is tracked by geisten/geistlib#622.

`geistr_decision_open(model, error_cap, opts, &decision, error)` owns an
independent scoring session and a tokenizer session sharing model weights.
The model retains a reference until every child chat/decision is closed;
caller model close may come first. Setup errors use the caller buffer. Score
errors belong to the instance; no global mutable prompt configuration exists.

One caller uses each decision instance, except atomic `cancel` from any thread.
Setup/teardown exclude every operation on that enabled model via a reader/writer
lock. CPU instances may score in parallel; Metal/Vulkan engine calls are
serialized per model. Different instances do not share KV, history or result
storage. A caller must prevent close from racing cancel, getters or scoring.

Every score validates/renders/maps the complete input before one scoring call.
DENSE uses the ordinary engine logits (including native normalization/softcap).
SELECTED_ROWS is checked explicitly; Gemma CPU currently rejects that mode.
Candidate logits and conditional probabilities are copied into wrapper-owned
arrays in original option order. Exact ties keep the engine's first-maximum
rule. Results/plans are borrowed until this instance's next score/reset/close;
copy for retention. A failed result has no selected ID/arrays, count zero and
index SIZE_MAX. Attempts/timing remain defined. Subsequent requests are usable.

Cancellation is observed before preparation, between tokenizer calls, before
scoring dispatch and after synchronous scoring. It cannot interrupt a running
engine kernel. Cancellation discards the output and is consumed; the next
request succeeds. Signal/other-thread cancellation must quiesce before close.

Four allocations per instance (handle, prompt, two checked token buffers) happen
at open. No wrapper allocation occurs during score/reset. Buffers are bounded by
input/token capacities, released at close, and observed by
`geistr_decision_resources_get`. These counters cover the **decision wrapper**,
not engine/KV/chat allocation. Sample with callers quiescent. Metal's provider
counter is `MTLDevice.currentAllocatedSize`, distinct from process RSS and
physical residency; CPU reports provider_known=false. Never add the counters.
`tools/stress_decisions.py` supplies a repeatable lifecycle fixture requiring a
frozen workload, time estimate and numeric RSS/Metal budgets. The 100-request,
three-model-cycle evidence gate has not been completed.

## CLI and Python

Use the existing binary and catalog/path lookup; neither execution nor listing
attempts a download:

```sh
geistr decide gemma4-e2b "Which team handles this?" billing technical sales
geistr decide gemma4-e2b --processor cpu \
  --question 'Which option equals 4?' --option four '4' --option five '5'
geistr decide model.gguf --config decisions.json --question-file - \
  --option yes 'Yes' --option no 'No'
geistr catalog --decision-config decisions.json --json
```

Without `--config`, every verified profile is enabled for its exact artifact
(the registry in `src/decision_profile.c`); other models are refused. With
`--config FILE`, only that file's policies apply, so a file can disable a
model; set a policy's enabled field to true explicitly. The short form takes
the question as the first argument that is not a flag and every further one as
an option ID that is also its description; it mixes with the flags.
`--question` and `--question-file FILE|-` are alternatives.
`--context`, `--mode dense|selected_rows`, `--profile NAME`, `--models` and
`--catalog` accept one value each. Repeated/conflicting flags are errors;
`--option ID DESCRIPTION` is the only repeatable flag. Profile override must
match the configured exact artifact. This first CLI limits model context to
512 tokens and refuses overflow. C/Python callers may choose a checked larger
capacity within the loaded model context.

In `geistr chat`, `/decide question | option | option …` scores the options
on the chat's model (each option ID is its own description; the conversation
is not part of the request) and prints the chosen ID and each option's
probability. The chat model is opened without a decision policy; the first
`/decide` looks up the verified profile for the model's file (catalog hash, or
the file's hash for a path), reopens the model with it as `/model` does (the
runtime hashes the file once) and keeps the decision instance for later
`/decide`s. A model without a verified profile is refused; a chat on a
service (`--socket`) has no `/decide`.

Success stdout is exactly the external ID plus LF when stdout is not a
terminal. On a terminal, each option's ID and selection probability follow,
one per line. Program-generated stderr JSON schema1 (written only when stderr
is not a terminal) includes profile/artifact/template hash, engine pin/version,
runtime version, actual backend, requested/resolved numeric mode, explicit
operation, prompt tokens, attempts and setup/preparation/scoring timing.
`until_result_ms` ends before stdout/teardown; it is **not whole command latency**.
Measure process entry through exit externally for that. No speed gate uses this
field. Errors return 1, usage/invalid supplied syntax 2, cancellation 130.
With `--decision-config`, offline catalog rows expose configured permission/profile and support
`not_loaded`, verified=false; this is no inference or numerical validation.

### `geistr decide-batch` (#164)

Several questions over one shared context, with the C API above. EXPERIMENTAL
and off by default like everything here: it needs `make geistr DECISION=1
DECISION_CONTEXT=1` and the model's permission, and `--shared-context` must be
given (V1 has no other operation). `geistr decide-batch --help` names both
requirements and says whether this build has them.

```sh
geistr decide-batch gemma4-e2b --config decisions.json --shared-context \
  --request-file request.json            # or --request-file - for stdin
```

```json
{"context": "…", "questions": [
  {"id": "q1", "question": "Which option equals 4?",
   "options": [{"id": "four", "description": "4"}, {"id": "five", "description": "5"}]}]}
```

The request is program input, at most 1 MiB (the serialized-request limit),
read bounded before parsing. Exactly these fields, each once, in any order;
the context may be empty. Missing, unknown or duplicate fields, other JSON
types, trailing input, raw controls in strings, `\u0000` and lone surrogates
are refused. 1–32 questions with 1–26 options; question IDs follow the option
ID rules and are unique; every question with the context meets the
single-question limits. All of that is checked before the model is loaded
(exit 2, naming the question). The context API then preflights every question
again (tokens, cached prefix, capacity) before any is scored.

| Flag | Default | Refused |
| --- | --- | --- |
| `--context-tokens N` | 512 (the C API's default); the model is loaded with this window | 0, non-decimal or above 2^32−1 (exit 2); above the loaded model's context (exit 1, before scoring) |
| `--snapshot-budget-mib N` | 256 | 0, non-decimal or overflow (exit 2); a larger snapshot (exit 1) |
| `--processor auto\|cpu\|gpu` | auto | anything else (exit 2) |
| `--profile NAME` | the policy's | another name than the artifact's policy (exit 1) |

`--models`, `--catalog`, `--config`, `--request-file` take one value each;
every flag at most once. DENSE and FP32 KV only: a `selected_rows` policy, a
disabled permission, a build without `DECISION_CONTEXT`, a backend without
snapshots or an unknown profile fail with exit 1 and nothing is guessed. No
download.

Only when every question succeeded does stdout get one line per question in
input order, `question_id<TAB>option_id<LF>`, staged in full and written once.
IDs contain no controls, so they cannot inject a tab or newline. A rejected,
failing or cancelled question anywhere in the batch leaves stdout empty; a
failed write (closed stdout, full disk) is exit 1 with `selection output
failed`, never success. Exit codes: 0, 1 execution failure, 2 usage or input
syntax, 130 Ctrl-C.

Once a model is loaded, the last stderr line is one JSON record (schema 1,
`operation` `decision_batch`, `exit`). It has the profile, artifact, template
and context hashes (the context hash is of the rendered prefix; the context
text, questions and descriptions are never logged), runtime and engine
revision/version, the actual backend, `kv_cache`, `activation` (null: not
reported by the engine), `mode`, `prefill_chunk`, `max_prompt_tokens`,
`cached_tokens`, `snapshot_bytes`, the budget, `setup_model_calls`, the
question, `prepared_questions` and `restores` counts, `failed_index` and
`failed_id`, and per question `prompt_tokens` and `uncached_tokens` (the
suffix after the restore). Times in ms, each ending at a synchronous return:
`load_ms` (configuration, catalog SHA check or hashing, model load),
`context_ms` (context open: tokenizing, prefix prefill, snapshot),
`preparation_ms`, `scoring_ms`, `batch_ms`, `until_result_ms` (command start to
the batch result), `output_ms`, `teardown_ms` and `total_ms` (command start to
after teardown). Context fields are absent when the context did not open.
`until_result_ms` is not command latency; whole-command claims are measured
from process start to exit outside the process (the tests check
`total_ms` ≤ that).

Tests: `tests/test_decide_cli.py` (in `make test`/`make sanitize`) drives the
parser and validation through the stub: flags, numeric bounds, every shape
refusal, Unicode and escaped IDs, reordered keys and whitespace, 32 questions,
26 options, the 1 MiB bound for file and stdin, and feature-off.
`make test-decision-cli DECISION=1 DECISION_CONTEXT=1` (`tests/test_decision_product_cli.py`)
adds the real model: ordered Unicode IDs and the record, stdin with other key
order, reversed questions and options, larger and too large capacity, a
middle question over capacity, the snapshot budget, a closed stdout,
permission/mode/profile refusals and Ctrl-C, after the ordinary `decide`
cases. Without `DECISION_CONTEXT` it checks the explicit refusal.

The installed wheel uses the same C implementation:

```python
import geistr
with geistr.DecisionConfig.from_file('decisions.json') as cfg:
    model = geistr.open('gemma4-e2b', processor='cpu', context=512,
                        decision_config=cfg)
with model.decision() as decision:
    result = decision.score('Which option equals 4?', [('four', '4'), ('five', '5')])
    print(result.external_id)
model.close()
```

Python results own copies of scores, external IDs and diagnostic IDs. Decisions
serialize score/reset/close while cancel remains callable from another thread.
Config/model lifetime is protected during child creation; policy data is copied.
There is no additional inference stack or ML dependency in the wheel.

## Evidence and known blocker

Independent native prompt/tokenizer fixtures are checksum-bound in
`tests/fixtures/decisions`. They use the embedded templates through
Transformers/Jinja and a separately compiled native tokenizer from clean Prism
commit `01ae597e3f7d4742909e1e831abb12fe3d24b2cf`. Manifests record the GGUF SHA,
source/binary/library hashes, special-token flags and reference command. Thinking
on/off controls, BOS, whitespace, Unicode and reordered options are frozen.
The helpers load metadata/vocabulary only for token fixtures.

Actual-wrapper tests compare identical loaded weights/backend/precision and
prompt/candidate IDs with direct geistlib calls. This establishes wrapper
identity; it does not establish independent model correctness. Gemma's separate
CPU native numerical oracle **FAILS** with the current engine pin: all three
fixtures exceed frozen atol=0.10/rtol=0.01; two of three choose another option.
Maximum candidate-logit differences are approximately 6.49, 3.52 and 7.13.
The failure and raw scores are retained in `gemma4_numeric.json` and
`gemma4_runtime_cpu.jsonl`; `tools/verify_decision_evidence.py` returns nonzero
for this known failure. No tolerance is widened.

Re-recorded on x86 at the current pin (#94, `gemma4_numeric_x86.json`, the same
reference outputs): maximum differences 2.03, 3.41 and 9.00; the first two now
choose the reference's option, the third does not, so the verdict stays FAIL.
The cause is isolated in geistlib#694: geistlib#697 fixed two cpu_x86
activation-scale bugs (the first fixture went from 6.49 to 2.03), cpu_neon has
the same pattern (geistlib#698), and the frozen atol 0.10 / rtol 0.01 cannot
hold across engines that quantize activations to int8: the llama.cpp reference
itself moves by up to 3.97 logits between batch sizes and changes its choice on
the third fixture. A new contract (a full-precision-activation reference; a
winner gate only where the reference's top-two gap is at least 3 logits) is
proposed there and needs a decision before Gemma numerical acceptance or
release eligibility.

Apple at engine `4afbfcd` (#17, [DECISION_EVIDENCE_17.md](DECISION_EVIDENCE_17.md)):
wrapper/CLI/Python controls and the paired 2 % overhead gate PASS on NEON and Metal for
both models; Gemma numerics: against the latest llama.cpp (v0.6.0; the oracle is always
the latest release, `tools/llama_oracle.py`) every backend picks the oracle's option,
logits within 1.3–2.4: **PASS** under the #94 contract, in force since 2026-10-09 (the
original atol 0.10 is still reported and fails)
(the old Prism oracle computed `order` differently, geistlib#728); the lifecycle passes on Metal and fails on NEON (geistlib#729).
Decisions use FP32 KV (measured free). Quality: a development pilot only.

`docs/DECISION_EVIDENCE_PLAN.json` freezes the development comparison;
`docs/DECISION_EVIDENCE_PLAN_17.json` the Apple runs. No paired overhead verdict, complete Apple lifecycle verdict,
MMLU quality eligibility, calibration, universal speed multiplier or Jev
-equivalence is claimed. The three toy questions are not an MMLU population.
Bonsai native numerical/held-out evidence stays owned by geistlib#587. Independent
Gemma parity is a blocker in runtime#17; the implementation remains a draft.

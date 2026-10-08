# Fixed-option decisions (V1)

The EXPERIMENTAL contract in `geistr_decision.h` keeps prompt policy in the
runtime and numerical execution in geistlib. Chat remains the model default.
A decision chooses exactly one supplied option; it does not produce free text,
JSON, a continuous rating or calibrated correctness/truth confidence.

## Permission and identity

Decision permission is absent/disabled by default. A model's options can point
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

## Execution, ownership and concurrency

Build the optional engine with `make runtime DECISION=1` (or `make geistr
DECISION=1 PULL=0`, `make wheel DECISION=1`). `DECISION=0` remains the default;
all decision symbols stay linkable and return explicit unsupported errors.
The engine is the Makefile's pin (`GEIST_REF`). The recorded validation evidence
(`tests/fixtures/decisions/validation`) was produced on engine
`5dd7e1747df86092a320e638c66993afd409e3b6` and is kept as recorded; it is not
re-established for later pins until it is recorded again.
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
geistr decide gemma4-e2b --config decisions.json --processor cpu \
  --question 'Which option equals 4?' --option four '4' --option five '5'
geistr decide model.gguf --config decisions.json --question-file - \
  --option yes 'Yes' --option no 'No'
geistr catalog --decision-config decisions.json --json
```

Set the intended artifact policy's enabled field to true explicitly; the sample
above is disabled. `--question` and `--question-file FILE|-` are alternatives.
`--context`, `--mode dense|selected_rows`, `--profile NAME`, `--models` and
`--catalog` accept one value each. Repeated/conflicting flags are errors;
`--option ID DESCRIPTION` is the only repeatable flag. Profile override must
match the configured exact artifact. This first CLI limits model context to
512 tokens and refuses overflow. C/Python callers may choose a checked larger
capacity within the loaded model context.

Success stdout is exactly the external ID plus LF. Program-generated stderr
JSON schema1 includes profile/artifact/template hash, engine pin/version,
runtime version, actual backend, requested/resolved numeric mode, explicit
operation, prompt tokens, attempts and setup/preparation/scoring timing.
`until_result_ms` ends before stdout/teardown; it is **not whole command latency**.
Measure process entry through exit externally for that. No speed gate uses this
field. Errors return 1, usage/invalid supplied syntax 2, cancellation 130.
With `--decision-config`, offline catalog rows expose configured permission/profile and support
`not_loaded`, verified=false; this is no inference or numerical validation.

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
for this known failure. No tolerance is widened. The cause is not yet isolated
between the pinned numerical implementations; an engine/reference investigation
is required before Gemma numerical acceptance or release eligibility.

`docs/DECISION_EVIDENCE_PLAN.json` freezes the development comparison and pending
performance gate. No paired overhead verdict, complete Apple lifecycle verdict,
MMLU quality eligibility, calibration, universal speed multiplier or Jev
-equivalence is claimed. The three toy questions are not an MMLU population.
Bonsai native numerical/held-out evidence stays owned by geistlib#587. Independent
Gemma parity is a blocker in runtime#17; the implementation remains a draft.

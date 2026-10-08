# Changelog

## Unreleased

- Add EXPERIMENTAL fixed-option request/configuration contracts in
  `geistr_decision.h`: default-off model policy, exact artifact binding,
  strict separate schema-1 configuration and bounded UTF-8 inputs.
- Append a copied decision policy to `geistr_model_opts`; previous option
  sizes work. Ordinary chat defaults and the schema-2 catalog are unchanged.
- Add checked memory SHA-256 for enabled borrowed-memory model policies.
- Add isolated decision scoring, capability/resource observation, cancellation
  and owned result storage with explicit unsupported-mode errors.
- Add `geistr decide`, offline permission inspection and ctypes decision bindings.
- Add independent native token/numerical fixtures. Gemma numerical acceptance
  is blocked by recorded candidate-logit/selection drift at the engine pin;
  no quality or speed eligibility is implied.

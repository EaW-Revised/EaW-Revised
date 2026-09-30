# Behaviour note template

Record only facts a consumer can implement or test. Keep evidence identifiers opaque and source locations relative. Do not include private paths, source excerpts, decompiler output, addresses, or pseudocode.

## Applicability

Name the product/build or data revision, the bounded question, and the source of each claim. Distinguish observed behavior, public-source behavior, project policy, and unknowns.

## Interface

State inputs, outputs, units, ordering, retained state, and invocation cadence where they affect the behavior.

## Rules

Give every rule a stable ID. State boundary comparisons, ties, defaults, failure behavior, and any required ordering directly.

## Cases

Use original synthetic inputs with an observable expected result. Give cases stable IDs and keep them smaller than the rules they illustrate.

## Unknowns

Name the unsupported case and its effect on consumers. Do not turn an inference into an implementation requirement.

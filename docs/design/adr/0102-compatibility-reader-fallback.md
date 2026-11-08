# ADR-0102 — Closed compatibility-reader fallback for native families

**Status:** Accepted (amends ADR-0065's "not a fallback for a native format" boundary).

## Context

The native glTF, OBJ, FBX and 3MF adapters are the default readers and have the
best validation, fidelity and security posture. They are also deliberately
strict: a valid file can still fail because it uses a required extension the
native adapter cannot represent, because a degenerate but readable face has no
native primitive, or because the native container preflight classifies it as
unsupported. ADR-0065 already ships a bounded Assimp compatibility adapter for
formats with no native reader, but explicitly forbids routing native families
through it.

T07 added a product reader table with scores and forced selection. T08 needs a
real compatibility path without turning Assimp into a blind universal retry or
weakening the native preflight.

## Decision

- The closed `AssimpFormat` discriminator gains `Gltf`, `Obj`, `Fbx` and
  `ThreeMf`. The hint is product-owned; no path, extension or user text reaches
  Assimp. The worker validates the value before selecting an importer.
- Four lower-scored readers — `gltf-assimp`, `obj-assimp`, `fbx-assimp`,
  `3mf-assimp` — declare the same extensions as their native family and
  `isCompatibility=true`. They reuse the existing worker sandbox and bounded
  Assimp adapter; they do not add a parser.
- Each new container gets product-owned admission before Assimp: a bounded
  JSON/GLB header and buffer-view preflight for glTF, bounded text admission
  for OBJ, binary/ASCII magic and header preflight for FBX, and the existing
  bounded OPC/ZIP preflight for 3MF (never Assimp's ZIP handling). The
  compatibility sidecar policy additionally allows only a glTF `.bin` buffer
  and an OBJ `.mtl` library; the broker repeats its canonical containment.
- The trusted broker owns one closed retry. It is eligible only for
  `UnsupportedFormat`, `UnsupportedRequiredFeature` and `EmptyGeometry`, only
  when the native reader was selected, and only before any batch has been
  published. It runs at most once per generation. Every security, containment,
  cancellation and resource failure is terminal. The retry copies the request
  and changes only the closed format/hint; the same worker executable,
  AppContainer, Job Object, budgets, source handle, sidecar policy and
  cancellation event are retained.
- A forced reader is an explicit choice and never triggers the automatic
  retry. If a forced native reader fails with an eligible error, the failure
  card offers **Try compatibility reader**, which forces the shipped alternate
  through the same classification and admission path.
- The broker records the native reader, compatibility reader and final reader
  as stable product names in the result diagnostic. The names never cross the
  process boundary.

## Consequences

- A native-reader representational gap now has a bounded, visible compatibility
  path. The native adapter remains the default and its security preflight is
  never reinterpreted by the compatibility reader.
- Assimp's attack surface grows by four container hints, so the existing
  `AssimpFuzz` dispatcher is extended to all eleven hints and the seed corpus
  covers each new container.
- The compatibility reader is not a fidelity promise: it is a static,
  bounded-best-effort route. Its output uses the native family's wire identity
  and unit/up-axis policy, and a failure of the compatibility attempt is final
  for the generation.
- ADR-0065's ban on using Assimp as a *universal* fallback remains: only the
  four named families, only the three eligible errors, and only one attempt.

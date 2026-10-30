---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T22 --configuration Debug --output TestResults/render-quality/T22
---
# T22 — Preserve authored material, UV, normal and AO semantics

## Goal

Stop losing or reinterpreting material inputs before they reach the shader, while preserving worker isolation and typed degradation.

## Context (read first)

- Depends on **T20, T21**; execute in roadmap order.
- `import-worker/src/GltfAdapter.cpp::ResolveMaterial`, texture resolution and tangent generation.
- `shared/model-core/include/model_core/MaterialPayload.h`, `WireFormat.h`, shared-section validator and hostile-worker tests.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp`, derived-cache identity; Khronos core glTF and transmission specifications.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Carry and apply normalTexture.scale, per-texture sampler/UV-set/transform metadata, and independent occlusionTexture sampling/strength. MR red is not AO unless an authored AO binding actually points there. Support the corpus's UV sets correctly; bound the supported set count and warn specifically on unsupported sets rather than aliasing them to UV0.
- [ ] Verify normal/tangent transforms for nonuniform and mirrored instances, transformed normal-map UVs, handedness, double-sided backfaces and missing tangents. Use a reference-compatible tangent basis or an evidence-backed equivalent; use front-face orientation rather than indiscriminately flipping a perturbed normal toward the eye.
- [ ] Preserve authored alpha coverage, alphaMode/cutoff, transmission and unlit semantics independently in normalized data. Audit the IOR≈1 transmission suppression and degenerate-BLEND recovery; check Suit and controlled IOR=1/tinted cases against Khronos. Preserve intentional compatibility conversions only with explicit, tested diagnostics; no model/material-name hacks.
- [ ] Version any wire/dependency/vertex-layout additions. Update every producer/consumer, validation, finite/range/index/size checks, hostile sections and cache/importer identities. Wire the actual parser/protocol identity into derived-cache keys; do not only bump an unused caller default.
- [ ] Keep color/emissive textures sRGB, data maps linear, scalar factors in specified spaces and decode shared images correctly when used by different semantics. Validate glTF alpha handling for OPAQUE/MASK/BLEND and unlit.
- [ ] Add generated cases with normal scale 0/0.45/1, independent AO versus non-AO MR red, UV1, different transforms/samplers per slot, mirrored tangents, alpha cutouts and IOR=1 transmission. Exercise both import isolation and actual GPU diagnostic images.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

## Done when

- [ ] T22 verification runs the new material-semantic import/unit/GPU lanes and relevant wire/hostile-worker/cache checks serially.
- [ ] Car/Camera/microscope/Suit before/after diagnostics explain each corrected input; Camera glass remains a reviewed nonregression case.
- [ ] Update material/wire/cache design, format-support limitations and hand-off.

## Out of scope

New PBR lobe texture slots (T25), BRDF recalibration (T24), transparency ordering (T26) or refraction/transmitted-scene rendering (T27).

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


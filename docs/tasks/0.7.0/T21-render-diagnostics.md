---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T21 --configuration Debug --output TestResults/render-quality/T21
---
# T21 — Material and light-contribution diagnostics

## Goal

Isolate why a region looks wrong before changing exposure or adding a light. Make imported values, actual GPU inputs and per-pass contributions inspectable.

## Context (read first)

- Depends on **T20**; execute in roadmap order.
- `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md` and T20 runner/report contract.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp` — PSMain, HDR resolve, material root constants and effective PSO selection.
- `interactive-viewer/src/render/Model.*`, `shared/model-core/include/model_core/MaterialPayload.h`.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Add bounded Debug inspection/capture channels for base color, geometric/shading normal, UV0/UV1, metallic, roughness, authored AO/strength, coverage alpha, transmission, IOR/F0, clearcoat and sheen. Mark unsupported/unbound channels explicitly rather than presenting neutral fallbacks as authored data.
- [ ] Add direct diffuse/specular, environment diffuse/specular, coat, emission and pre-tone-map HDR views, plus effective opaque/masked/blended/transmissive bucket and depth-write/cull state. Clamp visualization only at the display step; preserve numeric HDR readback.
- [ ] Generate a per-material diagnostic record linking source material/primitive IDs to validated wire values, bound texture/color-space/sampler/UV metadata and effective draw state. Read source metadata in the worker/test tooling; the viewer must not parse glTF.
- [ ] Make output channel selection and numeric pixel/region sampling part of T20's Debug capture route. Verify a small set of independently computed patches through actual GPU readback, not shader-token existence.
- [ ] Use Car windows/paint, Camera lens, Suit and a roughness/metal grid to record which contributions dominate. Keep the result as evidence; do not label hypotheses as proven causes without isolated comparisons.
- [ ] Keep shipping test-control rejection intact; inspection controls must not add a Release message/path authority surface.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

## Done when

- [ ] T21 verification covers diagnostic GPU outputs, material/texture identity and Release test-control rejection.
- [ ] A short cause-by-region report identifies the inputs to fix in T22–T27; images and numeric samples are included.
- [ ] Update rendering diagnostics documentation and hand-off.

## Out of scope

Material editing UI, altered default appearance, protocol expansion solely for convenient debug strings, or logging full user paths/model contents in Release.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


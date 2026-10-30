---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T25 --configuration Debug --output TestResults/render-quality/T25
---
# T25 — Texture-driven clearcoat, specular, sheen and transmission inputs

## Goal

Preserve authored variation in supported PBR extensions instead of rendering only their scalar factors.

## Context (read first)

- Depends on **T22, T24**; execute in roadmap order.
- T18 hand-off and format-support warnings for lobe maps.
- `GltfAdapter.cpp`, material/dependency wire schema, texture semantic/mip code and `D3D12ViewerPath.cpp`.
- Khronos clearcoat, specular, sheen and transmission extension specifications.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Add validated bindings and shader sampling for clearcoat factor/roughness/normal, specular factor/color, sheen color/roughness and transmission factor maps. Respect each extension's channel, color-space, factor multiplication, sampler and per-slot UV metadata from T22.
- [ ] Use a bounded descriptor/material layout; account for the 64-DWORD root-signature limit rather than appending unbounded root constants. Version wire/dependency/cache changes and update all producers, validator and hostile-worker coverage.
- [ ] Generate representative maps with nonconstant channels, mismatched dimensions, shared image/different color semantics and UV1 transforms. Verify imported values and GPU results; a uniform factor-only sphere cannot demonstrate map support.
- [ ] Exercise missing/invalid/oversized maps, cancellation, coarse-to-full residency, generation changes, fallback descriptors and device reset. Never upload an unvalidated reference or let a fallback descriptor masquerade as a resident authored map.
- [ ] Update extension compatibility claims only for paths now rendered; keep specific warnings for still-unsupported volume/anisotropy/iridescence/dispersion behavior. Preserve parsers/decoders in workers.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

## Done when

- [ ] T25 verification combines texture/lobe import, wire/hostile-section checks and actual GPU map-variation images.
- [ ] Textured coat/specular/sheen/transmission fixtures show the expected channels and no change to the no-extension baseline.
- [ ] Update format-support/material/streaming docs, provenance and hand-off.

## Out of scope

Anisotropy, iridescence, dispersion, material editing or a wholesale renderer replacement.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T32 --configuration Debug --output TestResults/render-quality/T32
---
# T32 — Scale-aware screen-space ambient occlusion

## Goal

Improve cavities and local contact on untextured/CAD and PBR models with restrained, geometry-based AO.

## Context (read first)

- Depends on **T24, T26, T27, T31**; execute in roadmap order.
- Current ORM AO shader and T16 ground hand-off.
- `D3D12ViewerPath.cpp` depth/normal/HDR resources and F3D SSAO pass construction.
- T20 cavity, contact, glass and thin-feature fixtures.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Implement a bounded SSAO/GTAO-class path with correct depth reconstruction, scene-scale-aware radius, edge-aware filtering and stable sampling. Use the existing projection/precision conventions rather than assuming one model size or perspective only.
- [ ] Apply AO to the appropriate indirect illumination; do not multiply all direct light, emission, glass reflection or the final tone-mapped image by an AO mask. Combine authored AO with screen-space AO without severe double darkening.
- [ ] Define transparent/masked handling in the pass graph so windows neither produce opaque AO sheets nor inherit background halos. Preserve narrow gaps, silhouettes, thin parts and the camera strap.
- [ ] Test perspective/orthographic, near/far scale, zoom/orbit, background/depth discontinuities, two-sided/mirrored instances, streamed geometry and alpha cutouts. Record AO-only, on/off crops and motion evidence.
- [ ] Calibrate separate sensible strengths/radii for geometry-review Clay and material-aware Studio/Directional while keeping controls/defaults understandable. Do not use AO to cover wrong normals, light transforms or missing shadows.
- [ ] Budget intermediate targets and work, include resize/device-loss/generation retirement, and document the unsupported/low-budget fallback. Measure GPU cost on recorded hardware.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

## Done when

- [ ] T32 verification includes AO-only correctness fixtures, halo/scale/motion checks and reviewed cavity/contact crops in all three modes.
- [ ] Glass/lens, black body, thin strap and emission remain nonregression regions; improvements do not come from globally darkening the scene.
- [ ] Update AO/pass/budget design and hand-off.

## Out of scope

Baked global illumination, ray tracing, temporal history without its required reprojection/lifecycle work, or unbounded resolution-independent AO allocations.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


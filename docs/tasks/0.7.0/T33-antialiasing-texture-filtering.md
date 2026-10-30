---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T33 --configuration Debug --output TestResults/render-quality/T33
---
# T33 — Edge, specular and texture sampling quality

## Goal

Remove jagged silhouettes and unstable highlights while retaining crisp textures and UI.

## Context (read first)

- Depends on **T24, T25, T26, T27, T31, T32**; execute in roadmap order.
- `D3D12ViewerPath.cpp` single-sample scene/pick targets, resolve and samplers.
- Texture semantic/mip generation and streaming code; F3D FXAA/SSAA pass construction.
- T20 thin-edge/orbit fixtures, Camera engraving, Car grille/windows, microscope bright rings.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Implement and justify a suitable default edge-AA path compatible with HDR, transparency and D3D12/D3D11On12 overlay ordering. Evaluate a sharper spatial solution or supported MSAA/resolve against FXAA; retain a deterministic supersampled reference for validation. TAA is optional only with complete reprojection/history handling.
- [ ] Handle R32_UINT picking separately with a nearest-visible/coverage-aware policy; never average instance IDs. Keep UI/text overlays crisp and outside scene AA. Verify transparency/OIT resolve ordering and mask/cutout coverage.
- [ ] Respect authored nearest/linear/mipmap samplers; add bounded anisotropic filtering where appropriate for oblique textures without overriding explicit nearest semantics.
- [ ] Audit mip generation by texture semantic: linear-light color reduction, normalized normal data, roughness/specular variance handling and coverage-preserving alpha cutouts. Reduce subpixel specular sparkle without globally blurring sharp materials or raising every material's authored roughness.
- [ ] Use stills and matched small-motion/orbit sequences at physical 1080p/1440p, multiple DPI and fractional resize. Inspect thin wires, grille, lens lettering, chrome edges, normal-map detail and glancing ground grids; global MAE alone is insufficient.
- [ ] Measure frame/memory costs and quality fallbacks; include target/sampler lifecycle, low-budget and device-loss tests.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

## Done when

- [ ] T33 verification includes AA/picking/mip invariants and reviewed edge/detail/motion comparisons against the supersampled reference.
- [ ] Aliasing/sparkle improves in every mode without blurry text, lost engravings, glass halos or changed cutout coverage.
- [ ] Update sampling/resolve design and hand-off.

## Out of scope

A postprocess blur used as a substitute for correct materials/normals, unbounded supersampling as the default, or claims of temporal AA without history correctness.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


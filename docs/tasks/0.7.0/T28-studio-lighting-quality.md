---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T28 --configuration Debug --output TestResults/render-quality/T28
---
# T28 — Calibrate Studio environment and first-open presentation

## Goal

Make Studio deliver balanced product-quality lighting on first open while preserving material identity.

## Context (read first)

- Depends on **T24, T25, T26, T27**; execute in roadmap order.
- `render/StudioEnvironment.*`, `Presentation.*`, `D3D12ViewerPath.cpp` direct rig and HDR resolve.
- F3D default HDRI/preset behavior and renderer pass construction.
- T20 reference conditions; T24 radiometric tests; Camera/Car/microscope/STL/STEP fixtures.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Preserve T21a's user-requested appearance: Windows Photos-style dark
  default, persisted Background choice and independent Ground highlight
  toggle. Studio presets may not replace these preferences or brighten the
  whole background through model exposure/tone mapping.
- [ ] Compare the corrected procedural environment with a product-owned, appropriately licensed studio HDR environment/bake. Choose a measured default with broad readable highlights, useful dark reflections and neutral diffuse response; do not assume a larger HDR file is automatically better.
- [ ] Set an explicit relationship between world-stable IBL and camera-relative fill/key lights. Avoid uncalibrated double illumination; preserve rotation/intensity/exposure semantics and verify white/gray/black patches under the chosen reference white.
- [ ] Calibrate exposure and highlight rolloff for white plastic/CAD, red paint, black leather/enamel, brass/chrome, emission and glass. Preserve color and texture detail rather than forcing every model to the same brightness.
- [ ] Revisit automatic presentation selection using stable scene/material contribution rather than raw material counts alone. Do not let a minor glass material unexpectedly force a checker or an untextured PBR mesh become a scan merely because it is dense. User overrides survive streaming and mode changes.
- [ ] Review first-open framing, background/ground/grid contrast, reflection-panel shape and useful object coverage. Also retain canonical fixed-camera comparisons so framing changes cannot hide shader regressions.
- [ ] Keep environment asset provenance, preprocessing/bake identity and bounded residency/startup cost explicit. User-supplied environment decoding remains brokered and isolated. Add a reproducible rebuild/reference comparison if shipping precomputed lighting data.
- [ ] Capture Studio default and controlled views for all critical corpus families, with Camera lens/Car windows mandatory nonregression regions; run the shared material invariants.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

## Done when

- [ ] T28 verification checks radiometry/preset stability and reviewed Studio images, including dark/light materials and glass.
- [ ] Record default-selection rationale, effective light/exposure settings and measured cost; no per-model-name tuning.
- [ ] Update presentation/environment design, asset licenses if applicable and hand-off.

## Out of scope

Clay/Directional art direction, replacing missing geometry shadows with a larger ellipse, or shipping third-party corpus assets.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T20 --configuration Debug --output TestResults/render-quality/T20
---
# T20 — Deterministic visual comparison harness and observed baseline

## Goal

Make rendering changes assessable with reproducible GPU images, close-ups and honest reference conditions. Capture the current defects before changing shading.

## Context (read first)

- Depends on **T19**; execute in roadmap order.
- `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md` — findings, reference versions and acceptance contract.
- `tests/app-smoke/studio.py`, `studio-presets.py`, `ground-shadow.py` — reusable capture/fixture code and current self-baselining weakness.
- `interactive-viewer/src/app/Preview3D.cpp` — Debug-only test control; do not expose it in Release.
- F3D installed/source preset `05_all.json`; `docs/0.7.0-QUALIFICATION.md`.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Create `tests/render-quality/verify.py`, a manifest-driven capture runner, generated fixture support and report schema. The task wrapper accepts `--task TNN --configuration Debug --output PATH`; later tasks register their own profiles. Unknown/unimplemented profiles, missing images, empty test selections, renderer errors and unsupported required capture capability fail with a nonzero exit.
- [ ] Separate first-open/default presentation from controlled-camera comparisons. Record source and binary hashes/versions, effective settings/config paths, GPU/driver/OS, physical viewport pixels, DPI, camera/FOV/projection, source-to-viewer transform, exposure/environment, model/sidecar hashes and residency/settled-frame status.
- [ ] Inventory every root asset in `D:/repos/binbuf/preview-3d-test-models`; resolve the two directory-shaped glTF assets to entry files. Seed actual captures for Car, Camera, microscope and one STL in Studio/Clay/Directional, including front and oblique views and lens/window close-ups. Record initial frame timings/memory on this hardware without claiming reference-hardware performance.
- [ ] Add canonical camera/viewport control through the existing Debug test surface as needed. Capture actual scene output after geometry/textures settle; mask or exclude UI, cursor, tooltips, loading overlays and selection. Include an orbit sequence. Do not compare differently framed full windows numerically.
- [ ] Render F3D with both its explicitly selected shipped preset and recorded controlled settings. Distinguish installed 3.5.0/VTK from the local 4.0.0 source. Establish a Khronos material-reference route for glass; F3D's gray Camera lens is not an accepted target.
- [ ] Write separate observed/known-defect baselines and accepted regression references. Verification never accepts its own new pixels. Add an explicit baseline-promotion operation with before/candidate/diff/crops, reason and image-review record. Missing accepted references fail their required regression lane.
- [ ] Prove the harness fails on missing references, missing corpus entries, blank/stale/proxy-only captures, changed camera, ignored lane and a deliberately damaged window/lens region. Establish an image-capable review route; current local Symphony vision is disabled, and provider settings must not be silently changed.

## Verification

The front-matter command is a planned interface introduced by T20. Create it in this task.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

## Done when

- [ ] The T20 wrapper runs its capture-contract checks and saves reviewed observed-baseline images plus a specific defect list; known existing defects remain labeled as such.
- [ ] Generated fixtures work without the external corpus; the external qualification lane reports unavailable assets as incomplete, not success. Fixture/screenshot redistribution rights and provenance are recorded.
- [ ] Document runner usage, profile extension, reference conditions and baseline promotion in `tests/render-quality/README.md`; fill the hand-off.

## Out of scope

No lighting/material fixes, new parsers, reference-engine embedding or automatic promotion of current rendering to a quality target.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


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

- [x] Create `tests/render-quality/verify.py`, a manifest-driven capture runner, generated fixture support and report schema. The task wrapper accepts `--task TNN --configuration Debug --output PATH`; later tasks register their own profiles. Unknown/unimplemented profiles, missing images, empty test selections, renderer errors and unsupported required capture capability fail with a nonzero exit.
- [x] Separate first-open/default presentation from controlled-camera comparisons. Record source and binary hashes/versions, effective settings/config paths, GPU/driver/OS, physical viewport pixels, DPI, camera/FOV/projection, source-to-viewer transform, exposure/environment, model/sidecar hashes and residency/settled-frame status.
- [x] Inventory every root asset in `D:/repos/binbuf/preview-3d-test-models`; resolve the two directory-shaped glTF assets to entry files. Seed actual captures for Car, Camera, microscope and one STL in Studio/Clay/Directional, including front and oblique views and lens/window close-ups. Record initial frame timings/memory on this hardware without claiming reference-hardware performance.
- [x] Add canonical camera/viewport control through the existing Debug test surface as needed. Capture actual scene output after geometry/textures settle; mask or exclude UI, cursor, tooltips, loading overlays and selection. Include an orbit sequence. Do not compare differently framed full windows numerically.
- [x] Render F3D with both its explicitly selected shipped preset and recorded controlled settings. Distinguish installed 3.5.0/VTK from the local 4.0.0 source. Establish a Khronos material-reference route for glass; F3D's gray Camera lens is not an accepted target.
- [x] Write separate observed/known-defect baselines and accepted regression references. Verification never accepts its own new pixels. Add an explicit baseline-promotion operation with before/candidate/diff/crops, reason and image-review record. Missing accepted references fail their required regression lane.
- [x] Prove the harness fails on missing references, missing corpus entries, blank/stale/proxy-only captures, changed camera, ignored lane and a deliberately damaged window/lens region. Establish an image-capable review route; current local Symphony vision is disabled, and provider settings must not be silently changed.

## Verification

The front-matter command is a planned interface introduced by T20. Create it in this task.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

## Done when

- [x] The T20 wrapper runs its capture-contract checks and saves reviewed observed-baseline images plus a specific defect list; known existing defects remain labeled as such.
- [x] Generated fixtures work without the external corpus; the external qualification lane reports unavailable assets as incomplete, not success. Fixture/screenshot redistribution rights and provenance are recorded.
- [x] Document runner usage, profile extension, reference conditions and baseline promotion in `tests/render-quality/README.md`; fill the hand-off.

## Out of scope

No lighting/material fixes, new parsers, reference-engine embedding or automatic promotion of current rendering to a quality target.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

### Implemented

- `tests/render-quality/verify.py` — manifest-driven task wrapper
  (`--task TNN --configuration Debug --output PATH`, `--no-build`,
  `--capture-only` for evidence collection only). It builds `Preview3D`, runs
  the capture-contract self-tests, inventories the corpus, generates fixtures,
  captures 53 T20 cases, renders F3D references, compares required accepted
  references and writes `report.json`, `captures.json`, `build.log` and images.
- `profiles/T20.json` — generated fixtures and external seed cases, canonical
  views, close-ups, orbit, regions, required capture capabilities and the
  specific defect list. Later tasks add a profile.
- `fixtures.py` — sphere grid, Khronos-style glass roles and STL box generated
  from code; no external corpus/rights dependency.
- `corpus.py` — every root asset inventoried; the two directory-shaped glTF
  assets resolve to `Camera_01_4k.gltf/Camera_01_4k.gltf` and
  `vintage_microscope_4k.gltf/vintage_microscope_4k.gltf`; entry + sidecar
  SHA-256s; absent corpus/assets report `incomplete`, never success.
- `compare.py` — capture contract, camera/viewport contract, global MAE/SSIM,
  per-region MAE, missing-reference failure and the explicit promotion command
  (before/candidate hashes, reason, reviewer, review record, layout).
- `selftest.py` — proves failure on missing references, missing corpus entries,
  blank captures, stale/proxy-only captures, changed camera, ignored/empty lane
  and a damaged window/lens region.
- Debug-only viewer controls in `Preview3D.cpp` / `Renderer.{h,cpp}` /
  `D3D11On12Overlay.{h,cpp}`: canonical yaw/pitch/distance/projection/immediate
  reset (fields 31/32/33/35/36), capture-clean overlay suppression (105), forced
  present (106) and capability mask (107). No Release test-control authority.
- `tests/render-quality/README.md`, ADR-0076 and the testing-strategy update.
- Committed observed/known-defect metadata:
  `baselines/observed/T20/observed.json` (53 cases), review
  `review/T20-review.json`; 15 generated-fixture accepted regression references
  under `baselines/accepted/`. External captures stay in ignored `TestResults/`
  (restricted corpus; Car metadata indicates CC-BY-NC-SA-4.0).

### Verification (actual)

```powershell
python tests/render-quality/verify.py --task T20 --configuration Debug --output TestResults/render-quality/T20
# status pass; exit 0; 53 captures; 15 comparisons; build exit 0; 4 F3D renders
```

Also checked: `--task T99` exits 1 with “unknown or unimplemented profile”.
Contract self-test: 8/8 expected failures detected. External lane: `available`
on this workstation (all four seed assets resolved). Generated-only run with a
synthetic absent corpus: 18/18 generated captures, no failures, external lane
`incomplete` (never success). F3D: Car and Camera each rendered with the shipped
`05_all.json` preset and recorded controlled settings; installed **3.5.0**/VTK
`9.6.1-1336-gdc285c2254`. Local 4.0.0 source commit
`7f32e1f3e02fecbcc0cc2ba74a97bcb18cc28e58` is recorded but has no built binary.
Khronos glass route recorded but `incomplete` (no sample viewer installed).
Provider/Symphony vision settings were not changed.

### Evidence and defects

First-open/default presentation and controlled views are separate cases. UI is
excluded by viewport cropping plus the Debug capture-clean flag. Observed
defects (also in the review record): Car windshield/side-glass white speckle and
flat roof/window depth; the generated Khronos-style tinted/rough/clear
transmission spheres collapse to nearly identical clear glass; delayed first
drawable frame (Car reported complete near 5 s but stayed background-only until
~15 s while texture counters were already stable); microscope ResourceLimit did
not reproduce (label retained); installed F3D gray Camera lens is not a target;
F3D preset 1920x1350 vs controlled 1280x900 is a recorded limitation. Accepted
references defend reviewed current behaviour, not quality.

Performance recorded on this hardware only (RTX 4080, driver 32.0.16.1714):
default-case load seconds Camera 8.4 / Car 18.1 / microscope 6.8 / base.stl 2.8;
accounted GPU ~37-170 MB; viewer private ~650-792 MB. No reference-hardware claim.

### Deviations / remaining work

- No local F3D 4.0.0 build and no Khronos sample viewer; both lanes are honestly
  incomplete rather than substituted.
- The Car first-draw wait is a workaround for complete-load readiness, not a
  rendering fix; T23 owns it (with the microscope ResourceLimit).
- T21-T35 add profiles/cases and fix the listed defects. T34/T35 complete the
  external matrix and make release/timing claims.

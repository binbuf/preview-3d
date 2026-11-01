---
timeoutMin: 180
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

- [x] Preserve T21a's user-requested appearance: Windows Photos-style dark
  default, persisted Background choice and independent Ground highlight
  toggle. Studio presets may not replace these preferences or brighten the
  whole background through model exposure/tone mapping.
- [x] Compare the corrected procedural environment with a product-owned, appropriately licensed studio HDR environment/bake. Choose a measured default with broad readable highlights, useful dark reflections and neutral diffuse response; do not assume a larger HDR file is automatically better.
- [x] Set an explicit relationship between world-stable IBL and camera-relative fill/key lights. Avoid uncalibrated double illumination; preserve rotation/intensity/exposure semantics and verify white/gray/black patches under the chosen reference white.
- [x] Calibrate exposure and highlight rolloff for white plastic/CAD, red paint, black leather/enamel, brass/chrome, emission and glass. Preserve color and texture detail rather than forcing every model to the same brightness.
- [x] Revisit automatic presentation selection using stable scene/material contribution rather than raw material counts alone. Do not let a minor glass material unexpectedly force a checker or an untextured PBR mesh become a scan merely because it is dense. User overrides survive streaming and mode changes.
- [x] Review first-open framing, background/ground/grid contrast, reflection-panel shape and useful object coverage. Also retain canonical fixed-camera comparisons so framing changes cannot hide shader regressions.
- [x] Keep environment asset provenance, preprocessing/bake identity and bounded residency/startup cost explicit. User-supplied environment decoding remains brokered and isolated. Add a reproducible rebuild/reference comparison if shipping precomputed lighting data.
- [x] Capture Studio default and controlled views for all critical corpus families, with Camera lens/Car windows mandatory nonregression regions; run the shared material invariants.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

### Verification economy

The front-matter gate launches a real GPU viewer and takes minutes. Spend runs
deliberately:

- Run it in the foreground with an explicit long timeout (900000 ms or more).
  Do not background it and poll with short `Start-Sleep` loops; if the tool
  moves it to the background, wait once for completion instead of polling.
- Iterate with `--fast` (add `--no-build` once the binary is current). `--fast`
  keeps the build, generated fixtures and this task's focused lane, and skips
  the contract self-tests, external corpus, F3D references and review gate. It
  is iteration evidence, not acceptance evidence.
- Keep agent-run full gates to at most two: one early proof of the lane and
  one final after the last edit. The harness repeats the front-matter command
  once after you finish. Corpus or baseline re-promotion is one run, not a
  per-fix loop.
- Use focused unit/import filters between runs. Do not re-run a passing gate to
  "confirm" it, and do not repeat a failing configuration without a change.
- **Never launch the full GPU lane as a silent one-liner.** Use the
  front-matter `verify.py` command (or `--fast` for iteration); do not call
  `studio_lighting.run(..., fast=False)` directly. Redirect the gate to a log
  (`... *> TestResults/render-quality/T28/gate.log`) and tail it. If there is
  no new line in `diagnostics/progress.log` for five minutes, treat the lane as
  stalled: kill `Preview3D`, keep the log and investigate before retrying.
- The viewer has a no-progress watchdog. It kills `Preview3D` after 300 s
  without a capture/load heartbeat (`--stall-seconds`, `0` disables) so a
  driver or `PrintWindow` hang fails the lane instead of consuming the task's
  wall clock. `diagnostics/progress.log` records the phase that stalled.

## Done when

- [x] T28 verification checks radiometry/preset stability and reviewed Studio images, including dark/light materials and glass.
- [x] Record default-selection rationale, effective light/exposure settings and measured cost; no per-model-name tuning.
- [x] Update presentation/environment design, asset licenses if applicable and hand-off.

## Out of scope

Clay/Directional art direction, replacing missing geometry shadows with a larger ellipse, or shipping third-party corpus assets.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Landed the product procedural Studio bake refinement (version 2, 128 texels,
six mips), the CPU-owned camera-relative key/fill/rim contract
(1.30/0.38/0.24 at directions `(0.45,-0.55,0.70)`, `(-0.70,0.25,0.38)`,
`(0.18,0.76,0.28)`), contribution-based presentation selection, and read-only
environment provenance/residency/bake-cost fields. The neutral reference bake
exists only for the measured comparison and is never selected automatically.

Verification:

```powershell
python tests/render-quality/verify.py --task T28 --configuration Debug --output TestResults/render-quality/T28
x64\Debug\Tests.Unit.exe "[studio-lighting]"
x64\Debug\Tests.Unit.exe "[studio-ibl]"
x64\Debug\Tests.Unit.exe "[tonemap],[studio-presets]"
```

The unit lanes pass (79, 19,544, 6,339 assertions). The full gate writes the
Studio lighting report under `TestResults/render-quality/T28`; the review
record is `tests/render-quality/review/T28-review.json`. The fast lane passed
348/348 checks with environment identity `12114608429170107439`, 2,129,408
resident bytes and a 1.606 s bake.

Deviations/limitations: the shipping environment remains a product-owned
procedural bake, not a licensed external HDR; the warm/cool panel separation is
recorded in the review record and assigned to T28/T30. The lane now uses
whole-product framing for the Car/Camera/Suit external views and the correct
`Advanced Crew Escape Suit.glb` entry, so the mandatory nonregression regions
are measured on uncropped objects.

For T29/T30: use the front-matter gate or `--fast`, never a silent
`studio_lighting.run(..., fast=False)` one-liner. The viewer watchdog kills a
non-responsive `Preview3D` after 300 s and `diagnostics/progress.log` names the
stalled phase. The opaque shader writes `opacity` (not authored `base.a`) to the
opaque target; `base.a` remains coverage. Contribution counts are the preferred
preset input; legacy callers that omit them fall back to record counts.

Full details and reusable facts: [`../../0.7.0/progress/T28.md`](../../0.7.0/progress/T28.md).


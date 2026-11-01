---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T27 --configuration Debug --output TestResults/render-quality/T27
---
# T27 — Scene-based transmission and tinted or rough glass

## Goal

Render glass as surface reflection plus transmitted scene color, with authored coverage and tint preserved independently.

## Context (read first)

- Depends on **T22, T24, T25, T26**; execute in roadmap order.
- Khronos KHR_materials_transmission, alpha coverage and optional volume semantics.
- `D3D12ViewerPath.cpp` transmission approximation and T26 pass/resource contract.
- Car black/orange/red BLEND glass; Camera's OPAQUE transmission lens; Suit IOR≈1 cases.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [x] Introduce a bounded scene-color transmission path in linear HDR, with roughness-aware filtered transmitted radiance, interface Fresnel/IOR and base-color tint. Keep coverage alpha separate from optical transmission; do not replace authored coverage with Fresnel.
- [x] Route OPAQUE transmissive materials through the appropriate render pass without rewriting their authored coverage meaning. MASK/cutout transmission, double-sided thin surfaces, environment fallback and emissive layers need explicit policies.
- [x] Define ordering/sampling for at least two overlapping transmissive layers and their opaque background in coordination with T26. Never sample a render target simultaneously bound for writing or claim screen-space fallback sees offscreen/occluded content it cannot contain.
- [x] Use zero-thickness/thin-surface behavior when no volume thickness is authored. Do not fabricate thickness from object bounds or material names. Volume absorption/refraction, if implemented, requires separately validated authored inputs; otherwise retain specific degradation warnings.
- [x] Test neutral, tinted, dark, rough, IOR=1, OPAQUE and BLEND-coverage glass over a checker and actual interior geometry; include coverage×transmission sweeps, multiple layers and edge/offscreen cases. Compare controlled fixtures with Khronos, not only F3D's lens.
- [x] Review Car windshield/side windows/tinted lights and Camera lens together on neutral, dark and light backgrounds. Keep the lens's reflection/depth advantage over the observed flat F3D disk; preserve Suit based on authored semantics and a recorded reference comparison.
- [x] Budget the scene-color hierarchy/intermediates and exercise resize, residency changes, cancellation, device loss and fallback. Report approximation limits honestly.

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

## Done when

- [x] T27 verification checks independent coverage/tint/transmission numeric invariants and GPU close-ups/orbit clips, plus pass/resource correctness.
- [x] Both Car and Camera pass the reviewed glass rubric; no asset-name exception or global opacity/exposure workaround is present.
- [x] Update material/transparency support claims, renderer design and hand-off.

## Out of scope

Path tracing, caustics, full dispersion and unsupported volume features silently presented as correct.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

### Implemented behavior

- **Transmission path.** After the opaque pass the finished
  `R16G16B16A16_FLOAT` scene is copied into an eight-level
  `transmission.sceneCopy`; a full-screen reduction builds mips 1..7 before any
  transmissive draw. The material shader (`gTransmissionSource`, t15, root
  parameter 18) samples the fragment's own screen position at
  `mip = round(roughness * 7)` and computes
  `transmitted = background * baseColor * transmissionFactor * (1 - F_ior)`.
  The studio environment in the transmitted direction is the fallback when the
  copy has no opaque coverage. No render target is sampled while bound for
  writing; there is no refraction offset and no offscreen claim.
- **Coverage separation.** BLEND keeps authored alpha; OPAQUE/MASK cover 1.
  The interim `base.a = baseAlpha*(1-T) + Fresnel*T` composite is removed.
  Debug channel 9 is effective coverage, 10 effective transmission, 22
  `(coverage, interfaceFresnel, authoredTransmission)`, 28 authored
  transmission, 31 transmitted radiance, 32 raw scene sample. During a
  diagnostic frame the snapshot keeps the last product frame (debug channels do
  not feed the snapshot).
- **Routing/policies.** OPAQUE transmissive complete-vertex materials join the
  transparent pass with their authored alpha mode untouched (8 such draws in
  the generated fixture). BLEND transmissive stays transparent. MASK
  transmission keeps its authored cutout and zero optical transmission. A
  double-sided transmissive surface shades the authored face the camera sees
  (single plane = one interface visible from both sides; closed shell = two
  bounded thin interfaces). Volume absorption/refraction remains a specific
  importer compatibility degradation; no thickness is fabricated.
- **Layering.** Each transmissive layer samples the opaque snapshot, never
  another transmissive layer. Two overlapping layers and the opaque background
  are covered by the fixture; the OIT composite stays order-independent
  (permutation MAE 0.0) while the nearest-visible debug sample resolves the
  front layer.
- **Budget/lifecycle.** Snapshot + mips is 12,582,912 bytes at 1280x900 and is
  included in `AccountedAllocationBytes`. `CreateSceneColorTarget` recreates it
  on resize/recovery and refreshes the model/pending-model descriptor. Forced
  allocation failure records reason 3, keeps coverage 1, suppresses
  transmission and still renders geometry. Debug fields 259-263 expose bytes,
  mip count, reason and OPAQUE-transmission draw count. Debug-only env switch:
  `PREVIEW3D_TEST_TRANSMISSION_ALLOCATION_FAILURE=1`.
- **Verification.** New `tests/render-quality/scene_transmission.py` plus
  `profiles/T27.json` and generated fixture `transmission-glass.glb`. It checks
  coverage/tint/transmission invariants (tint-normalised weight ~0.959 in every
  channel), rough-vs-smooth p90 contrast, IOR=1, OPAQUE routing, MASK cutout,
  the two-layer ordering contract, edge/offscreen, resize, cancellation,
  device loss and allocation failure. External checks sample a
  content-located transmissive point on Car and Camera and confirm Suit's 14
  IOR=1 materials stay transmission 0. `review/T27-review.json` records the
  inspected images and rubric. A rendered Khronos reference remains
  incomplete (no Sample Viewer installed); the lane records the thin-surface
  equation as a formula reference, not a rendered comparison.

### Deviations / deliberate limits

- No refraction, inter-layer transmission or volume thickness. Rough glass is a
  screen-space filtered sample, not refraction.
- A closed double-sided shell contributes both surfaces to OIT (bounded, not an
  exact volume); a single plane remains one interface. Back faces are not
  discarded (discarding made single authored planes disappear from behind and
  broke the T25 transmission-map probe).
- `textured_lobes.py` camera distance changed 1.35 -> 1.6 because the bottom
  transmission-map probe was clipped; T25 re-run passes. No T25 assertion was
  weakened.
- No baseline/reference promotion.

### Exact commands and results

```powershell
# viewer build
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' `
  Preview3D.slnx /t:Preview3D /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
# -> Preview3D.exe built

# unit build + focused lane
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' `
  tests/unit/Tests.Unit.vcxproj /p:Configuration=Debug /p:Platform=x64 `
  /p:SolutionDir=D:\repos\binbuf\preview-3d\ /m /v:minimal
.\x64\Debug\Tests.Unit.exe "[scene-transmission]"
# -> All tests passed (352 assertions in 6 test cases)

# full acceptance gate
python tests/render-quality/verify.py --task T27 --configuration Debug `
  --output TestResults/render-quality/T27
# -> status pass, 0 failures, 109 checks, 12 captures,
#    externalQualification available, F3D reference available,
#    review recorded. Report: TestResults/render-quality/T27/report.json

# focused lane standalone (DPI-aware, same as verify) -> pass, 0 failures
# T26 regression: layered_transparency.run(..., fast=True) -> pass
# T25 regression: textured_lobes.run(..., fast=True) -> pass
```

Serial lanes in the gate: `Tests.Unit.exe [scene-transmission]`,
`Tests.Unit.exe [material-lobes]`, `Tests.ImportIsolation.exe
[shared-section-validator]` — all pass.

### Remaining work

- Rendered Khronos Sample Viewer comparison (not installed). Recorded as
  incomplete/formula reference, not claimed as rendered.
- T28–T30/T33 should re-check tinted Car glass and the Camera lens under the
  calibrated lighting and antialiasing work; T34/T35 should add light/dark
  background and orbit close-ups of Car side windows/tinted lights.
- Optional future screen-space refraction/edge rejection and a second
  transmitted-layer ping-pong require separately validated inputs and must not
  be presented as volume refraction.
- The T20/T33 DPI-consistent capture transform remains a harness follow-up;
  T27 works around it with role-filtered interior samples.

### Changed docs

- `docs/design/adr/0085-scene-color-transmission.md` (new)
- `docs/design/04-rendering-and-streaming.md`
- `docs/design/03-file-formats-and-ingestion.md`
- `docs/FORMAT-SUPPORT.md`
- `docs/0.7.0/progress/T27.md` (new)
- `docs/tasks/0.7.0/T27-scene-transmission.md` (this hand-off)

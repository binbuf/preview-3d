---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T30 --configuration Debug --output TestResults/render-quality/T30
---
# T30 — Directional lighting, reflection and sun consistency

## Goal

Make Directional a controlled surface-inspection light with credible materials and a consistent visible sun direction.

## Context (read first)

- Depends on **T24, T27, T28, T29**; execute in roadmap order.
- `D3D12ViewerPath.cpp` Directional branch, clearcoat direct lights and coordinate transforms.
- `Preview3D.cpp` sun/gizmo azimuth/elevation, `render/GroundShadow.*`.
- T24 shared BRDF and T20 Directional scenes; observed bright Car trim/windows.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Use one coherent sun/light transform for base and coat BRDFs, light gizmo and subsequent shadow passes. Define/document the coordinate convention; prefer a world-anchored sun and transform it correctly into camera space as the camera moves.
- [ ] Remove leftover Studio coat/key directions and unweighted environment reflection. Keep a calibrated, Fresnel/BRDF-correct ambient reflection floor so metals remain visible without turning black paint/glass white.
- [ ] Make light intensity zero and environment intensity zero behave correctly, including transmission/coat; maintain predictable exposure between modes. Test light azimuth/elevation sweeps and grazing angles without clipping, NaNs or black discontinuities.
- [ ] Test camera orbit with a stationary sun, manual sun movement with a stationary camera and imported-up-axis changes. The gizmo, lit face, highlight and shadow input must agree.
- [ ] Review Car paint/window/trim, Camera lens/black body, microscope brass/enamel and CAD relief at several sun elevations. Use local contrast/highlight crops and motion stability; keep dark-side detail without a flat ambient wash.
- [ ] Preserve keyboard/accessibility and existing sun-control interactions; changing lighting mode must not reset user camera or persist an accidental per-asset light exception.

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

- [ ] T30 verification covers directional transform/energy GPU invariants, sun-control state and reviewed still/orbit/azimuth sequences.
- [ ] One documented light direction drives every Directional lobe and the T31 shadow interface; glass and black materials retain separation.
- [ ] Update lighting/gizmo design and hand-off.

## Out of scope

Additional user-facing light editors, multiple imported light authoring controls or shadows whose source disagrees with the visible sun.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


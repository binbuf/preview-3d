---
timeoutMin: 180
verify: python tests/render-quality/verify.py --task T31 --configuration Debug --output TestResults/render-quality/T31
---
# T31 — Geometry-derived ground and contact shadows

## Goal

Ground models using their geometry and light direction instead of a broad projected-bounds blob.

## Context (read first)

- Depends on **T26, T27, T28, T29, T30**; execute in roadmap order.
- `render/GroundShadow.*` and bounds-ellipse ground shader.
- `D3D12ViewerPath.cpp` render passes/resources and mode light transforms.
- F3D renderer shadow pass construction; T20 ground/strap/assembly fixtures.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [x] Preserve T21a's independent Ground highlight preference. The chosen
  background stays fixed; highlight off retains contact shadows, and
  shadows off does not force the highlight off. Keep grid state separate.
- [x] Implement a bounded geometry-derived shadow/contact path, such as a fitted shadow map with stable filtering, integrated into linear HDR. Use verified scene bounds for allocation/frustum setup, not as the shadow silhouette.
- [x] Derive Directional shadow direction from the same sun as T30 and a documented Studio/Clay key. Tune contact, penumbra, depth/normal bias and stabilization across tiny/large models and camera movement.
- [x] Test holes and disconnected parts: camera strap loop, car tire contacts, open STEP frame, STL cavities, a suspended part and point/line-only content. Preserve empty space instead of painting an ellipse across the entire assembly.
- [x] Respect alpha cutouts, double-sided/mirrored geometry and transparent casters. Clear glass must not cast an unexplained solid black silhouette; any approximate transmission-shadow attenuation is bounded and documented.
- [x] Keep shadow and grid toggles independent; a hidden grid does not remove grounding. Avoid z-fighting, plane seams, below-model light leaks and excessively broad floor darkening. Ground/background blending must survive exposure/mode changes.
- [x] Account for shadow targets in the GPU budget and retirement lifecycle. Test resize, generation change, cancellation, streamed geometry updates, device loss and a documented lower-quality fallback.

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

- [x] T31 verification includes silhouette/contact/bias tests and reviewed same-camera shadow on/off images in all three modes.
- [x] Camera strap holes and car tire contact are visible; timing/memory and fallback behavior are recorded without relaxing existing budgets.
- [x] Update ground/shadow design and hand-off; label the bounds ellipse only as a fallback if retained.

## Out of scope

Ray-traced soft shadows/caustics, full scene editing, or calling authored ORM texture occlusion a geometry shadow.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

### Implemented

- Fitted 2048² orthographic light depth map (1024² allocation fallback) plus an
  R8 caster-attenuation target, fitted from verified world bounds and the same
  world sun/key direction as shading. The centre is texel-snapped, grazing
  light is clamped, and the normalized depth bias is scale-invariant.
- Depth-only caster pass: MASK cutouts keep authored alpha test, mirrored and
  double-sided geometry culls nothing, BLEND coverage/decal surfaces are not
  occluders, and transmissive surfaces contribute the bounded approximation
  `clamp(1 − 0.60 × transmission, 0.40, 1)` instead of a solid black
  silhouette.
- Ground layer samples the fitted map with a comparison sampler and 3×3 PCF,
  writes premultiplied black coverage into linear HDR, and composites over the
  display backdrop. Pixels outside the fitted map are lit. Empty space and
  disconnected parts remain empty.
- Ground highlight stays the independent display-space backdrop layer; grid,
  shadow and highlight toggles are independent. Point/line-only content has no
  triangle caster and gets no ellipse.
- Shadow resources are per-device, accounted in `AccountedAllocationBytes`,
  not viewport-sized (resize keeps the same allocation), released/recreated
  for device recovery, and fall back to the labelled analytic bounds ellipse
  only on allocation failure or the Debug force switch.
- Debug fields 279–286 (mode, fallback, bytes, caster count, extent, pass time,
  forced fallback) stay inside the existing Debug test-control guard.
- New `contact-shadow-probe` fixture, `contact_shadows.py` lane,
  `profiles/T31.json`, T31 registration in `verify.py`, unit silhouette/bias/
  policy tests and `tests/render-quality/review/T31-review.json`.

### Verification

- Viewer Debug build: MSBuild `Preview3D.slnx /t:Preview3D` exit 0.
- Unit build: `Tests.Unit.vcxproj` exit 0. `Tests.Unit.exe "[ground-shadow]"`
  305 assertions / 11 cases; `"[directional-light]"` 380 assertions / 5 cases.
- Front-matter gate:
  `python tests/render-quality/verify.py --task T31 --configuration Debug --output TestResults/render-quality/T31`
  **pass**, 29 captures, external qualification `available`, F3D reference
  `available`, review `recorded`, no failures.
- Focused lane: `... --fast --no-build` at
  `TestResults/render-quality/T31-dev6` passes (15 captures); iteration
  evidence only.
- Generated same-camera pairs: ~16k changed pixels (~1.8 % of viewport), mean
  darkening 21.7–21.8, 7–8 connected regions, one enclosed ring/cutout hole,
  clear-panel component ~7.8 vs opaque 23.0. Grid independence 6,140 px;
  highlight independence 832,016 px; exposure 0.5/2.0 ratio 1.00.
- Shadow allocation 20,971,520 bytes at 2048²; caster pass 67–103 µs; whole
  accounted GPU bytes 96,600,064 for the contact fixture. Resize keeps the
  same allocation; a superseded fixture→STL request publishes only the second
  generation and rebuilds from its casters; device loss recovery 0→1 and map
  mode 1; Debug fallback mode 2 (broad ellipse ~23 % coverage) returns to mode
  1 when cleared.
- External reviewed: Car Studio low/oblique shows tyre contact patches; Camera
  Studio/Clay/Directional shows body/lens/strap shadow; STL and STEP keep
  discrete shadows. No accepted baseline promoted.

### Deviations / limitations

- The analytic bounds ellipse is retained only as the labelled allocation
  fallback (ADR-0088); it is not the shipping path for triangle content.
- The external Camera strap casts a visible shadow, but this asset's draped
  strap did not produce a fully enclosed external hole at the reviewed view.
  The generated suspended ring and MASK cutout sheet are the controlled
  hole-preservation evidence; T34 may add a top-down strap close-up.
- Transmission shadowing is an approximation, not caustics/volume absorption.

### Remaining work

- None in T31 scope. Follow-ups are recorded in
  `docs/0.7.0/progress/T31.md` (T32 AO separation, T33 AA edge stability,
  T34/T35 external strap close-up and release-hardware timing).

### Changed docs

- `docs/design/adr/0088-geometry-derived-fitted-shadow-map.md` (new)
- `docs/design/adr/0072-analytic-ground-shadow-and-orm-ao.md` (status note)
- `docs/design/04-rendering-and-streaming.md`
- `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`
- `docs/0.7.0/progress/T31.md`
 Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


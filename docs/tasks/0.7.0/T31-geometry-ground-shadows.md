---
timeoutMin: 300
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

- [ ] Implement a bounded geometry-derived shadow/contact path, such as a fitted shadow map with stable filtering, integrated into linear HDR. Use verified scene bounds for allocation/frustum setup, not as the shadow silhouette.
- [ ] Derive Directional shadow direction from the same sun as T30 and a documented Studio/Clay key. Tune contact, penumbra, depth/normal bias and stabilization across tiny/large models and camera movement.
- [ ] Test holes and disconnected parts: camera strap loop, car tire contacts, open STEP frame, STL cavities, a suspended part and point/line-only content. Preserve empty space instead of painting an ellipse across the entire assembly.
- [ ] Respect alpha cutouts, double-sided/mirrored geometry and transparent casters. Clear glass must not cast an unexplained solid black silhouette; any approximate transmission-shadow attenuation is bounded and documented.
- [ ] Keep shadow and grid toggles independent; a hidden grid does not remove grounding. Avoid z-fighting, plane seams, below-model light leaks and excessively broad floor darkening. Ground/background blending must survive exposure/mode changes.
- [ ] Account for shadow targets in the GPU budget and retirement lifecycle. Test resize, generation change, cancellation, streamed geometry updates, device loss and a documented lower-quality fallback.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

## Done when

- [ ] T31 verification includes silhouette/contact/bias tests and reviewed same-camera shadow on/off images in all three modes.
- [ ] Camera strap holes and car tire contact are visible; timing/memory and fallback behavior are recorded without relaxing existing budgets.
- [ ] Update ground/shadow design and hand-off; label the bounds ellipse only as a fallback if retained.

## Out of scope

Ray-traced soft shadows/caustics, full scene editing, or calling authored ORM texture occlusion a geometry shadow.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


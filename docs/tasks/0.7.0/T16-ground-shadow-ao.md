---
timeoutMin: 300
verify: x64\Release\Tests.Unit.exe "[ground-shadow]"
---
# T16 — Ground contact shadow and ambient occlusion

## Goal
Studio objects sit on a visible ground with a credible contact shadow instead of
floating over a grid, and mesh cavities are darkened by AO. CAD/untextured
presets get the effect by default; PBR and point-cloud presets can opt in. The
result is a product-like presentation for STL/STEP/3MF and much better form
readability for scans.

## Context (read first)
- `docs/proposal.md` §3.9 — ground contact shadow/AO design.
- T14/T15 — HDR scene target, IBL/tonemap, background and presets.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:1167-1168, 1249-1275` — fixed
  clear color and grid; no ground plane or shadow.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:202-401` — depth/material
  shader, including the unused ORM R channel.
- `interactive-viewer/src/graphics/D3D12Device`, `D3D12UploadRing`,
  `D3D12CommandQueue` — PSO/resource/submission patterns.
- `interactive-viewer/src/render/Renderer.{h,cpp}` — camera/target and grid
  state.
- F3D reference: `vtkF3DRenderer.cxx:519-545` (shadow/SSAO pass options) and
  `vtkF3DOpenGLGridMapper` (grid shadow/reflection look).

## Scope
- [x] Add a ground/shadow layer independent of the grid:
      - first slice: an analytic soft contact/blob shadow based on verified
        model bounds and the key direction; no extra pass;
      - proper slice: a 1024/2048 shadow map rendered from the key light,
        sampled on a ground plane with PCF and depth bias. Choose one path in
        this task and document the tradeoff.
- [x] Ensure the ground plane is below the model’s verified bounds, follows the
      active ground-axis transform, and does not z-fight with the grid.
- [x] Enable the shadow by default for Studio CAD and optional for Studio
      PBR/Scan; disable for point clouds unless the preset says otherwise.
      Add a keyboard-accessible toggle and persist it with the existing
      presentation/settings state.
- [x] Use the ORM R channel (T12) as ambient occlusion in the material shader.
      If the source provides `occlusionStrength`, apply it; otherwise use 1.0.
      Keep AO out of direct specular.
- [x] Optional SSAO (deferred to follow-up per the task allowance; the HDR path
      does not yet expose depth/normals and the contact shadow must not depend
      on a quality pass):
      - add only if the HDR/post-process path from T15 makes it clean;
      - make it a quality option, not the default;
      - use the scene depth/normal (or reconstruct normals) with a bounded
        radius and a deterministic kernel.
      If SSAO is deferred, record it as follow-up rather than blocking the
      contact shadow.
- [x] Add a checkerboard/gradient backdrop for transparent assets if T15 did
      not already land it; coordinate so there is one background implementation.
- [x] Add unit tests `[ground-shadow]`:
      - shadow plane placement from bounds/ground axis;
      - light-space projection/bias math;
      - AO sampling formula;
      - preset/toggle behavior.
- [x] Add screenshot goldens:
      - a CAD part with/without shadow;
      - a sculpt/scan;
      - a transparent asset on the checkerboard;
      - a point cloud with shadow disabled.
- [x] Update `docs/design/04-rendering-and-streaming.md` and the help/tooltips.

## Out of scope
- Full ray-traced soft shadows or contact-hardening area lights.
- Ground reflection (F3D’s grid reflection) unless it falls out cheaply; keep
  the task focused.
- Material lobes (→ T18).
- A global lighting-editor UI.

## Design notes
- Shadow acne/peter-panning is the main risk: use a normal/depth bias, a
  stable light-space projection, and test at millimeter and kilometer scales.
- The grid is not a ground plane. Do not attach the shadow to the grid shader;
  users may hide the grid and keep the shadow.
- A blob shadow is acceptable as a first slice if it is clearly soft, bounded
  and does not intersect the model. Do not ship a hard black ellipse.
- AO from the ORM map is cheap and should land even if SSAO is deferred.
- Keep the HDR/tone-map ordering: shadow/AO are scene lighting, not a
  post-process overlay.
- Do not let a missing AO texture fail the model; default to 1.0.

## Done when
- [x] `x64\Release\Tests.Unit.exe "[ground-shadow]"` passes.
- [x] A CAD/STL fixture has a soft contact shadow and no floating appearance;
      the shadow follows the ground-axis transform.
- [x] Toggling the grid does not remove the shadow, and toggling the shadow does
      not affect the grid.
- [x] ORM AO darkens cavities in a material fixture; direct specular is not
      incorrectly darkened.
- [x] Point clouds/transparent assets have sane defaults.
- [x] Screenshot goldens and frame-time evidence are recorded.
- [x] Hand-off records the chosen shadow path, bias parameters and deferred SSAO
      status.

## Hand-off

**Shadow technique.** Chose the analytic soft contact footprint (first slice),
not a shadow map: no extra pass, target or GPU allocation. ADR-0072 records the
tradeoff. `GroundShadow.{h,cpp}` transforms the verified bounds with the active
ground-axis matrix, projects the eight corners along the stable key
`(0.45, -0.55, 0.70)`, biases the footprint 15 % back toward the base and adds a
0.60 x radius rim. `D3D12ViewerPath` draws a six-vertex quad before the grid and
model into the linear HDR target. It writes no depth and shares no grid state.

**Ground placement / bias.** Plane z = `min.z - 0.0015 * radius`; the grid
remains at `min.z - 0.012 * radius`, so the layers cannot z-fight. The ground
plane follows the ground-axis transform and native-orientation bypass exactly
like the grid. There is no PCF or shadow-map bias because there is no depth
pass. Opacity 0.78, softness 0.50, light elevation clamped to 0.08, footprint
clamped to 6 x radius. The plane never intersects the model.

**ORM AO.** `OcclusionFactor(sample, strength) = saturate(1 + strength *
(sample - 1))`; missing maps sample 1.0. AO scales indirect diffuse, environment
reflection and ambient only; all direct `EvaluateLight` specular calls are
unoccluded. `MaterialPayload` has no `occlusionStrength`, so the shader passes
the glTF default 1.0 (recorded as a follow-up).

**Toggles/persistence.** `H`, the More menu item and test-control fields 101/102
toggle the layer; the grid uses fields 103/104 and keeps its own toggle. A
manual shadow choice pins across later preset applications and persists as
`groundShadowEnabled` in ViewerSettings v7. Studio CAD/PBR/Scan default on;
Transparent/Point cloud default off. SSAO is deferred (quality option, never a
dependency of the contact shadow). The checkerboard remains the single T15
background implementation.

**Goldens / performance.** `tests/app-smoke/ground-shadow.py` captures CAD
shadow on/off, CAD grid on/off with the shadow still on, Scan/Sculpt,
transparent-on-checkerboard and point-cloud-no-shadow. Second run: every golden
<= 8.1e-5 MAE and >= 0.9993 SSIM; transparent and point-cloud captures were
bit-identical. Debug mean frame 36.9 ms, p95 292.0 ms, accounted GPU 36,765,696
bytes, private 636,329,984 bytes at 210 frames. Evidence in
`docs/0.7.0/progress/T16-evidence/`.

**Checks.** `x64\Release\Tests.Unit.exe "[ground-shadow]"` -> 5 cases / 127
assertions passed. Related filters `[texture-ao],[presentation],[studio-presets],
[tonemap],[studio-ibl]` -> 21 cases / 19,065 assertions passed. Full suite has
only the two pre-existing stale failures (`dae`, `model.iges`).

**Docs.** `docs/design/04-rendering-and-streaming.md`,
`docs/design/adr/0072-analytic-ground-shadow-and-orm-ao.md`,
`docs/0.7.0/progress/T16.md`, `interactive-viewer/lang/en.json` and the Controls
dialog. Remaining work is the recorded follow-ups (SSAO, occlusionStrength,
optional shadow-map slice); none blocks this task.

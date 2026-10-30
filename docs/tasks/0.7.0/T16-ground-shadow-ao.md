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
- [ ] Add a ground/shadow layer independent of the grid:
      - first slice: an analytic soft contact/blob shadow based on verified
        model bounds and the key direction; no extra pass;
      - proper slice: a 1024/2048 shadow map rendered from the key light,
        sampled on a ground plane with PCF and depth bias. Choose one path in
        this task and document the tradeoff.
- [ ] Ensure the ground plane is below the model’s verified bounds, follows the
      active ground-axis transform, and does not z-fight with the grid.
- [ ] Enable the shadow by default for Studio CAD and optional for Studio
      PBR/Scan; disable for point clouds unless the preset says otherwise.
      Add a keyboard-accessible toggle and persist it with the existing
      presentation/settings state.
- [ ] Use the ORM R channel (T12) as ambient occlusion in the material shader.
      If the source provides `occlusionStrength`, apply it; otherwise use 1.0.
      Keep AO out of direct specular.
- [ ] Optional SSAO:
      - add only if the HDR/post-process path from T15 makes it clean;
      - make it a quality option, not the default;
      - use the scene depth/normal (or reconstruct normals) with a bounded
        radius and a deterministic kernel.
      If SSAO is deferred, record it as follow-up rather than blocking the
      contact shadow.
- [ ] Add a checkerboard/gradient backdrop for transparent assets if T15 did
      not already land it; coordinate so there is one background implementation.
- [ ] Add unit tests `[ground-shadow]`:
      - shadow plane placement from bounds/ground axis;
      - light-space projection/bias math;
      - AO sampling formula;
      - preset/toggle behavior.
- [ ] Add screenshot goldens:
      - a CAD part with/without shadow;
      - a sculpt/scan;
      - a transparent asset on the checkerboard;
      - a point cloud with shadow disabled.
- [ ] Update `docs/design/04-rendering-and-streaming.md` and the help/tooltips.

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
- [ ] `x64\Release\Tests.Unit.exe "[ground-shadow]"` passes.
- [ ] A CAD/STL fixture has a soft contact shadow and no floating appearance;
      the shadow follows the ground-axis transform.
- [ ] Toggling the grid does not remove the shadow, and toggling the shadow does
      not affect the grid.
- [ ] ORM AO darkens cavities in a material fixture; direct specular is not
      incorrectly darkened.
- [ ] Point clouds/transparent assets have sane defaults.
- [ ] Screenshot goldens and frame-time evidence are recorded.
- [ ] Hand-off records the chosen shadow path, bias parameters and deferred SSAO
      status.

## Hand-off
_(filled in by the implementing session: shadow technique, ground placement,
bias/PCF settings, AO formula, toggles, goldens and performance.)_

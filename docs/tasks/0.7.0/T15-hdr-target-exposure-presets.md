---
timeoutMin: 300
verify: x64\Release\Tests.Unit.exe "[tonemap],[studio-presets]"
---
# T15 — HDR target, exposure, background and material-aware presets

## Goal
Studio renders into an HDR scene target and resolves it with a full-screen
neutral tone-map/sRGB pass, so alpha, transmission and specular highlights
composite correctly and exposure is controllable. The viewer selects a small,
stable presentation preset from the format/material summary (Studio PBR, Studio
CAD, Scan/Sculpt, Transparent, Point cloud) with a manual override.

## Context (read first)
- `docs/proposal.md` §3.6 — HDR target, neutral tone map, background and
  material-aware presets.
- T14 — IBL, prefiltered specular and the first tone curve.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:1150-1213` — current
  back-buffer/depth/pick-target setup.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:1318-1326` — alpha sort; the
  current tone map is applied per object before blending.
- `interactive-viewer/src/graphics/D3D11On12Overlay.{h,cpp}` — UI compositing
  after the 3D pass.
- `interactive-viewer/src/graphics/D3D12SwapChain`, `D3D12Device`,
  `D3D12CommandQueue` — resource/PSO/resize ownership.
- `shared/model-core/include/model_core/FormatCatalog.h` — presentation
  profiles from T06.
- `shared/model-core/include/model_core/MaterialPayload.h` — material summary
  inputs (metallic/roughness/alpha/unlit/transmission/texture flags).

## Scope
- [ ] Add an HDR scene color target (e.g. `R16G16B16A16_FLOAT`) plus depth and
      the existing pick target. Render geometry in linear HDR; do not call the
      display transform in the object shader.
- [ ] Add a full-screen resolve pass:
      - neutral PBR tone mapping by default, with optional ACES/Filmic/None;
      - exposure control (keyboard-accessible + settings/persisted or
        per-session);
      - sRGB output to the swap-chain back buffer;
      - background compositing before the final transfer (or in the same pass).
- [ ] Preserve resize/minimize/device-loss behavior and UI overlay ordering.
      The D2D/D3D11On12 overlay remains after the 3D resolve.
- [ ] Add background modes:
      - dark UI-matched (current default);
      - neutral mid-grey studio;
      - transparent (where the shell/screenshot path can use it);
      - checkerboard for transparent assets if T16 does not own it.
- [ ] Add material-aware preset selection:
      - compute a bounded summary from imported materials/geometry (has
        base-color texture, metallic/roughness range, alpha/transmission share,
        unlit, point-only, CAD format);
      - map to Studio PBR, Studio CAD, Scan/Sculpt, Transparent or Point cloud;
      - apply environment intensity, key/fill/rim, exposure, background and
        grid/ground defaults;
      - manual override wins and persists for the session.
- [ ] Keep the Studio intensity control scaling direct + IBL together (as F3D
      does with `render.light.intensity`/`IBLIntensity`).
- [ ] Add unit tests `[tonemap]` and `[studio-presets]`:
      - tone-map monotonicity/white point/NaN handling;
      - exposure identity;
      - preset mapping for representative material summaries;
      - transparent/point-cloud selection;
      - background mode selection.
- [ ] Add screenshot goldens for each preset and a transparent asset. Reuse the
      `tests/app-smoke` window-capture path; record MAE/SSIM tolerance.
- [ ] Update `docs/design/04-rendering-and-streaming.md` and the user-facing
      settings/help text.

## Out of scope
- Ground contact shadow/SSAO (→ T16; coordinate the checkerboard if T16 owns it).
- Material lobes (→ T18).
- A full color-management/ICC system.
- HDR display output; the target is SDR presentation with an HDR intermediate.
- Post-process bloom/DOF/anti-aliasing; the target enables them later.

## Design notes
- Tone mapping after blending is mandatory for correct transparency/emission;
  do not keep per-object tone mapping as the default.
- The HDR target doubles color memory; keep the existing budget accounting and
  fail/fallback cleanly if allocation fails.
- Preset selection is a pure function over bounded summary data. Do not let a
  malicious material table force an expensive path; cap counts/loops.
- Exposure is a view setting, not a material mutation. Do not change imported
  factors.
- The background must be composited in the same color space as the scene; a
  dark background should not hide a dark object’s silhouette, so keep a subtle
  rim/backdrop where the preset calls for it.
- Keep the feature-level 11_0 path; a float render target is supported there.

## Done when
- [ ] `x64\Release\Tests.Unit.exe "[tonemap],[studio-presets]"` passes.
- [ ] HDR resolve/tone map works at all supported sizes; resize/minimize/device
      recovery still work.
- [ ] Alpha/transmission composites correctly against the background.
- [ ] Studio PBR/CAD/Scan/Transparent/Point presets select and can be
      overridden; exposure and background are usable.
- [ ] Screenshot goldens exist and are stable; frame time/memory are recorded.
- [ ] No import/parser behavior changed.

## Hand-off
_(filled in by the implementing session: target formats, resolve PSO, tone-map
options, preset mapping, background modes, goldens and performance evidence.)_

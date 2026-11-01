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
- [x] Add an HDR scene color target (e.g. `R16G16B16A16_FLOAT`) plus depth and
      the existing pick target. Render geometry in linear HDR; do not call the
      display transform in the object shader.
- [x] Add a full-screen resolve pass:
      - [x] neutral PBR tone mapping by default, with optional ACES/Filmic/None;
      - [x] exposure control (keyboard-accessible + settings/persisted or
        per-session);
      - [x] sRGB output to the swap-chain back buffer;
      - [x] background compositing before the final transfer (or in the same pass).
- [x] Preserve resize/minimize/device-loss behavior and UI overlay ordering.
      The D2D/D3D11On12 overlay remains after the 3D resolve.
- [x] Add background modes:
      - [x] dark UI-matched (current default);
      - [x] neutral mid-grey studio;
      - [x] transparent (where the shell/screenshot path can use it);
      - [x] checkerboard for transparent assets if T16 does not own it.
- [x] Add material-aware preset selection:
      - [x] compute a bounded summary from imported materials/geometry (has
        base-color texture, metallic/roughness range, alpha/transmission share,
        unlit, point-only, CAD format);
      - [x] map to Studio PBR, Studio CAD, Scan/Sculpt, Transparent or Point cloud;
      - [x] apply environment intensity, key/fill/rim, exposure, background and
        grid/ground defaults;
      - [x] manual override wins and persists for the session.
- [x] Keep the Studio intensity control scaling direct + IBL together (as F3D
      does with `render.light.intensity`/`IBLIntensity`).
- [x] Add unit tests `[tonemap]` and `[studio-presets]`:
      - [x] tone-map monotonicity/white point/NaN handling;
      - [x] exposure identity;
      - [x] preset mapping for representative material summaries;
      - [x] transparent/point-cloud selection;
      - [x] background mode selection.
- [x] Add screenshot goldens for each preset and a transparent asset. Reuse the
      `tests/app-smoke` window-capture path; record MAE/SSIM tolerance.
- [x] Update `docs/design/04-rendering-and-streaming.md` and the user-facing
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
- [x] `x64\Release\Tests.Unit.exe "[tonemap],[studio-presets]"` passes.
- [x] HDR resolve/tone map works at all supported sizes; resize/minimize/device
      recovery still work.
- [x] Alpha/transmission composites correctly against the background.
- [x] Studio PBR/CAD/Scan/Transparent/Point presets select and can be
      overridden; exposure and background are usable.
- [x] Screenshot goldens exist and are stable; frame time/memory are recorded.
- [x] No import/parser behavior changed.

## Hand-off

### What landed
- `interactive-viewer/src/render/Presentation.{h,cpp}` — pure exposure,
  Neutral/ACES/Filmic/None tone-map, background-mode and material-aware preset
  functions. `SummarizeMaterials` caps at 4096 records.
- `D3D12ViewerPath` now creates an `R16G16B16A16_FLOAT` scene target with RTV +
  shader-visible SRV, keeps the D32 depth and R32_UINT pick targets, and renders
  every object pipeline into linear HDR. A full-screen resolve
  (`kResolveVertexShaderSource`/`kResolvePixelShaderSource`, vs/ps_5_1) does
  background compositing, exposure, tone map and sRGB before the existing
  D3D11On12/D2D overlay. `RenderClearFrame` uses the same resolve.
- Backgrounds: Dark #1C1C1E (default), Mid-grey studio, Transparent, and a
  checkerboard. Dark/mid-grey clear opaque; transparent/checker clear alpha 0
  and preserve alpha through resolve. Checker is owned here, not T16.
- Presets: Studio PBR, Studio CAD, Scan/Sculpt, Transparent, Point cloud.
  Selection uses base-color texture presence, metallic/roughness range,
  alpha/transmission share, unlit flags, point-only geometry and CAD format.
  Each preset supplies environment intensity, key/fill/rim multipliers,
  exposure, background and grid defaults. A manual preset/lighting choice pins
  the session. Studio intensity scales direct key/fill/rim and IBL together.
- Persistence: `ViewerSettings` v6 adds `toneMapMode`, `backgroundMode` and
  `exposure`; `[`/`]` change exposure, `\` cycles background, `` ` `` cycles the
  tone curve. Controls-dialog help text documents them.
- Tests: `tests/unit/PresentationToneMapTests.cpp` (`[tonemap]`,
  `[studio-presets]`) covers monotonicity/white point/NaN/Inf containment,
  exposure identity, material-summary bounding, all preset mappings, background
  modes and the resolve/object-shader source contract plus a ps_5_1 compile.
- Screenshot lane: `tests/app-smoke/studio-presets.py` captures all five presets
  plus a dedicated transparent sphere, writes a downsampled golden and records
  MAE 0.06 / global SSIM 0.90 tolerances and frame/GPU/private memory.

### Target formats / PSO
- Scene color: `DXGI_FORMAT_R16G16B16A16_FLOAT`, single sample, one RTV and one
  shader-visible SRV; depth `D32_FLOAT`; pick `R32_UINT` MRT.
- Resolve PSO: no input layout, fullscreen triangle from `SV_VertexID`, 8
  32-bit constants (exposure, tone mode, background mode, checker size, linear
  background RGB) plus SRV t0 and a linear clamp static sampler, output
  `R8G8B8A8_UNORM`; depth disabled, opaque blend.
- Object PSOs keep the pick MRT but retarget RTVFormats[0] to the HDR format.

### Checks / evidence
- Verify: `x64\Release\Tests.Unit.exe "[tonemap],[studio-presets]"` — 9 cases /
  6314 assertions passed.
- Neighbouring: `[studio-ibl],[texture-ao],[presentation],[tonemap],[studio-presets]`
  — 21 cases / 19065 assertions passed.
- Full suite: 453 cases / 155641 assertions; only the two pre-existing stale
  expectations (`FormatRegistrationTests` dae, `ActiveInstanceTests`
  `model.iges`) fail.
- `studio-presets.py` second run: five presets MAE 0.0 / SSIM 1.0, transparent
  asset MAE 3.2e-5 / SSIM 0.99985. Debug perf: mean 43.0 ms, p95 300.1 ms,
  accounted GPU 37,224,448 bytes, private 632,061,952 bytes.
- Injected device removal recovered (`recovery count 1`, Ready, complete model,
  debugErrors 0); resize through 320x240 / 900x600 / 1280x860 / 16x16 kept
  Ready with no debug-layer errors.
- Evidence: `docs/0.7.0/progress/T15-evidence/`.

### Deviations / remaining work
- Transparent mode preserves alpha, but there is no screenshot/export consumer
  for that alpha yet; the checkerboard is the visible evidence path.
- No Settings-panel control was added; keyboard + test-control surfaces and
  persisted settings are in place.
- No import/parser behavior changed.

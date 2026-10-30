# ADR-0071 — HDR scene target, full-screen resolve and material-aware presets

- Status: Accepted
- Date: 2026-10-30
- Task: T15

## Context

T14 put the Khronos PBR Neutral curve in the object shader. That applies the
display transform before alpha blending, so a transparent surface is tone-mapped
on its own radiance and then blended against an already tone-mapped background.
Transmission, emission above 1.0 and specular highlights therefore cannot
composite correctly. The viewer also needs a controllable exposure, selectable
backgrounds and a small set of presentation presets derived from imported
materials rather than only the format catalog.

## Decision

1. Geometry renders into a linear `R16G16B16A16_FLOAT` scene target alongside
   the existing D32 depth and R32_UINT pick targets. Object shaders write linear
   radiance and apply no display transform.
2. A single full-screen resolve pass composites the background in linear space,
   multiplies by exposure, applies Neutral (default), ACES, Filmic or None, and
   writes sRGB to the R8G8B8A8_UNORM swap-chain back buffer. It leaves the back
   buffer in `RENDER_TARGET`; the D2D/D3D11On12 overlay still draws afterwards.
3. Backgrounds are Dark (UI-matched default), Mid-grey studio, Transparent and
   Checkerboard. Dark/mid-grey are cleared opaquely into the HDR target;
   transparent/checker clear alpha to zero and are composited in the resolve.
4. Presets (Studio PBR, Studio CAD, Scan/Sculpt, Transparent, Point cloud) are
   chosen by a pure function over a bounded material summary (base-color texture
   presence, metallic/roughness range, alpha/transmission share, unlit,
   point-only, CAD format). Each preset supplies environment intensity,
   key/fill/rim multipliers, exposure, background and grid defaults. An explicit
   user choice pins the session.
5. Studio environment intensity scales the direct rig and IBL together.

## Consequences

- Color memory doubles for the scene target; the existing budget accounting now
  includes it and allocation failure fails graphics startup cleanly.
- Resize recreates the HDR target and its RTV/SRV with the depth/pick targets;
  device recovery recreates all of them through the existing path rebuild.
- Exposure/background/tone-map choices are view state and persist in
  `settings.json`; they never mutate imported material factors.
- Tone mapping after blending is now the only correct path for transparency and
  emission. Future post-process passes (bloom, DOF, AA) can read the same HDR
  target before this resolve.

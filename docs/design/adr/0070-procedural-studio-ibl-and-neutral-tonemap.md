# ADR-0070 — Procedural Studio IBL and neutral tone mapping

- **Status:** accepted
- **Context:** T14 (0.7.0 Studio lighting, first slice)

## Context

The Studio view shaded from an analytic gradient evaluated in camera space.
Reflections were head-locked, metals had no real softbox highlights, and the
homebrew `DisplayMap` rolled off luminance per object before alpha blending.
The proposal's preferred default is a product-owned studio environment: no
user file, no licensing dependency, bounded startup work, and reflections fixed
to the model/world frame.

## Decision

1. The default Studio environment is procedural: three rectangular softboxes, a
   soft overhead strip and a neutral floor/ceiling gradient, baked once at
   startup by `interactive-viewer/src/render/StudioEnvironment.cpp`.
2. The bake is 128 texels per face, six mips by default. `BakeOptions` bounds
   the face size to 4–256 and the mip count to the full chain. The GGX
   prefilter uses a fixed Hammersley sequence, so the bake is deterministic.
3. Diffuse lighting is SH9 irradiance/π; specular lighting is the
   GGX-prefiltered cubemap with `roughness * (mipCount - 1)` selecting the mip.
4. The pipeline transforms camera-space normals/view back to the world frame
   with the world-from-camera quaternion before sampling. The camera-relative
   key/fill rig is kept for readability.
5. The cubemap SRV is appended to each model's material descriptor heap. D3D12
   permits one shader-visible CBV/SRV/UAV heap per binding, so the environment
   cannot have a second heap while materials are bound.
6. The display transform is the Khronos PBR Neutral curve with a per-frame
   exposure multiplier and the sRGB transfer, applied in the object shader for
   this slice.

## Consequences

- The environment is stable in the model frame: orbiting the camera moves
  highlights across the surface instead of dragging them along.
- Startup gains one bounded bake and one ~2 MiB upload; steady-state frame cost
  adds one cube sample and a short SH evaluation.
- Feature-level 11_0 formats are sufficient (`R32G32B32A32_FLOAT` cube). A
  future device without float sampling must fall back to an RGBA8 cube, not the
  old view-space gradient.
- T13's environment-asset hook remains unused for the default Studio; a
  user-selectable HDRI belongs to a later environment picker.
- T15 can move the neutral curve to the HDR post pass without changing the
  baked environment or the world-stable sampling contract.
- `tests/app-smoke/studio.py` records the material-sphere evidence and a
  tolerant per-machine golden on an interactive desktop.

---
timeoutMin: 300
verify: x64\Release\Tests.Unit.exe "[studio-ibl]"
---
# T14 — Studio IBL and neutral tone map (first slice)

## Goal
Studio stops using a camera-locked analytic gradient and starts using a real
studio environment: diffuse irradiance plus a GGX-prefiltered specular
cubemap, sampled in a stable world/yaw frame. The output uses a neutral PBR
tone curve with an exposure constant. Metals, glass, whites and rough
dielectrics read correctly, and reflections stay attached to the model when the
camera orbits.

## Context (read first)
- `docs/proposal.md` §3.6 — the Studio design.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:274-304` — current
  `StudioEnvironment`, `EnvironmentIrradiance`, `DisplayMap`.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:202-217, 311-401` — camera-
  space vertex/pixel shading; the environment is head-locked today.
- `interactive-viewer/src/graphics/D3D12UploadRing.{h,cpp}`,
  `D3D12Device`, `D3D12SwapChain` — texture upload/descriptor patterns.
- `interactive-viewer/src/render/Renderer.h:36-43` — Studio/Clay/Directional.
- F3D reference: `vtkF3DRenderer.cxx:1430-1717` (HDRI prefilter/SH cache),
  `vtkext/private/module/CMakeLists.txt:4-15` (embedded default HDRI),
  `resources/defaultHDRI.png/.hdr` (studio softbox look).
- T13 — environment asset hook if an embedded HDRI is chosen.

## Scope
- [ ] Choose and document the environment source:
      - **preferred:** a product-owned procedural studio (a few rectangular
        softboxes, gradient floor/ceiling) baked at startup into a cubemap;
      - **alternative:** a small embedded HDRI decoded through T13.
      Do not depend on a user file for the default Studio.
- [ ] Add a small environment baker:
      - equirect/cubemap resolution (e.g. 128–256 per face), bounded memory;
      - diffuse irradiance via SH9 or a low-res cosine convolution;
      - GGX-prefiltered specular mip chain with a roughness-to-mip mapping;
      - deterministic output and a unit-testable CPU reference.
- [ ] Add the D3D12 resources/descriptors:
      - a texture cube (or equirect 2D with mips) uploaded once;
      - SRV/sampler in a frame-level descriptor table (do not force a bindless
        rewrite; the feature-level 11_0 descriptor-table path is the baseline);
      - frame constants for environment intensity/rotation and exposure.
- [ ] Shader:
      - evaluate normals/view in a **world/yaw-stable** frame, not camera space,
        or transform the environment direction by the inverse view rotation;
      - replace `StudioEnvironment`/`EnvironmentIrradiance` with cubemap
        sampling (diffuse SH/irradiance + prefiltered specular with Fresnel);
      - keep a small camera-relative key/fill contribution for readability;
      - replace `DisplayMap` with a Khronos PBR Neutral (or equivalent) curve
        and an exposure multiplier. Tone mapping may remain in the object
        shader for this slice; T15 moves it to an HDR post pass.
- [ ] Preserve Clay (neutral grey, low specular) and Directional behavior; the
      environment can be lower intensity for Directional.
- [ ] Add unit tests `[studio-ibl]`:
      - environment baker dimensions/energy sanity;
      - SH9/irradiance monotonicity or reference values;
      - roughness-to-mip mapping;
      - neutral tonemap monotonicity and white-point behavior;
      - deterministic bake across runs.
- [ ] Add screenshot evidence: a material-sphere grid (white/black/red
      dielectric, rough/polished metal, glass) under Studio and Directional.
      Use the existing app-smoke/screenshot mechanism; if none exists for
      screenshots, add a small `tests/app-smoke/studio.py` that captures the
      window and records a tolerant MAE/SSIM golden.
- [ ] Update `docs/design/04-rendering-and-streaming.md` with the environment
      source, prefilter policy and color pipeline.

## Out of scope
- The HDR offscreen target, full-screen post pass and tone-mapping modes
  (→ T15).
- Ground contact shadow/SSAO (→ T16).
- Material lobes (→ T18).
- User-selectable HDRI files; T13/T15 may add an environment picker later.
- A bindless rewrite; keep descriptor tables.

## Design notes
- A head-locked environment is acceptable for a headlight but wrong for
  material review. The environment must be stable in the model/world frame; the
  key/fill may remain camera-relative.
- Do not bake lighting into vertex colors or materials.
- Prefiltering is a one-time cost; do not re-bake per frame or per camera move.
  Re-bake only if the environment asset/intensity changes.
- Keep the old analytic path behind a temporary developer switch until the new
  goldens are accepted; remove it in T15 if not needed.
- The tone curve must not desaturate saturated materials or clip white; compare
  white, black, red and metal spheres.
- Feature-level 11_0 is the floor. If a float/cube format is unsupported, fall
  back to RGBA8 cubemap/mips, not to the old view-space gradient.

## Done when
- [ ] `x64\Release\Tests.Unit.exe "[studio-ibl]"` passes.
- [ ] Metals show distinct softbox highlights and the highlight moves when the
      camera orbits; glass/white/black spheres read correctly.
- [ ] Studio/Clay/Directional still switch correctly; no shader compile errors.
- [ ] The environment bake is deterministic and bounded; frame time remains
      within the existing budget.
- [ ] Screenshot evidence is recorded and the rendering design doc is updated.
- [ ] No parser or import path was touched.

## Hand-off
_(filled in by the implementing session: environment source/resolution, SH/mip
policy, descriptor/constant-buffer changes, tone curve, goldens and any
remaining view-space assumptions.)_

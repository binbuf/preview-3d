---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[texture-hdr],[texture-exr]"
---
# T13 — HDR/EXR textures and environment assets

## Goal
Radiance HDR and OpenEXR images decode in the worker for model textures and for
the Studio environment path. Linear/HDR data stays linear where the renderer can
use it; otherwise it is deterministically tonemapped to RGBA8 for non-environment
slots. This removes the “EXR/TGA normal map” class of missing-material warnings
and gives T14 a real environment-asset loader.

## Context (read first)
- `docs/proposal.md` §3.7 and §3.6 — texture codecs and Studio IBL.
- T12 — image sniff/allowlist, texture budget and shader AO work.
- `import-worker/src/ImageFormatSniff.{h,cpp}`,
  `TextureTranscodeAdapter`, `TextureDecodePolicy.h` — decode path.
- `shared/model-core/include/model_core/PixelFormats.h` — format/bytes contract;
  add a float/BC6H format only if the GPU path samples it.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp` — environment/tonemap hooks
  that T14 will consume.
- Existing TinyUSDZ external sources include `tinyexr`/HDR support; check whether
  the pinned closure can be reused or whether a separate pinned decoder is
  needed. Do not link a decoder into the viewer.
- F3D fixtures: `grayscale.exr`, `Rec709.exr`, `shanghai_bund_1k.hdr`,
  `small_rural_road_1k.exr`, `viridis32.hdr`; verify licenses.

## Scope
- [ ] Add Radiance HDR (`#?RADIANCE`/`#?RGBE`) decode:
      - strict header/format/resolution/RLE validation;
      - decode to linear float; convert to RGBA8 with a documented tonemap for
        non-environment material slots, or expose a float format if the shader
        path supports it.
- [ ] Add EXR decode:
      - use a pinned, bounded decoder (tinyexr or equivalent) with
        file/scanline/channel/size limits;
      - support the common half/float RGB(A) scanline images used for normal,
        roughness, emissive and environment maps;
      - reject tiled/deep/multipart/unsupported compression combinations
        explicitly if not implemented.
- [ ] Extend `SniffImageFormat`/`HasDecodableImageExtension` and the sidecar
      allowlist for `.hdr`/`.exr`.
- [ ] Add a float texture path only if needed:
      - `PixelFormatId::RGBA16_FLOAT` and/or `BC6H_UF16` for HDR/EXR where the
        GPU format supports it;
      - update `PixelFormatBlockInfo`, `ComputeImagePixelBytes`,
        `DxgiFormatFor`, the validator and the shader sampling/tonemap policy;
      - otherwise decode/tonemap on CPU and keep RGBA8.
- [ ] Add an environment-asset loader API/format hook for T14:
      - load a packaged/procedural environment into an internal cubemap/equirect
        representation, not a `Material` image chunk;
      - enforce the same size/decode budgets and no path/network access.
- [ ] Add fixtures:
      - small HDR and EXR color maps;
      - EXR normal/roughness map;
      - malformed/truncated/deep/tiled EXR and malformed HDR;
      - a model referencing an EXR map (FBX/glTF/USD) that currently warns.
- [ ] Add `[texture-hdr]`, `[texture-exr]` cases asserting decode, format,
      linear/sRGB handling, fallback and budget.
- [ ] Add fuzz seeds for HDR and EXR headers/scanlines.
- [ ] Update `docs/FORMAT-SUPPORT.md`, texture policy, SBOM/license notices.

## Out of scope
- IBL prefiltering/tonemapping implementation (→ T14/T15). This task only
  provides the decoder and environment asset hook.
- EXR deep/tiled/multipart support unless a bounded implementation is already
  available and tested.
- A user-facing HDRI file picker; T14 may add the environment source.
- HDR/EXR write/export.

## Design notes
- EXR is a large attack surface. Decode only in the worker, with an explicit
  channel/scanline/pixel budget; reject unsupported features before allocating.
- HDR/EXR color data is linear. Do not apply sRGB transfer to normal/roughness/
  environment data; only base-color/emissive slots may be converted per the
  existing semantic rules.
- If a float format is added, prove D3D12 feature-level 11_0 support and include
  it in `DxgiFormatFor`/validator tests. Otherwise CPU tonemap is safer.
- A missing/unsupported optional EXR should still fall back, not fail the
  model.
- The environment hook must not accept a path from untrusted content; T14’s
  built-in environment is product-owned. User-supplied HDRI loading, if added,
  goes through the normal trusted-file/broker policy.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe "[texture-hdr],[texture-exr]"` passes.
- [ ] HDR/EXR color and data maps decode; malformed/over-budget files fail
      typed or fall back without a crash.
- [ ] An EXR normal/roughness map is used or explicitly skipped with a warning,
      not a fatal import error.
- [ ] The environment asset hook is callable by T14 and respects budgets.
- [ ] New decoders are fuzzed, licensed and in the SBOM/notices.
- [ ] Hand-off lists supported EXR/HDR subsets and the environment API.

## Hand-off
_(filled in by the implementing session: decoder choice/version, supported
EXR/HDR subset, pixel formats added, environment API and fixtures/fuzz seeds.)_

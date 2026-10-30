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
- [x] Add Radiance HDR (`#?RADIANCE`/`#?RGBE`) decode:
      - strict header/format/resolution/RLE validation;
      - decode to linear float; convert to RGBA8 with a documented tonemap for
        non-environment material slots, or expose a float format if the shader
        path supports it.
- [x] Add EXR decode:
      - use a pinned, bounded decoder (tinyexr or equivalent) with
        file/scanline/channel/size limits;
      - support the common half/float RGB(A) scanline images used for normal,
        roughness, emissive and environment maps;
      - reject tiled/deep/multipart/unsupported compression combinations
        explicitly if not implemented.
- [x] Extend `SniffImageFormat`/`HasDecodableImageExtension` and the sidecar
      allowlist for `.hdr`/`.exr`.
- [x] Add a float texture path only if needed:
      - `PixelFormatId::RGBA16_FLOAT` and/or `BC6H_UF16` for HDR/EXR where the
        GPU format supports it;
      - update `PixelFormatBlockInfo`, `ComputeImagePixelBytes`,
        `DxgiFormatFor`, the validator and the shader sampling/tonemap policy;
      - otherwise decode/tonemap on CPU and keep RGBA8.
- [x] Add an environment-asset loader API/format hook for T14:
      - load a packaged/procedural environment into an internal cubemap/equirect
        representation, not a `Material` image chunk;
      - enforce the same size/decode budgets and no path/network access.
- [x] Add fixtures:
      - small HDR and EXR color maps;
      - EXR normal/roughness map;
      - malformed/truncated/deep/tiled EXR and malformed HDR;
      - a model referencing an EXR map (FBX/glTF/USD) that currently warns.
- [x] Add `[texture-hdr]`, `[texture-exr]` cases asserting decode, format,
      linear/sRGB handling, fallback and budget.
- [x] Add fuzz seeds for HDR and EXR headers/scanlines.
- [x] Update `docs/FORMAT-SUPPORT.md`, texture policy, SBOM/license notices.

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
- [x] `x64\Release\Tests.ImportIsolation.exe "[texture-hdr],[texture-exr]"` passes.
- [x] HDR/EXR color and data maps decode; malformed/over-budget files fail
      typed or fall back without a crash.
- [x] An EXR normal/roughness map is used or explicitly skipped with a warning,
      not a fatal import error.
- [x] The environment asset hook is callable by T14 and respects budgets.
- [x] New decoders are fuzzed, licensed and in the SBOM/notices.
- [x] Hand-off lists supported EXR/HDR subsets and the environment API.

## Hand-off

Implemented in the import worker only; the viewer links no decoder.

- **HDR decoder (product-owned, no third-party code).**
  `import-worker/src/RadianceHdrDecode.{h,cpp}`. Supported: `#?RADIANCE`/
  `#?RGBE`, exactly one `FORMAT=32-bit_rle_rgbe`, `-Y <h> +X <w>` resolution
  (sign bits honored, Y-then-X required), new-style per-channel RLE
  (width 8..32767) and flat RGBE scanlines. Rejected typed: XYZE, X-first
  layouts, missing/duplicate FORMAT, old-style RLE, truncated/overrunning
  packets, over-limit header/resolution/dimensions/pixels/bytes.
- **EXR decoder.** `import-worker/src/OpenExrDecode.{h,cpp}` over a
  repository-vendored tinyexr header (`third_party/tinyexr/tinyexr.h`, pinned
  from the TinyUSDZ 0.9.1 closure, BSD-3-Clause, built with system zlib —
  `third_party/tinyexr/LICENSE`). Supported: single-part non-tiled non-deep
  scanline; NONE/RLE/ZIPS/ZIP/PIZ; HALF/FLOAT channels with x/y sampling 1;
  R/G/B/A or Y names including layered `layer.R`; first-three/one-channel
  fallback. Rejected before allocation: tiled, deep/non-image, multipart, UINT
  channels, B44/B44A/DWAA/DWAB/ZFP/unknown compression, bad data window,
  channel/pixel/transient-byte over-budget.
- **Pixel formats.** No new `PixelFormatId`. Both formats decode to linear
  RGBA float internally and finalize to `RGBA8_UNORM` on the CPU. Color/
  emissive use Reinhard `c/(1+c)` at exposure 1.0 then the slot's sRGB transfer;
  normal/data clamp only and remain linear. Shared helper:
  `import-worker/src/HdrFloatDecode.{h,cpp}`. This keeps `PixelFormatBlockInfo`,
  `ComputeImagePixelBytes`, `DxgiFormatFor` and the shared-section validator
  unchanged.
- **Environment API.** `EnvironmentAssetDecode.h`: `LoadEnvironmentAsset(
  std::span<const std::byte>, EnvironmentAssetOptions)` ->
  `EnvironmentAsset{width,height, std::vector<float> rgba}` (linear
  equirectangular, not a Material chunk). Bytes only (no path/URI); default
  budgets 64 MiB encoded / 256 MiB float / 64 Mi pixels / 4096 max dimension.
  T14's preferred procedural environment needs no decode; this is the embedded-
  HDRI hook.
- **Sniff/allowlist.** `SniffedImageFormat::{Hdr,Exr}` and
  `HasDecodableImageExtension` accept `.hdr`/`.exr`; sidecar resolver image
  allowlist + `UsdAdapter::ImageExtensionMatches` + Assimp adapter updated.
  Dispatch added in glTF/OBJ/FBX/USD/Assimp/OpenUSD-host. Malformed maps fall
  back to the existing texture warning, never a model failure.
- **Fixtures.** `python tests/fixtures/textures/generate_textures.py` now emits
  `hdr_color.hdr` (8x8 RLE), `hdr_flat.hdr`, malformed/truncated HDR,
  `exr_color.exr` (HALF ZIP), `exr_float_rgba.exr` (FLOAT NONE),
  `exr_normal.exr`, `exr_roughness.exr` (Y ZIPS), rejected
  tiled/deep/multipart/PIZ/UINT/truncated EXR, `exr/` OBJ+MTL package using EXR
  albedo+normal, `exr-gltf/` glTF sidecar package, and `bad-exr/` fallback
  package. All repository-owned.
- **Tests/fuzz.** `tests/import-isolation/TextureHdrExrTests.cpp`
  (`[texture-hdr]`, `[texture-exr]`) covers decode/format/linear-sRGB/fallback/
  budgets/cancellation/environment hook/OBJ package. `GltfFuzz` domains 9 (HDR)
  and 10 (EXR); `prepare_gltf_seeds.py` emits 61 seeds (16 HDR/EXR).
- **Docs/licensing.** `docs/FORMAT-SUPPORT.md`, `docs/design/03-file-formats-
  and-ingestion.md`, `docs/design/04-rendering-and-streaming.md`, ADR-0069,
  `THIRD-PARTY-LICENSES.md`, packaging SBOM/notices (`licenses/tinyexr.txt`),
  and `packaging/vcpkg-ports/tinyusdz/VENDORED-NOTICES.txt`.

Checks (Release x64):

- `Tests.ImportIsolation.exe "[texture-hdr],[texture-exr]"` — 9 cases / 228
  assertions passed.
- Full `Tests.ImportIsolation.exe` — 497 cases, 492 passed, 5 skipped.
- `Tests.Unit.exe "[texture-ao],[presentation]"` — 5 cases / 88 assertions.
- `GltfFuzz.exe <61-seed-dir> -runs=1000` — no crash.
- `pwsh -File packaging/Test-ReleaseMetadata.ps1` — mapping/notices gate ok.

Deviation: the first attempt referenced tinyexr symbols from
`tinyusdz_static.lib`; that re-pulled a BasisU transcoder object and produced
`LNK2005` collisions with `ktx.lib`. The implementation now vendors the pinned
tinyexr header and compiles it in the worker TU, which also keeps the decoder
out of the viewer. Assimp's local image-extension allowlist gained `.tga`/`.dds`
alongside `.hdr`/`.exr` to match the T12 dispatch it already had.

Remaining work: none for T13's blocking scope. Float/BC6H GPU upload, HDR
offscreen targets, exposure and IBL prefiltering remain T14/T15.

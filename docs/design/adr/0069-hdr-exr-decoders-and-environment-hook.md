# ADR-0069 — Bounded HDR/EXR decoders and the environment-asset hook

## Status

Accepted (T13).

## Context

Modern DCC exports ship base-color, emissive, normal and roughness maps as
Radiance HDR or OpenEXR, and F3D's Studio uses an HDRI environment. Before this
task the extension allowlist skipped `.hdr`/`.exr`, so those maps became
missing-material warnings. EXR is a large parser surface (tiles, deep data,
multi-part, several compressors), and the viewer must never link a decoder
(ADR-0048 keeps DirectXTex out for the same boundary reason).

The pinned TinyUSDZ 0.9.1 closure already vendors tinyexr 1.x (BSD-3-Clause),
but referencing its compiled symbols from the static archive re-pulled an
unrelated BasisU transcoder object and collided with KTX-Software's copy
(`LNK2005`). Linking that archive is therefore not a safe way to obtain tinyexr.

## Decision

1. Decode both formats only in the import worker, under the existing
   `TextureDecodeOptions` encoded/decoded/pixel/dimension budgets and
   cancellation policy. The viewer never links the decoders.
2. HDR is product-owned: strict `#?RADIANCE`/`#?RGBE` signature, exactly one
   `FORMAT=32-bit_rle_rgbe`, `-Y <h> +X <w>` resolution, new-style per-channel
   RLE and flat RGBE scanlines. XYZE, X-first layouts, old-style RLE, malformed
   headers, packet overruns and over-budget files soft-fail.
3. EXR uses a repository-vendored copy of the tinyexr single header (taken from
   the TinyUSDZ closure, compiled in the worker TU with system zlib). The product
   preflight rejects tiled, deep, multipart, UINT channels and every compression
   outside NONE/RLE/ZIPS/ZIP/PIZ before the decoder allocates; data-window,
   channel count, pixels and transient float bytes are bounded. HALF/FLOAT
   channels, sampling 1, and R/G/B/A or Y names (including layered names) are
   supported.
4. No float `PixelFormatId` is added. Both formats decode to linear RGBA float
   internally, then deterministically convert to the existing RGBA8 contract:
   color/emissive apply Reinhard `c/(1+c)` at exposure 1.0 and then the slot's
   sRGB transfer; normal/data clamp only and stay linear. This keeps the
   shared-section validator and `DxgiFormatFor` table unchanged and avoids
   claiming BC6H/RGBA16F upload support that the current shader does not sample.
5. `.hdr`/`.exr` join the image extension allowlist for glTF/OBJ/FBX/Assimp/USD
   sidecar resolution and the USD extension-match table. Malformed maps remain
   optional-texture warnings.
6. The environment hook (`LoadEnvironmentAsset`) takes bytes, never a path or
   URI, and returns a linear equirectangular RGBA-float image outside the
   `Material` chunk stream, with its own encoded/decoded/pixel/dimension budgets.
   It is the embedded-HDRI option for T14; the preferred procedural Studio
   environment needs no decode.

## Consequences

- EXR/HDR color and data maps render; an EXR normal map is used instead of
  warning, and a broken/over-budget one only warns.
- Tiled/deep/multipart EXR and non-tinyexr compressors are explicitly
  unsupported and fall back rather than crashing or silently mis-decoding.
- Color is tonemapped on the CPU; T14/T15 own HDR targets, exposure and IBL
  prefiltering, and can consume the float environment hook for an embedded HDRI.
- tinyexr is a vendored third-party component: its license is staged as
  `licenses/tinyexr.txt` and it is named in the SBOM/third-party notices, even
  though it is not a separate vcpkg port.
- The TGA/DDS truncation fixture suite and the shared texture policy remain the
  model; HDR/EXR add fuzz domains 9/10 in `GltfFuzz`.

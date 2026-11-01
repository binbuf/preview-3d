# ADR-0068 — Bounded product-owned TGA/DDS decoders and ORM ambient occlusion

## Status

Accepted (T12).

## Context

Common downloaded OBJ/FBX/USD/glTF packages ship TGA base-color maps and DDS
normal/DDS block maps. ADR-0048 deliberately dropped DirectXTex, so neither
container had a decode path and the material fell back to white/grey. The
closed `PixelFormatId` set already carries BC1/BC3/BC5/BC7 and the renderer
already uploads those blocks, so most of the GPU contract exists.

## Decision

1. Add two product-owned decoders in the import worker, sharing the existing
   `TextureDecodeOptions` encoded/decoded/pixel/dimension budgets,
   cancellation checks, bounded downscale policy, and color-aware mip
   generation. No DirectXTex, WIC codec discovery, or new third-party library
   is linked.
2. TGA: accept only true-color type 2/10, 24/32-bit, no palette, no
   interleaving, zero canvas origin; honor the origin bits; validate RLE
   packets against exactly `width*height` texels before writing. Select it from
   the decodable-extension candidate filter, then validate all header fields
   from bytes (TGA has no magic).
3. DDS: validate magic, both header sizes, resource shape, DX10 dimension/
   array/misc flags, and the payload extent. Accept DXT1/DXT5/ATI2 and DX10
   BC1/BC3/BC5/BC7 UNORM/SRGB blocks, preserving authored mip chains; decode
   uncompressed 24/32-bit RGB(A) to RGBA8. Reject cubemaps, arrays, volumes,
   and all other DXGI formats typed; clamp a mip count the payload cannot back.
4. Sidecar policy adds `.tga` and `.dds` to the image allowlist for
   glTF/OBJ/FBX/Assimp/USD; no other sidecar category changes.
5. Keep the 72-byte `MaterialPayload` at the time of this decision. The
   metallic-roughness texture follows the ORM convention: R is AO, G roughness,
   B metallic. The studio shader multiplies indirect diffuse and environment
   terms by R; direct-light specular stays unoccluded. No `occlusionStrength`
   field is added here; T18 later supersedes the size clause with the 140-byte
   protocol-v12 record and an authored occlusion strength (ADR 0074).

## Consequences

- TGA/DDS base-color and normal maps render instead of falling back; malformed
  TGA/DDS remain optional-texture warnings, never model failures.
- TGA sources larger than the decoded-byte bound are a warning/fallback
  because that decoder has no codec-native scaler; DDS BC sources can drop
  oversized authored mips to fit the preview dimension.
- `MSFT_texture_dds` remains unparsed; DDS support is for direct sidecar/image
  containers, not glTF alternate-image indexing.
- A future task that needs authored `occlusionStrength` must either add a
  material field or a flag plus value; until then AO is full-strength.

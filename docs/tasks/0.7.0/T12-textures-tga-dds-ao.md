---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[texture-tga],[texture-dds]"
---
# T12 — TGA/DDS textures and ORM ambient occlusion

## Goal
TGA and DDS textures referenced by OBJ/FBX/3MF/USD assets decode and render,
including BC1/BC3/BC5/BC7 DDS blocks passed directly to the GPU. The
metallic-roughness map’s R channel is used as ambient occlusion, so common
downloaded packages stop opening with white/grey fallback materials.

## Context (read first)
- `docs/proposal.md` §3.7 — texture codec expansion.
- `import-worker/src/ImageFormatSniff.{h,cpp}` — container sniff and
  decodable-extension filter.
- `import-worker/src/WicImageDecodeAdapter.{h,cpp}`,
  `WebpDecodeAdapter`, `TextureTranscodeAdapter`, `TextureDecodePolicy.h` — the
  existing decode/transcode/budget path.
- `shared/model-core/include/model_core/PixelFormats.h` — closed pixel format
  set and byte-size validation.
- `shared/model-core/include/model_core/MaterialPayload.h` — material flags;
  no occlusion field yet.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:356-357` — MR sampling
  currently uses `mr.b`/`mr.g`, not `mr.r`.
- `docs/design/03-file-formats-and-ingestion.md` § Texture policy and the
  sidecar extension set.
- ADR-0048 — the DirectXTex drop; amend only for a bounded decoder path.

## Scope
- [ ] Add TGA decode in the worker:
      - validate image type, dimensions, palette/RLE bounds, origin and bit
        depth before allocating;
      - support the common 24/32-bit uncompressed and RLE true-color case;
      - reject unsupported/malformed forms typed, never overrun.
- [ ] Add DDS decode:
      - support DDS magic/header validation and the BC1/BC3/BC5/BC7 formats
        already in `PixelFormatId`; preserve mip chain where valid;
      - for uncompressed RGBA8 DDS, decode/convert within budget;
      - reject cubemaps/arrays/volumes/unsupported DXGI formats unless a
        documented fallback exists;
      - do not trust declared sizes; cross-check against payload length.
- [ ] Extend `SniffImageFormat`/`HasDecodableImageExtension` and the sidecar
      allowlist for `.tga`/`.dds`.
- [ ] Add any needed `PixelFormatId` entries **only** if a target GPU format is
      required; reuse BC1/3/5/7 for DDS blocks. Keep the closed-enum/never-guess
      validator rule.
- [ ] Use the metallic-roughness texture R channel as ambient occlusion in the
      shader; multiply diffuse/environment by the sampled AO (and an optional
      `occlusionStrength` if the source provides one). Do not darken direct
      specular incorrectly.
- [ ] Update the material/image contract only if occlusion strength needs a new
      field; otherwise keep the existing 72-byte payload and document the ORM
      convention.
- [ ] Add fixtures:
      - small TGA (uncompressed and RLE) with/without alpha;
      - DDS BC1/BC3/BC5/BC7 and one malformed/truncated;
      - an FBX or OBJ package referencing a TGA/DDS base color/normal map;
      - a glTF/FBX with an ORM map where R differs from 1.
      Verify licenses or generate fixtures.
- [ ] Add `[texture-tga]`, `[texture-dds]` and `[texture-ao]` cases asserting
      decode success, dimensions/format, fallback on malformed and budget
      enforcement.
- [ ] Add a fuzz target/corpus for TGA and DDS headers/RLE/blocks.
- [ ] Update `docs/FORMAT-SUPPORT.md`, `THIRD-PARTY-LICENSES.md` if a new
      decoder is added, and the texture policy design doc.

## Out of scope
- HDR/EXR (→ T13).
- A general DirectXTex or arbitrary WIC-codec path.
- KTX2/Basis changes; keep the existing path.
- New material lobes (→ T18).

## Design notes
- TGA has no reliable magic; use the extension only as a candidate filter and
  validate the header/fields from bytes. A TGA-named file with invalid header
  bytes must not be treated as a valid texture.
- DDS is a container for several resource shapes. Reject what the product does
  not support rather than flattening it silently.
- All decoding stays in the worker under the existing decoded-texture budget;
  a texture over budget is a fallback/warning, not a model failure when the
  texture is optional.
- The shader must treat AO as a diffuse/environment multiplier; applying it to
  direct specular makes metals look dirty.
- Preserve the current sRGB/linear policy: base color/emissive are sRGB;
  normal/roughness/metallic/AO are linear.
- Do not add a new public limit without an ADR; reuse the texture budgets.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe "[texture-tga],[texture-dds]"` passes.
- [ ] TGA/DDS base-color and normal maps appear on a fixture; malformed inputs
      fall back with a warning.
- [ ] An ORM map visibly affects AO in a golden/unit case.
- [ ] The decoded-texture budget and downscale policy still hold.
- [ ] New decoders are fuzzed, licensed and in the SBOM/notices.
- [ ] Sidecar policy docs list the new extensions.
- [ ] Hand-off lists supported TGA/DDS subsets and rejected forms.

## Hand-off
_(filled in by the implementing session: decoder choice, supported DDS formats,
sidecar allowlist changes, AO formula and fixtures/fuzz commands.)_

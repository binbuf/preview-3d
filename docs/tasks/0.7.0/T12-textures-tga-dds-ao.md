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
- [x] Add TGA decode in the worker:
      - validate image type, dimensions, palette/RLE bounds, origin and bit
        depth before allocating;
      - support the common 24/32-bit uncompressed and RLE true-color case;
      - reject unsupported/malformed forms typed, never overrun.
- [x] Add DDS decode:
      - support DDS magic/header validation and the BC1/BC3/BC5/BC7 formats
        already in `PixelFormatId`; preserve mip chain where valid;
      - for uncompressed RGBA8 DDS, decode/convert within budget;
      - reject cubemaps/arrays/volumes/unsupported DXGI formats unless a
        documented fallback exists;
      - do not trust declared sizes; cross-check against payload length.
- [x] Extend `SniffImageFormat`/`HasDecodableImageExtension` and the sidecar
      allowlist for `.tga`/`.dds`.
- [x] Add any needed `PixelFormatId` entries **only** if a target GPU format is
      required; reuse BC1/3/5/7 for DDS blocks. Keep the closed-enum/never-guess
      validator rule.
- [x] Use the metallic-roughness texture R channel as ambient occlusion in the
      shader; multiply diffuse/environment by the sampled AO (and an optional
      `occlusionStrength` if the source provides one). Do not darken direct
      specular incorrectly.
- [x] Update the material/image contract only if occlusion strength needs a new
      field; otherwise keep the existing 72-byte payload and document the ORM
      convention.
- [x] Add fixtures:
      - small TGA (uncompressed and RLE) with/without alpha;
      - DDS BC1/BC3/BC5/BC7 and one malformed/truncated;
      - an FBX or OBJ package referencing a TGA/DDS base color/normal map;
      - a glTF/FBX with an ORM map where R differs from 1.
      Verify licenses or generate fixtures.
- [x] Add `[texture-tga]`, `[texture-dds]` and `[texture-ao]` cases asserting
      decode success, dimensions/format, fallback on malformed and budget
      enforcement.
- [x] Add a fuzz target/corpus for TGA and DDS headers/RLE/blocks.
- [x] Update `docs/FORMAT-SUPPORT.md`, `THIRD-PARTY-LICENSES.md` if a new
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
- [x] `x64\Release\Tests.ImportIsolation.exe "[texture-tga],[texture-dds]"` passes.
- [x] TGA/DDS base-color and normal maps appear on a fixture; malformed inputs
      fall back with a warning.
- [x] An ORM map visibly affects AO in a golden/unit case.
- [x] The decoded-texture budget and downscale policy still hold.
- [x] New decoders are fuzzed, licensed and in the SBOM/notices.
- [x] Sidecar policy docs list the new extensions.
- [x] Hand-off lists supported TGA/DDS subsets and rejected forms.

## Hand-off

### Decoder choice
- Two product-owned decoders in the import worker, no DirectXTex or new
  third-party library: `TgaImageDecode.{h,cpp}` and `DdsImageDecode.{h,cpp}`,
  sharing `RasterRgbaSupport.h` (`FinalizeRgba8`) with the existing
  `TextureDecodeOptions` budgets, cancellation, maxDimension downscale, and
  color-aware mip generation. ADR-0068 records the decision.
- All existing sniff-switch sites route the new formats: glTF, OBJ, FBX, USD
  fast path, Assimp compatibility textures, and the OpenUSD host.

### Supported subsets / rejected forms
- **TGA supported:** type 2 (uncompressed) and type 10 (RLE) true-color,
  24/32-bit, no color map, no interleave, zero origin, left/right and top/bottom
  origin bits, alpha in 32-bit. Rejected typed: palette/indexed or grayscale
  types, 15/16-bit, nonzero canvas origin, invalid alpha-bit descriptor,
  interleaving bits, truncated payloads, RLE packets that overrun
  `width*height`, and inputs over the pixel/decoded-byte budget. A `.tga` name
  with invalid header bytes is not treated as a texture.
- **DDS supported:** legacy `DXT1` (BC1), `DXT5` (BC3), `ATI2`/`BC5U` (BC5);
  DX10 `BC1_UNORM`/`BC3_UNORM`/`BC5_UNORM`/`BC7_UNORM` and their `_SRGB`
  variants; uncompressed legacy 24/32-bit RGB(A) and DXGI
  `R8G8B8A8_UNORM(_SRGB)`/`B8G8R8A8_UNORM(_SRGB)` → RGBA8. Authored block mip
  chains are preserved; uncompressed DDS regenerates mips. Rejected typed:
  cubemaps, arrays, volumes/3D, every other DXGI format, bad header/pixel
  format sizes, and payloads shorter than the declared extent. A mip count
  larger than the payload backs is clamped to the levels present.
- **Sidecar allowlist:** `.tga` and `.dds` added to
  `SidecarPathResolver::IsImageExtension` (glTF/OBJ/FBX/Assimp/USD), plus the
  USD/USD-host extension-match tables. 3MF keeps its spec PNG/JPEG content-type
  policy.

### AO formula
- ORM convention in the studio PBR pixel shader:
  `metallic *= mr.b; roughness *= mr.g; occlusion = saturate(mr.r)`.
- `occlusion` multiplies indirect diffuse (`EnvironmentIrradiance`), the
  analytic environment reflection (`StudioEnvironment(refl, …)`), and the
  ambient fill terms. `EvaluateLight` direct-light specular is untouched.
- `MaterialPayload` stays 72 bytes; no `occlusionStrength` field, so AO is
  applied at full strength and authored glTF occlusion strength is not yet
  represented.

### Fixtures and fuzz
- `tests/fixtures/textures/` (repository-owned, generated by
  `python tests/fixtures/textures/generate_textures.py`): TGA 24/32
  uncompressed and RLE, DDS BC1/BC3/BC5/BC7, BC1 four-level chain, RGBA8,
  truncated BC3, OBJ+MTL package using TGA base color + BC5 normal, a bad
  TGA/DDS package, and a glTF ORM package (R=32, G=200, B=20).
- `tests/import-isolation/TextureTgaDdsTests.cpp` supplies `[texture-tga]`,
  `[texture-dds]`, `[texture-ao]`; `tests/unit/PresentationTests.cpp` asserts
  the shader AO contract without a GPU device.
- Fuzz: `tests/fuzz/GltfFuzz.cpp` domains 7/8 drive the TGA/DDS decoders under
  ASan; `prepare_gltf_seeds.py` copies the fixtures into the seed set.

### Check results (Release x64)
- `x64\Release\Tests.ImportIsolation.exe "[texture-tga],[texture-dds]"` —
  7 cases / 178 assertions, all passed.
- `x64\Release\Tests.ImportIsolation.exe "[texture-tga],[texture-dds],[texture-ao]"`
  — 8 cases / 188 assertions, all passed.
- `x64\Release\Tests.ImportIsolation.exe "[texture-ao],[texture-decode],[texture-transcode],[texture-progressive]"`
  — 16 cases / 240100 assertions, all passed.
- `x64\Release\Tests.Unit.exe "[texture-ao],[presentation]"` — 5 cases / 88
  assertions, all passed.
- `tests\fuzz\x64\Release\GltfFuzz.exe <seeds> -runs=2000` — 45 seeds,
  2000 runs, no crash.
- Built `Preview3DImportWorker`, `Preview3DImportHost`,
  `Tests.ImportIsolation`, `Tests.Unit`, and `GltfFuzz` successfully.

### Docs changed
- `docs/FORMAT-SUPPORT.md`
- `docs/design/03-file-formats-and-ingestion.md`
- `docs/design/adr/0068-tga-dds-decoders-orm-ao.md`
- `THIRD-PARTY-LICENSES.md` (notes the decoders are product-owned; no SBOM row)
- `shared/import-broker/include/import_broker/SidecarPathResolver.h`
- `tests/fixtures/textures/README.md`
- `docs/0.7.0/progress/T12.md`

### Deviations / remaining work
- TGA has no codec-native scaler, so a source whose decoded base exceeds
  `maxDecodedBytes` falls back with a warning rather than being progressively
  downscaled; small/medium sources still use the shared maxDimension downscale.
- `MSFT_texture_dds` glTF alternate-image indexing remains disabled; direct DDS
  sidecars decode. 3MF texture2d content types remain PNG/JPEG only.
- Authored `occlusionStrength` is not represented by the 72-byte material
  payload; later material-contract work can add it if needed.

---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T25 --configuration Debug --output TestResults/render-quality/T25
---
# T25 — Texture-driven clearcoat, specular, sheen and transmission inputs

## Goal

Preserve authored variation in supported PBR extensions instead of rendering only their scalar factors.

## Context (read first)

- Depends on **T22, T24**; execute in roadmap order.
- T18 hand-off and format-support warnings for lobe maps.
- `GltfAdapter.cpp`, material/dependency wire schema, texture semantic/mip code and `D3D12ViewerPath.cpp`.
- Khronos clearcoat, specular, sheen and transmission extension specifications.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [x] Add validated bindings and shader sampling for clearcoat factor/roughness/normal, specular factor/color, sheen color/roughness and transmission factor maps. Respect each extension's channel, color-space, factor multiplication, sampler and per-slot UV metadata from T22.
- [x] Use a bounded descriptor/material layout; account for the 64-DWORD root-signature limit rather than appending unbounded root constants. Version wire/dependency/cache changes and update all producers, validator and hostile-worker coverage.
- [x] Generate representative maps with nonconstant channels, mismatched dimensions, shared image/different color semantics and UV1 transforms. Verify imported values and GPU results; a uniform factor-only sphere cannot demonstrate map support.
- [x] Exercise missing/invalid/oversized maps, cancellation, coarse-to-full residency, generation changes, fallback descriptors and device reset. Never upload an unvalidated reference or let a fallback descriptor masquerade as a resident authored map.
- [x] Update extension compatibility claims only for paths now rendered; keep specific warnings for still-unsupported volume/anisotropy/iridescence/dispersion behavior. Preserve parsers/decoders in workers.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

### Verification economy

The front-matter gate launches a real GPU viewer and takes minutes. Spend runs
deliberately:

- Run it in the foreground with an explicit long timeout (900000 ms or more).
  Do not background it and poll with short `Start-Sleep` loops; if the tool
  moves it to the background, wait once for completion instead of polling.
- Iterate with `--fast` (add `--no-build` once the binary is current). `--fast`
  keeps the build, generated fixtures and this task's focused lane, and skips
  the contract self-tests, external corpus, F3D references and review gate. It
  is iteration evidence, not acceptance evidence.
- Keep agent-run full gates to at most two: one early proof of the lane and
  one final after the last edit. The harness repeats the front-matter command
  once after you finish. Corpus or baseline re-promotion is one run, not a
  per-fix loop.
- Use focused unit/import filters between runs. Do not re-run a passing gate to
  "confirm" it, and do not repeat a failing configuration without a change.

## Done when

- [x] T25 verification combines texture/lobe import, wire/hostile-section checks and actual GPU map-variation images.
- [x] Textured coat/specular/sheen/transmission fixtures show the expected channels and no change to the no-extension baseline.
- [x] Update format-support/material/streaming docs, provenance and hand-off.

## Out of scope

Anisotropy, iridescence, dispersion, material editing or a wholesale renderer replacement.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

**Status: done.** Protocol v14 carries the eight extension-lobe maps in fixed
material slots and the shader samples them per texel. The full gate passes with
140/140 T25 checks and no failures.

### Implemented behavior

- **Wire v14.** `kCurrentProtocolVersion == 14`; `MaterialPayload` is 440 bytes;
  `ChunkDescriptor` is 208 bytes with 16 dependency ids;
  `DetailRequest` is 216 bytes. Thirteen material slots in fixed order:
  baseColor, metallicRoughness, normal, emissive, occlusion, clearcoat,
  clearcoatRoughness, clearcoatNormal, specular, specularColor, sheenColor,
  sheenRoughness, transmission. Slots 13-15 must stay zero.
- **Payload additions.** `clearcoatNormalScale` (offset 128),
  `slotFlags[13]` (132), `slotUvOffset[12][2]` (184), `slotUvScale[12][2]`
  (280), `slotUvRotation[12]` (376), `reserved[4]` (424). The validator applies
  the existing finite/range/flag checks to every slot.
- **glTF import.** `GltfAdapter.cpp` resolves all eight maps, keeps the Khronos
  channel and color space, and stores `clearcoatNormalTexture.scale`. UV1 maps
  remain unbound with `kTextureFeatureUnsupportedUvSet` (4000). The old
  map-dropped texture warnings for these extensions are removed.
- **GPU layout.** Fifteen single-descriptor SRV tables + 25-DWORD draw block +
  two CBVs = 44 of 64 root-signature DWORDs. Material CBV stride is 1024 bytes.
  Thirteen neutral descriptors serve unbound slots; the shader map mask skips
  them, so a fallback descriptor cannot masquerade as an authored map.
- **Shader.** Clearcoat R, clearcoat roughness G, clearcoat normal RGB (own
  scale, coat lobe only), specular factor A, specular color RGB, sheen color
  RGB, sheen roughness A and transmission R multiply their scalar factors.
  Each slot resolves its own UV set/transform/address mode/filter.
- **Diagnostics.** Channels 24-30 expose effective clearcoat/roughness,
  specular factor, IOR-tinted specular F0, sheen color, transmission, coat
  normal and sheen roughness. Field 233 selects an extension slot; fields
  234-242 expose its bounded metadata/binding window.

### Verification (actual commands)

```powershell
# Full front-matter gate: pass, 140/140 checks, 2 captures, review recorded.
python tests/render-quality/verify.py --task T25 --configuration Debug --output TestResults/render-quality/T25

# Serial lanes included by the lane (all pass):
#   Tests.Unit.exe [material-lobes] [cache]
#   Tests.ImportIsolation.exe [textured-lobes] [shared-section-validator]
#       [hostile-worker] [texture-budget] [coarse-proxy],[detail-budget]
#       [texture-decode]

# Focused import contract used during development:
.\x64\Debug\Tests.ImportIsolation.exe "[textured-lobes]"
# 55 assertions, 1 case, pass.
```

- Full report: `TestResults/render-quality/T25/report.json`.
- Lane report: `TestResults/render-quality/T25/diagnostics/textured-lobes.json`.
- Images: `TestResults/render-quality/T25/images/generated-textured-lobes-*.png`,
  `textured-lobes-studio.png`, `textured-lobes-after-device-reset.png`, and
  `channels/channel-{12,24..30}-*.png`.
- Review: `tests/render-quality/review/T25-review.json` (`imagesInspected`
  populated; no baseline/reference promoted).
- The injected device-removal check recovered all 9 records with their authored
  maps bound; the post-reset capture SHA-256 is recorded in the lane report.

### Deviations / limits

- The camera-facing probes are double-sided quads so the front view is stable
  under the viewer's Y-up presentation transform; the no-extension quad remains
  the factor-only baseline.
- Transmission still uses the T24 alpha-carried interface Fresnel and no
  transmitted-scene sampling. T25 verifies the per-texel transmission factor
  through channel 28; T27 owns the physical path.
- Anisotropy, iridescence, volume, dispersion and diffuse transmission remain
  parsed-but-unrendered with their specific compatibility records.
- UV1 map slots remain intentionally unbound; a second TEXCOORD set is a future
  vertex-layout/wire change.

### Next task must know

- Wire/cache identity moved to v14: any retained v13 section, cache entry or
  test fixture is stale by design.
- Update any downstream material producer that adds a dependency slot: slots
  5-12 are now semantic, 13-15 must stay zero.
- Material CBV stride is 1024 bytes; `MaterialConstantData` is 560 bytes.
- Debug fields 233-242 are append-only and bounded by the `wParam <= 242`
  guard; Release still returns 0 for every test-control field.
- T26/T27 should preserve the map-driven transmission factor and add scene
  sampling/tint/rough refraction without changing the T25 channel-28 meaning.


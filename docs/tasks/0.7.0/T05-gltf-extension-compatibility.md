---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[gltf-extensions]"
---
# T05 — glTF required-extension compatibility

## Goal
A glTF that requires a visual-only material extension (clearcoat, sheen,
specular, iridescence, emissive strength, etc.) opens with its core PBR material
and a specific warning instead of failing as “unsupported feature.” Critical
geometry/decoding extensions still fail when they cannot be decoded.

## Context (read first)
- `docs/proposal.md` §3.2 and Appendix D — the severity table and policy.
- `import-worker/src/GltfAdapter.cpp:111-118` — `MapFastgltfError`.
- `import-worker/src/GltfAdapter.cpp:400-411` — `IsSupportedExtension`.
- `import-worker/src/GltfAdapter.cpp:2399-2412` — parser extension mask.
- `vcpkg_installed/.../fastgltf/core.hpp` — the `Extensions` enum; enable every
  extension whose data path we can consume.
- `shared/model-core/include/model_core/Compatibility.h` — T04 report.
- `docs/FORMAT-SUPPORT.md` — the glTF row that must reflect the new policy.

## Scope
- [ ] Expand the fastgltf parser mask to include the material/scene extensions
  fastgltf supports and whose data path we can consume: clearcoat, sheen,
  specular, iridescence, volume, anisotropy, emissive strength, variants,
  lights punctual, and any other supported metadata extension. Do **not** enable
  `MSFT_texture_dds` until T12 lands a DDS decoder.
- [ ] Pre-scan `extensionsRequired` with simdjson (or the existing bounded JSON
  path) before fastgltf parsing. Classify each name with the T04 feature table:
  - geometry/decoder-critical: Draco, meshopt, quantization, any geometry
    replacement;
  - texture-critical: BasisU, WebP (fallback texture is allowed, but do not
    fail core geometry solely for a missing optional texture);
  - visual material/scene: degrade/ignore with a `CompatibilityReport`;
  - unknown: keep core geometry if present and report `Degraded`; fail only if
    the parser cannot produce usable geometry.
- [ ] If a truly unknown required extension makes fastgltf return
  `UnknownRequiredExtension`/`MissingExtensions`, retry once with a filtered
  JSON copy that removes only the **non-critical** required names. Bound the
  copy to the existing metadata limit; never filter critical names. Do not loop.
- [ ] Map `UnknownRequiredExtension`/`MissingExtensions` to the new behavior
  rather than unconditional `UnsupportedRequiredFeature`; keep a typed failure
  when all usable geometry is lost.
- [ ] Add fixtures:
  - `required-clearcoat.gltf` (core mesh + required clearcoat) → opens with
    warning;
  - `required-specular.gltf` → opens with warning;
  - `required-emissive-strength.gltf` → opens; emissive factor reflects
    strength if T18 has landed, otherwise warning;
  - `required-draco-bad.glb` → typed failure;
  - `unknown-required.gltf` with core geometry → degraded, not failed;
  - a required geometry-replacing unknown extension → typed failure.
- [ ] Add `[gltf-extensions]` cases asserting outcome, warning feature IDs and
  that the mesh/material counts remain sane.
- [ ] Update `docs/FORMAT-SUPPORT.md` with the new accepted/degraded extension
  classes and the “core geometry wins” rule.

## Out of scope
- Implementing the clearcoat/sheen/specular shader lobes (→ T18). This task
  parses and reports them; the core PBR fallback is acceptable.
- DDS/EXR texture decode (→ T12/T13).
- `EXT_mesh_gpu_instancing` expansion beyond a warning/one-instance fallback;
  full instancing is a separate design if needed.
- Animation/skins/morph playback.

## Design notes
- Do not remove names from `extensionsUsed`; only the required list is filtered.
  Unknown extension objects are ignored by fastgltf if not enabled.
- The filtered copy must preserve byte offsets for the GLB BIN chunk; only the
  JSON text is replaced. Do not re-pack the whole GLB unless necessary.
- A visual extension that is not parsed must not silently change geometry. The
  fallback is the core material with a reported feature ID.
- Keep the existing `KHR_materials_pbrSpecularGlossiness` approximation and
  transmission path unchanged unless the report needs a new feature ID.
- All work remains in the worker; the trusted host only reads the report.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe "[gltf-extensions]"` passes.
- [ ] The clearcoat/specular fixtures open with a specific warning and the core
  mesh visible; the bad-Draco fixture still fails typed.
- [ ] A file requiring an unknown geometry-replacing extension still fails and
  is not silently degraded.
- [ ] `docs/FORMAT-SUPPORT.md` is updated; no new limit was added without an ADR.
- [ ] Hand-off records the extension table and any fastgltf limitations.

## Hand-off

**Status: done.**

### Enabled fastgltf extension list
`KHR_draco_mesh_compression`, `KHR_texture_basisu`, `KHR_texture_transform`,
`KHR_mesh_quantization`, `EXT_meshopt_compression`, `EXT_texture_webp`,
`KHR_materials_unlit`, `transmission`, `ior`, `pbrSpecularGlossiness`,
`clearcoat`, `sheen`, `specular`, `iridescence`, `anisotropy`, `volume`,
`dispersion`, `diffuse_transmission`, `emissive_strength`,
`KHR_materials_variants`, `KHR_lights_punctual`. `MSFT_texture_dds` is still
disabled pending T12. The mask is generated from the policy table in
`GltfAdapter.cpp` (`GltfParserExtensionMask`).

### Classification table
| Class | Names | Behavior |
| --- | --- | --- |
| Geometry-critical | Draco, meshopt, quantization | Decode; undecodable data fails typed |
| Geometry-critical, no parser support | `KHR_implicit_shapes`, `KHR_accessor_float64` | Never filtered; required use returns `UnsupportedRequiredFeature` |
| Texture-critical | basisu, WebP | Decode/fallback; no core-geometry failure |
| Texture-critical, no decoder | `MSFT_texture_dds` | Filtered from required, reported, core geometry survives |
| Visual | clearcoat, sheen, specular, iridescence, anisotropy, volume, dispersion, diffuse transmission, emissive strength | Parsed by fastgltf; core PBR shown; `Degraded` record with the T04 feature ID |
| Scene | variants, lights punctual | Parsed/ignored; `Ignored` record |
| Instance | `EXT_mesh_gpu_instancing` | Filtered/one-instance fallback; `Degraded` |
| Truly unknown | any other required name | Filtered once; core scene opens and reports `Degraded` (ID 900); fails only if no usable geometry remains |

### Fallback behavior for unknown required extensions
The JSON preflight pre-scans the root `extensionsRequired`. If any non-critical
required name is present, the adapter builds a bounded copy that rewrites only
that array (spaces pad the replacement so GLB byte offsets and the mapped BIN are
unchanged) and parses it once. If the pre-scan misses a filterable name and
fastgltf returns `MissingExtensions`/`UnknownRequiredExtension`, the adapter
performs exactly one filtered retry. Geometry-critical names are never removed,
so those still fail `UnsupportedRequiredFeature`. `extensionsUsed` is never
edited; unsupported used names (and required names missing from used) produce
`CompatibilityReport` records.

### Fixtures and tests
Added `interactive-viewer/test-assets/required-clearcoat.gltf`,
`required-specular.gltf`, `required-emissive-strength.gltf`,
`unknown-required.gltf`, `required-geometry-unknown.gltf`, and
`required-draco-bad.glb`. Added seven `[gltf-extensions]` cases in
`tests/import-isolation/GltfImportTests.cpp`; updated the previous
unknown-required rejection case and the `[recovery]` typed-error fixture.

### Checks
- `MSBuild Preview3D.slnx /t:Tests_ImportIsolation /p:Configuration=Release /p:Platform=x64 /v:q` — pass.
- `x64\Release\Tests.ImportIsolation.exe "[gltf-extensions]"` — 104 assertions, 7 cases, pass.
  `required-draco-bad.glb` fails with the typed `MalformedData` code from the
  bounded Draco decoder.
- `[gltf-import]` — 655 assertions, 31 cases, pass.
- `[compatibility]` — 93 assertions, 10 cases, pass.
- `[recovery]` — 2327 assertions, 38 cases (35 pass, 3 fault-harness skips), pass.

### Docs
Updated `docs/FORMAT-SUPPORT.md` and
`docs/design/03-file-formats-and-ingestion.md`; added ADR
`docs/design/adr/0060-gltf-required-extension-compatibility.md`; wrote reusable
facts and follow-ups to `docs/0.7.0/progress/T05.md`.

### Deviations / limitations
- Emissive strength is parsed but not applied; T18 owns baking it into the
  emissive factor, so the fixture reports the warning as allowed by the task.
- No new numeric limit was added: required-name count reuses
  `kTierAObjectLimit`, and the filtered copy stays inside the existing metadata
  and Tier A source ceilings.
- `KHR_implicit_shapes` stands in for an unknown geometry-replacing extension in
  the typed-failure fixture; it is a real Khronos name compiled out of the pinned
  fastgltf build.

### Remaining work
None for T05. Follow-ups are recorded in `docs/0.7.0/progress/T05.md` (T18 lobes,
T12 DDS, optional named scene IDs).

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
_(filled in by the implementing session: enabled extension list, classification
table, fixture names, and the exact fallback behavior for unknown required
extensions.)_

---
timeoutMin: 300
verify: x64\Release\Tests.Unit.exe "[material-lobes]"
---
# T18 — Material lobes and transparency

## Goal
Modern PBR materials keep more of their authored appearance: clearcoat, sheen,
specular/IOR and emissive strength are represented in the normalized material
and approximated in the shader, and transparent/transmission assets composite
correctly in the HDR pipeline. Unsupported lobes still degrade with a specific
warning rather than silently disappearing.

## Context (read first)
- `docs/proposal.md` §3.10 — material model expansion.
- `shared/model-core/include/model_core/MaterialPayload.h:60-76` — current
  72-byte payload and flags.
- `shared/model-core/include/model_core/WireFormat.h:215-218` — material
  dependency slots (base color, metallic/roughness, normal, emissive).
- `import-worker/src/GltfAdapter.cpp` — material mapping and the T05 extension
  table.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:311-401` — PBR/transmission
  shader.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:1318-1326` — alpha sort;
  T15 moves tone mapping after blending.
- T05/T15 — parsed extensions and HDR resolve.
- F3D reference: `vtkF3DMetaImporter.cxx:487-527` (legacy-to-PBR conversion)
  and `options.json` material fields (`model.material.base_ior`, etc.).

## Scope
- [ ] Extend `MaterialPayload` with a versioned fixed-size record for:
      - `emissiveStrength`;
      - `clearcoatFactor` / `clearcoatRoughness`;
      - `sheenColor` / `sheenRoughness`;
      - `specularFactor` / `specularColor` and IOR-derived F0;
      - `occlusionStrength` (unless T12 keeps it entirely in the ORM texture);
      - reserved words for future anisotropy/iridescence/volume.
      Bump the wire/protocol version; update `SharedSectionValidator`, the
      hostile-worker suite and cache/importer identity.
- [ ] Map the fields from fastgltf (and the format-specific adapters where
      available):
      - `KHR_materials_clearcoat` → clearcoat;
      - `KHR_materials_sheen` → sheen;
      - `KHR_materials_specular`/`ior` → specular/F0;
      - `KHR_materials_emissive_strength` → emissive multiplier;
      - unsupported lobes → `Ignored`/`Degraded` compatibility records.
- [ ] Shader:
      - clearcoat as a second GGX lobe over the base;
      - sheen as a soft velvet lobe;
      - specular/F0 from authored values instead of the fixed 0.04;
      - emissive strength applied before tone mapping;
      - keep the existing energy/roughness clamps and double-sided normal flip.
- [ ] Transparency:
      - ensure T15’s HDR resolve composites sorted transparency after opaque;
      - for glass/transmission-heavy scenes, add weighted blended OIT or depth
        peeling as an optional quality mode (choose one; document the other as
        deferred);
      - keep the transmission approximation (Fresnel-weighted alpha) unless the
        new pipeline supports a better one.
- [ ] Add material inspection modes if cheap: base color, metallic, roughness,
      normal, UV checker. These help validate the new fields and are useful for
      a viewer. Keep them out of the default path.
- [ ] Add fixtures/goldens:
      - clearcoat car-paint sphere;
      - sheen cloth sphere;
      - specular/IOR gemstone/dielectric;
      - emissive-strength sign;
      - glass sphere against the Studio backdrop;
      - unsupported lobe with a warning.
- [ ] Add `[material-lobes]` unit tests and `[material]` import-isolation cases
      for field mapping/validation and warning records.
- [ ] Update `docs/design/03-file-formats-and-ingestion.md` (material contract),
      `docs/design/04-rendering-and-streaming.md` (shader), and
      `docs/FORMAT-SUPPORT.md`.

## Out of scope
- Full anisotropy/iridescence/volume/dispersion rendering; parse/ignore with a
  warning, or reserve fields for a later task.
- Refractive ray tracing or screen-space refraction; transmission remains an
  approximation.
- Material authoring/editing UI.
- Texture decode changes (→ T12/T13).

## Design notes
- A material payload size change is a protocol change. Do not reuse reserved
  words silently; bump the version and update every producer/consumer.
- Approximations are acceptable if the compatibility report says so. Do not
  pretend a lobe is exact.
- Keep the shader feature-level 11_0 compatible; avoid a bindless/material
  buffer rewrite.
- Clearcoat/sheen should not double-count energy; use a documented layering
  model and goldens.
- Transparency sorting is not a substitute for OIT; if OIT is deferred, state
  the remaining limitation and keep the sorted path correct.
- Do not change the default material for formats that do not author the new
  fields.

## Done when
- [ ] `x64\Release\Tests.Unit.exe "[material-lobes]"` passes.
- [ ] `x64\Release\Tests.ImportIsolation.exe "[material]"` passes.
- [ ] Clearcoat/sheen/specular/emissive fixtures show distinct, documented
      behavior; unsupported lobes warn.
- [ ] Glass/transmission composites correctly against the backdrop; no dark
      halo from tone mapping.
- [ ] Wire/validator/hostile-worker/cache tests cover the new payload.
- [ ] Docs and format-support matrix are updated.
- [ ] Hand-off records the payload layout, mapping table and deferred lobes.

## Hand-off
Implemented protocol-v12 material lobes end to end.

- **Payload/version:** `kCurrentProtocolVersion = 12`; `MaterialPayload` grows
  from 72 to 140 bytes. Layout: base color/metallic/roughness/emissive/UV/alpha/
  transmission unchanged at offsets 0-68; `emissiveStrength` @72, clearcoat
  factor/roughness @76/80, `sheenColor[3]` @84, `sheenRoughness` @96,
  `specularFactor` @100, `specularColor[3]` @104, `ior` @116,
  `occlusionStrength` @120, `reserved[4]` @124 (must be zero). Defaults preserve
  pre-v12 appearance. Shared `ValidateMaterialLobeFields` is used by the wire
  validator and unit tests.
- **Mapping:** glTF clearcoat/sheen/specular/IOR/emissive-strength/occlusion
  strength, plus ufbx FBX/OBJ equivalents. T05 now treats clearcoat/sheen/
  specular/emissive-strength as adapter-supported; iridescence/anisotropy/
  volume/dispersion/diffuse-transmission remain parse-and-warn with specific
  Degraded records. Texture-driven lobe maps warn because the fixed record has
  no extra texture slots.
- **Shader layering:** 36 material root-constant dwords (20 old + 4 float4
  lobes). Clearcoat is a second dielectric GGX lobe with base specular scaled by
  `(1-clearcoatFactor)`; sheen is a grazing-only velvet term; IOR/specular drive
  dielectric F0; emissive strength applies before the HDR resolve; AO uses the
  authored strength. Roughness/energy clamps and the double-sided normal flip
  remain. Clay suppresses lobes.
- **Transparency/OIT:** T15's linear HDR resolve still composites sorted
  straight-alpha transparency after opaque, avoiding the dark-halo tone-map
  ordering. Weighted blended OIT/depth peeling are deferred, so intersecting
  transparent surfaces remain order-dependent; transmission stays
  Fresnel-weighted alpha (now using the authored F0).
- **Fixtures/tests:** new `tests/unit/MaterialLobesTests.cpp` (`[material-lobes]`),
  `[material]` glTF cases for clearcoat/sheen/specular/emissive strength, new
  `required-sheen.gltf`, lobe range/reserved validator cases, and a hostile
  `--catalog-bad-material-lobes` mode. Existing glass/transmission coverage
  remains in `[gltf-import][material][transmission]`.
- **Docs:** `docs/design/03-file-formats-and-ingestion.md` (v12 layout and
  mapping table), `docs/design/04-rendering-and-streaming.md` (layering/
  transparency), `docs/FORMAT-SUPPORT.md`, `docs/design/interfaces.md`,
  ADR-0074, and supersession notes in ADR-0068/0072. Derived-cache schema is v3.
- **Checks:** `Tests.Unit.exe "[material-lobes]"` 6 cases/67 assertions pass;
  `Tests.ImportIsolation.exe "[material]"` 8 cases/166 assertions pass;
  `[material-lobes]` import 1/15; `[wire-format]` 8/80; `[gltf-extensions]`
  8/140; `[chunk-batch]` 21/102; `[hostile-worker]` 15/304; `[compatibility]`
  11/145. Full `Tests.Unit` is 463/465 with only the two pre-existing stale
  expectations (`dae`, `model.iges`). `Preview3D` Release builds. Run
  import-isolation lanes serially; the sandbox is not parallel-process safe.
- **Deferred:** OIT/depth peeling, material inspection modes, texture-driven
  lobe maps, thumbnail clearcoat/sheen shading, anisotropy/iridescence/volume/
  dispersion rendering, and wiring `DerivedCache::parserVersion` from the
  protocol version (the cache is not currently populated in this tree).

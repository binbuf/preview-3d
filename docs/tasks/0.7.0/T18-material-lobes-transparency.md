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
_(filled in by the implementing session: payload layout/version, field mapping,
shader layering, OIT choice, fixtures/goldens and remaining limitations.)_

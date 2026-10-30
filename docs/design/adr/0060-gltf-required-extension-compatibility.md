# 0060 — glTF required-extension compatibility filtering

- Status: Accepted
- Context: task T05 (0.7.0)

## Context

A glTF that lists a visual-only extension in `extensionsRequired` used to make
fastgltf return `MissingExtensions`/`UnknownRequiredExtension`, which the worker
mapped unconditionally to `UnsupportedRequiredFeature`. The same happened to a
truly unknown required name even when the core scene was complete. That rejected
valid files whose only missing feature was a material lobe the renderer does not
draw yet.

## Decision

The adapter owns a table-driven extension policy with four decisions per name:
class, fastgltf parser bit, whether the adapter consumes the data completely, and
therefore whether a warning is needed.

1. Enable in the fastgltf mask every extension whose data path the adapter can
   consume, including the visual/scene names `KHR_materials_clearcoat`, `sheen`,
   `specular`, `iridescence`, `anisotropy`, `volume`, `dispersion`,
   `diffuse_transmission`, `emissive_strength`, `KHR_materials_variants`, and
   `KHR_lights_punctual`. `MSFT_texture_dds` stays disabled until T12.
2. Pre-scan the root `extensionsRequired` array with simdjson before parsing.
   Geometry/decoder-critical names (Draco, meshopt, quantization, implicit
   shapes, float64 accessors) are never filtered; an unsupported required one
   fails typed. Texture-critical, visual, scene, instance and truly unknown names
   may be filtered.
3. When a filterable name is present, rewrite only the root
   `extensionsRequired` array in a bounded copy. Padding preserves the JSON length
   and every GLB byte offset, so the already-mapped BIN is reused and the copy is
   capped by the existing metadata bounds. `extensionsUsed` is never edited.
4. Report every unsupported `extensionsUsed` name and any required name omitted
   from `extensionsUsed` through the existing `CompatibilityReport` feature IDs:
   `Degraded` for visual/instance/unknown, `Ignored` for scene. Unknown geometry
   required names remain typed failures, not silent degradations.

## Consequences

- A required clearcoat/specular/emissive-strength file opens with core PBR and a
  specific warning; a bad required Draco still fails typed.
- The filtered retry runs at most once and cannot loop because it is built from
  the pre-scanned classification, not from parser output.
- No numeric limit was added: the required-name count reuses
  `kTierAObjectLimit`, and the filtered copy stays inside the existing 32 MiB
  metadata and Tier A primary-source ceilings.
- Later tasks may promote a visual extension to fully supported by changing its
  policy row and parser/consumer path; they must append feature IDs, never
  renumber.

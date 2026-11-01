# ADR-0083 — Extension-lobe maps use fixed material slots, not appended root constants

- **Status:** Accepted
- **Date:** 2026-10-30
- **Task:** T25

## Context

T22 established a fixed five-slot material texture record (base color, metallic/
roughness, normal, emissive, independent occlusion) and a per-material `b2` CBV
(ADR-0080). Clearcoat, specular, sheen and transmission each author optional
factor/roughness/normal/color maps. Adding them as new root constants or a
variable per-material binding list would grow D3D12's 64-DWORD root signature
without bound and would make the wire record self-describing.

## Decision

- Protocol v14 fixes the material slot set at thirteen in a closed order:
  `{baseColor, metallicRoughness, normal, emissive, occlusion, clearcoat,
  clearcoatRoughness, clearcoatNormal, specular, specularColor, sheenColor,
  sheenRoughness, transmission}`. `ChunkDescriptor` widens to sixteen dependency
  ids (slots 13–15 must stay zero); `MaterialPayload` grows to 440 bytes with
  `clearcoatNormalScale`, thirteen `slotFlags`, and twelve per-slot UV
  transforms. Wire version, cache identity and every producer/consumer move
  together.
- Each map keeps the Khronos channel and color space. Clearcoat factor is R,
  clearcoat roughness G, specular factor A, sheen roughness A, transmission R;
  specular color and sheen color are sRGB RGB maps. The coat normal uses its own
  scale and perturbs only the coat lobe.
- The textured root signature uses fifteen one-descriptor SRV tables (thirteen
  material slots + studio cubemap + BRDF LUT), the 25-DWORD draw block and two
  CBVs: 44 of 64 DWORDs. No per-slot data is placed in root constants.
- The per-material CBV holds thirteen `gSlotUv/gSlotParams` pairs and keeps a
  fixed 1024-byte per-frame stride. Unbound slots bind one of thirteen neutral
  descriptors; the shader's map mask skips them, so a fallback descriptor is
  never presented as an authored resident map.
- Debug channel 233 selects an extension slot; fields 234–242 expose its
  metadata and binding through a bounded window.

## Consequences

- Adding a future extension map is another versioned slot and descriptor table,
  not an unbounded root-constant append. The current 44-DWORD budget leaves 20
  DWORDs before the hard limit.
- The material CBV grows from 304 to 560 bytes; at the 1024-byte stride this is
  still one fixed, aligned region per frame per material chunk.
- Anisotropy, iridescence, volume, dispersion and diffuse transmission remain
  parsed-but-unrendered and keep their specific compatibility records; this ADR
  claims only the four rendered lobes.
- Tests must prove per-texel variation and fallback distinction: a factor-only
  value is not evidence that a map path works.

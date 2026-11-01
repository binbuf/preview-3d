# ADR 0074 - Material lobes, fixed-record versioning and transparency limits
Status: accepted (T18, 0.7.0)

## Context
glTF clearcoat, sheen, specular/IOR and emissive strength are common in modern
PBR assets, but the 72-byte `MaterialPayload` had no fields for them. The shader
used a fixed 0.04 dielectric F0 and ignored emissive strength, so authored
appearance collapsed to core metallic-roughness. Adding fields to a packed wire
record is a protocol change; reusing the old reserved words silently would let a
v11 producer and a v12 consumer disagree about the same bytes. Glass also needs
to composite after opaque geometry but before the HDR tone map.

## Decision
- Bump `kCurrentProtocolVersion` to 12 and grow `MaterialPayload` to 140 bytes
  with `emissiveStrength`, clearcoat factor/roughness, sheen color/roughness,
  specular factor/color, `ior`, `occlusionStrength`, and four reserved words
  that must be zero. Old sections are rejected outright, not migrated.
- Share one field validator (`ValidateMaterialLobeFields`) between the
  `SharedSectionValidator` trust boundary and the `[material-lobes]` unit tests.
- Normalize clearcoat/sheen/specular/emissive strength from fastgltf and the
  ufbx adapters. Scalar lobes are carried; texture-driven lobe maps emit a
  bounded texture warning because the fixed record has no extra texture slots.
  Iridescence/anisotropy/volume/dispersion stay parsed-but-unrendered and keep a
  specific `Degraded` compatibility record.
- Layer clearcoat as a second dielectric GGX lobe and scale base specular by
  `(1 - clearcoatFactor)` so energy is not double-counted. Sheen is a
  grazing-only velvet term. IOR/specular replace the fixed dielectric F0.
  Emissive strength multiplies before the resolve. Clay mode suppresses lobes.
- Keep straight-alpha transparency, stable-sort blend draws after opaque, and
  let T15's resolve own exposure/tone mapping. Defer weighted blended OIT and
  depth peeling; document sorted-path order dependence for intersecting glass.
- No format that does not author these fields changes appearance: the new
  fields default to the exact pre-v12 values.

## Consequences
- Every producer and consumer of `MaterialPayload` moves with protocol v12;
  cache entries from v11 cannot be reused. Hostile-worker, validator, wire and
  import-isolation tests cover the new record.
- Approximations are explicit rather than silently exact: clearcoat/sheen are
  scalar lobes, transmission is Fresnel-weighted alpha, and OIT is a documented
  remaining limitation.
- A future anisotropy/iridescence/volume task can use the reserved words only
  through another version bump, preserving the "zero means not authored"
  contract.
- T26 supersedes the OIT deferral: the product path is now bounded weighted
  blended OIT with a recorded sorted fallback (ADR-0084). The sorted path is no
  longer described as correct for intersecting transparent surfaces.

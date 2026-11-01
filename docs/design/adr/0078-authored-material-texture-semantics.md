# ADR-0078 — Authored material texture semantics are normalized, not inferred

- Status: Accepted
- Date: 2026-10-30
- Task: T22

## Context

The 0.7.0 renderer lost or reinterpreted authored material inputs: normal
scale was dropped, occlusion was read from the metallic-roughness red channel,
one shared UV transform was applied to every slot, and an authored OPAQUE
transmission was rewritten to BLEND. The glTF corpus (notably Car's 0.45/0.37
normal scales and two occlusion bindings) and the Suit (transmission=1,
ior=1 on every material) exercise these paths.

## Decision

1. Normalize authored semantics explicitly. Protocol v13 `MaterialPayload`
   (244 bytes) carries `normalScale`, per-slot UV set/sampler/transform
   metadata, and an independent occlusion field. Material image dependencies
   are `{baseColor, metallicRoughness, normal, emissive, occlusion}`; the
   dependency array is widened to 8 and slots 5-7 must be zero.
2. The metallic-roughness red channel is never AO. AO is sampled only from an
   authored occlusion binding.
3. Only UV set 0 is supported by the current vertex layouts. An unsupported
   set leaves its slot unbound with the specific
   `kTextureFeatureUnsupportedUvSet` record; it is never aliased to UV0.
4. Authored `alphaMode`/cutoff are preserved. Transmission, unlit and the
   alpha bucket are independent normalized facts; the renderer derives its
   blend policy from the transmissive flag.
5. Intentional compatibility conversions require an explicit, tested
   diagnostic: IOR<=1 transmission suppression (Suit) and degenerate-BLEND
   recovery are `CompatibilityRecord` ids 9 and 10. No asset- or
   material-name exceptions are permitted.
6. Derived-cache keys use the actual parser/protocol/material/vertex identity
   (`kDerivedCacheParserIdentity`); cache schema is 4.

7. The renderer consumes every v13 field: normal scale multiplies the sampled
   tangent-space XY, each slot applies its own UV set/transform/sampler, and AO
   is sampled only from the occlusion slot. Double-sided back faces reverse the
   geometric normal before the TBN is formed; a mirrored instance flips tangent
   handedness by its determinant sign; a zero tangent falls back to
   screen-space derivatives. The binding mechanism (per-material `b2` CBV,
   one region per swap-chain frame) is ADR-0080.

## Consequences

- Every producer/consumer and the hostile-section validator move together with
  protocol 13. Older entries cannot be reused.
- Adding a new material slot or transform layout is a wire change.
- The renderer now depends on the v13 record; a producer that leaves a slot
  flag zero inherits the material-wide address/nearest fallback.
- Only UV set 0 exists on the wire. Supporting UV1 requires a vertex-layout and
  wire change, not a shader-side alias.
- Evidence is the generated `material-semantics` fixture, its numeric Debug
  readback (normal-scale ordering, independent AO 0.2 versus MR-red 1.0,
  UV1-unbound, MASK alpha, IOR=1 suppression) and the Car/Camera/Suit/
  microscope captures.

# ADR-0078 — Authored material texture semantics are normalized, not inferred

- Status: Accepted for the wire/import contract; renderer application pending
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

## Consequences

- Every producer/consumer and the hostile-section validator move together with
  protocol 13. Older entries cannot be reused.
- Adding a new material slot or transform layout is a wire change.
- The renderer must eventually consume the v13 fields; until it does, the
  normalized data is ahead of the image. That gap is recorded as a T22
  follow-up and must not be papered over with MR-red AO.

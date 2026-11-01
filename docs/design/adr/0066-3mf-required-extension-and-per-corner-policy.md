# ADR-0066 — 3MF best-effort required-extension and per-corner material policy

- **Status:** Accepted
- **Date:** 2026-10-29
- **Task:** T10b (3MF best-effort compatibility)

## Context

The 3MF adapter previously rejected any `requiredextensions` namespace outside
Core, Materials, Production, Beam and Ball, and rejected a triangle whose three
corners resolved to different texture/sampler/texture-layer/mix groupings.
Real slicer exports routinely declare private metadata namespaces and can
produce corner seams that must not discard otherwise usable Core geometry.
Slice, Volumetric, Implicit and Secure Content, by contrast, carry the visible
geometry itself.

## Decision

Required namespaces are classified at the product-owned XML pre-scan, before
lib3mf runs:

| Class | Examples | Result |
| --- | --- | --- |
| Understood | Core, Materials, Production, Beam, Ball | Continue, no record |
| Geometry-replacing | Slice (`1001`), Volumetric (`1002`), Implicit (`1003`), Secure Content (`1004`) | `UnsupportedRequiredFeature`; the feature ID is retained on the display catalog |
| Unknown | Slicer-private or any other namespace | Continue with an `Ignored` compatibility record (`1000`); Core mesh/build data stays authoritative |

An undeclared required prefix stays a typed failure because the XML is
malformed.

Per-corner material variation is handled in the adapter rather than the parser:

- If all corners share a grouping, emit one triangle as before.
- Otherwise, if a centroid fan (three sub-triangles, one per corner) keeps the
  whole generation inside the existing triangle and material ceilings, emit the
  fan. Positions, winding, bounds and authored corner UVs are unchanged; each
  piece carries a single material grouping.
- Otherwise, or when a corner-only image/material cannot be produced, use the
  first most-frequent corner (deterministic tie-break by lowest corner index)
  for the whole triangle. Never return `ResourceLimit` while a usable preview
  remains.
- Report `Degraded` records `1010` (split) or `1011` (dominant).

The OPC/ZIP trust boundary and all archive limits are untouched. No new numeric
limit is introduced; the triangle fan reuses `kTierBTriangleLimit` and
`kTierBMaterialLimit`.

## Consequences

- Slicer-metadata packages open and report exactly what was ignored.
- A required slice/volumetric/implicit/secure-content package still fails with
  the same typed error and now retains a numeric feature ID for diagnostics.
- Material chunk ordering stays deterministic (first-use order, then deduped
  key), and scan/refinement identities remain stable because geometry only
  changes by a bounded fan when per-corner grouping is actually present.
- lib3mf 2.5 binds one property resource to a triangle, so a compliant file
  cannot express differing texture identities across corners through the typed
  API. The split/degrade rule is therefore defensive for extended producers and
  is covered by a pure planner test plus a real per-corner UV fixture.

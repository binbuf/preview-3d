# ADR-0067 — USD procedural Gprims, points and control-cage fallback

- **Status:** Accepted
- **Date:** 2026-10-29
- **Task:** T11 (USD primitives, points and crease tolerance)

## Context

TinyUSDZ's render-scene converter only represents triangle meshes, so the fast
path classified visible `Sphere`, `Cube`, `Cylinder`, `Cone`, `Capsule`,
`Points` and creased/subdivided meshes as `UnsupportedRequiredFeature`. The
OpenUSD compatibility host emitted only `UsdGeomMesh`, and the broker retried
the host only for a pre-publication `UnsupportedComposition`. A USD scene made
only of primitives therefore could not open at all.

## Decision

- The OpenUSD host owns deterministic, product-owned tessellation. It does not
  link OpenSubdiv and adds no subdivision-level option.
- Fixed segment counts: sphere 24 longitude x 16 latitude bands; capsule 24
  longitude x 8 latitude bands per hemisphere plus the cylindrical spine; cube
  12 triangles; cylinder and cone 24 radial segments with caps. `radius`,
  `height`, `size` and the X/Y/Z `axis` token (default Z) are honored. `extent`
  remains only an authoring hint; bounds are recomputed from generated vertices
  and revalidated by the shared-section validator.
- `UsdGeomPoints` is emitted as a `PointList` chunk with display color and
  opacity. Authored `widths` are ignored and produce an optional warning; the
  viewer's existing bounded point-splat size policy remains authoritative.
- Creases and an authored non-`none` subdivision scheme render the authored
  control cage and increment the compatibility warning count (`Degraded`).
  An unauthored scheme keeps the historical plain-mesh fast path.
- The fast path signals host-supported primitives, points and creases with the
  existing closed `UnsupportedComposition` code. No new error code is added.
  The broker still accepts that code only from the fast worker, for USD, and
  before any batch has been accepted; a late signal is a protocol violation.
  Malformed, unsafe, over-limit and genuinely unsupported schemas stay terminal.

## Consequences

- Primitive-only, point-only, mixed and creased USD scenes open with correct
  bounds, colors and warning severity.
- Mesh-only USD files retain byte-equivalent fast-path output; only primitives,
  points and explicitly subdiv/creased meshes take the host retry.
- Curves/NURBS remain terminal until line topology work (T17); this task does
  not introduce a renderer feature beyond `PointList`.
- The tessellation budgets reuse `kTierBTriangleLimit`, `kTierBVertexLimit`,
  `kTierAObjectLimit`, the section byte capacity and the host deadline; no new
  numeric ceiling is introduced.

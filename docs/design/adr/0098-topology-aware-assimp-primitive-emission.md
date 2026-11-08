# ADR-0098 — Topology-aware Assimp primitive emission

**Status:** Accepted (amends ADR-0065's primitive scope).

## Context

ADR-0065 admitted the Assimp compatibility family but only emitted faces with
exactly three indices and skipped any mesh without `aiPrimitiveType_TRIANGLE`.
That rejected valid line sets in DXF, OFF, DAE and X3D, and point sets that the
importers represent as one-index faces. The wire contract already carries
`ChunkTopology::LineList` and `PointList`, and the renderer already draws both.

## Decision

- The adapter classifies every face by index count: one index is a point
  (`PointList`, no index buffer), two indices are one independent segment
  (`LineList`, even index count), and three or more indices are triangles.
  Polygons larger than three are fan-triangulated as a bounded fallback for
  importers that leave them behind after `aiProcess_Triangulate`; a line or
  point is never expanded into a triangle.
- A mixed source mesh emits one chunk per topology and material. The chunk
  order is deterministic (triangles, then lines, then points) and the existing
  material/texture mapping and vertex layout are reused, so authored normals,
  UVs and colors survive on all three topologies.
- Degenerate line segments (identical world positions) and non-finite points or
  line endpoints are skipped with the bounded compatibility-warning budget
  rather than failing an otherwise usable scene. Invalid indices remain a typed
  `MalformedData` failure.
- `EmptyGeometry` is decided from validated emitted primitives, never from
  Assimp's primitive flags. A mesh with no supported face is skipped with a
  bounded warning; a file with only unsupported content fails typed.
- Per-topology chunk limits are derived from the same output window and the
  existing per-chunk primitive ceilings (`kChunkTriangles` for triangles and
  lines, `kChunkPoints` for points). Every emitted primitive counts against the
  Tier-B `maxTriangles` face budget and every vertex against `maxVertices`, so
  lines and points cannot bypass the existing budgets.
- The OFF admission graph accepts one- and two-index faces. Assimp's synthetic
  skeleton "hero" mesh is disabled because animation is out of scope and an
  empty COLLADA document must not present fabricated triangles.

## Consequences

Line-only and point-only Assimp files open with visible geometry instead of
`EmptyGeometry`; mixed files publish separate chunks that the existing line PSO
and point-splat pipeline already consume. COLLADA `<points>` remains a typed
failure because Assimp's COLLADA importer has no point primitive. No path,
network, file-write or child-process authority changes: the broker `IOSystem`,
admission, cancellation, validation and scratch/texture budgets are untouched.

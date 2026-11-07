# ADR-0099 — CAD edge/wire fallback and LineList emission

**Status:** Accepted (extends ADR-0062/0063/0064's CAD scope; leaves the wire
protocol unchanged).

## Context

The dedicated CAD host walked only `TopAbs_FACE` shapes. A valid STEP, IGES,
BREP, or FCStd definition whose visible geometry is edges or wires (a
wireframe part, a free polyline, a draft sketch) produced no triangles, no
chunks, and ended as `EmptyGeometry`. The wire contract already carries
`ChunkTopology::LineList` (protocol v11) and the viewer already draws it with
the depth-biased line PSO, so this is adapter work, not a renderer or wire
version change.

## Decision

- After a definition's face pass, if the definition produced **no triangles**
  (or an explicit `readWire` request is active), run one bounded edge pass:
  collect the definition's `TopAbs_EDGE` shapes, mesh them as a single
  `TopoDS_Compound` with the same versioned `StepTessellationProfile`
  deflection/angular deflection, and read each edge's
  `BRep_Tool::Polygon3D`.
- Apply the polygon's `TopLoc_Location` transform exactly as the face path
  does. Expand a polyline into consecutive independent pairs (`0-1`, `1-2`, …)
  because `LineList` is a pair list, never a strip. Degenerate/non-finite
  segments are skipped with a bounded warning; a chunk is never emitted with
  fewer than two distinct finite points.
- Emit one de-indexed `LineList` chunk per material or bounded group using the
  existing `PositionNormalUv0_F32` layout and the same double-origin/cluster
  scheme as triangles. `GeometryRecord` and `AddGeometry` now carry the
  topology identity; the wire version is not bumped.
- Budgets are versioned profile members: `maxEdgesPerDefinition` (existing),
  `maxLineSegmentsPerDefinition`, and `chunkLineSegments`. Face and edge
  counts are checked **before** `BRepMesh` runs, and a polygon's segment count
  is checked before its vertices are appended. Exceeding any budget fails
  typed (`TessellationFailed`), never by hanging or allocating unboundedly.
- Edge styles come from the XDE reader where it exposes them
  (`XCAFDoc_ColorCurv`, then `ColorGen`), with the definition-level shape
  color as fallback. Otherwise the chunk uses neutral material `0`; no
  per-edge material is invented.
- **Fallback policy:** solids keep triangles only. Edges are emitted alongside
  faces only through the internal `StepImportPolicy::readWire` seam, which
  T06 exposes as the user-facing `read_wire` option. T05 leaves it false.
- Empty/repair policy: a definition with edges but no faces is visible, and
  the generation succeeds when either triangles or lines were emitted.
  `TryBoundedRepair` no longer rejects a zero-face shape solely for that
  reason, while the repair trigger remains limited to a face-bearing
  definition that actually failed to tessellate.
- `kStepTessellationProfileVersion` is now 7, so the STEP importer/cache
  identity changes with the new emitted geometry.

## Consequences

Wireframe and edge-only STEP/IGES/BREP/FCStd files open with visible lines and
verified finite bounds instead of `EmptyGeometry`. Solid files keep their
existing triangle output and gain no default edge overlay. No parser or path
authority enters the viewer: OCCT stays in `Preview3DStepHost.exe`, and the
new chunk type is the already-validated `LineList`. T06 can enable the
internal `readWire` seam without another adapter change; USD curves remain a
separate limitation.

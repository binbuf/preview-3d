# ADR 0073 - Line topology, strip/fan normalization and the line pipeline

Status: accepted (T17, 0.7.0)

## Context

The normalized wire format had only `TriangleList` and `PointList`. glTF
triangle strips/fans, glTF/OBJ/FBX line curves, and USD curve/STEP wire
prospects were either skipped or failed as `EmptyGeometry`. The task required a
new renderable topology without smuggling lines in as degenerate triangles, a
breaking wire decision that older consumers cannot misinterpret, and a
depth-biased renderer path.

## Decision

- Append `ChunkTopology::LineList = 11` and bump `kCurrentProtocolVersion` to
  11. Every v10 numeric identity is unchanged; old consumers reject v11 rather
  than misread line indices. Cache entries bump to schema v2 because normalized
  bytes can now use the new topology.
- `LineList` uses the existing `[vertices][uint32 indices]` packing.
  `indexCount` is even and each pair is one independent segment. Strips/loops
  are converted in the adapter. `PointList` keeps its no-index contract.
- `SharedSectionValidator` rejects odd index counts, out-of-range indices,
  wrong byte sizes, and non-finite vertices/bounds. Hostile-worker mode
  `--lie-line-topology` publishes an odd count to prove the typed rejection.
- glTF triangle strips/fans are expanded to indexed triangles in
  `GltfAdapter.cpp` (degenerate stitch triangles dropped). glTF
  `LINES`/`LINE_STRIP`/`LINE_LOOP` become LineList; `POINTS` stays PointList.
  OBJ `l`/`p` elements are scanned directly when ufbx exposes no mesh, because
  ufbx does not surface them. FBX `ufbx_line_curve` elements emit LineList.
- The viewer gets a position-only line PSO with `PrimitiveTopologyType_LINE`,
  negative depth bias/clamp, `LESS_EQUAL`, and depth writes disabled. Lines use
  a stable grey-blue color, still write the pick target, and participate in
  bounds, visibility, LOD and coarse proxy sampling.
- USD curves/NURBS and STEP wires remain deferred: USD curves still classify as
  `UnsupportedRequiredFeature`; STEP continues to emit tessellated triangles.
  A later task can emit polylines through the now-available LineList.

## Consequences

- Line-only glTF/OBJ/FBX assets now open instead of returning `EmptyGeometry`;
  malformed line data fails typed as `MalformedData`.
- Protocol-v10 and cache-schema-v1 readers must not be mixed with v11 writers.
- Coarse proxy accounting now divides line indices by two and samples two
  corners per primitive; point clouds are unchanged.
- The line PSO is position-only, so all existing vertex layouts share it; line
  geometry carries no material/texture binding yet.

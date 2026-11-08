---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[assimp-primitives]"
---
# T04 — Assimp point and line primitives

## Goal
Open valid line-only and point-only content in the Assimp compatibility family
instead of failing with `EmptyGeometry`. Today every face whose index count is
not three is skipped, and every mesh without `aiPrimitiveType_TRIANGLE` is
skipped. That rejects valid line sets in DXF, OFF, DAE, and X3D, plus point
clouds those formats can carry.

## Context (read first)
- `import-worker/src/AssimpAdapter.cpp` — `EmitMesh` skips
  `face.mNumIndices != 3` (1080–1086); `WalkNode` skips meshes without
  `aiPrimitiveType_TRIANGLE` (1151–1154); the final check requires
  `geometry.totalTriangles` (1298).
- `shared/model-core/include/model_core/WireFormat.h` — `TriangleList`,
  `LineList` (even index count, pairs are independent segments) and
  `PointList` (no index buffer).
- `import-worker/src/BoundedChunkWriter.h` and `ChunkBatchSink.h` — the
  existing bounded chunk emission and section accounting.
- `interactive-viewer/src/app/D3D12ImportBridge.cpp` — how validated chunks
  become `ImportedMesh` and how line/point topology reaches the renderer.
- `docs/design/03-file-formats-and-ingestion.md` — the Assimp compatibility
  contract and the line/point limitation this task removes.
- F3D reference: `plugins/assimp/module/vtkF3DAssimpImporter.cxx:494-522`
  maps 1-index faces to vertices, 2-index faces to lines, and larger faces to
  polygons. Preview 3D already has the equivalent `PointList`/`LineList`
  wire topologies.

## Scope
- [x] Refactor the Assimp emission path to be topology-aware:
  - 1 index → one `PointList` vertex;
  - 2 indices → one `LineList` segment;
  - 3 indices → one `TriangleList` triangle;
  - larger faces → triangulate with the existing bounded polygon path or
    `aiProcess_Triangulate`; never fabricate triangles from a line or point.
- [x] Handle mixed meshes: one mesh may contain points, lines, and triangles.
  Emit separate chunks per topology and material, preserving deterministic
  order and the existing material/texture mapping.
- [x] Remove the `aiPrimitiveType_TRIANGLE`-only skip in `WalkNode`. Skip a
  mesh only when it contains no supported primitive at all, with a bounded
  compatibility warning.
- [x] Change the final empty check to count all supported primitives, not only
  triangles. A file with only lines or only points is success.
- [x] Preserve vertex colors, normals, UVs, and material association where the
  source provides them. Line rendering may use the existing neutral line PSO;
  point rendering may use the existing splat pipeline. Do not invent material
  semantics the wire contract does not carry.
- [x] Keep the existing Assimp admission, broker `IOSystem`, budgets,
  cancellation, and `aiProcess_ValidateDataStructure` behavior unchanged.
- [x] Add small licensed/generated fixtures:
  - line-only DXF (3DFACE/polyline subset is fine);
  - OFF with edges;
  - DAE with a `<lines>` element;
  - X3D with a line set;
  - a point-only DAE/OFF/X3D if the importer represents it as 1-index faces.
- [x] Add `[assimp-primitives]` cases asserting `LineList`/`PointList` chunks,
  bounds, material/color presence, and that a genuinely empty file still fails
  typed.
- [x] Update `docs/FORMAT-SUPPORT.md` and the task README; remove the
  “line/point-only content is skipped” limitation for the Assimp family.

## Out of scope
- Adding new Assimp format hints (glTF/OBJ/FBX/3MF) — that is T08.
- Full DXF entity/block support. The Assimp DXF subset remains limited.
- Curve/NURBS tessellation beyond what Assimp already produces.
- Animation, cameras, lights, or file export.

## Design notes
- `PointList` has no index buffer; `LineList` uses the same
  `[vertices][uint32 indices]` packing as `TriangleList`, with an even index
  count. The validator already enforces both.
- The chunk writer’s triangle-specific limits and accounting must become
  topology-aware. Keep the same bounded per-chunk primitive ceiling; do not
  let a line/point mesh bypass the object/material/vertex budgets.
- Do not use `aiProcess_SortByPType` as the only mechanism; it is useful, but
  the adapter must handle mixed primitive types even when the importer does
  not split them.
- Degenerate lines (two identical positions) and non-finite points are
  skipped with a bounded warning, not emitted.
- The final `EmptyGeometry` decision must be based on validated emitted
  chunks, not on Assimp’s primitive flags alone.

## Done when
- [x] `x64\Release\Tests.ImportIsolation.exe "[assimp-primitives]"` passes.
- [x] `x64\Debug\Tests.ImportIsolation.exe "[assimp-primitives]"` passes.
- [x] Line-only and point-only fixtures open with visible geometry and no
  `EmptyGeometry`.
- [x] Mixed point/line/triangle fixtures emit separate chunks with correct
  topology and counts.
- [x] Malformed, empty, and over-budget inputs still fail typed.
- [x] No path/network/file-write/child-process authority was added.

## Hand-off

**Topology mapping.** `EmitMesh` now classifies each face by index count:
one index → a `PointList` vertex (no index buffer), two indices → one
`LineList` segment, three or more → `TriangleList`. A larger face is
fan-triangulated as a bounded fallback; `aiProcess_Triangulate` normally
already reduced it. Mixed meshes keep one chunk per topology and material
(triangles, then lines, then points, deterministic) and reuse the existing
`PositionNormalUv0TangentColor_F32` layout, so normals/UVs/colors and the
mesh's material dependency are preserved. `WalkNode` no longer checks
`aiPrimitiveType_TRIANGLE`; `HasSupportedPrimitive` skips only a mesh with no
face carrying an index and records a bounded warning. `EmptyGeometry` is now
decided from `geometry.totalPrimitives`, which counts all three topologies.

**Budgets.** Per-topology chunk limits are derived from the same output window
and the existing primitive ceilings (`kChunkTriangles` for triangles and
lines, `kChunkPoints` for points). Every emitted primitive counts against the
Tier-B `maxTriangles` face budget and every vertex against `maxVertices`, so a
line/point mesh cannot bypass them. Degenerate segments (identical world
endpoints) and non-finite points/line endpoints are skipped with the bounded
optional-warning count; invalid indices remain `MalformedData`.

**Fixtures.** `tests/fixtures/assimp-spike/` gained `lines.dxf`, `lines.off`,
`lines.dae`, `lines.x3d`, `points.off`, `points.x3d`, `mixed.x3d`,
`degenerate-lines.off`, `non-finite-points.off`, `empty.dae`,
`over-budget-lines.off` and `over-budget-points.off`, all generated by the
updated `generate_fixtures.py`. The valid-file corpus was flipped: `line.dxf`,
`line.off`, `line.dae`, `line.x3d` and `points.x3d` now expect `opened` with
`lines`/`points`; `line.off` was regenerated as a valid three-edge OFF.

**Deviations / Assimp quirks.**
- COLLADA `<points>` is not represented by Assimp's COLLADA importer at all.
  `points.dae` previously looked "opened with 8 triangles" only because
  Assimp's `SkeletonMeshBuilder` fabricates a hero mesh for a scene with no
  meshes. The adapter now sets `AI_CONFIG_IMPORT_NO_SKELETON_MESHES`, so a
  point-only DAE fails typed with `EmptyGeometry`; point-only OFF and X3D are
  the supported point paths.
- `OffAdmission` now accepts one- and two-index faces (previously `corners < 3`
  rejected valid edge/point OFF before Assimp saw it).
- `aiProcess_SortByPType` splits most mixed meshes before this adapter sees
  them; the face-by-face implementation still handles an unsplit mixed mesh.
- DXF `LINE`/3D `POLYLINE`, COLLADA `<lines>` and X3D `IndexedLineSet` arrive
  as 2-index faces; X3D `PointSet` arrives as 1-index faces via
  `StandardShapes::MakeMesh`.
- A finite double such as `1e300` becomes infinity when Assimp stores it in a
  float; the non-finite point fixture covers that skip path.
- Assimp can leave NaN normals on line/point corners of a mixed mesh
  (`GenVertexNormalsProcess` writes NaN for faces with fewer than three
  indices). The adapter treats the position as the usability test and falls
  back to a neutral normal, so the line/point is not dropped.

**Checks.**
- `x64\Release\Tests.ImportIsolation.exe "[assimp-primitives]"` — pass
  (79 assertions, 7 cases).
- `x64\Debug\Tests.ImportIsolation.exe "[assimp-primitives]"` — pass
  (79 assertions, 7 cases).
- `x64\Release\Tests.ImportIsolation.exe "[assimp]"` — pass (164 assertions,
  14 cases).
- `x64\Debug\Tests.ImportIsolation.exe "[assimp]"` — pass (164 assertions,
  14 cases).
- `x64\Release\Tests.ImportIsolation.exe "[valid-file],[valid-file-self-check]"`
  — pass (168 assertions, 3 cases); the Release report artifact was
  regenerated with `[write-valid-file-report]`.

**Docs.** `docs/FORMAT-SUPPORT.md`,
`docs/design/03-file-formats-and-ingestion.md`,
`docs/design/adr/0065-bounded-assimp-compatibility-adapter.md` (primitive
scope note), ADR-0098, `tests/fixtures/assimp-spike/README.md` and
`tests/fixtures/valid-file/README.md`. No remaining blockers.

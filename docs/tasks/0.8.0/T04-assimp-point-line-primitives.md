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
- [ ] Refactor the Assimp emission path to be topology-aware:
  - 1 index → one `PointList` vertex;
  - 2 indices → one `LineList` segment;
  - 3 indices → one `TriangleList` triangle;
  - larger faces → triangulate with the existing bounded polygon path or
    `aiProcess_Triangulate`; never fabricate triangles from a line or point.
- [ ] Handle mixed meshes: one mesh may contain points, lines, and triangles.
  Emit separate chunks per topology and material, preserving deterministic
  order and the existing material/texture mapping.
- [ ] Remove the `aiPrimitiveType_TRIANGLE`-only skip in `WalkNode`. Skip a
  mesh only when it contains no supported primitive at all, with a bounded
  compatibility warning.
- [ ] Change the final empty check to count all supported primitives, not only
  triangles. A file with only lines or only points is success.
- [ ] Preserve vertex colors, normals, UVs, and material association where the
  source provides them. Line rendering may use the existing neutral line PSO;
  point rendering may use the existing splat pipeline. Do not invent material
  semantics the wire contract does not carry.
- [ ] Keep the existing Assimp admission, broker `IOSystem`, budgets,
  cancellation, and `aiProcess_ValidateDataStructure` behavior unchanged.
- [ ] Add small licensed/generated fixtures:
  - line-only DXF (3DFACE/polyline subset is fine);
  - OFF with edges;
  - DAE with a `<lines>` element;
  - X3D with a line set;
  - a point-only DAE/OFF/X3D if the importer represents it as 1-index faces.
- [ ] Add `[assimp-primitives]` cases asserting `LineList`/`PointList` chunks,
  bounds, material/color presence, and that a genuinely empty file still fails
  typed.
- [ ] Update `docs/FORMAT-SUPPORT.md` and the task README; remove the
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
- [ ] `x64\Release\Tests.ImportIsolation.exe "[assimp-primitives]"` passes.
- [ ] `x64\Debug\Tests.ImportIsolation.exe "[assimp-primitives]"` passes.
- [ ] Line-only and point-only fixtures open with visible geometry and no
  `EmptyGeometry`.
- [ ] Mixed point/line/triangle fixtures emit separate chunks with correct
  topology and counts.
- [ ] Malformed, empty, and over-budget inputs still fail typed.
- [ ] No path/network/file-write/child-process authority was added.

## Hand-off
_(filled in by the implementing session: topology mapping, fixtures, budget behavior, and any Assimp primitive quirk the next task must know)_

---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[topology]"
---
# T17 — Line/strip/fan/point topology

## Goal
Line-only and strip/fan assets open instead of being silently skipped: glTF
triangle strips/fans become triangles, glTF/OBJ/FBX lines and points become
renderable geometry, USD curves/STEP wires can use the new line topology, and
the renderer has a depth-biased line pipeline.

## Context (read first)
- `docs/proposal.md` §3.10 — topology expansion.
- `shared/model-core/include/model_core/WireFormat.h:40-51` — current
  `ChunkTopology` only has TriangleList/PointList and the fixed descriptor.
- `import-worker/src/GltfAdapter.cpp:1210-1212` — non-triangle primitives are
  skipped.
- `import-worker/src/ObjAdapter.cpp:507-512` — only triangles are emitted.
- `import-worker/src/FbxAdapter.cpp` — unsupported mesh geometry policy.
- `shared/import-broker/src/SharedSectionValidator.cpp` — topology-specific
  validation; new topology needs explicit rules.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:1277-1400` — mesh draw loop,
  point pipeline, wireframe mode.
- `import-worker/src/BoundedChunkWriter.h` — chunk emission and layout IDs.
- T11 — USD points; T07/T08 — STEP wires; coordinate line emission.

## Scope
- [ ] Add a `LineList` topology (and, only if useful, `LineStrip`) with a
      documented vertex layout and index rules. Update:
      - `ChunkTopology`, descriptor validation, byte-size/layout helpers;
      - `BoundedChunkWriter` and any chunk-batch accounting;
      - `SharedSectionValidator` (counts, index range, topology/dependency
        rules) and hostile-worker tests;
      - renderer pipeline/PSO for lines with depth bias and a stable color
        (constant or vertex color).
- [ ] Convert triangle strips/fans to indexed triangles in the glTF adapter
      (no wire change needed for these).
- [ ] Emit lines/points for:
      - glTF `LINES`, `LINE_STRIP`, `LINE_LOOP`;
      - OBJ line/point elements where ufbx exposes them;
      - FBX line/point geometry where the ufbx API exposes it;
      - USD `BasisCurves`/`NurbsCurves` as polylines (if T11 has points and the
        data is bounded);
      - STEP wires when `read_wire` is enabled (coordinate with T07).
- [ ] Keep point clouds unchanged; use the existing PointList path. Add a
      point-size/roundness check only if the format needs it.
- [ ] Ensure line geometry participates correctly in:
      - bounds/proxy/LOD and visibility;
      - picking (either a narrow line pick or an explicit “not pickable” flag);
      - transparency sorting (lines are usually opaque; document exceptions).
- [ ] Add fixtures:
      - glTF triangle-strip and triangle-fan meshes;
      - glTF line-strip/line-loop;
      - OBJ line-only/point-only;
      - FBX line/point (if available);
      - USD curve (if T11 supports) and STEP wire (if T07/T08 support).
- [ ] Add `[topology]` cases asserting chunk counts/topology, bounds, and that
      a line-only file no longer returns `EmptyGeometry`.
- [ ] Update wire-format docs, protocol version/compatibility notes and
      cache/importer identity.

## Out of scope
- Full NURBS curve evaluation; polylines/control points are acceptable first.
- Tube/ribbon generation for lines; use a line pipeline.
- Mesh shaders or geometry shader line expansion beyond what the existing
  pipeline needs.
- Material lobes (→ T18).

## Design notes
- A topology change is a breaking wire/version decision. Add the topology
  append-only, bump the protocol version if old consumers would reject it, and
  update fixtures/hostile-worker cases. Do not smuggle lines in as degenerate
  triangles.
- Define whether `LineList` uses two indices per segment and whether strips are
  converted to lists. Prefer conversion in the adapter for determinism.
- The validator must reject odd counts, out-of-range indices and a topology
  whose vertex layout does not match.
- Line depth bias must keep lines visible on coplanar surfaces without
  z-fighting; test at multiple scales.
- If a format only has points/lines and the product cannot render them yet, it
  still fails typed with a clear “unsupported geometry” reason, not
  `EmptyGeometry`.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe "[topology]"` passes.
- [ ] `x64\Release\Tests.Unit.exe "[wire-format]"` passes (new topology/
      validator cases).
- [ ] A triangle-strip/fan glTF renders complete geometry.
- [ ] A line-only glTF/OBJ opens and displays lines; a point-only file still
      works.
- [ ] Malformed/odd-count line data fails typed; hostile-worker cases cover
      the new topology.
- [ ] Wire docs/version and cache identity are updated.
- [ ] Hand-off lists the exact topology/layout semantics and format coverage.

## Hand-off

### Topology and wire version

- `ChunkTopology::LineList = 11`, `kCurrentProtocolVersion = 11` (append-only:
  all v10 numeric identities unchanged; v10 consumers reject v11 instead of
  misreading line indices).
- `LineList` payload is `[vertex bytes][uint32 indices]` like `TriangleList`.
  `indexCount` is even; pair `{indices[2i], indices[2i+1]}` is one independent
  segment. Strips/loops are converted by the adapter, never carried on the
  wire. Points keep the existing no-index `PointList`.
- `WireFormat.h` gained `IsGeometryTopology`, `GeometryPrimitiveCount` and
  `GeometrySourceElementCount`; `BoundedChunkWriter`, `CoarseSampler`,
  `ImportSession` and `SharedSectionValidator` use them. The validator rejects
  odd counts, out-of-range indices, wrong byte size and non-finite data.
- `DerivedCache` schema version is now 2 (protocol/topology identity change).

### Strip/fan and line emission

- glTF triangle strips/fans become indexed `TriangleList` (stitch degenerates
  dropped); `LINES` becomes `LineList`, `LINE_STRIP`/`LINE_LOOP` become pair
  lists, `POINTS` stays `PointList`.
- OBJ `l`/`p`: ufbx does not expose these elements, so when no triangle geometry
  exists the adapter scans the bounded OBJ text directly. `l` is an open
  polyline pair list, `p` is `PointList`; invalid/out-of-range/too-short
  elements are `MalformedData`.
- FBX `ufbx_line_curve` elements emit transformed `LineList` pairs.

### Rendering

- New position-only line PSO in `D3D12ViewerPath`: `LINE` topology, negative
  depth bias/clamp, depth writes disabled, `LESS_EQUAL`, stable grey-blue color.
  Lines still write the pick target and flow through existing bounds, proxy/LOD,
  visibility and instance handling.
- `GpuMesh::lines` selects `D3D_PRIMITIVE_TOPOLOGY_LINELIST` and rejects a
  partial segment at upload time.

### Coverage

- Fixtures owned by the repo: `tests/fixtures/topology/` (glTF strip/fan/line
  strip/line loop/LINES plus OBJ line-only and point-only), opened through the
  real sandboxed import session by `[topology]`.
- `[topology]` also covers validator acceptance/rejection and
  `--lie-line-topology` hostile-worker odd-count rejection.
- USD curves/NURBS and STEP wires remain deferred (USD still
  `UnsupportedRequiredFeature`; STEP still emits tessellated triangles). The
  wire topology is ready for a follow-up. Line material/color and line-width
  policy are also deferred.

### Checks (Release x64)

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' Preview3D.slnx /t:Tests_ImportIsolation /p:Configuration=Release /p:Platform=x64
& '...\MSBuild.exe' Preview3D.slnx /t:Tests_Unit /p:Configuration=Release /p:Platform=x64
& '...\MSBuild.exe' Preview3D.slnx /t:Preview3DHostileWorker /p:Configuration=Release /p:Platform=x64
& '...\MSBuild.exe' Preview3D.slnx /t:Preview3D /p:Configuration=Release /p:Platform=x64

x64\Release\Tests.ImportIsolation.exe "[topology]"       # 7 cases / 92 assertions, pass
x64\Release\Tests.Unit.exe "[wire-format]"               # 7 / 66, pass
x64\Release\Tests.Unit.exe "[cache]"                     # 8 / 71, pass
x64\Release\Tests.ImportIsolation.exe "[gltf-import]"    # 31 / 655, pass
x64\Release\Tests.ImportIsolation.exe "[obj]"            # 10 / 110, pass
x64\Release\Tests.ImportIsolation.exe "[fbx]"            # 31 / 48044, pass, 1 external skip
x64\Release\Tests.ImportIsolation.exe "[hostile-worker]" # 15 / 304, pass
```

### Docs changed

- `docs/design/03-file-formats-and-ingestion.md` (protocol v11, LineList rules,
  dependency table).
- `docs/design/adr/0073-line-topology-and-strip-fan-normalization.md`.
- `docs/FORMAT-SUPPORT.md` (glTF/OBJ/FBX line and strip/fan support).
- `docs/0.7.0/progress/T17.md`.

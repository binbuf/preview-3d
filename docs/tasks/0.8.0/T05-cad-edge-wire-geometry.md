---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[cad-wires]"
---
# T05 — CAD edge and wire geometry

## Goal
Open valid wireframe and edge-only STEP, IGES, BREP, and FCStd files. Today the
CAD host only walks `TopAbs_FACE`; a file whose visible geometry is edges or
wires produces no chunks and ends as `EmptyGeometry`. The wire contract already
has `LineList`, so this is adapter work, not a renderer redesign.

## Context (read first)
- `compatibility-host-step/src/StepXdeAdapter.cpp` — the face-only mesh walk
  (1241–1349), the `definitionTriangles == 0` silent-skip path (1335–1342),
  the final `EmptyGeometry` decision (1987), and `TryBoundedRepair`’s
  `faces == 0` refusal (1446).
- `compatibility-host-step/src/StepHostImport.cpp` and `StepTessellationProfile.h`
  — the shared CAD quality/deflection profile and per-definition budgets.
- `shared/model-core/include/model_core/WireFormat.h` — `LineList` payload
  rules: `[vertices][uint32 indices]`, even index count, each pair one
  independent segment.
- `interactive-viewer/src/app/D3D12ImportBridge.cpp` and the line PSO path —
  validated `LineList` chunks already render with the existing depth-biased
  line pipeline.
- F3D reference: `plugins/occt/module/F3DOCCTShapeConverter.cxx:70-110,
  147-177` meshes a compound of edges, reads `Poly_Polygon3D`, and inserts
  polylines; `FORMAT.inl.in` exposes `read_wire` with a default of true.
  `plugins/occt/module/Testing/TestF3DFCStdReader.cxx:184-197` covers a
  `draft_wireframe.FCStd` case. Use these as behavior references, not runtime
  dependencies.

## Scope
- [x] Add an edge pass to the CAD host’s per-definition mesh path:
  - collect `TopAbs_EDGE` shapes (or a compound) under the definition;
  - run `BRepMesh_IncrementalMesh` with the same versioned deflection profile;
  - read `BRep_Tool::Polygon3D` and emit one `LineList` chunk per material or
    bounded group, applying the definition’s location/world transform;
  - keep edge counts, point counts, and segment counts inside the existing
    per-definition and generation budgets.
- [x] Make `GeometryRecord`/`AddGeometry` topology-aware if they currently
  assume `TriangleList`. Do not bump the wire version: `LineList` already
  exists and is validated.
- [x] Fallback policy for this task: emit edges only when the definition
  produced no triangles. Do not draw every face edge over a solid by default;
  T06 will expose an explicit `read_wire` option for users who want edges
  alongside faces.
- [x] Update the empty/repair logic:
  - a definition with edges but no faces is not “nothing visible”;
  - `TryBoundedRepair` must not reject a shape solely because it has zero
    faces; skip repair for a wire-only shape or repair the edge geometry
    without requiring a face;
  - the final result is success when either triangles or lines were emitted.
- [x] Preserve authored colors/transparency for edges where the XDE reader
  exposes them; otherwise use the neutral line appearance. Do not invent
  per-edge materials that the source does not author.
- [x] Add small licensed/generated fixtures:
  - wireframe STEP (edges only);
  - wireframe IGES;
  - BREP line/wire shape (for example an `infinite_line.brep`-style fixture if
    license/provenance permits, or a generated bounded line);
  - FCStd draft/wireframe part with a Part shape that has edges but no faces.
- [x] Add `[cad-wires]` cases asserting `LineList` chunks, bounds, finite
  positions, no `EmptyGeometry`, and that a solid file still produces
  triangles and no unexpected default edge overlay.
- [x] Update `docs/FORMAT-SUPPORT.md` and the task README: remove the
  “wireframe-only content fails” limitation and document the default fallback.

## Out of scope
- The user-facing `read_wire` option and deflection/quality controls — T06.
- Curves/NURBS in USD; USD curves remain a separate limitation.
- PMI/GD&T, exact measurement, shape healing, or editing.
- New wire-format topology; `LineList` is sufficient.

## Design notes
- Reuse the existing `StepTessellationProfile` deflection and angular
  deflection. Do not add a second tolerance model.
- Edge tessellation must be deterministic and bounded. A shape with millions
  of edges must fail `TessellationFailed`/`ResourceLimit` before allocation,
  not hang the host.
- Apply the same `TopLoc_Location` transform used for faces; mirrored
  locations must not reverse line order in a way that breaks bounds.
- `LineList` indices are independent segments. If `Poly_Polygon3D` gives a
  polyline, expand it into consecutive pairs (`0-1`, `1-2`, …) rather than
  emitting a strip the validator does not accept.
- Do not emit a line chunk with fewer than two distinct finite points.
- Keep the existing cancellation checks between bounded edge groups.

## Done when
- [x] `x64\Release\Tests.ImportIsolation.exe "[cad-wires]"` passes.
- [x] `x64\Debug\Tests.ImportIsolation.exe "[cad-wires]"` passes.
- [x] Wireframe STEP, IGES, BREP, and FCStd fixtures open with visible lines
  and finite bounds.
- [x] Solid STEP/IGES/BREP/FCStd fixtures still open with triangles and no
  default edge overlay.
- [x] Over-budget edge shapes fail typed, not with a hang or crash.
- [x] No parser or path authority entered the viewer; the dedicated CAD host
  still owns OCCT.

## Hand-off

**Edge extraction approach.** `DefinitionMesher::Mesh` now counts faces and
edges before `BRepMesh`, runs the existing face pass, and when the definition
produced no triangles (or the internal `readWire` seam is true) collects the
visible `TopAbs_EDGE` shapes into one `TopoDS_Compound`, meshes it with the
same versioned `IMeshTools_Parameters`, reads `BRep_Tool::Polygon3D` per edge,
applies the polygon's `TopLoc_Location`, and expands each polyline into
independent consecutive pairs. Vertices are de-indexed
`PositionNormalUv0_F32`, grouped by material with the same double-origin /
cluster scheme as triangles, and emitted as `LineList` chunks (one per
material or bounded group). `GeometryRecord`/`AddGeometry` are topology-aware;
the wire version did not change. Degenerate/non-finite segments and edges
without a usable polygon are skipped with the bounded warning budget.

**Fallback policy.** Edges are emitted only when a definition produced no
triangles. `StepImportPolicy::readWire` is the internal T06 seam and is false
for this task, so solids keep triangle-only output and get no default edge
overlay.

**Colors.** Authored edge styles are read from `XCAFDoc_ColorCurv` then
`XCAFDoc_ColorGen`, with the definition-level shape color as fallback. No
per-edge material is invented; the neutral line appearance is material 0 (no
`Material` chunk). The current wire fixtures are uncolored and the
`[cad-wires]` cases assert no material chunk.

**Empty/repair logic.** A definition with edges but no faces is visible.
`TryBoundedRepair` now rejects only a shape with neither faces nor edges, and
the repair trigger is limited to a face-bearing definition that actually
failed to tessellate. The final `EmptyGeometry` decision still keys on emitted
instances, so success follows either triangles or lines.

**Fixtures.** Reused the T01 generated fixtures under
`tests/fixtures/valid-file/`: `wire.step`, `wire.igs`, `wire.brep`,
`wire.FCStd`. The IGES generator's global-section Hollerith lengths were
corrected (`8Hwire.igs`, `9HPreview3D`); the old `13Hwire.igs` shifted the
unit flag and produced a spurious non-millimetre warning.

**Budgets.** Profile v7 adds `maxLineSegmentsPerDefinition = 4'000'000` and
`chunkLineSegments = 65'536`; the existing `maxEdgesPerDefinition =
2'000'000` still applies. Edge/face counts are checked before meshing and a
polygon's segment count is checked before vertices are appended, so an
over-budget shape fails typed `TessellationFailed`. A test-only tiny-budget
request flag proves the typed failure without a large fixture.

**Checks (actual).**
- `x64\Release\Tests.ImportIsolation.exe "[cad-wires]"` — 486 assertions, 3
  cases, passed.
- `x64\Debug\Tests.ImportIsolation.exe "[cad-wires]"` — 486 assertions, 3
  cases, passed.
- `x64\Release\Tests.ImportIsolation.exe "[valid-file]"` — 163 assertions,
  passed. `wire-step`/`wire-iges` are `opened/lines`; `wire-brep`/
  `wire-fcstd` are `degraded/lines` for the existing units-assumption record.
- `x64\Debug\Tests.ImportIsolation.exe "[valid-file]"` — 163 assertions,
  passed.
- `x64\Release\Tests.ImportIsolation.exe "[compat]"` — 600 assertions, passed.
- Broader CAD/STEP/topology filter — 1046 assertions, 58 cases, passed.
- `x64\Release\Tests.Unit.exe "[compatibility]"` — 150 assertions, passed.

**Changed docs.** `docs/FORMAT-SUPPORT.md`,
`docs/design/03-file-formats-and-ingestion.md`,
`docs/design/adr/0099-cad-edge-wire-fallback.md`,
`tests/fixtures/valid-file/README.md`, and
`docs/0.8.0/progress/T05.md`.

**Deviation / note.** `ImportSessionRequest request;` left the new bool
uninitialized in this toolchain (arbitrary bytes immediately after
construction), so the viewer and test helpers now use
`ImportSessionRequest request{};`. This is initialization hygiene, not a
parser or path-authority change.

**What T06 must expose.** Map the user-facing `read_wire` option to
`StepImportPolicy::readWire`. The mesher already runs the edge pass alongside
triangles when that flag is true; T06 owns the default, cache identity, UI,
and any face-boundary style policy. Remaining T05 work: none.

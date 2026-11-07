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
- [ ] Add an edge pass to the CAD host’s per-definition mesh path:
  - collect `TopAbs_EDGE` shapes (or a compound) under the definition;
  - run `BRepMesh_IncrementalMesh` with the same versioned deflection profile;
  - read `BRep_Tool::Polygon3D` and emit one `LineList` chunk per material or
    bounded group, applying the definition’s location/world transform;
  - keep edge counts, point counts, and segment counts inside the existing
    per-definition and generation budgets.
- [ ] Make `GeometryRecord`/`AddGeometry` topology-aware if they currently
  assume `TriangleList`. Do not bump the wire version: `LineList` already
  exists and is validated.
- [ ] Fallback policy for this task: emit edges only when the definition
  produced no triangles. Do not draw every face edge over a solid by default;
  T06 will expose an explicit `read_wire` option for users who want edges
  alongside faces.
- [ ] Update the empty/repair logic:
  - a definition with edges but no faces is not “nothing visible”;
  - `TryBoundedRepair` must not reject a shape solely because it has zero
    faces; skip repair for a wire-only shape or repair the edge geometry
    without requiring a face;
  - the final result is success when either triangles or lines were emitted.
- [ ] Preserve authored colors/transparency for edges where the XDE reader
  exposes them; otherwise use the neutral line appearance. Do not invent
  per-edge materials that the source does not author.
- [ ] Add small licensed/generated fixtures:
  - wireframe STEP (edges only);
  - wireframe IGES;
  - BREP line/wire shape (for example an `infinite_line.brep`-style fixture if
    license/provenance permits, or a generated bounded line);
  - FCStd draft/wireframe part with a Part shape that has edges but no faces.
- [ ] Add `[cad-wires]` cases asserting `LineList` chunks, bounds, finite
  positions, no `EmptyGeometry`, and that a solid file still produces
  triangles and no unexpected default edge overlay.
- [ ] Update `docs/FORMAT-SUPPORT.md` and the task README: remove the
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
- [ ] `x64\Release\Tests.ImportIsolation.exe "[cad-wires]"` passes.
- [ ] `x64\Debug\Tests.ImportIsolation.exe "[cad-wires]"` passes.
- [ ] Wireframe STEP, IGES, BREP, and FCStd fixtures open with visible lines
  and finite bounds.
- [ ] Solid STEP/IGES/BREP/FCStd fixtures still open with triangles and no
  default edge overlay.
- [ ] Over-budget edge shapes fail typed, not with a hang or crash.
- [ ] No parser or path authority entered the viewer; the dedicated CAD host
  still owns OCCT.

## Hand-off
_(filled in by the implementing session: edge extraction approach, fallback policy, fixtures, budget measurements, and what T06 must expose)_

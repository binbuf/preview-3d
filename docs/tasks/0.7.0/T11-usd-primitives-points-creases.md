---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[usd-primitives]"
---
# T11 — USD primitives, points and crease tolerance

## Goal
A USD scene made of `Sphere`, `Cube`, `Cylinder`, `Cone`, `Capsule` or `Points`
opens, and a mesh with creases/subdivision no longer fails outright. The
TinyUSDZ fast path hands host-supported features to OpenUSD instead of treating
them as terminal unsupported features.

## Context (read first)
- `docs/proposal.md` §3.5 — the USD gap and F3D’s reference behavior.
- `import-worker/src/UsdAdapter.cpp:330-405` — `ClassifyPrim`,
  `unsupportedRequired`, procedural primitives and creases.
- `import-worker/src/UsdAdapter.cpp:1456-1463` — fast-path failure mapping.
- `compatibility-host/src/OpenUsdHost.cpp:1150-1183` — mesh crease/subdiv
  rejection.
- `compatibility-host/src/OpenUsdHost.cpp:1470-1552` — mesh-only geometry
  emission and `EmptyGeometry`/`UnsupportedRequiredFeature`.
- `shared/import-broker/src/ImportSession.cpp:1474-1488` — fallback only on
  `UnsupportedComposition`, before any accepted batch.
- F3D reference: `plugins/usd/module/vtkF3DUSDImporter.cxx:764-953` for
  sphere/cube/capsule/cylinder/cone/points tessellation and axis handling.
- Fixtures: F3D `primitives.usda`, `primitivesZ.usda`, `usd_points_rgb.usda`,
  `subdiv_cube_cc.usda`; verify licenses before committing.

## Scope
- [x] Add a bounded `Gprim` tessellator to the OpenUSD host:
  - sphere (latitude/longitude segments, e.g. 24×16), cube (12 triangles),
    cylinder/cone/capsule (24 radial segments, capped), honoring `radius`,
    `height`, `size`, `axis`, `extent` where authored;
  - apply the local transform; generate finite normals; emit through the
    existing `NodeRecord`/mesh-instance/chunk writer;
  - enforce node/triangle/vertex/time budgets.
- [x] Add `UsdGeomPoints` as a `PointList` chunk with `displayColor`,
      `displayOpacity`, transforms and bounds.
- [x] Treat creases/subdivision as a control-cage approximation with a
      `Degraded` compatibility warning rather than a fatal error in both the
      fast path and the host. If a bounded subdivision option exists, keep it
      off by default.
- [x] Change the fast-path classification for host-supported features
      (procedural primitives, points, creases/subdivision) from
      `UnsupportedRequiredFeature` to `UnsupportedComposition` or a new
      `NeedsCompatibilityHost` code. The broker must still require no accepted
      fast-path batch before the retry, so producers cannot mix. If a new code
      is added, update `IsKnownImportErrorCode`, the broker mapping, the
      user-facing error text and the hostile-worker cases in the same change.
- [x] Add fixtures and `[usd-primitives]` cases:
      `primitives.usda`, `primitivesZ.usda`, `usd_points_rgb.usda`,
      `subdiv_cube_cc.usda`, a primitive-only scene and a mixed mesh+primitive
      scene. Assert bounds, chunk counts, point counts, colors and warning
      severity.
- [x] Add a protocol test proving a fast-path `NeedsCompatibilityHost` before
      any batch retries, and one proving a feature signaled after a batch is
      rejected as a protocol violation.
- [x] Update `docs/FORMAT-SUPPORT.md` and the USD support notes.

## Out of scope
- USD curves/NURBS tessellation (→ T17 adds line topology; then a follow-up can
  emit polylines/tubes).
- Skeletal animation/playback; the rest pose remains the contract.
- MaterialX/procedural shader evaluation.
- Full Catmull-Clark/Loop subdivision; control cage is the accepted preview.
- Changing USD composition policy or the host’s path authority.

## Design notes
- Tessellation is product-owned and deterministic. Do not link OpenSubdiv just
  for this task.
- Axis handling must match USD semantics: `Cylinder`/`Cone`/`Capsule` default
  axis is Z; `Capsule`/`Cylinder`/`Cone` may author X/Y/Z. Test each.
- `extent` is a hint; compute actual bounds from generated vertices and let the
  host’s existing bounds logic verify them.
- Points need a size policy; use the existing point-splat policy and a bounded
  default. Do not add a new renderer feature here beyond PointList.
- The fallback is only for valid, host-supported features. Malformed, unsafe,
  over-limit or unsupported-schema failures remain terminal.
- Preserve the existing fast-path output for mesh-only USD files; a regression
  test must compare counts/bounds where practical.

## Done when
- [x] `x64\Release\Tests.ImportIsolation.exe "[usd-primitives]"` passes.
- [x] `primitives.usda`/`primitivesZ.usda` open through the host with all five
      primitive types visible.
- [x] `usd_points_rgb.usda` opens as points with colors/opacity.
- [x] `subdiv_cube_cc.usda` opens as a control cage with a warning, not a
      failure.
- [x] A mesh-only USD file still takes the fast path and produces equivalent
      normalized output.
- [x] A malformed/unsafe USD stage still fails and is not retried.
- [x] Host commit/time limits and hostile-worker tests remain green.
- [x] Hand-off lists the tessellation parameters and the fallback code used.

## Hand-off

### Gprim tessellation parameters
- Sphere: 24 longitude x 16 latitude bands, 720 triangles.
- Cube: authored `size`, 6 quads / 12 triangles, face normals.
- Cylinder: 24 radial segments, capped, 96 triangles.
- Cone: 24 radial segments with capped base, 48 triangles.
- Capsule: 24 radial x 8 latitude bands per hemisphere plus the spine, 768
  triangles. `height` is the spine length, so total axis extent is
  `height + 2*radius`.
- `radius`, `height`, `size` are honored; `extent` is treated as a hint and
  bounds are recomputed from generated vertices. Segment counts are fixed and
  deterministic; OpenSubdiv is not linked.

### Axis handling
`Cylinder`, `Cone` and `Capsule` honor authored `axis` and default to Z. X, Y
and Z use proper rotations so winding is preserved. `primitives.usda` authors
Cylinder Y, Cone X and Capsule Z; `primitivesZ.usda` leaves all three at the
default Z. Both are asserted through bounds.

### Point policy
`UsdGeomPoints` emits a `PointList` chunk with `indexCount == 0`, full
PositionNormalUv0TangentColor layout, displayColor and displayOpacity, node
transform, per-chunk origin and actual bounds. Chunks are bounded by
`kChunkPoints` and the section capacity. Authored `widths` are ignored with one
optional warning; the viewer's existing bounded point-splat size policy remains
authoritative.

### Fallback classification
Uses the existing closed `ImportErrorCode::UnsupportedComposition` (22), not a
new code. Visible procedural Gprims, `Points`, authored creases and an authored
non-`none` subdivision scheme set `StagePolicy::needsCompatibilityHost`, which
`ImportUsd` maps to `UnsupportedComposition` before the render-scene converter
or any chunk writer exists. The broker's existing rule accepts that code only
from the fast USD worker before any accepted batch; a late signal is
`ImportProtocolViolation`. Malformed, unsafe, over-limit and curves/NURBS stay
terminal. An unauthored subdivision scheme keeps the historical fast path
(TinyUSDZ reports it as its `catmullClark` default even when not authored).

### Control cage
The host no longer rejects creases. Authored non-`none` subdivision triangulates
the authored cage and increments `optionalWarnings` (`Degraded`). No subdivision
level option is added. Holes remain skipped by the existing cage loop.

### Fixture licenses
All five new fixtures (`primitives.usda`, `primitivesZ.usda`,
`usd_points_rgb.usda`, `subdiv_cube_cc.usda`, `mixed_mesh_primitives.usda`) are
original Preview3D-authored text files under the repository license. No F3D or
other third-party asset was copied, so no new license text is required. Hashes
and byte counts are recorded in `tests/fixtures/usd/manifest.json` and the
fixture README.

### Protocol / acceptance results
- `[usd-primitives]`: 7 cases / 149 assertions, all pass.
- Gprim real retry: primitive-only fixtures open through the compatibility host
  with 5 triangle geometries, 5 instances, 1644 triangles and exact bounds.
- Points: one colored PointList, exact per-point RGBA, translated bounds.
- Subdivision fixture: 12 cage triangles plus an ImportStatus warning.
- Mesh-only `mesh.usda` still uses `FastWorker`.
- Malformed primitive stage: `MalformedData`, fallback not requested, zero
  batches, zero chunks.
- Protocol: pre-batch `UnsupportedComposition` requests fallback with zero
  batches; the same signal after a batch is a protocol violation with one
  published batch.
- Full USD suite: 45 cases / 1093 assertions (1 Release fault-harness skip).
- `[hostile-worker],[openusd]`: 29 cases / 843 assertions, all pass.
- `[compat]`: 2 cases / 312 assertions, all pass.
- `python tests/fixtures/usd/verify.py`: 15 sources and 13 derived cases.

### Docs changed
- `docs/FORMAT-SUPPORT.md`
- `docs/design/03-file-formats-and-ingestion.md`
- `docs/design/adr/0067-usd-gprim-points-control-cage.md`
- `tests/fixtures/usd-spike/README.md`, `tests/fixtures/usd/README.md`,
  `tests/fixtures/usd/manifest.json`
- `docs/0.7.0/progress/T11.md` (reusable facts and follow-ups)

### Deviations / remaining work
- Curves/NURBS remain terminal by design until T17 line topology.
- Only displayColor/displayOpacity are consumed for points; `widths`, point
  `normals` and `ids` are ignored.
- Point-instancer prototypes containing Gprims are supported by the host's
  generic expansion but have no dedicated fixture yet.
- Full Catmull-Clark/Loop evaluation remains out of scope; the control cage is
  the accepted preview.

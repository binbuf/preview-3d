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
- [ ] Add a bounded `Gprim` tessellator to the OpenUSD host:
  - sphere (latitude/longitude segments, e.g. 24×16), cube (12 triangles),
    cylinder/cone/capsule (24 radial segments, capped), honoring `radius`,
    `height`, `size`, `axis`, `extent` where authored;
  - apply the local transform; generate finite normals; emit through the
    existing `NodeRecord`/mesh-instance/chunk writer;
  - enforce node/triangle/vertex/time budgets.
- [ ] Add `UsdGeomPoints` as a `PointList` chunk with `displayColor`,
      `displayOpacity`, transforms and bounds.
- [ ] Treat creases/subdivision as a control-cage approximation with a
      `Degraded` compatibility warning rather than a fatal error in both the
      fast path and the host. If a bounded subdivision option exists, keep it
      off by default.
- [ ] Change the fast-path classification for host-supported features
      (procedural primitives, points, creases/subdivision) from
      `UnsupportedRequiredFeature` to `UnsupportedComposition` or a new
      `NeedsCompatibilityHost` code. The broker must still require no accepted
      fast-path batch before the retry, so producers cannot mix. If a new code
      is added, update `IsKnownImportErrorCode`, the broker mapping, the
      user-facing error text and the hostile-worker cases in the same change.
- [ ] Add fixtures and `[usd-primitives]` cases:
      `primitives.usda`, `primitivesZ.usda`, `usd_points_rgb.usda`,
      `subdiv_cube_cc.usda`, a primitive-only scene and a mixed mesh+primitive
      scene. Assert bounds, chunk counts, point counts, colors and warning
      severity.
- [ ] Add a protocol test proving a fast-path `NeedsCompatibilityHost` before
      any batch retries, and one proving a feature signaled after a batch is
      rejected as a protocol violation.
- [ ] Update `docs/FORMAT-SUPPORT.md` and the USD support notes.

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
- [ ] `x64\Release\Tests.ImportIsolation.exe "[usd-primitives]"` passes.
- [ ] `primitives.usda`/`primitivesZ.usda` open through the host with all five
      primitive types visible.
- [ ] `usd_points_rgb.usda` opens as points with colors/opacity.
- [ ] `subdiv_cube_cc.usda` opens as a control cage with a warning, not a
      failure.
- [ ] A mesh-only USD file still takes the fast path and produces equivalent
      normalized output.
- [ ] A malformed/unsafe USD stage still fails and is not retried.
- [ ] Host commit/time limits and hostile-worker tests remain green.
- [ ] Hand-off lists the tessellation parameters and the fallback code used.

## Hand-off
_(filled in by the implementing session: Gprim segment counts, axis handling,
point policy, new fallback classification, fixture licenses and protocol test
results.)_

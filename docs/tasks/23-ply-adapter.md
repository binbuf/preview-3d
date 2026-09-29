---
verify: x64\Release\Tests.Unit.exe
---
# T23 — Implement the PLY thumbnail adapter

## Goal
Render ASCII and binary (both endian) `.ply` files in Explorer, for triangle meshes and point clouds,
including vertex colors, under provider limits.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — point-cloud splat rendering and sampling.
- `docs/design/03-file-formats-and-ingestion.md` — PLY subset (meshes/points, both endian).
- `import-worker/src/PlyAdapter.*` — the viewer parser to reuse as source (see ADR-0004).
- `docs/tasks/14-geometry-sampler.md`, `docs/tasks/15-cpu-tile-rasterizer.md`, `docs/tasks/17-provider-test-harness.md`.

## Scope
- [x] Route `.ply` to this adapter via its fixed CLSID; parse ASCII and little/big-endian binary.
- [x] Support triangle meshes and point clouds; carry vertex colors when present; emit product-owned geometry.
- [x] Bound unknown-property/list skipping, element counts and per-face vertex counts; never require all source positions in private memory.
- [x] Feed point clouds to the depth-tested round-splat path and meshes to the triangle path.
- [x] Add fixtures and goldens: ASCII/binary mesh, little/big-endian point cloud, colored points, unknown properties/lists, hostile element count, truncation — the last three asserting typed failures.

## Out of scope
- STL parsing (→ T21).
- Viewer Tier-A PLY proxy/LOD qualification (viewer program).

## Design notes
- Point-only PLY must produce a usable thumbnail, not a fallback.
- Use deterministic reservoir sampling for points and verify bounds incrementally.
- Variable-length face lists need a bounded offset/scan policy; do not trust declared counts without checked reads.

## Done when
- [x] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes in Debug and Release across encodings, meshes and clouds.
- [x] Hostile list/count inputs fail safely before allocation.
- [x] The family is smoke-checked in Explorer using the T22 procedure.
- [x] Hand-off below filled in.

## Hand-off
**What landed**
- `thumbnail-provider/PlyFamilyAdapter.{h,cpp}` (`preview3d::provider::PlyAdapter`), PCH/COM/GDI-free,
  registered for `Family::Ply` in `FamilyAdapterRegistry.cpp`. Compiled into the DLL,
  `Tests.Unit.exe` and `Tests.ProviderHost.exe`.
- ASCII and binary little/big-endian; vertex `x/y/z` required; optional `nx/ny/nz` and
  `red/green/blue[/alpha]` (or `r/g/b[/a]`); `face` + integer `vertex_indices`/`vertex_index` list
  = fan-triangulated mesh, otherwise a point cloud. Colors use material 1 with a white base so
  `vertexColor * baseColor` preserves the source; no colors keeps the neutral palette.
- **Binary mesh never materializes positions:** `ComputeBinaryOffsets` proves vertex/face body
  offsets and the vertex extent in `Parse`, then each referenced record is read at
  `vertexStart + index * stride` through a 256-entry cache (record + cache charged to the ledger).
  An ASCII mesh retains a bounded `VertexSample` table, rejected over the 192 MiB scratch cap and
  charged to the ledger before allocation. A binary vertex element with a list property (no fixed
  stride), or a list-bearing element before the face body, fails closed with
  `UnsupportedRequiredFeature`.
- Bounds: per-face list ≤255, unknown list length ≤65 536, skipped element records ≤6 M, vertex
  count ≤6 M; hostile counts rejected before allocation; deadline polled per bounded unit.
- Tests `tests/unit/ProviderPlyAdapterTests.cpp` (`[provider][ply]`, 20 cases): ASCII/binary-LE/BE
  meshes, points, colors, normals, unknown scalar/list props + unknown element, out-of-range and
  degenerate faces, hostile vertex count, hostile face list, truncated binary/ASCII, missing
  position, cap stop, expired deadline, ledger exhaustion, pipeline error mapping, a real rendered
  bitmap, CLSID routing.
- Host harness: `ply-cube` and `ply-points` fixtures in `FixtureRegistry.cpp` with committed goldens
  `tests/provider-host/goldens/ply-cube-256.pam` / `ply-points-256.pam`; `Tests.ProviderHost.exe`
  compiles `PlyFamilyAdapter.cpp` and now also `shared/parser-core/src/PlyParserCore.cpp`.
- Smoke (T22 procedure): `Register-ProviderSmoke.ps1`/`Unregister-ProviderSmoke.ps1` now write both
  STL and PLY (`{F4DC6119-E235-4BAC-8089-54EDD84F8492}` CLSID + `{...8493}` AppID + `.ply\shellex`), `ProviderSmokeHost`
  accepts `--ply`, `Invoke-ProviderSmoke.ps1` runs both and only exits 0 when both match, and
  `fixtures/smoke-cube.ply` is the committed colored ASCII cube.

**Deviations**
- ASCII mesh resolution is a retained table rather than the sampler-only path; the design note's
  "never all source positions in memory" is met for binary (stride random access) and bounded for
  ASCII by the scratch cap (ADR-0021). Point sampling is delegated to the T14 sampler, not a
  second reservoir in the adapter.
- Binary mesh with a list-typed vertex property is unsupported (generic-icon fallback) rather than
  scanned, because random index resolution has no fixed stride.

**Checks (Release x64)**
- `x64\Release\Tests.Unit.exe` = 234 cases / 134495 assertions green; `[provider][ply]` 20 cases / 95
  assertions green.
- `x64\Release\Tests.ProviderHost.exe` = 5 cases / 64 assertions green (goldens regenerated).
- `Invoke-ProviderSmoke.ps1` exit 0: STL reference 256² opaque 0.7407 maxChannel 212; PLY reference
  256² opaque 0.7407 maxChannel 232; both Shell thumbnails `WTSAT_ARGB` in `dllhost.exe`,
  reference-vs-Shell `meanAbs=0.0000 maxAbs=0`; registration cleaned up.
- Provider + host projects build clean under `/W4 /WX`.

**Changed docs**
- New `docs/design/adr/0021-ply-adapter-stride-and-bounded-ascii.md`.
- Updated `docs/design/05-thumbnail-provider.md` (PLY adapter paragraph in "Geometry sampling").
- `docs/PROGRESS.md` T23 section; the T22 smoke follow-up now names T24–T34.

**Remaining / next task**
- T24 (OBJ) can mostly disregard PLY. If a later task adds streaming ASCII face resolution, the
  bounded table in `EmitAsciiMesh` is the only change point.
- Visual-quality pass across families (bright non-degenerate models) remains tracked as a T22
  follow-up; the PLY smoke reference already renders the colored cube brightly.
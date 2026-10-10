---
verify: x64\Release\Tests.Unit.exe
---
# T24 — Implement the OBJ thumbnail adapter

## Goal
Render `.obj` geometry in Explorer with generated normals and object/group separation, while never
resolving its `.mtl` or texture sidecars — the documented stream-only behavior.

## Context (read first)
- `docs/design/03-file-formats-and-ingestion.md` — OBJ/MTL section: the thumbnail path does not resolve sidecars.
- `docs/design/05-thumbnail-provider.md` — stream ingestion and the OBJ rule.
- `docs/design/adapters/fbx-008-thumbnail.md` — ufbx provider-local configuration (shared with FBX).
- `import-worker/src/` OBJ/ufbx usage — the viewer policy reference.
- `docs/tasks/17-provider-test-harness.md`.

## Scope
- [x] Route `.obj` to this adapter via its fixed CLSID; parse through the provider-local pinned ufbx copy with path/external-file access disabled.
- [x] Triangulate polygons, generate/smooth normals, carry UVs/vertex colors, and separate object/group meshes.
- [x] Ignore `mtllib` and every texture reference: render the neutral material; an MTL/texture dependency must never fail an otherwise valid mesh or trigger an open.
- [x] Add fixtures and goldens: plain mesh, polygon/quad mesh, object-group mesh, colored mesh, and an OBJ that references an MTL/texture (must still render neutrally with no filesystem access).

## Out of scope
- MTL/material factor and texture mapping (viewer-only).
- FBX parsing (→ T31), even though both use ufbx.

## Design notes
- The provider-local ufbx copy must have path, geometry-cache, plug-in, script and environment-codec access disabled.
- No filesystem location is available to the provider; do not attempt sidecar recovery.

## Done when
- [x] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes in Debug and Release, including the MTL-referencing case with no path access.
- [x] The isolation test confirms no file open occurs for OBJ.
- [ ] The family is smoke-checked in Explorer using the T22 procedure.
- [x] Hand-off below filled in.

## Hand-off

### What landed
- `thumbnail-provider/ObjFamilyAdapter.{h,cpp}` (`preview3d::provider::ObjAdapter`), selected only
  by the routed `Family::Obj` CLSID and registered in `FamilyAdapterRegistry.cpp`. PCH/COM/GDI-free,
  so the DLL, `Tests.Unit.exe` and `Tests.ProviderHost.exe` compile the exact shipped source.
- Parsing goes through the provider-local pinned ufbx static library (vcpkg
  `x64-windows-static-md`). The adapter forces `UFBX_FILE_FORMAT_OBJ`, disables format detection
  from content and extension, and sets `load_external_files = false` plus
  `ignore_missing_external_files = true`. `open_file_cb` is a deny hook, so even if a later
  configuration attempted a sidecar it could never open a path. `temp_allocator`/`result_allocator`
  `memory_limit` are each capped at half of `kAccountedScratchMaxBytes` (96 MiB).
- OBJ defaults keep `obj_merge_objects`/`obj_merge_groups` false, so ufbx splits by `o`/`g` into
  separate meshes; polygons are triangulated with `ufbx_triangulate_face` (per-face ceiling 65536
  triangles), normals are generated/normalized by ufbx and re-normalized here, and vertex colors
  (`v x y z r g b`) are carried with a white base so `vertexColor * baseColor` preserves the source.
  `mtllib`/`usemtl` and all textures are ignored: the neutral material is index 1.
- OBJ text is parsed in one contiguous pass: the frozen `ContiguousView()` when available, otherwise
  a checked, ledger-charged backing buffer bounded by `kContiguousBackingMaxBytes` (128 MiB). The
  `progress_cb` polls `AdapterInput::deadline->Checkpoint()`; `EnumerateGeometry` polls every 1024
  faces and treats a false sink return as the sampler inspect-cap stop.
- Tests `tests/unit/ProviderObjAdapterTests.cpp` (`[provider][obj]`, 15 cases): triangle mesh,
  quad/ngon triangulation, multiple objects, UVs, vertex colors + white base, the MTL/texture
  isolation case (`ExternalFileDenied() == false`), non-contiguous backing, sink cap stop, expired
  deadline, two typed-failure cases, an over-ceiling polygon resource limit, pipeline error mapping,
  a real rendered bitmap, CLSID routing.
- Host harness: `obj-cube` fixture in `FixtureRegistry.cpp` with committed golden
  `tests/provider-host/goldens/obj-cube-256.pam`; the fixture text carries `mtllib`/`usemtl` so the
  golden proves the sidecar is ignored.

### Decisions / deviations
- **The provider now enables the vcpkg manifest.** ufbx is consumed the same way the worker and the
  test executables already consume it: `VcpkgEnableManifest=true` selects the static
  `x64-windows-static-md` triplet and autolinks ufbx. This appends the whole triplet's `.lib` list to
  the provider link line, but unused libraries contribute no objects and the DLL stays import-clean
  (dependency-closure check still passes). A narrower explicit `ufbx.lib` link was rejected because
  it would hardcode a triplet/configuration path the way only `compatibility-host-step` does.
- **UVs are decoded but not carried.** The frozen `VertexSample` has no UV channel and the
  thumbnail path renders the neutral material, so OBJ `vt` data has nowhere to go; the adapter still
  parses UV-carrying faces correctly.
- **File name `ObjFamilyAdapter.*`.** Same include-path collision reason as `PlyFamilyAdapter.*`
  (`import-worker/src/ObjAdapter.h` is on the Tests.Unit include path).
- [ADR-0022](../design/adr/0022-obj-adapter-ufbx-isolation.md) records the ufbx linkage and
  external-access policy; the isolation rule is also restated in
  `design/05-thumbnail-provider.md`.

### Checks (Release x64 unless noted)
- `x64\Release\Tests.Unit.exe` = 249 cases / 134553 assertions green; `[provider][obj]` = 15 cases /
  57 assertions green.
- `x64\Debug\Tests.Unit.exe` = 249 cases / 134636 assertions green.
- `x64\Release\Tests.ProviderHost.exe` = 5 cases / 68 assertions green (goldens regenerated).
- `check-provider-dependency-closure.ps1 -Configuration Release` = OK (no viewer/worker/host/core
  imports).
- Provider + host projects build clean under `/W4 /WX`.

### Remaining / next task
- T25 (glTF) can disregard OBJ. T31 (FBX) reuses this task's provider-local ufbx linkage and the
  same `load_external_files = false` + deny-hook policy.
- Explorer smoke using the T22 procedure was not run in this non-interactive session; the family is
  covered by the provider-host COM harness instead.
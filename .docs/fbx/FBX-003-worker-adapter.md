# FBX-003: sandboxed FBX worker adapter

Status: blocked on FBX-002  
Depends on: FBX-001, FBX-002  
Unblocks: FBX-004

## Objective

Add the FBX control/broker path and a bounded `ufbx` adapter for static,
undeformed polygon scenes. Keep it test-only and undiscoverable from the viewer
until deformation and materials are complete.

## Context to load

- `import-worker/src/ObjAdapter.{h,cpp}` and `ObjImportWorker.{h,cpp}`
- `import-worker/src/WorkerRequestDispatch.cpp` and `main.cpp`
- `import-worker/Preview3DImportWorker.vcxproj`
- `shared/model-core/include/model_core/{ControlProtocol,WireFormat,TierALimits,ImportError}.h`
- `shared/import-broker/include/import_broker/ImportSession.h`
- `shared/import-broker/src/ImportSession.cpp`
- `tests/import-isolation/ObjImportTests.cpp`
- FBX-001 results

## Work

1. Add `SourceFormatId::Fbx`, `ImportFormat::Fbx`, a dedicated FBX start opcode
   and request struct, worker dispatch, one-shot `--parse-fbx`, pooled dispatch,
   and strict source-format validation. Follow the repository's deliberate
   per-format request-struct convention.
2. Add `FbxAdapter` and `FbxImportWorker`. The worker maps only the brokered
   primary handle, enforces the 2 GiB Tier-B primary limit, uses the real
   cancellation event, catches allocation/library failures at the boundary,
   and emits typed phase/error results.
3. Configure `ufbx` exactly as approved by FBX-001: forced FBX format, strict
   index/Unicode handling, node depth, explicit temp/result limits, progress
   callback, generated/normalized normals, safe axis/unit/geometry-transform
   policy, no plug-ins, and no geometry-cache/external-file evaluation.
4. Normalize finite undeformed polygon geometry in bounded 4–16 MiB chunks,
   triangulating faces without an unbounded scene-wide remap. Preserve UVs,
   vertex colors, mesh parts, local double origins, verified bounds, and stable
   source provenance. Enforce Tier-B triangle/vertex/node/material limits before
   allocations and checked arithmetic.
5. Emit the FBX-002 node/instance contract. Reuse geometry for compatible
   instances and split it only for chunk/material/layout limits. Do not
   accidentally count one shared source mesh's vertices once per instance for
   parser limits; separately cap instance/draw records.
6. For this intermediate task, reject any scene requiring skin/blend evaluation
   with `UnsupportedRequiredFeature`; emit neutral materials. This prevents the
   incomplete adapter from looking successful in tests.
7. Add focused binary and ASCII tests for hierarchy, instances, transforms,
   triangulation, units/axes, generated normals, UV/color data, malformed input,
   depth/count limits, progressive small-section batches, cancellation, pooled
   recovery, and source-format spoofing.

## Constraints

- Do not add `.fbx` to `Preview3D.cpp`, `ActiveInstance`, `ShellIntegration`,
  picker/drop text, installer, or public documentation in this task.
- Do not copy source bytes into `Preview3D.exe` or let the worker open a path.
- Do not clone the whole OBJ adapter. Extract only narrowly reusable `ufbx`
  helpers with regression coverage; FBX options and policy remain explicit.

## Verification

- The new adapter is reachable through direct ImportIsolation requests and
  pooled worker dispatch only.
- Binary/ASCII static fixtures produce identical normalized results where their
  authored scenes match.
- Shared meshes create one geometry payload plus bounded instance records.
- Malformed/over-limit/deformed input fails with the expected typed error and a
  subsequent valid import succeeds in the same application session.
- Debug and Release worker, Unit, and full ImportIsolation suites pass.


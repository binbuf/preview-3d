---
verify: x64\Release\Tests.ProviderHost.exe
---

# T06 — Provider adapter exception containment

## Goal
A `std::bad_alloc` (or any C++ throw) inside the STL/PLY/OBJ/glTF thumbnail adapters returns a
typed provider failure instead of calling `std::terminate`, so one hostile file cannot kill the
whole Explorer surrogate family and every in-flight thumbnail with it.

## Context (read first)
- `thumbnail-provider/FamilyAdapter.h:76-90` — the frozen adapter interface is `noexcept`.
- `thumbnail-provider/ThumbnailPipeline.cpp:139-249` — `RunThumbnailPipeline` is `noexcept` and
  calls the adapter methods directly.
- `thumbnail-provider/ComCore.cpp:80-84` — `InvokePipeline` is `noexcept`.
- `thumbnail-provider/Containment.cpp:36-51` — `CppBoundary` catches `bad_alloc`/`...`, but a throw
  inside a `noexcept` function terminates before unwinding reaches it.
- Unguarded examples: `GltfFamilyAdapter.cpp:739,762,772,791,823,1114`; `StlFamilyAdapter.cpp:139`
  (`std::vector` at `:199`); `PlyFamilyAdapter.cpp:586,648`; `ObjFamilyAdapter.cpp:140`.
  STEP/3MF/USD adapters already contain exceptions (`StepFamilyAdapter.cpp:927,981-985`,
  `ThreeMfFamilyAdapter.cpp:1110-1114`, `UsdFamilyAdapter.cpp:925-927`) — copy that pattern.
- Docs: `thumbnail-provider/Containment.h` claims a hostile call cannot take down the surrogate.
- Tests: `tests/provider-host/Tests.ProviderHost.vcxproj`; the provider soak lane (→ SEC-17).

## Scope
- [x] Decide and record the pattern: a per-adapter non-`noexcept` shim that routes the body through
      `RunContained` (STEP's approach), or removing `noexcept` from the interface and relying on the
      single top-level `CppBoundary`. Do not leave a mixed model.
- [x] Apply it to `StlFamilyAdapter`, `PlyFamilyAdapter`, `ObjFamilyAdapter`, `GltfFamilyAdapter`
      (including material/vector growth and `make_unique` sites), and audit the other families for
      raw allocation outside their existing catch blocks.
- [x] Add a provider-host test that forces allocation failure (fault-injecting allocator or
      deliberately large ledger-reserved request) and asserts a typed `OutOfMemory`, not a process
      exit.
- [x] Keep `Initialize`/`Parse`/`Enumerate*`/`Reset` semantics unchanged; `Reset` must still release
      everything after a failed parse.

## Out of scope
- Adapter-owned parser bugs (→ SEC-01/02/05 for worker counterparts).
- The AV/stack-overflow policy (→ SEC-08).

## Design notes
- Allocation failure is a normal hostile-input outcome; the provider must return
  `ProviderOutcome::OutOfMemory` and leave no partially trusted intermediate.
- If the interface changes, update the frozen-contract docs (`docs/design/05-thumbnail-provider.md`,
  ADR-0009/0012 as applicable) rather than only the header.
- Do not add a catch-all that swallows real logic errors silently; diagnostics should still record
  the stage (existing `Diagnostics` path).

## Done when
- [x] `Tests.ProviderHost` passes in Debug and Release, including the fault-injection case.
- [x] Every family adapter has a documented, uniform containment path.
- [x] Hand-off filled in with the chosen pattern and why.

## Hand-off

**Chosen pattern.** Keep the frozen `IFamilyAdapter` methods `noexcept` and route each allocating
lifecycle stage through a per-adapter shim, `preview3d::provider::RunContainedStage`. Every adapter's
`Initialize`/`Parse`/`EnumerateMaterials`/`EnumerateGeometry` is now a thin `noexcept` wrapper over a
non-`noexcept` `...Impl()` body. `RunContainedStage` (declared in the new Windows-free
`thumbnail-provider/ContainmentStage.h`, implemented in `Containment.cpp` under the existing
`RunContained` boundary) returns the body's `ErrorCode` unchanged, `ErrorCode::OutOfMemory` for
`std::bad_alloc`, and `InternalImporterFailure` for any other C++/structured fault. This matches the
STEP adapter's established model, needs no interface change, and keeps every in-process
`RunThumbnailPipeline` caller's contract. The alternative (remove `noexcept` from the interface and
rely on the single top-level `CppBoundary`) was rejected because it would force
`RunThumbnailPipeline`/`InvokePipeline` non-`noexcept`, change the in-process contract, and leave the
existing internal STEP/3MF/USD containment as a mixed model. Recorded in ADR-0034.

**What landed.**
- `thumbnail-provider/DiagnosticStage.h` (stage enum split out of `Diagnostics.h`) and
  `thumbnail-provider/ContainmentStage.h` (`ContainmentStage.h` stays Windows-free so adapters that
  include a vendored parser header do not pull in `windows.h`); `RunContainedStage` in
  `Containment.cpp`.
- STL, PLY, OBJ, glTF, FBX: all four public stages wrapped; internal allocating helpers had
  `noexcept` removed (`EmitBinary`/`EmitAscii`, `LoadHeader`/`EmitBinary*`/`EmitAsciiMesh`,
  `LoadSourceBytes`/`BuildMaterials`, `DecodeDraco`/`DecodeImages`).
- 3MF and USD: same wrapper applied to their public stages; the audited holes
  (`LoadSourceBytes`, and for USD `BuildScene`/`ValidateScene`/`LoadStage`/`PreflightArchive`/
  `SniffContainer`) had `noexcept` removed so their `bad_alloc` reaches the boundary (their own
  library-specific catches are preserved).
- STEP: `Parse` and `EnumerateGeometry` now catch `std::bad_alloc` as `OutOfMemory` before their
  `catch (const std::exception&)` (which previously swallowed it as `InternalImporterFailure`).
- `tests/provider-host/FaultInjectingAllocator.{h,cpp}` (global `operator new`/`delete` replacement,
  armable) and the `[host][containment]` case in `ProviderHostTests.cpp`.
- Docs: ADR-0034, `docs/design/05-thumbnail-provider.md`, `docs/design/interfaces.md`, PROGRESS.

**Deviations / extra findings (documented).**
- The audit found FBX had no exception containment at all, and 3MF/USD had `noexcept` allocation
  holes; all were fixed so "every family adapter has a uniform containment path" holds.
- The fault-injection test uses a global allocator replacement, which cannot reach the C allocators
  used inside the 3MF/USD/STEP libraries; the test exercises the STL/PLY/OBJ/glTF/FBX product-owned
  paths. Library-allocator paths are left to the SEC-17 soak lane (Follow-ups).
- Debug-only: MSVC's iterator-debug `_Container_proxy` is allocated in a `noexcept` container
  constructor, so the injector does not fail allocations `<= sizeof(std::_Container_proxy)`. This is
  a harness detail, not a product change.

**Check results (all run in the foreground).**
- `MSBuild tests\provider-host\Tests.ProviderHost.vcxproj /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\" /m` — succeeded.
- `x64\Debug\Tests.ProviderHost.exe "[host][containment]"` — passed (25 assertions / 1 case).
- `x64\Debug\Tests.ProviderHost.exe` — passed (128 assertions / 6 cases).
- Same two Release builds/runs — passed identically.
- `MSBuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Release ...` succeeded;
  `x64\Release\Tests.Unit.exe "[provider]"` — passed (60901 assertions / 214 cases).

**Remaining / next task.** No blockers. Add the SEC-17 provider soak/fuzz lane for the
library-allocator (3MF/USD/STEP) paths, and remove/implement the pre-existing dead
`ThreeMfAdapter::ScanRequiredExtensions()` declaration (both listed under PROGRESS "Follow-ups").
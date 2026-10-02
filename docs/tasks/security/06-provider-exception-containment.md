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
- [ ] Decide and record the pattern: a per-adapter non-`noexcept` shim that routes the body through
      `RunContained` (STEP's approach), or removing `noexcept` from the interface and relying on the
      single top-level `CppBoundary`. Do not leave a mixed model.
- [ ] Apply it to `StlFamilyAdapter`, `PlyFamilyAdapter`, `ObjFamilyAdapter`, `GltfFamilyAdapter`
      (including material/vector growth and `make_unique` sites), and audit the other families for
      raw allocation outside their existing catch blocks.
- [ ] Add a provider-host test that forces allocation failure (fault-injecting allocator or
      deliberately large ledger-reserved request) and asserts a typed `OutOfMemory`, not a process
      exit.
- [ ] Keep `Initialize`/`Parse`/`Enumerate*`/`Reset` semantics unchanged; `Reset` must still release
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
- [ ] `Tests.ProviderHost` passes in Debug and Release, including the fault-injection case.
- [ ] Every family adapter has a documented, uniform containment path.
- [ ] Hand-off filled in with the chosen pattern and why.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
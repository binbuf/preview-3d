# 0034 — Per-adapter stage containment for product-owned allocation

## Status
accepted

## Context
The frozen `IFamilyAdapter` lifecycle (`thumbnail-provider/FamilyAdapter.h`) is `noexcept`, and
`RunThumbnailPipeline` / `InvokePipeline` are `noexcept` too. `RunContained` (Containment.{h,cpp})
is the last-resort COM-boundary translator, but a C++ throw that starts inside a `noexcept`
function calls `std::terminate` before unwinding reaches that boundary. The STEP/3MF/USD adapters
already contained their own allocations with `try`/`catch` or an inner `RunContained`; the
STL/PLY/OBJ/glTF/FBX adapters did not. A `std::bad_alloc` from a product-owned `std::vector`,
`std::string`, `std::unordered_map` or `std::make_unique` in those adapters could therefore kill
the whole Explorer surrogate and every in-flight thumbnail with it (SEC-06).

## Decision
Keep the frozen interface `noexcept` and give every family adapter one uniform containment path:
each of `Initialize`/`Parse`/`EnumerateMaterials`/`EnumerateGeometry` becomes a thin `noexcept`
wrapper that runs its non-`noexcept` `...Impl` body through
`preview3d::provider::RunContainedStage` (`thumbnail-provider/ContainmentStage.h`, implemented in
`Containment.cpp`, compiled `/EHsc`). `RunContainedStage` runs the stage under the existing
`RunContained` exception/structured-exception boundary and returns the typed `ErrorCode`:
the body's own code unchanged, `ErrorCode::OutOfMemory` for `std::bad_alloc`,
`ErrorCode::InternalImporterFailure` for any other C++/structured fault. Internal helpers that
allocate are no longer `noexcept` so their throw reaches the boundary. `Reset` stays `noexcept`
and unchanged (it only releases).

`ContainmentStage.h` is deliberately Windows-free: it includes only `ProviderTypes.h` and the new
`DiagnosticStage.h`, so including it in an adapter does not pull `windows.h` and its macros into a
translation unit that also includes a vendored parser header. `DiagnosticStage` moved out of
`Diagnostics.h` for the same reason.

Rejected: removing `noexcept` from the interface and relying on the single top-level `CppBoundary`.
That would also require making `RunThumbnailPipeline`/`InvokePipeline` non-`noexcept` and would
change the contract for every in-process caller; and it would leave the existing internal
STEP/3MF/USD containment in a mixed model. Also rejected: a raw `try`/`catch` in each adapter, which
would lose the structured-exception and path-redacted diagnostic event that `RunContained` emits.

## Consequences
- Every family adapter (STL, PLY, OBJ, glTF, FBX, 3MF, USD, STEP) now has the same documented
  containment path; a hostile file that exhausts memory returns the tabulated
  `E_OUTOFMEMORY` (mapped through `ClassifyError`) instead of terminating the surrogate.
- Semantics are unchanged: `RunContainedStage` passes the stage's returned code through untouched
  and passes `nullptr` for the deadline, so the existing cooperative `Checkpoint()` checks and the
  pipeline's between-stage deadline check keep their exact behavior.
- The diagnostics path records the stage (`AdapterInitialize`/`Parse`/`Materials`/`Geometry`) for a
  contained fault; the underlying fault is still expected to be fuzzed and fixed (SEC-17), not
  treated as handled.
- The build/export surface is unchanged: `RunContainedStage` lives in the already-linked
  `Containment.cpp` and adds no import.
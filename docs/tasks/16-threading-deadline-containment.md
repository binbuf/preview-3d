---
verify: x64\Release\Tests.Unit.exe
---
# T16 — Implement threading, deadline, and containment behavior

## Goal
Make the provider deterministic and crash-contained under Shell scheduling: deadline checks throughout
parse/sample/raster, an exception/SEH boundary at the third-party calls, and path-redacted diagnostics.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — "Threading and unload", "Security and robustness", "HRESULT mapping".
- `docs/design/adr/0005-safety-bounded-parsing-not-surrogate.md` — containment philosophy.
- `docs/design/09-quality-performance-and-security.md` — threat controls and diagnostic rules.
- `docs/tasks/06-budgets-deadlines-hresults.md` — the deadline type and mapping.

## Scope
- [x] Perform `GetThumbnail` work on the calling thread; create no lasting pool and no process-global mutable caches.
- [x] Add deadline checkpoints at bounded parser/sampler/raster intervals and make long product-owned adapter loops cancellable. Record actual elapsed time when an uninterruptible library call returns after the stop point.
- [x] Wrap third-party parser calls in exception and structured-exception containment at the COM boundary where legally safe, returning the tabulated HRESULT without swallowing ordinary memory faults.
- [x] Keep diagnostic events path-redacted and disabled unless troubleshooting is enabled.
- [x] Add tests for deadline expiry, injected parser exceptions/SEH, and unload while a call is active.

## Out of scope
- Fixing specific parser defects (→ T43 fuzzing/hardening).
- AppContainer or job-object isolation (not applicable inside the Shell surrogate).

## Design notes
- The 2 s stop point is cooperative inside a non-interruptible third-party call. Preflight bounds input, not execution time or unaccounted allocation. A call already in progress is allowed to return before the request is rejected; do not claim an in-call interrupt or hard wall-clock cutoff.
- SEH containment is a last-resort HRESULT boundary, not a correctness mechanism.
- `DllCanUnloadNow` must account for active calls; thread-local scratch must not keep the module alive.
- Never start, message or wait on any product process.

## Done when
- [x] Deadline, containment and unload-during-call tests pass in Debug and Release.
- [x] Diagnostics contain no file path by default.
- [x] Hand-off below filled in.

## Hand-off

**What landed**
- `thumbnail-provider/ModuleLifetime.{h,cpp}` (new, PCH/COM/Windows-free): the T11
  object/lock/active-call counters and `ActiveCallGuard` moved out of `ComCore.cpp`, with added
  `LiveObjects()/Locks()/ActiveCalls()` introspection. `ComCore.h` now includes it. Both
  `ProviderObject::Initialize` and `GetThumbnail` hold an `ActiveCallGuard` for their whole body, so
  `DllCanUnloadNow` stays `S_FALSE` while a call is in flight even if the caller releases every
  external reference. The only thread-local is a trivial `std::uint32_t` (no TLS destructor; cannot
  keep the module alive).
- `thumbnail-provider/Diagnostics.{h,cpp}` (new, PCH/COM-free): `DiagnosticEvent` carries only
  numeric/enum fields (stage, outcome, elapsedMs, overrun/cpp/SEH flags, SEH code) — no string field
  exists, so a path cannot leak. `Diagnostics::Enabled()` is off by default; `SetEnabled` and the
  live `PREVIEW3D_THUMBNAIL_DIAGNOSTICS=1` probe enable it; `SetSink` is the test/troubleshooting
  capture. `FormatDiagnostic` emits fixed `key=value` text with no `\`, `/` or `:`.
- `thumbnail-provider/Containment.{h,cpp}` (new, PCH/COM-free, compiled `/EHsc` in both projects):
  `RunContained(call, context, deadline, stage)`. C++ boundary maps `std::bad_alloc` ->
  `E_OUTOFMEMORY` and any other C++ exception -> `E_FAIL`; a separate `__try/__except` boundary maps a
  contained structured exception -> `E_FAIL` and records its code. `EXCEPTION_STACK_OVERFLOW`,
  `EXCEPTION_BREAKPOINT` and `EXCEPTION_SINGLE_STEP` are deliberately not swallowed; an access
  violation is. `RunContained` records real elapsed time and, when the deadline's 2 s stop point was
  passed, rejects a completed call afterwards as `ERROR_TIMEOUT` — no in-call interrupt is claimed.
- `ComCore.cpp`: `GetThumbnail`/`Initialize` take `ActiveCallGuard`; the pipeline and DIB conversion
  run through `RunContained` via non-capturing function pointers (`InvokePipeline`/`InvokeBitmap`); a
  partially-created bitmap is `DeleteObject`-ed if the DIB boundary fails.
- `ThumbnailPipeline.cpp`: a `Deadline::Checkpoint()` between every bounded stage (after Initialize,
  Parse, EnumerateMaterials, EnumerateGeometry and before Render), in addition to the existing per-unit
  polls in the T12 source and T15 rasterizer.
- `tests/unit/ProviderThreadingTests.cpp` (new, `[provider][threading]`, 10 cases): active-call gate
  (including from another thread), lock gate, injected C++ exception, `std::bad_alloc`, raised and
  genuine access violations, the not-swallowed code table, after-the-fact overrun rejection + elapsed
  recording, and default-off/path-free diagnostics.
- Wiring: the three new sources + `ProviderThreadingTests.cpp` added to `Tests.Unit.vcxproj` and the
  provider `.vcxproj`; `Containment.cpp` sets `<ExceptionHandling>Sync</ExceptionHandling>`. Docs:
  `design/05` ("Threading and unload", "Security and robustness"), `design/interfaces.md`, new
  `design/adr/0018-provider-threading-containment-and-diagnostics.md`, this hand-off, `PROGRESS.md`.

**Deviations / decisions**
- "Without swallowing ordinary memory faults" is implemented as: an access violation is translated to
  `E_FAIL` with a diagnostic (never a fabricated success) and the underlying fault is still expected to
  be fuzzed/fixed (T43); only stack-overflow/breakpoint/single-step are allowed to propagate.
- The containment boundary is exercised directly (injected calls), because no adapter exists yet and
  the frozen adapter methods are `noexcept` (an adapter must call `RunContained` itself). T21-T34 must
  route third-party calls through it; the COM boundary is the last-resort net.
- The counters moved to their own PCH-free TU so `Tests.Unit.exe` compiles the shipped source for the
  unload-during-call test; this is a structural refactor, not a behavior change (ADR-0018).
- Enabling diagnostics in a shipping build (registry/installer or signed diagnostics build) is left to
  installer/T52; the provider provides only the tested gate.

**Check results**
- `msbuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Release|Debug /p:Platform=x64
  /p:SolutionDir=<root>\` → clean (builds the provider through the build-order ProjectReference).
- `x64\Release\Tests.Unit.exe` → All tests passed (200 cases / 134337 assertions).
- `x64\Debug\Tests.Unit.exe` → All tests passed (200 cases / 134422 assertions).
- `x64\Release\Tests.Unit.exe "[provider][threading]"` → 10 cases / 47 assertions; Debug same.
- `[provider]` → 71 cases / 60303 assertions Release, 71 / 60299 Debug.
- `tests\unit\check-provider-dependency-closure.ps1 -Configuration Release|Debug` → exit 0 both
  (Release 9 modules, Debug 6); `dumpbin /exports` = exactly `DllCanUnloadNow`, `DllGetClassObject`.
  Debug DLL gained a `.tls` section for the trivial `thread_local` (expected, no destructor).

**Next task must know**
- T17/T21-T34: a third-party parser call must go through `RunContained(&call, &ctx, deadline, stage)`
  because the frozen `IFamilyAdapter` methods are `noexcept`; poll `AdapterInput::deadline->Checkpoint()`
  inside long loops. Do not add another ledger, deadline or HRESULT table.
- `RunContained` returns `ProviderOutcome`; `HresultFor` maps it. It emits a `DiagnosticEvent` only when
  diagnostics are enabled; never add a string/path field to `DiagnosticEvent`.
- Adapters must remain PCH-free if they are to be compiled into `Tests.Unit.exe`; `Containment.cpp`
  must keep `/EHsc` or SEH will be misclassified as a C++ exception.
- The provider budget (`[provider][budget]`) and `ClassifyError` invariants are unchanged.

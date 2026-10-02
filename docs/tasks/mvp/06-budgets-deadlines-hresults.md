---
verify: x64\Release\Tests.Unit.exe
---
# T06 — Define budgets, deadlines, and HRESULT mapping

## Goal
Encode the provider's accountable limits, monotonic deadline policy and error taxonomy as tested constants
and helpers, so every later adapter and the rasterizer share one checked source of truth.

## Context (read first)
- `docs/design/03-file-formats-and-ingestion.md` — the thumbnail-host budget column and error taxonomy.
- `docs/design/05-thumbnail-provider.md` — stream/backing/accounted scratch caps, process-commit target, 750 ms p95 target, cooperative 2 s stop point, and the HRESULT table.
- `docs/design/testing-strategy.md` — memory and deadline measurement method.

## Scope
- [x] Define checked constants: 256 MiB stream max, 128 MiB contiguous backing, 192 MiB accounted scratch, 384 MiB product-owned allocation ledger/total-process qualification target, 2 M triangles inspected, 6 M points inspected, 250 k rasterized samples, 32 MP decoded texture, 10 k nodes, 4 096 materials, 96 MiB / 1 M-triangle Draco unit.
- [x] Implement a monotonic deadline type (750 ms p95 target, cooperative 2 s stop point checked at bounded intervals) with an `expired()`/`remaining()` API and checkpoints. Its contract must state that input preflight does not bound time spent inside a third-party call.
- [x] Implement a process-wide ledger that atomically reserves/releases product-owned allocations across concurrent calls and rejects a charge that would cross 384 MiB. Specify treatment of decoder allocations with callbacks, unaccounted library allocations, DLL/GDI overhead and the idle, loaded surrogate baseline. T12/T14/T15 and adapters charge controlled allocations; total process private commit is measured in T51, not claimed to be enforced by the ledger.
- [x] Implement checked integer/range helpers used at every file-derived allocation/offset.
- [x] Implement the HRESULT mapping from `05-thumbnail-provider.md` (bad call, unsupported, bad format, limit/deadline, OOM, decoder failure) as one function.
- [x] Add unit tests proving each cap boundary, concurrent reservations and releases, aggregate-ledger crossing, deadline arithmetic, overflow rejection, and every HRESULT mapping.

## Out of scope
- Using the limits inside adapters (→ T12–T15, T21–T34).
- The viewer's larger budgets (separate constants; do not reuse them).

## Design notes
- Limits are acceptance ceilings, not a guarantee that a limit-sized scene fits at once.
- Above-limit accountable input must name the first exceeded limit and still leave Explorer able to show its icon. An uninterruptible parser-call overrun is measured and reported; it is not described as a hard timeout.
- Prefer `HRESULT_FROM_WIN32(ERROR_*)` values exactly as tabulated; do not invent new codes without an ADR.

## Done when
- [x] `x64\Release\Tests.Unit.exe` (the `verify:` command) covers every constant, the aggregate ledger, and every mapping and passes in Debug and Release.
- [x] The constants file is referenced from `design/05-thumbnail-provider.md`.
- [x] Hand-off below filled in.

## Hand-off

**What landed**
- `thumbnail-provider/ProviderLimits.h` — `ProviderLimits` with the frozen `static constexpr` caps
  (256 MiB stream, 128 MiB contiguous backing, 192 MiB accounted scratch, 384 MiB ledger,
  384 MiB process-commit qualification target, 2 M triangles, 6 M points, 250 k rasterized samples,
  32 MP decoded texture, 10 k nodes, 4 096 materials, 96 MiB / 1 M Draco unit) plus checked
  `CheckedAdd`/`CheckedMultiply`/`CheckedRangeEnd`/`FitsInRange`/`CheckedNarrow<T>` built on
  `platform/CheckedMath.h`.
- `thumbnail-provider/Deadline.h` — monotonic `steady_clock` `Deadline` (750 ms p95 target, 2 s
  cooperative stop) with `expired()`, `remaining()`, `remainingToTarget()`, `overTarget()`,
  `Checkpoint()`, `Restart()`; contract documents that preflight does not bound a third-party call.
- `thumbnail-provider/AllocationLedger.h` — process-wide lock-free atomic ledger (384 MiB default)
  with `TryReserve`/`Release`/`ReserveScoped`/`Reserved`/`Available` and RAII `AllocationReservation`;
  documents decoder-callback, unaccounted-library, DLL/GDI and surrogate-baseline treatment.
- `thumbnail-provider/ProviderErrors.h` — `ProviderOutcome`, the single `HresultFor` table mapping,
  `ClassifyError` for `model_core::ImportErrorCode`, and `HresultForError`.
- `tests/unit/ProviderBudgetTests.cpp` — 8 cases (`[provider][budget]`): constants, cap boundaries
  and overflow, ledger crossing/aggregate, concurrent atomic reservations (8 threads, exactly
  capacity granted), RAII release, deadline arithmetic, and every HRESULT row/mapping.
- Wiring: headers in `Preview3DThumbnailProvider.vcxproj` (`..\shared\platform\include` added to the
  provider include path), compiled with static_asserts in `ProviderContracts.cpp`; test include path
  `..\..\thumbnail-provider` added to `Tests.Unit.vcxproj`.
- Docs: `design/05-thumbnail-provider.md` now names the four T06 header files; `design/interfaces.md`
  header table + follow-on updated; new `design/adr/0011-provider-budgets-deadline-ledger-and-hresult.md`.

**Deviations / decisions**
- The HRESULT table is implemented as `HresultFor(ProviderOutcome)` (one function for the six rows +
  success) with `ClassifyError`/`HresultForError` composing adapter `ImportErrorCode`s into it. This
  keeps "bad pointer / invalid call order" (not an `ImportErrorCode`) in the same mapping.
- File-level `FileUnavailable`/`FileChanged` classify as `ERROR_BAD_FORMAT`; `Cancelled`/`WorkerTimedOut`
  as `ERROR_TIMEOUT`; all resource/host limits as `ERROR_FILE_TOO_LARGE` (see the header comment and
  ADR-0011). No new HRESULT codes were invented.
- 32 MP is decimal (`32'000'000`) to match the viewer's gigapixel/megapixel convention.

**Check results**
- `msbuild thumbnail-provider\Preview3DThumbnailProvider.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=<root>\` → clean (0 warnings under /W4 /WX).
- `msbuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Debug|Release /p:Platform=x64 /p:SolutionDir=<root>\` → clean.
- `x64\Release\Tests.Unit.exe` → All tests passed (76215 assertions in 134 test cases).
- `x64\Release\Tests.Unit.exe "[provider][budget]"` → passed (2191 assertions in 8 test cases).
- `x64\Debug\Tests.Unit.exe` → All tests passed (76304 assertions in 134 test cases);
  Debug `[provider][budget]` passed (2179 assertions in 8 test cases).

**Next task must know**
- T12/T14/T15 and adapters call `AllocationLedger::ProcessWide().TryReserve/ReserveScoped` and
  `Deadline::Checkpoint()`; do not add private limits or a second ledger.
- The provider project now includes `..\shared\platform\include` (platform `CheckedMath.h`), which
  T07's extraction must keep available.
- Any new `model_core::ImportErrorCode` must be added to `ClassifyError`; the test walks the enum range.

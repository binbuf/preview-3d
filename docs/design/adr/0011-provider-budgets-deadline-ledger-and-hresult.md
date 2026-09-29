# 0011 — Provider budgets, deadline, allocation ledger and HRESULT mapping

## Status
accepted

## Context
T04 froze the names `ProviderLimits`, `Deadline` and `AllocationLedger` (forward-declared in
`ProviderTypes.h`) and left their definitions to T06, and design/05/03 fix the accountable caps, the
750 ms p95 / cooperative 2 s policy, the process-wide 384 MiB ledger and the HRESULT table. Before
any adapter or foundation task consumes them, the exact representation, accounting rules and
classification policy need to be recorded so they cannot drift per family.

## Decision
- The checked constants and the file-derived integer/range helpers live in
  `thumbnail-provider/ProviderLimits.h` as `static constexpr` members of `ProviderLimits`; the shared
  primitives come from `shared/platform/include/platform/CheckedMath.h`, and `FitsInRange`,
  `CheckedRangeEnd`, `CheckedAdd/Multiply` and `CheckedNarrow<T>` are added on top. Every file-derived
  offset/allocation is checked with them before use.
- `Deadline` (`Deadline.h`) reads `std::chrono::steady_clock` and carries a 750 ms p95 target and a
  2 s cooperative stop point. `expired()` observes the stop point, `remaining()` reports time to it,
  `overTarget()` reports the p95 target and `Checkpoint()` is the bounded-interval poll. It is
  cooperative: input preflight bounds admitted input, not the runtime of an opaque third-party call.
- `AllocationLedger` (`AllocationLedger.h`) is a process-wide, lock-free `std::atomic<uint64_t>`
  counter with a 384 MiB default ceiling and an RAII `AllocationReservation`. `TryReserve` /
  `Release` keep `Reserved() <= Limit()` under concurrency and reject the charge that would cross the
  ceiling; T12/T14/T15 and adapters charge product-owned allocations through it.
- The 384 MiB **total process private commit above the idle, loaded surrogate baseline** stays a
  measured release target (`kProcessCommitQualificationTargetBytes`, T51). Unaccounted library
  allocations, DLL/GDI overhead and decoder allocations without callbacks are explicitly outside the
  ledger and are not claimed to be bounded by it.
- `ProviderErrors.h` holds the single HRESULT mapping: `ProviderOutcome`
  (success, bad pointer, invalid call order, unsupported, bad format, limit, deadline, out of memory,
  decoder failure) → `HresultFor`, with `ClassifyError` folding the `model_core::ImportErrorCode`
  taxonomy into one row and `HresultForError` composing the two. Only the tabulated
  `HRESULT_FROM_WIN32(ERROR_*)`/`E_*` values are used; new codes require a new ADR.

## Consequences
- Family tasks (T21–T34) and the sampler/raster/backing tasks consume these headers instead of
  private limits; `ProviderContracts.cpp` compiles them under `/W4 /WX`, and `Tests.Unit.exe`
  (`[provider][budget]`) gates each constant, the concurrent ledger, deadline arithmetic, overflow
  rejection and every HRESULT row.
- The provider build now needs `..\shared\platform\include` on its include path (the platform
  `CheckedMath.h` the design already classifies as compiled into the DLL).
- Adding a new `ImportErrorCode` forces a `ClassifyError` update; the test walks the enum range so an
  unmapped code cannot land silently.
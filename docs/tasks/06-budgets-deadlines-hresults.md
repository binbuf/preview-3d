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
- [ ] Define checked constants: 256 MiB stream max, 128 MiB contiguous backing, 192 MiB accounted scratch, 384 MiB product-owned allocation ledger/total-process qualification target, 2 M triangles inspected, 6 M points inspected, 250 k rasterized samples, 32 MP decoded texture, 10 k nodes, 4 096 materials, 96 MiB / 1 M-triangle Draco unit.
- [ ] Implement a monotonic deadline type (750 ms p95 target, cooperative 2 s stop point checked at bounded intervals) with an `expired()`/`remaining()` API and checkpoints. Its contract must state that input preflight does not bound time spent inside a third-party call.
- [ ] Implement a process-wide ledger that atomically reserves/releases product-owned allocations across concurrent calls and rejects a charge that would cross 384 MiB. Specify treatment of decoder allocations with callbacks, unaccounted library allocations, DLL/GDI overhead and the idle, loaded surrogate baseline. T12/T14/T15 and adapters charge controlled allocations; total process private commit is measured in T51, not claimed to be enforced by the ledger.
- [ ] Implement checked integer/range helpers used at every file-derived allocation/offset.
- [ ] Implement the HRESULT mapping from `05-thumbnail-provider.md` (bad call, unsupported, bad format, limit/deadline, OOM, decoder failure) as one function.
- [ ] Add unit tests proving each cap boundary, concurrent reservations and releases, aggregate-ledger crossing, deadline arithmetic, overflow rejection, and every HRESULT mapping.

## Out of scope
- Using the limits inside adapters (→ T12–T15, T21–T34).
- The viewer's larger budgets (separate constants; do not reuse them).

## Design notes
- Limits are acceptance ceilings, not a guarantee that a limit-sized scene fits at once.
- Above-limit accountable input must name the first exceeded limit and still leave Explorer able to show its icon. An uninterruptible parser-call overrun is measured and reported; it is not described as a hard timeout.
- Prefer `HRESULT_FROM_WIN32(ERROR_*)` values exactly as tabulated; do not invent new codes without an ADR.

## Done when
- [ ] `x64\Release\Tests.Unit.exe` (the `verify:` command) covers every constant, the aggregate ledger, and every mapping and passes in Debug and Release.
- [ ] The constants file is referenced from `design/05-thumbnail-provider.md`.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_

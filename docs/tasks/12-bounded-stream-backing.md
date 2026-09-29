---
verify: x64\Release\Tests.Unit.exe
---
# T12 — Implement bounded stream backing over IInitializeWithStream

## Goal
Turn the Shell-managed `IStream` into the only input the provider ever reads, safely: a bounded,
deadline-aware source that supports seekable and non-seekable streams without trusting `STATSTG`.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — "Call contract" and "Stream ingestion".
- `docs/design/03-file-formats-and-ingestion.md` — the thumbnail budget column.
- `docs/tasks/06-budgets-deadlines-hresults.md` — cap constants, deadline and HRESULT helpers.
- `docs/tasks/11-com-core-lifetime.md` — the COM object the stream lives on.

## Scope
- [ ] Implement `IInitializeWithStream::Initialize`: accept exactly one non-null stream, take an independent reference, reject a second initialization.
- [ ] Query `STATSTG` when supported but verify via checked reads; fail fast to the safe fallback when a reported size exceeds 256 MiB.
- [ ] Implement serialized bounded range reads with a small block cache for seekable streams.
- [ ] Implement a checked in-process backing buffer up to 128 MiB for adapters needing contiguity or non-seekable input; larger input returns the safe fallback. Reserve backing and block-cache bytes in the T06 process-wide product-owned allocation ledger before allocation; a charge that would cross 384 MiB returns the safe fallback.
- [ ] Abort on deadline, limit, or short-read inconsistency; never recover or assume a filesystem path; write no temporary or persistent file.
- [ ] Add tests for seekable/non-seekable streams, oversized size hints, short reads, deadline abort and repeated initialization.

## Out of scope
- Family parsing (→ T21–T34).
- Viewer `MapViewOfFile` mapping semantics (intentionally not used here).

## Design notes
- There is no zero-copy mapping guarantee for Shell `IStream`; bounded memory and deterministic latency win.
- The provider never resolves `.mtl`, `.bin`, USD layers or any external sidecar from a stream.
- Every file-derived duration offset goes through the checked helpers from T06.

## Done when
- [ ] Stream tests pass in Debug and Release, including the oversized and short-read fallbacks.
- [ ] `IInitializeWithStream` rejects second init and null input with the tabulated HRESULTs.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_

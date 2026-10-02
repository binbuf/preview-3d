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
- [x] Implement `IInitializeWithStream::Initialize`: accept exactly one non-null stream, take an independent reference, reject a second initialization.
- [x] Query `STATSTG` when supported but verify via checked reads; fail fast to the safe fallback when a reported size exceeds 256 MiB.
- [x] Implement serialized bounded range reads with a small block cache for seekable streams.
- [x] Implement a checked in-process backing buffer up to 128 MiB for adapters needing contiguity or non-seekable input; larger input returns the safe fallback. Reserve backing and block-cache bytes in the T06 process-wide product-owned allocation ledger before allocation; a charge that would cross 384 MiB returns the safe fallback.
- [x] Abort on deadline, limit, or short-read inconsistency; never recover or assume a filesystem path; write no temporary or persistent file.
- [x] Add tests for seekable/non-seekable streams, oversized size hints, short reads, deadline abort and repeated initialization.

## Out of scope
- Family parsing (→ T21–T34).
- Viewer `MapViewOfFile` mapping semantics (intentionally not used here).

## Design notes
- There is no zero-copy mapping guarantee for Shell `IStream`; bounded memory and deterministic latency win.
- The provider never resolves `.mtl`, `.bin`, USD layers or any external sidecar from a stream.
- Every file-derived duration offset goes through the checked helpers from T06.

## Done when
- [x] Stream tests pass in Debug and Release, including the oversized and short-read fallbacks.
- [x] `IInitializeWithStream` rejects second init and null input with the tabulated HRESULTs.
- [x] Hand-off below filled in.

## Hand-off

**What landed**
- `thumbnail-provider/StreamSource.h`/`StreamSource.cpp` — the single `BoundedSource`
  implementation (`BoundedStreamSource`). `Create(IStream*, Deadline, AllocationLedger&,
  ProviderOutcome&)` probes seekability (`Seek(STREAM_SEEK_CUR)`), applies the STATSTG/seek-to-end
  preflight (fail fast `LimitExceeded` above 256 MiB), reserves the 64 KiB × 8-slot block cache
  against the T06 ledger, and returns the failure row. Seekable reads go through a checked block
  cache (`FillBlock`/`ReadViaCache`); non-seekable or contiguous reads materialize the backing
  buffer up to 128 MiB, charging each growth to the ledger. Short reads inside the reported extent
  abort with `BadFormat`; deadline, limit and OOM abort with the tabulated rows. No path recovery,
  no file write, no `MapViewOfFile`. `StreamSource.cpp` compiles without the provider PCH (like the
  shared parser subset) so the same TU builds in the DLL and `Tests.Unit.exe`.
- `thumbnail-provider/ComCore.cpp` — `ProviderObject` now derives from `IInitializeWithStream`
  (no second object type): `QueryInterface` answers `IID_IInitializeWithStream`, `Initialize`
  returns `E_POINTER` for a null stream, `E_UNEXPECTED` once a stream is adopted, and
  `HresultFor(outcome)` for preflight failures. A `StreamSource()` accessor hands the source to
  T13's `GetThumbnail`. `ComCore.h` comment updated.
- `tests/unit/ProviderStreamTests.cpp` (`[provider][stream]`, 9 cases) — an in-memory `IStream`
  double, direct `BoundedStreamSource` behaviour (seekable ranges across block boundaries,
  non-seekable materialization, oversized hint, short read, expired deadline, ledger charge/release,
  contiguous cap) and DLL `[provider][stream][com]` cases (null/repeated init, oversized HRESULT).
- Docs: [ADR-0014](design/adr/0014-bounded-stream-backing.md); `design/05-thumbnail-provider.md`
  ("Stream ingestion", "Threading and unload"); `design/interfaces.md` ("BoundedSource",
  "COM core and lifetime"). Build wiring in `Preview3DThumbnailProvider.vcxproj`
  (`StreamSource.{h,cpp}`, `NotUsing` PCH) and `Tests.Unit.vcxproj`.

**Deviations / decisions**
- The source owns one `Deadline` copied at `Initialize`; the caller must restart it at
  `GetThumbnail` entry via `MutableDeadline()` (documented in ADR-0014). `Initialize` does not
  check the deadline, so a stale per-object deadline cannot fail the cheap preflight.
- Seekability/contiguity policy: an unknown-size seekable source pins its validated size on the
  first short tail block; `ContiguousView()` on an unknown-size seekable source returns empty
  (`Unsupported`) because a bounded window cannot be placed.
- Non-seekable materialization charges the ledger incrementally (growth reservation approximated
  by the target capacity); over-reservation is released on destruction.
- The test stream double keeps its initial reference and never self-deletes, so it can live on the
  stack while the source holds its own reference.

**Check results**
- `msbuild thumbnail-provider\Preview3DThumbnailProvider.vcxproj /p:Configuration=Release|Debug
  /p:Platform=x64 /p:SolutionDir=<root>\` → clean (0 warnings under /W4 /WX).
- `msbuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Debug|Release /p:Platform=x64
  /p:SolutionDir=<root>\` → clean.
- `x64\Release\Tests.Unit.exe` → All tests passed (76405 assertions in 155 test cases);
  `x64\Debug\Tests.Unit.exe` → All tests passed (76491 assertions in 155 test cases).
- `x64\Release\Tests.Unit.exe "[provider][stream]"` and `"x64\Debug\..."` → 9 cases / 57
  assertions green in both.
- `x64\Release\Tests.Unit.exe "[provider]"` → 26 cases / 2371 assertions green (was 17).
- `dumpbin /exports x64\Release\Preview3DThumbnailProvider.dll` → exactly `DllCanUnloadNow`,
  `DllGetClassObject`.
- `tests\unit\check-provider-dependency-closure.ps1 -Configuration Release` → exit 0 (no
  viewer/worker/host/core import; no ole32/propsys import added).

**Next task must know**
- T13 adds `IThumbnailProvider` to the same `ProviderObject` and reads through
  `StreamSource()`: pass it to adapters as `BoundedSource*` and restart its deadline at
  `GetThumbnail` entry with `MutableDeadline().Restart(...)` (or hand `&MutableDeadline()` as the
  `AdapterInput::deadline`). Treat an empty `ContiguousView()` as the safe fallback.
- The source charges the T06 `AllocationLedger::ProcessWide()` in the DLL path; do not add a
  second ledger. `StreamSource.cpp` must stay PCH-free and free of worker/broker/viewer headers.
- The `.def` two-symbol export surface and `CLASS_E_CLASSNOTAVAILABLE` for unknown CLSIDs remain
  frozen (T11/ADR-0013).

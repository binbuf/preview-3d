# 0014 — Bounded stream backing over IInitializeWithStream

## Status
accepted

## Context
design/05-thumbnail-provider.md ("Call contract", "Stream ingestion") fixes the provider's
only input: the Shell-managed `IStream`. T04 froze the `BoundedSource` interface
(`ProviderTypes.h`) and T06 froze the caps, deadline, ledger and HRESULT mapping, but the
implementation that turns an untrusted stream into checked, deadline-aware reads did not
exist. The decisions below constrain every adapter (T21–T34), the sampler (T14) and the
`GetThumbnail` call path (T13/T16), so they are recorded before family work starts.

## Decision
- One implementation, `thumbnail-provider/StreamSource.{h,cpp}`, of the frozen
  `BoundedSource` interface: `BoundedStreamSource`. It is the T12 Windows boundary
  ADR-0009 permits; `ProviderTypes.h` stays free of `IStream`/`STATSTG`.
- `IInitializeWithStream::Initialize` lives on the T11 `ProviderObject` (no second object
  type). It accepts exactly one non-null stream (null → `E_POINTER`), takes an independent
  reference through the source, and returns `E_UNEXPECTED` once a stream has been adopted.
  Preflight failures map through the T06 table.
- Preflight queries `Stat` and a seek-to-end probe. A reported size over 256 MiB fails fast
  with `LimitExceeded` **before any read**, even when another probe reports a smaller size,
  so an oversized file costs Explorer only a size check. STATSTG is otherwise a hint: reads
  verify it.
- Seekability is advertised, never assumed (`Seek(STREAM_SEEK_CUR)` probe). Seekable streams
  are served by checked range reads through a fixed small block cache (64 KiB × 8 slots)
  charged to the T06 ledger before allocation. A short read inside the reported extent is an
  inconsistency that aborts with `BadFormat`; an unknown-size seekable stream pins its
  validated size on the first short tail block instead.
- The contiguous backing buffer is materialized on demand for non-seekable input or an
  adapter that needs one buffer, and is capped at 128 MiB (`kContiguousBackingMaxBytes`). A
  larger source returns an empty `ContiguousView()` (`LimitExceeded`) rather than
  over-allocating. Backing bytes are reserved against the ledger before each growth; a charge
  crossing the 384 MiB ledger ceiling returns the safe fallback.
- Reads, block fills and materialization call `Deadline::Checkpoint()` and abort with
  `Deadline` when the cooperative stop point passes. The source owns one `Deadline`; the
  caller restarts it at `GetThumbnail` entry through `MutableDeadline()`.
- No path recovery, no sidecar resolution, no temporary or persistent file, and no
  `MapViewOfFile`: consistent with design/05's "no zero-copy mapping guarantee".

## Consequences
- T13/T16 read through `BoundedSource*` only and restart the source deadline at call entry;
  adapters that need contiguity call `ContiguousView()` and treat an empty span as the safe
  fallback.
- `StreamSource.cpp` compiles without the provider PCH (like the shared parser subset) so
  `Tests.Unit.exe` links the same source and exercises it directly.
- `Tests.Unit.exe` `[provider][stream]` covers seekable/non-seekable reads, the oversized
  hint, short reads, deadline abort, ledger charging and the contiguous cap; the
  `[provider][stream][com]` cases load the DLL for null/repeated initialization and the
  oversized HRESULT.
- The `.def` export surface is unchanged (still exactly `DllGetClassObject`,
  `DllCanUnloadNow`); the dependency closure gains no product or ole32 import.
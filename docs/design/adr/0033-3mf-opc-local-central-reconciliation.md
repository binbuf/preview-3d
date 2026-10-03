# 0033 — 3MF OPC preflight reconciles local/central headers and rejects data descriptors

## Status
accepted (SEC-05)

## Context
The product-owned 3MF/OPC preflight (`import-worker/src/ThreeMfOpcPreflight.cpp`) is the admission
gate for bytes that the pinned lib3mf 2.5 reader later consumes with `SetStrictModeActive(false)`
(`ThreeMfImportWorker.cpp`). Before SEC-05 the preflight rejected only encryption, compared just the
local `flags`/`method`/`nameLength` against the central directory, and required the ZIP64 locator's
total-disk field to be 0. Three gaps followed: a general-purpose bit-3 entry (sizes/CRC in a trailing
data descriptor) was accepted while lib3mf read it; local CRC and sizes that disagreed with the
central directory were not detected, so the two boundaries could describe different bytes; and the
attacker-controlled ZIP64 EOCD offset `zoff` was used in unchecked 64-bit additions (bounds checks in
`r32`/`r64` prevent OOB, but the wrap could redirect reads). The stricter total-disk rule also rejected
valid ZIP64 files whose locator correctly said 1. USDZ already used the stricter policy
(`UsdZipPreflight.cpp`).

## Decision
1. **Reject general-purpose bit 3** (streaming data descriptor) with `InvalidDirectory`, matching
   USDZ, instead of implementing full descriptor reconciliation. The preflight admits only archives
   whose central directory is authoritative.
2. **Reconcile local against central after ZIP64 resolution**: the local CRC, compressed size,
   uncompressed size and the raw name bytes must equal the central-directory values. Local ZIP64
   extra fields are parsed with the same sentinel-selected order as the central entry.
3. **Bound the ZIP64 locator offset before use**: `zoff + 56` must not overflow and must end at or
   before the locator (`e - 20`), so every subsequent read is in range and cannot wrap.
4. **Accept the spec-correct locator total-disk value 1** for a single-disk archive; any other value
   fails as `MultiDisk` (multi-disk EOCDs were already rejected). The local-header offset is likewise
   required to satisfy `off + 30 <= centralOffset` before any local field is read.

## Consequences
- The preflight is a true admission gate: a package that lib3mf could read divergently is rejected
  before the library sees it. `InvalidDirectory` maps to `ArchiveLimit` at the worker/provider
  boundaries, consistent with existing malformed-archive handling.
- Streaming/data-descriptor 3MF packages are no longer accepted (deliberate divergence from the
  original 3MF-002 plan text). This matches USDZ and is stricter, not weaker.
- ZIP64 files with the spec value 1 are now accepted, fixing a fail-closed compatibility bug.
- Regression coverage: `[3mf][archive][security]` and the `3MF-007` cases assert typed rejection
  through both `InspectThreeMfOpc` and the real worker; the `bit3-entry`,
  `local-central-size-mismatch`, `local-name-mismatch`, `zip64-valid` and `zip64-locator-wrap`
  derived seeds in `tests/fixtures/3mf/manifest.json` carry the same shapes into `ThreeMfFuzz`.
- Extraction (`ExtractThreeMfOpcPart`) is unchanged; it still validates CRC and exact inflate totals.
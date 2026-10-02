---
verify: x64\Release\Tests.ImportIsolation.exe
---

# T05 — 3MF OPC preflight/library reconciliation

## Goal
The product-owned OPC preflight validates the same bytes lib3mf will consume, so the preflight is a
real admission gate rather than a second opinion, and ZIP64 fields cannot confuse the scanner.

## Context (read first)
- `import-worker/src/ThreeMfOpcPreflight.cpp:25` — per-entry flags reject only encryption
  (`flags & 1`); ZIP general-purpose bit 3 (data descriptor) is not rejected.
- `import-worker/src/ThreeMfOpcPreflight.cpp:32` — local header is compared only on
  `flags`, `method`, and `nameLength`; local CRC/compressed/uncompressed sizes and the local name
  bytes are not compared with the central directory, while `lib3mf` reads with
  `SetStrictModeActive(false)` (`ThreeMfImportWorker.cpp:155`). The two boundaries can disagree.
- `import-worker/src/ThreeMfOpcPreflight.cpp:23` — attacker-controlled ZIP64 EOCD offset `zoff` is
  used in unchecked 64-bit additions; each `r32/r64` is bounds-checked, so this is wrap/confusion,
  not OOB. The locator's "total disks" field is required to be 0, but the spec value is 1, so valid
  ZIP64 files are rejected (fail-closed compatibility bug).
- `import-worker/src/UsdZipPreflight.cpp:158-201` — the policy to copy: reject bit 3, require local
  CRC/compressed/uncompressed/name-length equality, byte-compare the two names.
- Tests: `tests/import-isolation/ThreeMfSpikeTests.cpp`; fuzz: `tests/fuzz/ThreeMfFuzz.cpp` and its
  seed preparer.

## Scope
- [ ] Reject general-purpose bit 3 (or implement full local/central reconciliation). Prefer rejecting
      it, matching USDZ.
- [ ] Compare local CRC, compressed size, uncompressed size, and name bytes against the central
      directory (after ZIP64 resolution).
- [ ] Bound the ZIP64 locator offset with checked math and a range requirement before use; accept the
      spec-correct total-disk value (1) or document the intentional stricter value.
- [ ] Regression cases: bit-3 entry, local/central size mismatch, local name mismatch, ZIP64 offset
      near `UINT64_MAX`; assert typed rejection and no divergent read.
- [ ] Add the new cases as fuzz seeds (`tests/fuzz/README.md` conventions; seeds are immutable).

## Out of scope
- Replacing lib3mf or enabling lib3mf strict mode (behavior change → separate ADR if desired).
- USDZ preflight (already meets this bar).

## Design notes
- The preflight's job is admission; if it cannot prove the ranges, reject. Do not defer to lib3mf.
- Keep extraction (`ExtractThreeMfOpcPart`) validating CRC and exact inflate totals; the new checks
  are additive.
- If a deliberate divergence from ZIP spec is kept, write the reason in a comment and record it in
  the task Hand-off.

## Done when
- [ ] `Tests.ImportIsolation` passes with the new cases, and `ThreeMfFuzz` runs the new seeds for a
      bounded smoke without findings.
- [ ] Hand-off filled in.

## Hand-off
Landed (SEC-05):
- `import-worker/src/ThreeMfOpcPreflight.cpp`: added a shared `zip64()` extra parser; central
  entries now resolve ZIP64 fields through it. Rejects general-purpose bit 3 (`flags & 8` →
  `InvalidDirectory`). Local header now reconciles CRC, compressed/uncompressed size and raw name
  bytes against the central directory (after local ZIP64 resolution). The local offset is bounded
  (`off + 30 <= coff`) and the ZIP64 locator offset is bounded with checked add
  (`zoff + 56` no-wrap, `<= e - 20`) before any field is read. Locator total-disk must be 1
  (was 0), fixing a fail-closed compatibility bug; other values → `MultiDisk`.
- `tests/import-isolation/ThreeMfSpikeTests.cpp`: new helpers (`Read16`/`Put16`/`FindEocd`,
  `WithFirstCentralFlags`, `WithLocalSizeMismatch`, `WithLocalNameMismatch`, `AsZip64`,
  `WithZip64LocatorOffset`) and a new `[3mf][archive][security]` case asserting `None` for a
  spec-shaped ZIP64 package (total-disk 1), `MultiDisk` for total-disk 0, and `InvalidDirectory`
  for locator offsets `0x...ffe0`/`0x...ffff`. The `3MF-007` worker test gained bit-3, local-size
  and local-name mismatch cases that assert `InvalidDirectory`/`ArchiveLimit` through the real
  worker (no divergent read).
- `tests/fixtures/3mf/verify.py` + `manifest.json`: five derived cases (`bit3-entry`,
  `local-central-size-mismatch`, `local-name-mismatch`, `zip64-valid`, `zip64-locator-wrap`),
  materialized as fuzz seeds. `verify.py` reports 7 sources / 12 derived.
- Docs: new ADR 0033; `docs/design/03-file-formats-and-ingestion.md` 3MF paragraph updated;
  `docs/design/adapters/3mf-thumbnail.md` 3MF-002 text updated (bit-3 rejection replaces the
  earlier "streaming-extension packages pass" wording).

Deviations: chose to reject bit 3 rather than implement full data-descriptor reconciliation
(explicitly permitted and matches USDZ). ZIP64 locator total-disk now requires 1 (spec-correct)
instead of the previous 0; documented in ADR 0033.

Checks:
- `MSBuild tests\import-isolation\Tests.ImportIsolation.vcxproj /p:Configuration=Debug ...` — built.
- `x64\Debug\Tests.ImportIsolation.exe "[3mf][archive][security]"` — 1 case, 216 assertions, pass.
- `x64\Debug\Tests.ImportIsolation.exe "[3mf-007][archive][recovery]"` — 1 case, 1540 assertions, pass.
- Full Debug suite: 393 cases / 387 passed / 6 failed — exactly the documented pre-existing set
  (SidecarPathResolver ×3, ThreeMfSpike Job pressure, UsdSpike:230, OpenUsdHostSpike:244); the new
  case passes.
- `MSBuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Release ...` then `npm test` — 350 cases,
  all pass.
- `python tests/fixtures/3mf/verify.py` — verified 7 sources and 12 derived cases.
- `tests\fuzz\x64\Release\ThreeMfFuzz.exe TestResults\3mf-007\fuzz-seeds-t05 ... -max_total_time=45`
  — 872,334 units, no crash/leak/timeout artifacts.

Remaining: none. Next task (T06) is independent.
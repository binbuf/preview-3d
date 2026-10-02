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
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
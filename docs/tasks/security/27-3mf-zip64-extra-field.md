---
verify: x64\Release\Tests.ImportIsolation.exe
---

# T27 — Bound 3MF ZIP64 extra-field reads to the sub-record (SEC-05 follow-up)

## Goal
`ThreeMfOpcPreflight` rejects a ZIP64 extra field whose declared size is too small for the sentinel-
selected values, instead of reading adjacent bytes, so the preflight validates exactly the bytes
lib3mf will consume.

## Context (read first)
- `docs/tasks/security/05-3mf-opc-reconciliation.md`.
- `import-worker/src/ThreeMfOpcPreflight.cpp:23` `zip64()` validates `p+4+sz <= xe` for the field
  header, then reads up to three `r64` values for the sentinel-selected fields without checking the
  field's own `sz` covers them (`p+8 <= fieldEnd`). The reads stay inside the file span, so this is
  not an out-of-bounds read, but it breaks the "preflight validates the same bytes the library
  consumes" property.
- `tests/import-isolation/ThreeMfSpikeTests.cpp` has the ZIP mutators (`AsZip64`,
  `WithZip64LocatorOffset`, `Put16`/`Put32`) to build the case.

## Scope
- [x] Reject a ZIP64 extra field whose size does not cover the present sentinel-selected values;
      keep the existing wrap and bounds checks.
- [x] Regression: a ZIP64 central (and local) entry with a truncated extra field is typed
      `InvalidDirectory`, and no divergent read occurs.
- [x] Add the case as a fuzz seed per `tests/fuzz/README.md` conventions.

## Out of scope
- The bit-3/name/CRC/locator reconciliation (done, SEC-05).

## Design notes
- The fix is in the shared `zip64()` helper, so both the central and local call sites are covered.
- Keep `InvalidDirectory` mapping to `ArchiveLimit` at the boundary.

## Done when
- [x] `x64\Release\Tests.ImportIsolation.exe` passes with the new `[3mf][archive][security]` case.
- [x] `ThreeMfFuzz` runs the new seed for a bounded smoke without findings.
- [x] Hand-off filled in.

## Hand-off
Landed (SEC-05 follow-up, T27):
- `import-worker/src/ThreeMfOpcPreflight.cpp`: the shared `zip64()` helper now bounds each 64-bit
  read to the ZIP64 field's own declared size (`end = p + sz`), so a short extra field can no longer
  consume the adjacent name/extra bytes. Because the pre-existing bounds/wrap failures are genuine
  truncation while a short field is a malformed directory, the helper now returns
  `ThreeMfOpcError` instead of `bool`: a field that does not cover the sentinel-selected values is
  `InvalidDirectory`; `r16`/`p+4+sz>xe`/`r64` failures keep `Truncated`. Both the central and local
  call sites propagate the returned code, so the fix covers both paths. No other behaviour changed;
  `InvalidDirectory` still maps to `ArchiveLimit` in `ThreeMfImportWorker.cpp:122`.
- `tests/import-isolation/ThreeMfSpikeTests.cpp`: added `WithTruncatedCentralZip64Extra` and
  `WithTruncatedLocalZip64Extra` (append a ZIP64 id-1 sub-record after any existing central/local
  extras, set the sentinel, and grow the central size/offset consistently). The
  `[3mf][archive][security]` case now asserts `InvalidDirectory` for declared sizes 0/4/7 on both the
  central and local entry, plus a declared-size-8/two-sentinel case where the second read would have
  crossed into the next sub-record. Existing valid-ZIP64, total-disk and locator-wrap assertions are
  unchanged.
- `tests/fixtures/3mf/verify.py` + `manifest.json`: two derived cases
  `zip64-truncated-central-extra.3mf` and `zip64-truncated-local-extra.3mf` (each 1688 bytes),
  materialized as fuzz seeds by `prepare_3mf_seeds.py`; `verify.py` now reports 7 sources / 14
  derived.

Deviations / decisions:
- Changed the `zip64()` helper's return type (`bool` → `ThreeMfOpcError`) rather than mapping every
  helper failure to a single code, so only the new field-size mismatch is `InvalidDirectory` and the
  pre-existing truncation paths keep `Truncated`. Both map to `ArchiveLimit` at the broker boundary,
  so the product-visible error is unchanged.
- Used a deliberately truncated declared size rather than appending a second sub-record of adjacent
  bytes in the seed itself; the C++ underflow case (size 8, two sentinels) covers the "read into the
  next sub-record" shape directly.

Checks (Release x64):
- `msbuild tests\import-isolation\Tests.ImportIsolation.vcxproj /p:Configuration=Release
  /p:Platform=x64 /p:SolutionDir=<root>\` — exit 0.
- `x64\Release\Tests.ImportIsolation.exe "[3mf][archive][security]"` — 1 case, 598 assertions, pass.
- `x64\Release\Tests.ImportIsolation.exe` — 408 cases / 403 passed / 5 skipped, 293485 assertions,
  exit 0 (matches the T26 baseline: the 5 skipped are the Release-compiled-out fault harnesses).
- `python tests/fixtures/3mf/verify.py` — verified 7 sources and 14 derived cases.
- `python tests/fuzz/prepare_3mf_seeds.py TestResults\3mf-007\fuzz-seeds-t27` — 21 seeds.
- `msbuild tests\fuzz\ThreeMfFuzz.vcxproj /p:Configuration=Release /p:Platform=x64
  /p:SolutionDir=<root>\ /m:1` — exit 0.
- `tests\fuzz\x64\Release\ThreeMfFuzz.exe TestResults\3mf-007\fuzz-seeds-t27 -max_total_time=30
  -timeout=5 -rss_limit_mb=1024 -max_len=1048576 -print_final_stats=1 -verbosity=0` — 344016
  executions, 415 MiB peak RSS, exit 0, no crash/ASan report.

Remaining work / blockers: none.
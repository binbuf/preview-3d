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
- [ ] Reject a ZIP64 extra field whose size does not cover the present sentinel-selected values;
      keep the existing wrap and bounds checks.
- [ ] Regression: a ZIP64 central (and local) entry with a truncated extra field is typed
      `InvalidDirectory`, and no divergent read occurs.
- [ ] Add the case as a fuzz seed per `tests/fuzz/README.md` conventions.

## Out of scope
- The bit-3/name/CRC/locator reconciliation (done, SEC-05).

## Design notes
- The fix is in the shared `zip64()` helper, so both the central and local call sites are covered.
- Keep `InvalidDirectory` mapping to `ArchiveLimit` at the boundary.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe` passes with the new `[3mf][archive][security]` case.
- [ ] `ThreeMfFuzz` runs the new seed for a bounded smoke without findings.
- [ ] Hand-off filled in.

## Hand-off
_(filled in by the implementing session)_
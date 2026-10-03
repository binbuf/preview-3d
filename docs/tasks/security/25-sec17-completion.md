---
verify: x64\Release\Tests.Unit.exe "~[graphics]"
---

# T25 — Complete SEC-17: promote provider fuzz/soak into the gate and classify hostile failures

## Goal
The provider fuzz target and surrogate soak are part of the scheduled/required gate, and the soak
classifies a contained AV (quarantine), a stack overflow, and an uncatchable `__fastfail` as the
SEC-08 allowed failures rather than a pass.

## Context (read first)
- `docs/tasks/security/17-fuzz-provider-and-soak.md` — the landed fuzz/soak and its two admitted
  remaining items: `fuzz-smoke` promotion and the hostile-input SEC-08 classification lane.
- `docs/design/adr/0047-provider-pipeline-fuzz-and-surrogate-soak.md`; SEC-08 / ADR-0037.
- `tests/fuzz/ProviderFuzz.vcxproj` links all eight adapters including STEP/OCCT, so a cold build
  needs both the root and `thumbnail-provider\step-occt` vcpkg manifests; `fuzz-smoke` currently
  restores only the root manifest.
- `.github/workflows/ci.yml` `fuzz-smoke` matrix (STEP/3MF/STL/PLY/OBJ/glTF).

## Scope
- [ ] Make the `ProviderFuzz` build viable in `fuzz-smoke` (restore the second manifest in that job,
      or split STEP out) and add it to the matrix, then promote the lane per the SEC-13 policy.
- [ ] Add a hostile-input soak classification: a contained AV quarantines and is recorded as a
      failure-with-reason; a stack-overflow/`__fastfail` surrogate death is recorded as the
      documented allowed failure, not a pass.
- [ ] Record runtimes and peak RSS; update ADR-0047, design/05, design/09 and README.

## Out of scope
- Worker-format fuzzing (SEC-15/16/16b, done).

## Design notes
- Keep one target per runner (shared `tests\fuzz\x64\Release` intermediate dir, MSB8028).
- The soak is Explorer-stability evidence, not a security boundary (ADR-0005).
- Provider stack-overflow/recursion hardening is T20's successor; this task records the failure
  class, it does not re-implement containment.

## Done when
- [ ] `x64\Release\Tests.Unit.exe "~[graphics]"` passes; a bounded `ProviderFuzz` smoke exits 0.
- [ ] The soak reports the raw classification numbers in the Hand-off.

## Hand-off
_(filled in by the implementing session)_
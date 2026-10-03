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

**What landed.**
- **`ProviderFuzz` is in the `fuzz-smoke` gate.** `.github/workflows/ci.yml` `fuzz-smoke` adds a
  `Provider` matrix entry (`tests\fuzz\ProviderFuzz.vcxproj`, `prepare_provider_seeds.py`,
  `TestResults/security-t17/provider-seeds`) and a `Provider`-only step that restores the isolated
  `thumbnail-provider/step-occt` OCCT manifest the target links (the root manifest is still installed
  by MSBuild manifest mode). The job reuses the test/dependencies vcpkg files cache (credential-free,
  so it is safe on a fork PR) and passes `/p:VcpkgRoot`. The lane was **promoted** out of the
  nightly-only `if: schedule || workflow_dispatch` guard: it now runs on every `pull_request`/`push`
  and through `workflow_call` from `release.yml` (SEC-13/T13 policy), one target per runner.
- **Hostile surrogate-death classification.** `packaging/smoke/ProviderSoak.{h,cpp}` now keeps a
  handle on every `dllhost` that hosts the provider, reads its exit code on death, and classifies:
  a contained fault (a valid request refused while no surrogate died) as a quarantine
  *failure-with-reason* (exit 1); a stack overflow (`0xC00000FD`) or `__fastfail`/stack-cookie
  (`0xC0000409`, `0xC0000602`) death as the documented *allowed* failure (reports `ALLOWED`, not
  `PASS`, exit 0); any other death as an unexpected fault (exit 1). `--hostile <path>` (repeatable,
  new) is an expected fail-closed lane recorded as `rejected`. Raw counters
  (`rejected/quarantined/allowed_faults/unexpected_faults`), fault codes, elapsed ms and host peak
  working set are printed and written to `soak-report.txt`. `ProviderSmokeHost.exe
  --soak-classify-selftest` deterministically checks the six-case classifier.
- **Fixture.** `packaging/smoke/fixtures/hostile-truncated.stl` (84 bytes: an STL header + a
  declared 12-facet count with no facet data) drives the hostile lane.
- **Docs.** ADR-0052 (new) records the promotion and classification decisions; ADR-0042/0046/0047,
  `docs/design/05-thumbnail-provider.md`, `docs/design/09-quality-performance-and-security.md`,
  `tests/fuzz/README.md`, `packaging/smoke/README.md` and `CONTRIBUTING.md` are updated.

**Deviations.**
- The classification identifies a quarantine from the *valid* phase (the quarantine is
  process-global, so a refused valid model is the evidence) and an allowed death from the tracked
  `dllhost` exit code. A real stack-overflow/`__fastfail` death was **not** induced on this machine
  (there is no deterministic hostile fixture for it), so the allowed-death branch is covered by
  `--soak-classify-selftest`, not by a live surrogate that dies with `0xC00000FD`; the hostile lane
  run exercises the `rejected` and quarantine-detection paths.
- Promoting `fuzz-smoke` to required makes it run on every PR; a cache miss rebuilds from source,
  which is the pre-promotion nightly behaviour. Hosted runtime/AppContainer behaviour remains an
  external observation (recorded as a follow-up).

**Check results (Release x64).**
- `x64\Release\Tests.Unit.exe "~[graphics]"` — 131561 assertions / 314 cases, exit 0.
- `MSBuild tests\fuzz\ProviderFuzz.vcxproj /p:Configuration=Release /p:Platform=x64
  "/p:SolutionDir=<repo>\" /p:VcpkgRoot=C:\vcpkg /p:VcpkgManifestInstall=false` — exit 0.
- `tests\fuzz\x64\Release\ProviderFuzz.exe TestResults\security-t25\provider-seeds
  -max_total_time=30 -timeout=10 -rss_limit_mb=2048 -max_len=2100000 -print_final_stats=1
  -verbosity=0` — exit 0; 580 executions, 37 seeds, 338 MiB peak RSS, no crash/ASan report.
- `ProviderSmokeHost.exe --soak-classify-selftest` — exit 0; all six exit-code cases match.
- Soak classification run (7 valid families, 2 apartments x 20 iterations + hostile 2x20, staged and
  HKCU-registered): valid 40/40, hostile `rejected=40` (`first_hostile_failure_hr=0x8004B200`),
  `quarantined=0 allowed_faults=0 unexpected_faults=0`, growth within tolerance, teardown 5.3 s, no
  persistent surrogate, exit 0; elapsed 7.0 s, host peak working set 30.8 MB.
- `actionlint .github/workflows/ci.yml` — exit 0.

**Remaining work / blockers.** None for the task. Hosted verification that (a) a fork `pull_request`
runs `fuzz-smoke` from the files cache with no Packages token and (b) a maintainer adds the
`Fuzz smoke (<target>)` checks to branch protection is external; recorded under Follow-ups in
`docs/security/PROGRESS.md`.
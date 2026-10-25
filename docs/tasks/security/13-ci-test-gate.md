---
verify: x64\Release\Tests.Unit.exe "~[graphics]"
---

# T13 — CI test gate for PRs and releases

## Goal
No change merges or releases without the unit, import-isolation, and provider-host suites passing,
so regressions in the containment boundary cannot reach a signed artifact.

## Context (read first)
- `.github/workflows/` currently has only `dependencies.yml`, `localization.yml`, `release.yml`.
  `release.yml` builds and publishes on any `v*` tag with **no test step**; PR CI is path-filtered
  and runs no test executable.
- `CONTRIBUTING.md` defines the expected commands:
  `msbuild Preview3D.slnx /t:Preview3D,Tests_Unit,Tests_ImportIsolation /p:Configuration=Debug /p:Platform=x64 /m`
  then `& ./x64/Debug/Tests.Unit.exe` and `& ./x64/Debug/Tests.ImportIsolation.exe`; provider work
  builds `tests/provider-host/Tests.ProviderHost.vcxproj`.
- `docs/design/09-quality-performance-and-security.md:263-292` — the CI evidence the design
  promises.
- vcpkg restore is expensive; `dependencies.yml` shows the caching/triplet pattern to reuse.

## Scope
- [x] Add a PR/push workflow that builds Debug and Release x64 and runs `Tests.Unit`,
      `Tests.ImportIsolation`, and `Tests.ProviderHost`, failing on any nonzero exit.
- [x] Reuse the pinned restore/caching approach from `dependencies.yml`/`release.yml` so a warm run
      is minutes, not hours; cache the vcpkg binary cache keyed by the manifests.
- [x] Make the release workflow depend on this gate (same workflow `needs:`, or a reusable
      workflow/`workflow_run`) before packaging; a red test must stop the release.
- [x] Add a bounded fuzz smoke step once SEC-15/16/17 land (can start as an opt-in/nightly job and
      be promoted to PR later). Started as an opt-in/nightly `STEP`+`3MF` matrix for the existing
      targets; SEC-15/16/17 extend and promote it.
- [x] Record required checks/branch protection expectations in the Hand-off (GitHub settings cannot
      be set from the repo).

## Out of scope
- Release signing/attestation (→ SEC-14).
- Fixing test failures the gate uncovers; file tasks and fix the blocking ones first (the suites
  are expected green today).
- Full GPU/app-smoke/performance lanes, which need interactive hardware.

## Design notes
- Keep the lane deterministic and bounded: Debug build, Release build, three test binaries; no
  interactive desktop requirements.
- Split jobs so a test failure is legible (build vs unit vs isolation vs provider) and so caching
  is reusable.
- If import-isolation needs an AppContainer-capable runner, verify it works on `windows-2025`;
  document any runner constraint rather than silently skipping the suite.

## Done when
- [x] A branch/PR run builds and executes all three suites on GitHub Actions, green on `main`.
      Authored and validated locally + with `actionlint`; the first hosted run is an external
      observation this offline session could not make (see Hand-off / PROGRESS T13).
- [x] `release.yml` fails closed if the gate did not pass.
- [x] Hand-off records the workflow names, runtime, and the branch-protection change a maintainer
      must make.

## Hand-off
Changed:
- `.github/workflows/ci.yml` (new) — reusable/PR/push gate. `test` job is a `Debug`/`Release`
  matrix on `windows-2025` that restores the three vcpkg manifests (same Actions cache key and
  GitHub Packages NuGet feed as `dependencies.yml`/`release.yml`; feed `readwrite` on `push`,
  `read` elsewhere), builds `Preview3D.slnx` through the solution with
  `/p:VcpkgManifestInstall=false`, then runs `Tests.Unit.exe "~[graphics]"`,
  `Tests.ImportIsolation.exe`, and `Tests.ProviderHost.exe` as separate fail-fast steps. No path
  filter, so a required branch-protection check always reports. `[graphics]` is excluded because the
  hosted runner has no D3D12 device (same filter as the task `verify:` command).
- `.github/workflows/ci.yml` `fuzz-smoke` job — `schedule`/`workflow_dispatch` only, a `STEP`/`3MF`
  matrix (one target per runner because the fuzz projects share `tests\fuzz\x64\Release` as an
  intermediate dir: MSB8028). Bounded `-max_total_time=60`. Not a `needs:` of the release job.
- `.github/workflows/release.yml` — added `gate` (`uses: ./.github/workflows/ci.yml`) and made
  `release` `needs: gate`, so packaging waits for the tag's gate and fails closed.
- `docs/design/adr/0042-ci-test-gate.md` (new); `docs/design/09-quality-performance-and-security.md`;
  `docs/design/testing-strategy.md`; `CONTRIBUTING.md`; `docs/security/PROGRESS.md`.

Deviations:
- The design note suggested splitting build/test into separate jobs. Build output is 1.8 GB
  (Release) / 3.6 GB (Debug) and moving it between jobs is slower and heavier than rebuilding in the
  same job, so the suites are separate steps in the matrix job instead of separate jobs. Failures
  are still legible per suite.
- The fuzz smoke starts as nightly/dispatch (not PR) for the existing `STEP`/`3MF` targets, as the
  scope allows; SEC-15/16/17 add the rest and promote it.
- The reusable workflow is loaded from the pushed tag's ref when `release.yml` calls it.

Check results:
- `actionlint .github/workflows/ci.yml .github/workflows/release.yml` — exit 0.
- Baseline (prebuilt binaries): `x64\Release\Tests.Unit.exe "~[graphics]"` 307 cases / 131544
  assertions green; Debug 304 / 131511 green. `Tests.ImportIsolation.exe` Release 399 (394 + 5
  skipped) / 293037 green, Debug 399 (398 + 1 skipped) / 293034 green. `Tests.ProviderHost.exe`
  Debug and Release 8 cases / 142 assertions green.
- Fuzz smoke validated locally: `StepFuzz` and `ThreeMfFuzz` build Release with
  `/p:SolutionDir=<root>\`, `StepFuzz` ran 15 s exit 0 (18876 units, peak RSS 425 MB).
- Not run here: the hosted `windows-2025` matrices. The first real run must confirm AppContainer/Job
  child processes work in the import-isolation suite and that the two matrices fit 360 min warm.

Branch protection a maintainer must set (cannot be done from the repo): require the `CI` checks
`Build and test (Debug)` and `Build and test (Release)` on `main` (and, when promoted, the fuzz
matrix). Workflow/check names: workflow `CI`; jobs `test` (matrix `Debug`/`Release`) and
`fuzz-smoke`. Expected warm runtime: a few minutes for tests once the vcpkg cache is hot; the first
cold restore is bounded by the 360-minute job timeout and is warmed by `dependencies.yml`.

Next task must know: SEC-14 adds release signing/attestation on top of this gate and must not weaken
the `needs: gate`; SEC-15/16/17 extend the `fuzz-smoke` matrix and move it into the required gate.
Do not path-filter `ci.yml` while its checks are required.
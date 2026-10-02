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
- [ ] Add a PR/push workflow that builds Debug and Release x64 and runs `Tests.Unit`,
      `Tests.ImportIsolation`, and `Tests.ProviderHost`, failing on any nonzero exit.
- [ ] Reuse the pinned restore/caching approach from `dependencies.yml`/`release.yml` so a warm run
      is minutes, not hours; cache the vcpkg binary cache keyed by the manifests.
- [ ] Make the release workflow depend on this gate (same workflow `needs:`, or a reusable
      workflow/`workflow_run`) before packaging; a red test must stop the release.
- [ ] Add a bounded fuzz smoke step once SEC-15/16/17 land (can start as an opt-in/nightly job and
      be promoted to PR later).
- [ ] Record required checks/branch protection expectations in the Hand-off (GitHub settings cannot
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
- [ ] A branch/PR run builds and executes all three suites on GitHub Actions, green on `main`.
- [ ] `release.yml` fails closed if the gate did not pass.
- [ ] Hand-off records the workflow names, runtime, and the branch-protection change a maintainer
      must make.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
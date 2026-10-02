# 0042 — CI test gate for pull requests and releases

## Status
accepted

## Context
`docs/design/09-quality-performance-and-security.md` promises a per-change
Debug/Release build plus unit and headless regression evidence, but the repository
had no lane that ran a test executable: `dependencies.yml` and `localization.yml`
are path-filtered and test-free, and `release.yml` built and published on any
`v*` tag with no test step. A containment regression could therefore reach a
signed artifact. Restoring OpenUSD and OCCT from source is expensive, so a new
lane had to reuse the pinned vcpkg restore/cache pattern rather than invent one.

## Decision
- Add `.github/workflows/ci.yml`, running on every `pull_request` and push to
  `main` (not path-filtered, so a required branch-protection check always
  reports). It builds `Preview3D.slnx` in Debug and Release x64 through the
  solution and runs `Tests.Unit.exe "~[graphics]"`, `Tests.ImportIsolation.exe`
  and `Tests.ProviderHost.exe`, each as its own step and failing on any nonzero
  exit.
- Reuse the release/dependencies vcpkg restore: the same Actions files cache key,
  the same GitHub Packages NuGet binary feed (readwrite on `push`, read-only
  elsewhere so fork PRs degrade instead of failing), and all three manifests
  (root, STEP, provider STEP) restored before the build. The build passes
  `VcpkgManifestInstall=false`, matching `release.yml`.
- Gate the release with a reusable-workflow call: `release.yml` job `gate` uses
  `./.github/workflows/ci.yml`, and the `release` job declares `needs: gate`, so a
  red test fails closed before packaging.
- Keep a bounded libFuzzer smoke (`StepFuzz`, `ThreeMfFuzz`) as an
  opt-in/nightly job. It is deliberately not a `needs:` of the release job and
  runs only on `schedule`/`workflow_dispatch`; SEC-15/16/17 extend it and promote
  it into the required gate.
- Exclude `[graphics]` unit cases: they require a real D3D12 device the hosted
  runner does not have. This is the same filter as the harness verify command.

Rejected: a path-filtered PR lane (a skipped required check stays pending and
cannot satisfy branch protection); moving multi-GB `x64\<Config>` trees between
build and test jobs (artifact transfer is slower and larger than rebuilding in the
same job); a single-config lane (the design promises Debug *and* Release).

## Consequences
- A maintainer must set branch protection to require the `CI` checks
  (`Build and test (Debug)`, `Build and test (Release)`) on `main`; repository
  code cannot do this. GitHub settings are out of band per the task hand-off.
- Cold restores are still bounded by the 360-minute job limit; warm runs rely on
  the GitHub Packages feed and the Actions cache being populated by
  `dependencies.yml`/`release.yml`.
- The lane assumes `windows-2025` accepts AppContainer/Job child processes and
  has the `v145` toolset; this is the same image the release workflow already
  uses. If a suite proves environment-sensitive on the runner, fix or scope it
  explicitly — do not silently skip it.
- Fuzz output is not sanitizer-instrumented for OCCT/lib3mf/OpenUSD; the existing
  real AppContainer isolation suites remain the containment proof.
# 0093 - The import-isolation suite runs locally, not in CI

Status: accepted (2026-11-01). The "required before every release, patch
releases included" clause is superseded by
[ADR-0094](0094-release-testing-scoped-to-risk.md) (2026-11-02); the suite
stays local-only and remains required for import-affecting changes.

## Context

`Tests.ImportIsolation.exe` is the regression gate for everything between the
file and the renderer: every format adapter, the compatibility corpus, the
isolated CAD hosts, the hostile worker, shared-section validation, and the STEP
performance/qualification lanes. It is 516 cases and takes minutes even warm.

Running it on every pull request and push has two costs:

- It needs the whole fixture tree, including the optional local corpus under
  `test-models/` (git-ignored). A fixture that is present in a developer's
  working tree but missing from the repository passes locally and fails on a
  clean checkout. That is exactly what happened on 2026-11-01: seven Wavefront
  `.obj` fixtures were silently swallowed by the `*.obj` MSVC ignore rule, so
  `obj-textured-mtl` reported `FileUnavailable` and the OBJ line/point topology
  case failed only in CI.
- Most failures it produces are in pinned third-party parsers or in
  environment-specific corpora, not in mergeable product code, so it is a poor
  merge blocker even when it is healthy.

The required merge gate should answer "does this change compile and pass the
product-owned unit and provider contracts", quickly and deterministically.

## Decision

- Remove the `Run import-isolation suite (<configuration>)` step from
  `.github/workflows/ci.yml`. The projects still build in both configurations,
  so a compile break is caught; the suites are not executed.
- The suite is a **local, required** check: run `Tests.ImportIsolation.exe` in
  Debug and Release before every release, patch releases included, and after
  any change to an importer, the wire format, the broker, a fixture, or a
  third-party dependency. `docs/New_Release_Instructions.md` step 4 owns that
  requirement and the evidence goes in the release notes/PR.
- CI keeps Debug and Release x64 builds, `Tests.Unit.exe "~[graphics]"`, and
  `Tests.ProviderHost.exe`. `release.yml` still gates packaging on that
  workflow, so the tag build re-runs it against the tagged source.
- Fixtures are now tracked correctly: `.gitignore` negates the `*.obj` rule for
  `tests/fixtures/**` and `interactive-viewer/test-assets/**`.

## Consequences

- `main` being green no longer proves format-import regressions are absent. The
  release checklist is the gate for those; skipping step 4 is how an importer
  regression would ship.
- The corpus (`tests/compat/manifest.json`) and the adapters are exercised on
  the maintainer's machine, where the full `test-models/` corpus exists, instead
  of on a runner that only has the committed fixtures.
- No check name changes, so branch protection needs no edit for this decision
  (unlike ADR-0092, which leaves `Fuzz smoke (*)` reporting `skipped`).

# Contributing to Preview 3D

Thanks for taking the time to improve Preview 3D. This document covers how to
build, test, and submit changes. By participating you agree to the
[Code of Conduct](CODE_OF_CONDUCT.md).

## Ways to contribute

- **Report a bug or request a feature** using the [issue templates](https://github.com/binbuf/preview-3d/issues/new/choose).
- **Improve documentation**, including this file and the docs under [.docs](.docs).
- **Submit code** for fixes, performance, or supported-format coverage.
- **Test with real models.** Compatibility reports are valuable. Never attach a
  model you do not have the right to share, and remove confidential data first.

For security issues, follow [SECURITY.md](SECURITY.md) instead of opening a
public issue.

## Before you start

- Search existing issues and pull requests to avoid duplicate work.
- For anything beyond a small fix, open an issue first so the approach can be
  agreed on before you invest time.
- Keep changes focused. One logical change per pull request is much easier to
  review and validate.

## Development setup

The [README](README.md#build-from-source) has the full prerequisites and
workstation setup. In short: Windows 11 x64, Visual Studio with the Desktop
development with C++ workload (v145 MSVC toolset, C++20), a Windows 10/11 SDK,
and vcpkg with `vcpkg integrate install` run once. The first dependency restore
takes tens of minutes to a few hours.

```powershell
msbuild Preview3D.slnx /p:Configuration=Release /p:Platform=x64
```

## Repository layout

| Path | Contents |
| --- | --- |
| `interactive-viewer/` | The native Windows viewer: D3D12 renderer, UI, camera, caching |
| `import-worker/` | The sandboxed importer that parses model formats into validated chunks |
| `shared/` | Import broker, model core, and platform code shared by the executables |
| `compatibility-host/`, `compatibility-host-step/` | Isolated OpenUSD and OCCT hosts |
| `tests/` | Catch2 suites, fixture generators, app smoke lanes, fuzz targets, performance qualification |
| `packaging/` | Portable and installer packaging, vcpkg ports and triplets |

## Testing

Run the unit and import-isolation suites for every change that touches viewer or
importer behavior:

```powershell
msbuild Preview3D.slnx /t:Preview3D,Tests_Unit,Tests_ImportIsolation /p:Configuration=Debug /p:Platform=x64 /m
& ./x64/Debug/Tests.Unit.exe
& ./x64/Debug/Tests.ImportIsolation.exe
```

Then repeat in `Release` before opening a pull request. A solution target only
builds; each test executable returns nonzero on failure.

The `CI` workflow (`.github/workflows/ci.yml`) runs this gate on every pull
request and every push to `main`: it builds the solution in Debug and Release
x64, then runs `Tests.Unit.exe "~[graphics]"`, `Tests.ImportIsolation.exe`, and
`Tests.ProviderHost.exe`, failing on any nonzero exit. The `[graphics]` cases
are excluded because the hosted runner has no GPU; run them locally when a change
touches rendering. The `Release` workflow calls the same gate and will not
package until it passes. See `docs/design/adr/0042-ci-test-gate.md`.

- **Fixture and app-smoke lanes:** [tests/fixtures/README.md](tests/fixtures/README.md).
  App smoke needs an interactive desktop (not a locked, minimized, or occluded
  session) and Python 3.11+.
- **Fuzz targets:** [tests/fuzz/README.md](tests/fuzz/README.md). These are
  explicitly built sanitizer targets, not part of the shipping solution. A
  bounded smoke is part of the required `CI` gate as of SEC-17/T25: it runs on
  every pull request and push (and the `Release` workflow waits for it).
- **Performance qualification:** `tests/performance/qualify.py` with the options
  and limits in [.docs/TSK-302_VERIFICATION.md](.docs/TSK-302_VERIFICATION.md).

Full GPU suites, app smoke, and extended fuzzing still do not run as required
pull-request checks. State exactly which commands and configurations you ran in
your pull request, and note your GPU and Windows build for rendering or timing
changes.

## Localization

User-facing strings are written in place as English fallbacks,
`Loc("area.name", L"English text")` (see
`interactive-viewer/src/app/Localization.h`). To add or change a string, edit the
call site; to add a language, drop a `<code>.json` pack into
`interactive-viewer/lang` and run `msbuild` (the pack is copied beside the
executable). Regenerate the shipped packs and translation catalogue with:

```powershell
pwsh interactive-viewer/tools/generate-language-packs.ps1
```

English in the source is canonical; `interactive-viewer/lang/*.json` are derived
and should never be hand-edited except to improve a translation. The
`Localization` workflow enforces this by running the generator's check mode,
which fails on a key collision (two call sites disagreeing on English), a value
or key that drifted from the source, a missing/stale key, an empty translation,
or a translation that dropped a `{0}`-style placeholder:

```powershell
pwsh interactive-viewer/tools/generate-language-packs.ps1 -Verify
```

Machine translations are a starting point; hand-reviewed corrections to
`interactive-viewer/lang/*.json` are welcome and should be committed (run
`-Verify` afterward). `Tests.Unit`'s `[localization]` cases exercise the loader
against the real `interactive-viewer/lang` tree.

## Coding guidelines

- Match the style of the surrounding code. Keep changes small and focused.
- Preserve the trust boundaries: untrusted parsing stays in the AppContainer
  worker and compatibility hosts. Do not add remote asset fetching, arbitrary
  resolvers/plugins, or path handling that can escape the brokered sidecar
  policy.
- Keep resource use bounded. New parser paths need explicit limits for input
  size, expansion, counts, and allocations, and should fail closed.
- Avoid new dependencies unless they are clearly justified. Dependencies are
  pinned through `vcpkg.json`/`vcpkg-configuration.json`; do not fetch anything
  unpinned at build or release time.
- Do not commit generated output: `vcpkg_installed/`, `artifacts/`,
  `TestResults/`, `test-models/`, build directories, or logs. Large binaries are
  never added to Git.
- Update documentation when behavior or supported subsets change. Format claims
  belong in [.docs/FORMAT-SUPPORT.md](.docs/FORMAT-SUPPORT.md).

## Commits and pull requests

- Branch from `main` and keep the history clean.
- Write short, imperative commit subjects. Prefixes such as `ci:`, `docs:`, or
  `tests:` are welcome when they add clarity.
- In the pull request, describe the problem, the approach, and the test evidence.
  Reference the issue it closes.
- Expect review comments about containment, bounds, and recovery behavior for
  importer changes — these are the project's core guarantees.
- Contributions are accepted under the [Apache License 2.0](LICENSE). No
  separate contributor license agreement is required.

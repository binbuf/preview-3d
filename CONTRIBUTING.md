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

- **Fixture and app-smoke lanes:** [tests/fixtures/README.md](tests/fixtures/README.md).
  App smoke needs an interactive desktop (not a locked, minimized, or occluded
  session) and Python 3.11+.
- **Fuzz targets:** [tests/fuzz/README.md](tests/fuzz/README.md). These are
  explicitly built sanitizer targets, not part of the shipping solution.
- **Performance qualification:** `tests/performance/qualify.py` with the options
  and limits in [.docs/TSK-302_VERIFICATION.md](.docs/TSK-302_VERIFICATION.md).

Full GPU suites, app smoke, and fuzzing are not run by GitHub Actions; the
release workflow only builds and packages. State exactly which commands and
configurations you ran in your pull request, and note your GPU and Windows build
for rendering or timing changes.

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

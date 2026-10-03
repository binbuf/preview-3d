# 0049 — SBOM and license metadata generated from the installed vcpkg closure

## Status
accepted

## Context
SEC-19/T19 found that the shipped CycloneDX SBOM and the `licenses/` copy list
were driven by a hardcoded component array in
`packaging/portable/Create-PortableRelease.ps1`. Adding a root dependency only
worked if a maintainer also edited that array, and a missed entry did not fail
the build. `THIRD-PARTY-LICENSES.md` had no versions and omitted `fast-float`,
a transitive of the TinyUSDZ overlay port; `vcpkg.json` still declared `0.1.0`
while the release version was `0.5.0`; and the OpenUSD host suppressed
`4244;4305` project-wide rather than around the upstream headers that emit them.

The repository already restores exactly the manifest closure plus transitives
into `vcpkg_installed/**/vcpkg/status` (and each isolated STEP manifest), and
vcpkg installs each port's `copyright` file under its `share/<pkg>` directory.
That status file is the authoritative inventory; the manual list was the defect.

## Decision
- Add `packaging/ReleaseMetadata.ps1`, the single source of truth for: parsing
  an installed vcpkg tree into a target-architecture dependency closure,
  importing the reviewed SPDX mapping, and checking release-version consistency.
- Add `packaging/portable/dependency-licenses.json`, an SPDX expression per
  package (reviewed when a dependency is added). Packaging **fails closed** if
  an installed package has no mapping or no upstream `copyright` file, then
  copies the license from the matching tree and emits one CycloneDX component
  per package with the SPDX `expression` plus baseline/ABI/license-file
  properties. The `THIRD-PARTY-NOTICES.txt` index is generated from the same
  closure.
- Make `vcpkg.json`, `Directory.Solution.targets`, `Preview3D.nsi`, and the
  packaging `-Version` argument agree; both packaging scripts and
  `packaging/Test-ReleaseMetadata.ps1` refuse to run on drift.
- Keep `THIRD-PARTY-LICENSES.md` as a checked navigation aid: it must name every
  mapped package and link the pinned vcpkg baseline.
- Scope the OpenUSD `4244;4305` suppression to `OpenUsdHost.cpp` (the sole
  consumer of the `pxr` headers) with `push`/`disable`/`pop`; the project-wide
  suppression is removed. An audit of every emitted site found only upstream
  `pxr/base/gf/*` and `pxr/base/arch/timing.h` code, never project code.
- Add `tests/unit/ReleaseMetadataTests.cpp`, which runs the gate with
  `-SelfTest` so the negative cases (an unmapped installed dependency and a
  version mismatch) must fail. This is the build-failing packaging self-test.

## Consequences
- A newly added port can no longer ship without a reviewed license: the build
  stops until `dependency-licenses.json` is updated. License selection is still
  a human judgement, but it is now fail-closed and reviewable in one file.
- The SBOM is deterministic per installed closure (only the serial number and
  timestamps vary), and the OpenUSD host keeps `/W4` as errors for its own code.
- `THIRD-PARTY-LICENSES.md` versions are informational; the release-package
  SBOM/notices/`licenses/` remain authoritative for a given build.
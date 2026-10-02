---
verify: x64\Release\Tests.Unit.exe "~[graphics]"
---

# T19 — License/SBOM/dependency metadata

## Goal
The shipped SBOM and license notices are generated from the actual dependency closure and fail the
build when a dependency is missing, so the release cannot silently omit a license or ship an
unversioned, drifting notice file.

## Context (read first)
- `packaging/portable/Create-PortableRelease.ps1:277-289,422-507` — license copying and the
  CycloneDX SBOM iterate a hardcoded component array; a newly added root dependency is only covered
  if a maintainer also edits the array, and the build does not fail when it is omitted.
- `THIRD-PARTY-LICENSES.md:16-33` — hand-maintained table with no versions; omits `fast-float`, a
  transitive dependency of the TinyUSDZ overlay port (`packaging/vcpkg-ports/tinyusdz/vcpkg.json:10`).
- `vcpkg.json:4` still declares `0.1.0` while the product/release version is `0.5.0`
  (`Directory.Solution.targets:4`, `packaging/installer/Preview3D.nsi:10`).
- `compatibility-host/Preview3DOpenUsdCore.vcxproj:26` suppresses `4244;4305` project-wide rather
  than around third-party headers; `import-worker/src/GltfAdapter.cpp:24-27` shows the correct
  scoped pattern.
- `docs/design/08-installation-and-registration.md:225-226` — the lock/SBOM/checksum obligation.

## Scope
- [ ] Generate the SBOM and license-copy list from the installed vcpkg status
      (`vcpkg_installed/**/vcpkg/status` and each isolated tree) rather than a hardcoded array;
      fail packaging when an installed package has no license mapping.
- [ ] Add versions to `THIRD-PARTY-LICENSES.md` (or generate it), including `fast-float`, and link
      the baseline commit in `vcpkg-configuration.json`.
- [ ] Bump `vcpkg.json`'s version to the current release version, and add a check that it matches
      `Directory.Solution.targets`/the installer version so they cannot drift.
- [ ] Scope the OpenUSD project-wide `4244;4305` suppression to the third-party includes using the
      `push`/`disable`/`pop` pattern; re-audit each suppressed site rather than hiding conversions.
- [ ] Add a packaging self-test that a deliberately missing dependency entry fails the build (or
      assert the generated list equals the installed set in CI).

## Out of scope
- Signing/attestation (→ SEC-14).
- Dependency upgrades.

## Design notes
- Prefer generation over manual lists everywhere; a manual list that cannot fail is a defect.
- Keep the SBOM deterministic where the existing format already includes baseline/signing status;
  only the serial number should vary.
- The license mapping table (SPDX id per package) can live next to the packaging script; it must be
  reviewed when a dependency is added.

## Done when
- [ ] A Release package run generates SBOM/licenses from the installed closure, and the generated
      set matches `vcpkg.json` plus transitives.
- [ ] The version-drift check passes and fails on a deliberate mismatch.
- [ ] Hand-off lists the generated artifacts and the mapping file location.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
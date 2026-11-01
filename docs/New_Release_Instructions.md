# New Release Instructions

This is the maintainer checklist for cutting a Preview 3D release. It covers
everything that happens **before** the version number is incremented, the version
files themselves, merging to `main`, tagging, and what the tag triggers.

The short version:

1. Get `main` green and frozen.
2. Build the `CHANGELOG.md` entry from the commits since the last tag.
3. Bump every version source (there are more than one).
4. Run the release-metadata and build/test gates locally.
5. Commit as `chore: release vX.Y.Z`.
6. Merge to `main`, confirm the required checks pass.
7. Push the `vX.Y.Z` annotated tag.
8. Approve the `release` environment deployment and verify the published assets.

Skipping step 2 or 3 is the usual cause of a bad release: the `version.txt`
asset is the update feed, and the packaging scripts fail closed on version
drift, so a missing bump stops the tag build rather than shipping a wrong
version.

---

## 0. Choose the version

Preview 3D follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Tags are `vMAJOR.MINOR.PATCH`, optionally with a SemVer prerelease/build suffix
(for example `v0.7.0-rc.1`). The tag regex in
`.github/workflows/release.yml` is the authority:

```text
^v(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$
```

A tag containing `-` is published as a GitHub prerelease. A normal release
becomes the repository's "latest" release, which is what the in-app update
check reads (`releases/latest/download/version.txt`).

Decide the next version before doing anything else, because the changelog
heading, every version file, the commit subject, and the tag must all agree.

---

## 1. Get `main` into a releasable state

- [ ] All intended work is merged. A release is cut from `main`, not from a
      feature branch.
- [ ] Local `main` is up to date and matches `origin/main`.
- [ ] The full `CI` workflow is green on the `main` commit you are about to
      release. It builds Debug and Release x64 and runs
      `Tests.Unit.exe "~[graphics]"`, `Tests.ImportIsolation.exe`, and
      `Tests.ProviderHost.exe`, plus the bounded fuzz-smoke matrix.
- [ ] The `Localization` workflow is green if any user-facing string changed.
- [ ] The `Dependencies` workflow is green if `vcpkg.json`,
      `vcpkg-configuration.json`, or the overlay ports/triplets changed.
- [ ] No release is already in progress
      (`.github/workflows/release.yml` uses `cancel-in-progress: false`).

The tag build deliberately re-runs only the Release configuration and skips the
fuzz matrix, on the assumption that the tagged commit already carried the full
matrix on `main`. Do not tag a commit whose full `main` checks were not green.

---

## 2. Build the `CHANGELOG.md` entry

Entries are newest first, grouped by release tag, and follow
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) style. Review
everything that landed since the previous tag, not just the most recent commit.

```powershell
# The previous release tag
git describe --tags --abbrev=0

# Everything since it (merge commits included; read the PRs for context)
git log --oneline (git describe --tags --abbrev=0)..HEAD

# Full diffs/stat if a commit subject is not enough
git log --stat (git describe --tags --abbrev=0)..HEAD
```

Write the entry as:

```markdown
## [X.Y.Z] - YYYY-MM-DD

### Added

- **Short user-facing headline.** What changed, why it matters, and any
  migration or manual action the user must take.

### Changed

- ...

### Fixed

- ...

### Security

- ...
```

Only include the categories that apply. Match the existing voice: lead with a
bold user-facing sentence, then the detail. Call out anything a user must do
manually (for example the 0.6.7 note that affected copies had to be installed
once by hand), deprecations, and removed behavior. Internal-only refactors do
not need an entry unless they change behavior.

GitHub's release page also gets auto-generated notes from the tag's commits
(`--generate-notes`), but that is not a substitute for the curated changelog.

---

## 3. Bump every version source

Use the previous release commit as the template:

```powershell
git show --stat v0.6.7
```

The release commit touches these files. All of them must carry the new version:

| File | What to change |
| --- | --- |
| `vcpkg.json` | `"version": "X.Y.Z"` |
| `Directory.Solution.targets` | `<ReleaseVersion ...>X.Y.Z</ReleaseVersion>` |
| `interactive-viewer/src/app/Version.h` | `#define PREVIEW3D_VERSION L"X.Y.Z"` — the version compiled into the app and compared with the update feed |
| `version.txt` | `X.Y.Z` on one line — published as the release's `version.txt`, i.e. the update feed |
| `packaging/installer/Preview3D.nsi` | `PRODUCT_VERSION "X.Y.Z"` **and** `PRODUCT_FILE_VERSION "X.Y.Z.0"` |
| `packaging/CreatePortableRelease.proj` | `ReleaseVersion` default |
| `packaging/CreateInstaller.proj` | `ReleaseVersion` default |
| `packaging/portable/Create-PortableRelease.ps1` | `-Version` default |
| `packaging/installer/Create-Installer.ps1` | `-Version` default |
| `CHANGELOG.md` | New `## [X.Y.Z] - YYYY-MM-DD` section |

The packaging scripts call `Assert-ReleaseVersionConsistency` in
`packaging/ReleaseMetadata.ps1`, which fails if `vcpkg.json`,
`Directory.Solution.targets`, `Preview3D.nsi`, `version.txt`,
`Version.h`, or the `-Version` passed by the workflow disagree. The four
packaging defaults and `PRODUCT_FILE_VERSION` are not part of that specific
gate, so update them too — the release commit has always included them, and a
mismatch only shows up when someone runs a packaging target directly.

Useful sanity checks:

```powershell
# See all remaining occurrences of the old version (CHANGELOG history excepted)
Select-String -Path `
  vcpkg.json, Directory.Solution.targets, version.txt, `
  interactive-viewer/src/app/Version.h, packaging/installer/Preview3D.nsi, `
  packaging/CreatePortableRelease.proj, packaging/CreateInstaller.proj, `
  packaging/portable/Create-PortableRelease.ps1, `
  packaging/installer/Create-Installer.ps1 `
  -Pattern 'OLD\.VERSION'

# The version gate plus the license/SBOM gates and their negative self-tests
pwsh -File packaging/Test-ReleaseMetadata.ps1 -SelfTest
```

Also update `THIRD-PARTY-LICENSES.md` and
`packaging/portable/dependency-licenses.json` if the dependency closure
changed; `Test-ReleaseMetadata.ps1` fails if a mapped package is missing its
licenses entry, has no SPDX mapping, or lacks an installed copyright file.

---

## 4. Run the local verification gates

At minimum, run what CI runs before tagging:

```powershell
# Debug
msbuild Preview3D.slnx /p:Configuration=Debug /p:Platform=x64
& ./x64/Debug/Tests.Unit.exe "~[graphics]"
& ./x64/Debug/Tests.ImportIsolation.exe
& ./x64/Debug/Tests.ProviderHost.exe

# Release
msbuild Preview3D.slnx /p:Configuration=Release /p:Platform=x64
& ./x64/Release/Tests.Unit.exe "~[graphics]"
& ./x64/Release/Tests.ImportIsolation.exe
& ./x64/Release/Tests.ProviderHost.exe
```

`[graphics]` is excluded on headless CI. Run those cases locally when the
release touches rendering, and say so in the release PR/notes if GPU coverage
was manual.

Recommended: produce both packages locally once before the tag, so packaging
problems are found before CI:

```powershell
msbuild Preview3D.slnx /t:CreatePortableRelease /p:Configuration=Release /p:Platform=x64
msbuild Preview3D.slnx /t:CreateInstaller /p:Configuration=Release /p:Platform=x64
```

The installer target needs NSIS 3. Output goes to `artifacts/portable` and
`artifacts/installer`; never commit those directories.

If localization strings changed, also verify the generated packs:

```powershell
pwsh interactive-viewer/tools/generate-language-packs.ps1 -Verify
```

---

## 5. Update the human documentation

Go through the docs that describe shipped behavior:

- [ ] `README.md` — install steps, supported formats, signing/attestation
      wording, or anything else user-visible.
- [ ] `docs/FORMAT-SUPPORT.md` — if a format or supported subset changed.
- [ ] `docs/WINDOWS-SECURITY.md` and/or `SECURITY.md` — if the signing or
      security posture changed (see ADR-0055 for the signed/unsigned rules).
- [ ] `docs/design/` — ADRs for architectural decisions, and the design pages
      whose contracts changed.
- [ ] `docs/ROADMAP.md`, `docs/PROGRESS.md`, `docs/tasks/...` — milestone and
      task status.
- [ ] `THIRD-PARTY-LICENSES.md` — dependency/license changes (see step 3).

Do not hand-edit the generated language packs; edit the call sites and run the
generator.

---

## 6. Commit the release preparation

Keep the release commit focused on the version bump and changelog, matching
history:

```powershell
git add CHANGELOG.md Directory.Solution.targets `
  interactive-viewer/src/app/Version.h `
  packaging/CreateInstaller.proj packaging/CreatePortableRelease.proj `
  packaging/installer/Create-Installer.ps1 packaging/installer/Preview3D.nsi `
  packaging/portable/Create-PortableRelease.ps1 `
  vcpkg.json version.txt

git commit -m "chore: release vX.Y.Z"
```

If documentation changes are part of this release, include them in the same
commit or in a separate, reviewed commit before the tag. Do not include build
output, `artifacts/`, `x64/`, `TestResults/`, or logs.

---

## 7. Merge into `main`

Do the merge through the normal protected-branch path:

```powershell
git switch main
git pull --ff-only
git merge --no-ff release/vX.Y.Z   # or merge the release PR in GitHub
git push origin main
```

- [ ] The merge commit is on `main` and `origin/main` points at it.
- [ ] The required `main` checks (full `CI`, `Localization` if applicable,
      `Dependencies` if applicable) are green **on that commit**.
- [ ] `git log --oneline --decorate -5` shows the release commit on `main`.

If the repository uses a release PR instead of a local merge, wait for the
required checks and merge normally; do not push directly around branch
protection.

---

## 8. Tag and push

The tag **must** point at the commit that contains the bumped version files.
Packaging passes the tag's version as `-Version`, and the consistency gate will
fail if the tree still says the old version.

```powershell
git switch main
git pull --ff-only

# Annotated tags are used for releases; match the existing message style.
git tag -a vX.Y.Z -m "Preview 3D X.Y.Z"

# Confirm the tag peels to the intended commit
git rev-parse vX.Y.Z^{}
git rev-parse HEAD
git show --stat vX.Y.Z

git push origin main
git push origin vX.Y.Z
```

Push `main` first, then the tag. Never move or delete a tag that has already
produced a published release; release artifacts are immutable by policy.

---

## 9. What the tag triggers

Pushing `v*` starts `.github/workflows/release.yml`:

1. **`gate`** calls `ci.yml` as a reusable workflow with `Release` only and
   `fuzz: false`. It builds the tagged source in Release x64 and runs the unit,
   import-isolation, and provider-host suites. This is fail-closed: no artifact
   is produced if it fails.
2. **`release`** waits for `gate`, then waits for approval from the `release`
   GitHub environment (required reviewers; deployment branches are tags). The
   tag push alone cannot publish.
3. After approval it restores the vcpkg binary cache, validates the tag,
   builds the portable ZIP and the NSIS installer, generates SBOMs/manifests
   and `SHA256SUMS`, publishes `version.txt`, and:
   - signs the binaries and checksums if `WINDOWS_CERTIFICATE_BASE64` and
     `WINDOWS_CERTIFICATE_PASSWORD` are configured (otherwise it warns and
     publishes unsigned, still checksummed and attested — ADR-0055);
   - creates GitHub build-provenance attestations;
   - creates a draft GitHub release, uploads every asset, then publishes it.
4. The job refuses to overwrite an already-published release. If a previous
   attempt left a **draft**, it deletes and recreates that draft.

Expected release assets:

- `Preview3D-X.Y.Z-portable-x64.zip` and `.sha256`
- `Preview3D-X.Y.Z-x64-setup.exe` and `.sha256`
- `Preview3D-X.Y.Z-portable-x64.sbom.cdx.json`
- `Preview3D-X.Y.Z-portable-x64.manifest.json`
- `Preview3D-X.Y.Z-x64-setup.sbom.cdx.json`
- `Preview3D-X.Y.Z-x64-setup.manifest.json`
- `Preview3D-X.Y.Z-SHA256SUMS`
- `version.txt`
- `.p7` detached signatures if a certificate is configured

---

## 10. Verify the published release

- [ ] The `gate` job passed and the `release` environment deployment was
      approved.
- [ ] The `Release` workflow completed; no step needed a re-run.
- [ ] The releases page shows `vX.Y.Z` with the expected title
      (`Preview 3D X.Y.Z`), assets, and prerelease flag.
- [ ] `version.txt` on the release says `X.Y.Z`.
- [ ] Checksums verify, and attestation verifies:
      `gh attestation verify <file> --repo binbuf/preview-3d`.
- [ ] Smoke-test both downloads on a clean Windows 11 x64 machine/VM:
      install the installer and run the portable ZIP; **Help > About** reports
      `X.Y.Z`.
- [ ] The update badge does not appear when already on `X.Y.Z`, and the update
      check resolves the release's `version.txt`.
- [ ] For a normal release, GitHub's "latest" release is the new tag (the
      in-app update check depends on this). A prerelease is not "latest".
- [ ] Close the milestone/task docs and update the roadmap if the release was a
      milestone.

---

## 11. If something goes wrong

- **Gate failed:** fix the source on `main` and cut a new patch version. Do not
  move the tag to a new commit; the tag must continue to identify the source
  that was tested.
- **Packaging failed before publishing:** a stale draft can be recreated by a
  workflow re-run. If the tag itself is wrong (wrong commit or wrong version)
  and nothing has been published, delete the draft and the remote tag, fix the
  commit, and retag deliberately.
- **Release already published:** do not overwrite or `--clobber` it. The
  workflow explicitly refuses this. Fix forward with a new version.
- **Bad binaries after publishing:** publish a corrective patch release. Do not
  silently replace assets on a published release.

---

## Quick reference

```powershell
# 1. Review what changed
git describe --tags --abbrev=0
git log --oneline (git describe --tags --abbrev=0)..HEAD

# 2. Edit CHANGELOG.md and all version files

# 3. Verify
pwsh -File packaging/Test-ReleaseMetadata.ps1 -SelfTest
pwsh interactive-viewer/tools/generate-language-packs.ps1 -Verify
msbuild Preview3D.slnx /p:Configuration=Release /p:Platform=x64
& ./x64/Release/Tests.Unit.exe "~[graphics]"
& ./x64/Release/Tests.ImportIsolation.exe
& ./x64/Release/Tests.ProviderHost.exe

# 4. Commit, merge, tag
git commit -m "chore: release vX.Y.Z"
git switch main
git pull --ff-only
git merge --no-ff release/vX.Y.Z
git push origin main
git tag -a vX.Y.Z -m "Preview 3D X.Y.Z"
git push origin vX.Y.Z
```

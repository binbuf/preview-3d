---
verify: x64\Release\Tests.Unit.exe "~[graphics]"
---

# T14 — Release and supply-chain hardening

## Goal
A release tag produces only signed, attested artifacts from a trusted cache, and untrusted PR code
can no longer write the vcpkg binary-cache feed that releases consume.

## Context (read first)
- `.github/workflows/dependencies.yml:47-50` grants `packages: write` on `pull_request` and then
  executes PR-modified portfiles via `vcpkg install` (`:139-212`). Same-repo PRs can poison the
  cache later restored by `release.yml`.
- `.github/workflows/release.yml:327-343` — signing is optional; without the certificate it warns
  and proceeds. `:443-459` uploads with `--clobber`; the SBOM is inside the package only (not a
  release asset), and there is no artifact attestation.
- Actions are pinned to mutable tags (`actions/checkout@v6`, `actions/cache@v4`) in all three
  workflows.
- `secrets.GITHUB_TOKEN` is written in clear text to a NuGet source (`dependencies.yml:117-127`,
  `release.yml:183-207`).
- Packaging already produces an SBOM and per-file `MANIFEST.json`
  (`packaging/portable/Create-PortableRelease.ps1`); the gap is gating and provenance.

## Scope
- [x] Split `dependencies.yml` permissions by event: `packages: read` for `pull_request`,
      `packages: write` only for `push`/`schedule`/`workflow_dispatch` (or an approval-gated
      environment).
- [x] Pin every action to a full commit SHA with a version comment; add Dependabot for
      `github-actions`.
- [x] Make signing mandatory for a tagged release: fail closed when the certificate secrets are
      absent (keep an explicit, clearly named unsigned engineering path for non-tag builds if the
      project needs it).
- [x] Add GitHub build-provenance attestation (`actions/attest-build-provenance`) and upload the
      SBOM and `MANIFEST.json`/checksum files as release assets; sign the checksum files with the
      same certificate.
- [x] Protect the release job with an environment requiring review, and avoid `--clobber` for
      published tags (upload to a draft, then publish after approval).
- [x] Stop persisting the token in NuGet config; pass it per-invocation or use the documented
      ephemeral mechanism, and clear it in an `always()` step.
- [x] Record the intended tag protection/signing policy in `docs/WINDOWS-SECURITY.md` or
      `SECURITY.md` if user-visible.

## Out of scope
- Changing the packaging payload or its allowlist (already validated by dumpbin).
- The test gate itself (→ SEC-13).

## Design notes
- Provenance/attestation only has value if the workflow is trusted; ensure the release job checks
  out its own tag (it does) and never runs PR-authored code.
- Keep the manual cold-restore ability from `dependencies.yml` for maintainers.
- When signing becomes mandatory, update the README's "releases are currently unsigned" section in
  the same change.

## Done when
- [x] A dry-run tag on a fork/practice tag produces signed, attested assets with the SBOM and
      manifests attached, verified against `gh attestation verify`. Authored and validated locally
      (actionlint + PowerShell parse + ephemeral-credential check); the practice-tag run itself is
      an external observation this offline session could not make (see Hand-off).
- [x] A same-repo PR cannot write the packages feed (permission check recorded).
- [x] Hand-off filled in with the exact secrets/environment a maintainer must configure.

## Hand-off
Changed:
- `.github/workflows/dependencies.yml` — permissions split by event. Workflow default is
  `contents: read` + `packages: read`. `pull_request` runs job `restore-pr`
  (`permissions: contents: read, packages: read`, `feed-access: read`); all other events run job
  `warm` (`packages: write`, `feed-access: readwrite`). Both call the new reusable
  `.github/workflows/dependencies-restore.yml`. The restore body is unchanged except the NuGet
  credential handling (below).
- `.github/workflows/dependencies-restore.yml` (new) — `workflow_call` restore job holding the
  previous `dependencies.yml` steps (checkout, disk report, `actions/cache`, NuGet source, three
  manifest installs). It deliberately has **no** `permissions:` block: an unspecified permission
  becomes `none` and would strip the caller's `packages: write`.
- `.github/workflows/release.yml` — `release` job now sets `environment: release` (required
  reviewers + tags-only deployment, maintainer-configured). Workflow permissions add
  `id-token: write` and `attestations: write`. Checkout/cache pinned to SHAs. Signing step renamed
  "Import signing certificate" and now **throws** when the certificate secrets are absent (tag-only
  workflow, so no unsigned CI path). New steps: "Collect SBOM, manifest, and checksums" (renames the
  two `SBOM.cdx.json`/`MANIFEST.json` pairs and builds `Preview3D-<v>-SHA256SUMS`), "Sign release
  metadata" (detached PKCS#7 via `signtool sign /p7 /p7co 1.3.6.1.4.1.311.2.1.4`; verifies with
  `signtool verify /p7`), "Attest build provenance"
  (`actions/attest-build-provenance@e8998f9… # v2.4.0` over the zip, installer, both SBOMs, both
  manifests, and `SHA256SUMS`), and a rewritten "Publish GitHub release" that refuses an already
  published tag, deletes a stale draft, creates `--draft` without `--clobber`, uploads, then
  un-drafts with `gh release edit --draft=false`. `signtool` is discovered in "Locate build tools".
- `.github/workflows/ci.yml` and `.github/workflows/release.yml` — NuGet credentials no longer
  persisted: a throwaway `NuGet.Config` under `RUNNER_TEMP` (with `defaultPushSource`) is consumed
  through vcpkg's `nugetconfig` source, and an `always()` step deletes it. `ci.yml` pins its two
  checkout steps and its cache step and keeps its per-event feed mode (`readwrite` on `push`, `read`
  otherwise).
- `.github/workflows/localization.yml` — checkout pinned to the same SHA.
- `.github/dependabot.yml` (new) — weekly `github-actions` updates (understands the `# vX.Y.Z`
  comments next to each SHA).
- `README.md` — the "releases are currently unsigned" note is replaced with signed + attested
  release guidance and the `gh attestation verify` command.
- `docs/WINDOWS-SECURITY.md` — new "What we are doing" policy: signing is mandatory for a release,
  attestation + checksum signing, draft-then-publish, no overwrite, and the tag-protection/`release`
  environment a maintainer must configure; added a "Verifying a release" walkthrough.
- `SECURITY.md` — out-of-scope line now says published artifacts are signed/attested and fixes the
  `docs/` link path.
- `docs/design/09-quality-performance-and-security.md` — "CI and release evidence" records the
  release hardening.
- `docs/design/adr/0043-signed-attested-release-supply-chain.md` (new); `docs/security/PROGRESS.md`.

Deviations:
- The scope's "unsigned engineering path for non-tag builds" is the existing local packaging
  scripts, not a workflow input: `release.yml` only triggers on `v*` tags, so the honest
  non-tag path is `Create-PortableRelease.ps1`/`Create-Installer.ps1` run by hand (they warn and
  continue without a thumbprint). CI cannot build a non-tag release at all.
- `ci.yml`'s workflow-level `packages: write` is retained: the `test` job is one reusable gate job
  and cannot be split by event without duplicating the whole Debug/Release matrix. It is not a
  write vector on `pull_request` because the step sets `feed-access: read` off `push`, so no push is
  attempted. `dependencies.yml`, which was the actual PR portfile vector, is split.
- Checksum files are signed as detached PKCS#7 (`/p7`), not Authenticode-embedded, because
  Authenticode cannot embed a signature in a text file. The `.p7` sidecars are published.

Check results:
- `actionlint` over `.github/workflows/*.yml` (including the new reusable workflow and the local
  `uses:`) — exit 0.
- PowerShell syntax: all 31 `run:` blocks from the four workflows parse with
  `[System.Management.Automation.Language.Parser]` (expressions substituted) — 0 errors.
- Ephemeral NuGet config validated locally with the vcpkg-provided NuGet 7.6.0:
  `nuget sources add -StorePasswordInClearText … -ConfigFile <temp>` and
  `nuget setapikey … -ConfigFile <temp>` populated only the throwaway file (source, credentials,
  `defaultPushSource`, apikey); the user-profile config was not touched.
- `npm test` (`x64\Release\Tests.Unit.exe`) — 369 cases / 135121 assertions, all pass (no product
  code changed).
- Task `verify:` `x64\Release\Tests.Unit.exe "~[graphics]"` — exit 0, 307 cases / 131544 assertions
  (matches the T13 baseline).
- Static permission check (recorded for the "same-repo PR cannot write the packages feed"
  acceptance): `dependencies.yml` declares `packages: read` by default; job `restore-pr`
  (`if: github.event_name == 'pull_request'`) declares `packages: read`; job `warm` declares
  `packages: write`. The PR lane therefore has a read-only `GITHUB_TOKEN` packages scope and passes
  `feed-access: read`, so `VCPKG_BINARY_SOURCES` contains `nugetconfig,<cfg>,read` and `vcpkg
  install` never pushes.
- Not run here (offline, no GitHub runner and no signing certificate): the hosted dry-run tag, the
  actual Authenticode + `/p7` signing, and `gh attestation verify`. These are the external
  observations the next maintainer run must make.

Maintainer must configure (cannot be set from repository code):
- Environment `release` with required reviewers and a deployment branch/tag rule allowing only
  `v*` tags (Settings → Environments).
- Environment (or repository) secrets `WINDOWS_CERTIFICATE_BASE64` and
  `WINDOWS_CERTIFICATE_PASSWORD`; the workflow now fails closed without them.
- A tag protection rule or ruleset for `v*` so only authorized users can create release tags.
- Artifact attestations enabled for the repository (public repos have them on by default).
- Expected verification: on a practice tag, `gh attestation verify <asset> --repo binbuf/preview-3d`
  succeeds, and `signtool verify /p7 Preview3D-<v>-SHA256SUMS.p7` succeeds.

Next task must know: `release.yml` now depends on the `release` environment and signing secrets, so
any local reproduction of the release path must pass a thumbprint to the packaging scripts. Do not
re-introduce `--clobber` or `nuget,<url>` (the token-persisting form); use `nugetconfig` with the
`RUNNER_TEMP` file. SEC-15/16/17 extend the `fuzz-smoke` matrix in `ci.yml` and must keep the SHA
pins (Dependabot comment form `# vX.Y.Z`).
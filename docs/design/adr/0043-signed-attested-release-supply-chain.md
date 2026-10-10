# 0043 — Signed, attested releases from an unpoisonable binary cache

## Status
accepted

## Context
The release lane built and published on a `v*` tag, but the supply chain around it
was weak. `dependencies.yml` declared `packages: write` for every event, so a
same-repo pull request could execute PR-modified overlay portfiles through
`vcpkg install` and push a poisoned binary package to the GitHub Packages feed
that `release.yml` later restored. Actions were pinned to mutable tags.
Signing was optional: with no certificate the workflow warned and published an
unsigned installer. The SBOM and `MANIFEST.json` existed but only inside the
archive, with no release asset and no build-provenance attestation. The NuGet
token was written in clear text to the user-profile `NuGet.Config`. The release
job had no environment gate and `gh release upload --clobber` overwrote
published assets.

## Decision
- Split `dependencies.yml` by event. The shared restore moves to
  `dependencies-restore.yml` (`workflow_call`); a `pull_request` runs it as
  `restore-pr` with `packages: read` and `feed-access: read`, while
  `push`/`schedule`/`workflow_dispatch` run `warm` with `packages: write` and
  `feed-access: readwrite`.
- Pin every action to a full commit SHA with a `# vX.Y.Z` comment and add
  `.github/dependabot.yml` for `github-actions`.
- Make signing mandatory for a tag: the release job fails closed when
  `WINDOWS_CERTIFICATE_BASE64`/`WINDOWS_CERTIFICATE_PASSWORD` are absent. The
  unsigned path stays local in the packaging scripts (they warn and continue
  without a thumbprint) and is never published by CI.
- Gate `release` behind the required-reviewer `release` environment, publish to a
  draft, and only then un-draft. A published tag is refused instead of
  `--clobber`ed; a leftover draft is deleted and recreated.
- Attest the zip, installer, SBOMs, manifests, and `SHA256SUMS` with
  `actions/attest-build-provenance`; upload the renamed SBOM/manifest copies and
  checksums as release assets.
- Sign the checksum files with a detached PKCS#7 (SPC indirect data) signature
  (`signtool sign /p7 /p7co 1.3.6.1.4.1.311.2.1.4`), since Authenticode cannot
  embed a signature in a text file; the `.p7` sidecars are published too.
- Write the NuGet token to a throwaway `NuGet.Config` under `RUNNER_TEMP` and
  consume it through vcpkg's `nugetconfig` source, then delete the file in an
  `always()` step. This is applied to `dependencies-restore.yml`, `ci.yml`, and
  `release.yml`.

Rejected: keeping `packages: write` on PRs and relying only on the feed mode
(the permission would still be a live write token); a path-filtered or
environment-only gate without the permission split; embedding the checksum in a
catalog instead of a detached PKCS#7 (extra store/distribution machinery);
publishing without a draft (would require `--clobber` to recover a failed run).

## Consequences
- A maintainer must configure the `release` environment (required reviewers;
  deployment tags), the `WINDOWS_CERTIFICATE_BASE64`/
  `WINDOWS_CERTIFICATE_PASSWORD` environment secrets, and tag protection on
  `v*`. Repository code cannot set these.
- A `pull_request` cannot write the binary-cache feed even for a same-repo
  branch; the feed is only warmed by trusted events.
- A tag with no certificate produces no release at all. This is intentional:
  `gh attestation verify` and Smart App Control only have value if the workflow
  is trusted and the artifact is signed.
- Dependabot must keep the SHA pins and their version comments in sync; a manual
  bump must update both.
- The `gate` (`needs: gate`) fail-closed property from ADR-0042 is unchanged.
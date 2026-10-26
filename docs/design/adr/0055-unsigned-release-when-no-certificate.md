# 0055 — Publish an unsigned release when no certificate is configured

## Status
accepted

## Context
ADR-0043 made signing mandatory for a `v*` tag: `release.yml` failed closed when
`WINDOWS_CERTIFICATE_BASE64` and `WINDOWS_CERTIFICATE_PASSWORD` were absent. The
intent was that nothing unsigned could reach Releases.

No code-signing certificate has been provisioned for the repository. The
`release` environment has neither secret, so every tag that reached the release
job stopped at "Import signing certificate" and no release was published. The
`v0.6.0` run (40376615393) failed exactly there after the test gate had passed.

## Decision
- Signing is optional. When both certificate secrets are configured, the job
  keeps the ADR-0043 behaviour: every executable image is Authenticode-signed
  and the checksum files get detached PKCS#7 signatures.
- When neither secret is configured, the job logs a warning, skips signing, and
  publishes the artifacts unsigned. It still publishes the SBOM, `MANIFEST.json`,
  `SHA256SUMS`, and the GitHub build-provenance attestation.
- Configuring only one of the two secrets remains an error, and a configured
  certificate still requires `signtool.exe`.
- The packaging scripts already accept an empty thumbprint and warn; the
  workflow passes no thumbprint in the unsigned case.

Rejected: generating a self-signed certificate (CI would pass, but Windows would
not trust it, so the release would look signed without earning SmartScreen or
Smart App Control trust); keeping the fail-closed gate and leaving tags
unreleased (the project needs downloadable builds); dropping attestation or
checksums (they remain useful without Authenticode).

## Consequences
- Releases are downloadable again, but they may be unsigned until a certificate
  is configured. SmartScreen and Smart App Control can therefore warn about or
  block a release even though its provenance attestation verifies.
- `docs/WINDOWS-SECURITY.md`, `README.md`, and `SECURITY.md` must describe both
  states instead of claiming every release is signed.
- This supersedes only ADR-0043's "make signing mandatory" decision. Its
  permission split, pinned actions, draft-then-publish, no-clobber, and
  attestation decisions stand.
- If signing is provisioned later, the workflow signs automatically; no code
  change is needed.

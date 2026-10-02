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
- [ ] Split `dependencies.yml` permissions by event: `packages: read` for `pull_request`,
      `packages: write` only for `push`/`schedule`/`workflow_dispatch` (or an approval-gated
      environment).
- [ ] Pin every action to a full commit SHA with a version comment; add Dependabot for
      `github-actions`.
- [ ] Make signing mandatory for a tagged release: fail closed when the certificate secrets are
      absent (keep an explicit, clearly named unsigned engineering path for non-tag builds if the
      project needs it).
- [ ] Add GitHub build-provenance attestation (`actions/attest-build-provenance`) and upload the
      SBOM and `MANIFEST.json`/checksum files as release assets; sign the checksum files with the
      same certificate.
- [ ] Protect the release job with an environment requiring review, and avoid `--clobber` for
      published tags (upload to a draft, then publish after approval).
- [ ] Stop persisting the token in NuGet config; pass it per-invocation or use the documented
      ephemeral mechanism, and clear it in an `always()` step.
- [ ] Record the intended tag protection/signing policy in `docs/WINDOWS-SECURITY.md` or
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
- [ ] A dry-run tag on a fork/practice tag produces signed, attested assets with the SBOM and
      manifests attached, verified against `gh attestation verify`.
- [ ] A same-repo PR cannot write the packages feed (permission check recorded).
- [ ] Hand-off filled in with the exact secrets/environment a maintainer must configure.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
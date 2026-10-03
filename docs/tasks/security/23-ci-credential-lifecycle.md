---
verify: x64\Release\Tests.Unit.exe "~[graphics]"
---

# T23 — Fix the release/CI NuGet credential lifecycle and durable-cache restore (SEC-14 follow-up)

## Goal
The GitHub Packages vcpkg binary cache is actually usable during restore, and the ephemeral NuGet
config is removed after the steps that need it without exposing the token to PR-authored code.

## Context (read first)
- `docs/tasks/security/14-ci-release-supply-chain.md` — the ephemeral-credential design.
- Follow-up audit evidence: the step `Remove ephemeral NuGet credentials` runs **before**
  `Restore vcpkg dependencies`, whose `VCPKG_BINARY_SOURCES` points at that config:
  - `.github/workflows/ci.yml:185` cleanup vs `:194` restore
  - `.github/workflows/release.yml:274` cleanup vs `:283` restore
  - `.github/workflows/dependencies-restore.yml:128` cleanup vs `:137` restore
  The durable feed is therefore never used and the jobs silently fall back to the files cache.
- `ci.yml:34` keeps workflow-level `packages: write` on `pull_request` while the step sets
  `feed-access: read` off push; the PR lane should not need a write scope.

## Scope
- [ ] Reorder/restructure the credential steps so the throwaway config lives through the restore and
      is deleted in a final `always()` step — while ensuring PR-authored portfiles cannot read it.
      Choose the safe shape (for example configure the Packages feed only on trusted events and use
      the files cache on `pull_request`) and record the reasoning.
- [ ] Narrow `ci.yml` permissions so the `pull_request` lane does not carry `packages: write`.
- [ ] `actionlint` the changed workflows.

## Out of scope
- The test gate itself (→ T13); signing/attestation (→ T14).

## Design notes
- Do not trade the functional cache for a token exposed to PR code; the trusted-event split already
  used by `dependencies.yml` is the precedent.
- Offline task: the durable-cache restore is verified by configuration review plus `actionlint`,
  recorded in the Hand-off. Note which observation still needs a hosted run.

## Done when
- [ ] `actionlint` reports clean; the step order is correct in all three workflows.
- [ ] `x64\Release\Tests.Unit.exe "~[graphics]"` still passes.
- [ ] Hand-off filled in.

## Hand-off
_(filled in by the implementing session)_
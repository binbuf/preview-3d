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
- [x] Reorder/restructure the credential steps so the throwaway config lives through the restore and
      is deleted in a final `always()` step — while ensuring PR-authored portfiles cannot read it.
      Chosen shape: configure the Packages feed **only on trusted events** (`github.event_name !=
      'pull_request'` in `ci.yml`; a `configure-feed` input in `dependencies-restore.yml`); a
      `pull_request` uses the files cache alone. Reasoning in ADR-0050.
- [x] Narrow `ci.yml` permissions so the `pull_request` lane does not carry `packages: write`.
- [x] `actionlint` the changed workflows.

## Out of scope
- The test gate itself (→ T13); signing/attestation (→ T14).

## Design notes
- Do not trade the functional cache for a token exposed to PR code; the trusted-event split already
  used by `dependencies.yml` is the precedent.
- Offline task: the durable-cache restore is verified by configuration review plus `actionlint`,
  recorded in the Hand-off. Note which observation still needs a hosted run.

## Done when
- [x] `actionlint` reports clean; the step order is correct in all three workflows.
- [x] `x64\Release\Tests.Unit.exe "~[graphics]"` still passes.
- [x] Hand-off filled in.

## Hand-off
Changed:
- `.github/workflows/ci.yml` — the `test` job's "Add NuGet binary-cache source" step now publishes
  the files cache alone on `pull_request` and only creates the ephemeral Packages config on trusted
  events (always read, since the workflow is read-only). The `always()` "Remove ephemeral NuGet
  credentials" step moved from before `Restore vcpkg dependencies` to the end of the job. Workflow
  permissions dropped from `packages: write` to `packages: read`. Comment updated.
- `.github/workflows/release.yml` — "Remove ephemeral NuGet credentials" moved from before
  `Restore vcpkg dependencies` to the end of the job (after "Publish GitHub release"). The config
  still uses `readwrite`; a tag lane is trusted, so no PR-exposure change.
- `.github/workflows/dependencies-restore.yml` — new `configure-feed` boolean input (default `true`).
  When false the step publishes the files cache alone and never fetches NuGet or writes the token.
  Cleanup moved to a final `always()` step after the restore. Header comment updated.
- `.github/workflows/dependencies.yml` — `restore-pr` passes `configure-feed: false`; `warm` passes
  `configure-feed: true`.
- `docs/design/09-quality-performance-and-security.md` — "CI and release evidence" updated for the
  `packages: read` ci lane, the trusted-event feed split, and the corrected cleanup ordering.
- `docs/design/adr/0050-trusted-event-nuget-feed-config.md` (new) — records the shape and the
  rejected alternatives.

Deviations:
- The task named three files with the ordering bug. Fixing the PR read exposure required
  `dependencies.yml` to pass the new `configure-feed: false` to the reusable workflow, so a fourth
  file changed. The reusable workflow alone cannot tell a trusted read from an untrusted read.
- `ci.yml` no longer warms the feed on a push to `main` (it is `packages: read`). This is redundant
  with `dependencies.yml`'s trusted `warm` job, whose `push` trigger is already scoped to the
  dependency manifest/port/triplet paths that change the cache key. Per-event job permissions are
  static, so keeping write for the push lane would have meant also carrying it on `pull_request` or
  duplicating the whole Debug/Release matrix.

Check results:
- `actionlint -color <all .github/workflows/*.yml>` — exit 0.
- PowerShell parse of every `run:` block in the four changed workflows
  (`[System.Management.Automation.Language.Parser]::ParseInput`, GitHub `${{ }}` expressions
  substituted with `EXPR`): 30 blocks, 0 errors.
- Static order check: in all three consumers the `Remove ephemeral NuGet credentials` step now
  appears after `Restore vcpkg dependencies` (`ci.yml` and `release.yml` as the last job step;
  `dependencies-restore.yml` as the last step).
- Task `verify:` `x64\Release\Tests.Unit.exe "~[graphics]"` — exit 0, 314 cases / 131561 assertions,
  all pass (no product code changed).
- Not run here (offline, no GitHub runner and no feed token): the actual durable-feed hit on a
  hosted trusted event and the PR lane's files-only fallback. The next hosted run must observe a
  restore that resolves packages from the GitHub Packages NuGet source rather than only the local
  files cache. This is the external observation this offline session could not make.
- Not run: a hosted `dependencies.yml` run to confirm the PR lane still restores from the files
  cache when `configure-feed: false`.

Next task must know: the packages feed config must be created only on trusted events; do not let a
`pull_request` create the token-bearing `NuGet.Config` (PR-authored overlay portfiles run under
`vcpkg install`). The cleanup step must stay after `Restore vcpkg dependencies`. `ci.yml` is
`packages: read` by design; do not restore `packages: write` there (see ADR-0050).
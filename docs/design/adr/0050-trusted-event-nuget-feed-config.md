# 0050 — Trusted-event NuGet feed config and correct cleanup ordering

## Status
accepted

## Context
ADR-0043 moved the GitHub Packages `GITHUB_TOKEN` out of the user-profile
`NuGet.Config` into a throwaway file under `RUNNER_TEMP` consumed through vcpkg's
`nugetconfig` source. Two defects remained in that design.

First, the `always()` step named `Remove ephemeral NuGet credentials` was placed
*before* `Restore vcpkg dependencies` in all three consumers
(`ci.yml`, `release.yml`, `dependencies-restore.yml`), so the config was deleted
before the step whose `VCPKG_BINARY_SOURCES` pointed at it. Every job silently
fell back to the Actions files cache and never used the durable feed — the
opposite of the intended behaviour.

Second, `ci.yml` kept workflow-level `packages: write` for every event, including
`pull_request`, and `dependencies-restore.yml` created the token-bearing config
for a `pull_request` too. `vcpkg install` executes overlay portfiles from the
checkout, which are PR-authored on a pull request, so those portfiles could read
the clear-text token even when only read access was granted. A `packages: write`
grant on the PR lane was also a live write token.

## Decision
- The ephemeral Packages config is created **only on trusted events**. A
  `pull_request` uses the trusted-populated local files cache alone and never
  sees the token. This is enforced in `ci.yml` by event name and in the reusable
  `dependencies-restore.yml` by a new `configure-feed` boolean input
  (`false` for the `restore-pr` job, `true` for `warm`).
- The cleanup step moves **after** the restore (it is now the final `always()`
  step of each job), so the config survives the step that consumes it and is
  still deleted even when an earlier step fails.
- `ci.yml` drops to workflow-level `packages: read`. The shared test matrix
  never warms the feed; the trusted `warm` job in `dependencies.yml`, which
  already triggers on the dependency-change paths, is the only writer. A caller
  (`release.yml`) cannot elevate the called workflow beyond `read`, and the PR
  lane can never carry `packages: write`.

Rejected: keeping `packages: write` at the workflow level and relying on the
`feed-access` string (the permission is still a live write token on a same-repo
PR); splitting `ci.yml` into two permission-scoped jobs (would duplicate the
whole Debug/Release matrix); leaving the config in place for a PR but stripping
the token (the portfiles still run in the same step that needs the token).

## Consequences
- Restores now genuinely consult the durable GitHub Packages feed on trusted
  events; a Packages outage still degrades to the files cache.
- A same-repo PR can neither write the feed nor read the token that can write
  it, closing the read side that ADR-0043 left open.
- `ci.yml` no longer warms the feed on a push to `main`; that was redundant with
  `dependencies.yml`, whose `push` trigger is already scoped to dependency
  manifest/port/triplet changes (the only changes that alter the cache key).
- The `configure-feed` input is the contract the reusable workflow and its
  callers must keep in sync; a future trusted read-only caller sets it `true`.
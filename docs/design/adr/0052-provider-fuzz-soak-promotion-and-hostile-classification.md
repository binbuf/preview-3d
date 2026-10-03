# 0052 — Promote the `fuzz-smoke` lane and classify hostile surrogate deaths

## Status
accepted

## Context
ADR-0042 kept the bounded libFuzzer smoke as an opt-in/nightly job and said
SEC-15/16/17 would extend it and promote it. Through T17 the lane covered seven
targets, but `ProviderFuzz` was deliberately absent because it links the
STEP/OCCT adapter and the job restored only the root manifest; the provider's
OCCT closure lives in the separate `thumbnail-provider/step-occt` manifest
(ADR-0002). The SEC-17 surrogate soak also treated any failed thumbnail as an
anonymous crash/hang, so it could not distinguish the SEC-08 allowed failures
(a re-raised stack overflow or an uncatchable `__fastfail`, ADR-0037) from a
contained access violation that quarantines the subject (ADR-0037).

## Decision
- Add `ProviderFuzz` to the `fuzz-smoke` matrix. On the `Provider` runner the
  job restores the isolated `thumbnail-provider/step-occt` manifest (the root
  manifest is installed by manifest mode for every target) and reuses the
  test/dependencies vcpkg files cache so a warm run stays bounded. It holds no
  Packages credentials, so the lane stays safe on a fork pull request. One
  target per runner is retained (the fuzz projects share
  `tests\fuzz\x64\Release`, MSB8028).
- Promote `fuzz-smoke` out of the nightly-only schedule into the required gate:
  it runs on every `pull_request`/`push` and through `workflow_call` from
  `release.yml`, so a red fuzz smoke stops a release (SEC-13/T13).
- Classify hostile surrogate deaths in the soak. The soak keeps a handle on
  every `dllhost` that hosts the provider, reads its exit code when it dies, and
  records: a contained access violation as a quarantine *failure-with-reason*
  (a valid request refused while no surrogate died); a stack overflow
  (`0xC00000FD`) or `__fastfail`/stack-cookie (`0xC0000409`, `0xC0000602`) death
  as the documented *allowed* failure (the run reports `ALLOWED`, not `PASS`,
  and exits 0); any other death as an unexpected fault (exit 1). `--hostile
  <path>` adds an expected fail-closed lane whose refusals are recorded as
  `rejected`, and `--soak-classify-selftest` checks the exit-code classifier.

## Consequences
- A maintainer must add the `Fuzz smoke (<target>)` checks to branch protection
  alongside `Build and test (Debug/Release)`; repository code cannot set that.
- The lane is heavier on every pull request, so it depends on the shared binary
  cache; a miss rebuilds from source, which is the pre-promotion behaviour.
- The soak no longer masks an allowed death as a pass and treats a contained
  fault as a positive failure signal; the exit-code classifier is deterministic
  and self-checked.
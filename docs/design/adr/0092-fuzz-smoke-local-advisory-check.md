# 0092 - Fuzz smoke is a local, advisory pre-release check

Status: accepted (2026-11-01). Supersedes the promotion clause of
[ADR-0052](0052-provider-fuzz-soak-promotion-and-hostile-classification.md).

## Context

ADR-0052 moved the bounded libFuzzer smoke (`fuzz-smoke` in
`.github/workflows/ci.yml`) out of the nightly-only schedule and into the
required `pull_request`/`push` gate so that a finding would stop a release.

In practice the lane is a poor fit for the merge gate:

- It is eight runners per pull request (STEP, 3MF, STL, PLY, OBJ, Assimp, glTF,
  Provider), each restoring vcpkg and building an ASan+libFuzzer binary. A
  cache miss rebuilds the dependency closure from source, which is heavier than
  the Debug and Release build jobs it sits beside.
- Findings are almost always in pinned third-party parsers, not in this
  repository's code. The first run of the newly added `Assimp` target
  (2026-11-01) aborted with an AddressSanitizer out-of-memory in Assimp's ASCII
  `.x` reader, where an authored count sizes a `std::vector` without a bound.
  That single upstream defect cancelled `Build and test (Debug/Release)` and
  blocked the 0.7.0 release, even though the product already contains the
  allocation behind the import worker's Job commit ceiling.
- GitHub Actions is meant to build, test and publish. Long-running randomized
  exploration belongs on a machine the maintainer controls, where the corpus,
  the findings and the triage notes live.

## Decision

- `fuzz-smoke` no longer runs on `pull_request`, `push`, or the `release.yml`
  `workflow_call`. It runs only on an explicit `workflow_dispatch` with the
  `fuzz` input left at `true`, so the matrix stays exercisable in CI without
  gating a merge or a release.
- The bounded smoke is a **local** check. Maintainers run it on their own
  schedule (the targets and per-target commands are in `tests/fuzz/README.md`
  and `tests/fuzz/Invoke-FuzzSmoke.ps1`) and always before a **minor or major**
  release. Patch releases inherit the most recent run unless a parser, codec or
  dependency changed in the release.
- The result is **advisory**: a finding is triaged like any other defect (fix,
  mitigate, or record as a known follow-up in the release notes). It does not
  block CI, and it does not block a tag by itself.
- The required gate stays what it was before ADR-0052: Debug and Release x64
  builds plus `Tests.Unit.exe "~[graphics]"` and `Tests.ProviderHost.exe` on
  every pull request and push to `main`; `Tests.ImportIsolation.exe` is local
  per [ADR-0093](0093-import-isolation-local-suite.md); the tag build re-runs
  the Release lane against the tagged source before packaging (ADR-0042).

## Consequences

- A maintainer must remove the `Fuzz smoke (<target>)` checks from branch
  protection; repository code cannot change that setting. Leaving them required
  would keep every pull request permanently pending.
- The lane is no longer continuous evidence. The corpus and seeds still live in
  `tests/fuzz/corpus/`, so a local run starts warm and reproduces the same
  inputs.
- Known follow-up from the 0.7.0 run: the Assimp ASCII `.x` element counts
  (`ParseDataObjectMesh`/`ParseDataObjectMeshNormals` in the vendored port) are
  unbounded. Until the port or the adapter preflight bounds them, `AssimpFuzz`
  will report the same out-of-memory locally; it is a recorded finding, not a
  gate.
- A future decision can re-promote the lane once upstream findings are
  addressed and the runtime is bounded, but that is a new ADR, not a default.

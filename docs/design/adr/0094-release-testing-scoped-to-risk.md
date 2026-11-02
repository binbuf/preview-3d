# 0094 - Local release testing is scoped to release risk

Status: accepted (2026-11-02). Supersedes the "required before every release,
patch releases included" clause of
[ADR-0093](0093-import-isolation-local-suite.md).

## Context

ADR-0093 moved `Tests.ImportIsolation.exe` out of CI because it is slow, needs
the optional local corpus, and most of its failures are in pinned third-party
parsers rather than product code. Its replacement requirement was the full
local Debug and Release gauntlet before every release, including patch
releases.

In practice that duplicates the mandatory CI gate for patches that cannot
affect an importer: CI already builds Debug and Release and runs
`Tests.Unit.exe "~[graphics]"` and `Tests.ProviderHost.exe` on the release
commit, and the tag build re-runs the Release lane before packaging. Running
the same build and suites locally again adds little signal for a low-risk
patch, and it makes a quick patch release take as long as a minor release.

The part CI genuinely cannot cover is the import path: the format corpus, the
CAD hosts, the hostile worker, shared-section validation, and the optional
`test-models/` corpus. Those are relevant exactly when an importer, the wire
format, the broker, a fixture, or a third-party dependency changes.

## Decision

Required on **every** release, patch included:

- the remote `CI` workflow is green on the release commit;
- `pwsh -File packaging/Test-ReleaseMetadata.ps1 -SelfTest`;
- `generate-language-packs.ps1 -Verify` when user-facing strings changed;
- the changelog entry, every version source, the tag, and the `release`
  environment approval.

Required for a **minor or major** release, and for any release (patch
included) that changes an importer, the wire format, the broker, a fixture, or
a third-party dependency:

- Debug and Release x64 builds;
- `Tests.Unit.exe "~[graphics]"`, `Tests.ImportIsolation.exe`, and
  `Tests.ProviderHost.exe` in both configurations;
- `Tests.Unit.exe "[graphics]"` when the release touches rendering;
- the fuzz smoke per [ADR-0092](0092-fuzz-smoke-local-advisory-check.md).

Optional for a **low-risk patch**:

- the local build/suite gauntlet above (run it anyway when CI is unavailable,
  the change surface is uncertain, or extra confidence is wanted);
- local portable and installer packaging. The tag workflow builds both, so
  this is a convenience check, not a gate.

`docs/New_Release_Instructions.md` step 4 owns the exact commands and the
evidence expectations.

## Consequences

- A low-risk patch can ship after CI plus the metadata self-test, so patches no
  longer pay the minor/major verification cost.
- The trigger list is the safeguard: a patch that changes an importer, the wire
  format, the broker, a fixture, or a dependency owes the full local suite. A
  misjudged change surface can still ship an import regression, which is the
  risk this decision accepts.
- ADR-0093 remains in force for the suite's placement and scope: it stays
  local-only, and CI continues to build but not run it.
- No check name changes; branch protection and the tag workflow keep working
  unchanged.

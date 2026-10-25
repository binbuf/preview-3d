# Thumbnail provider design

Architecture documents for the Explorer thumbnail provider program. Agent sessions read the docs
relevant to their task before editing code, and update them when behaviour changes. Decisions that
constrain later tasks go into [`adr/`](adr/) as numbered records.

This package is the active plan for **full Explorer thumbnail support** across every direct format,
plus the prerequisite decisions, spikes and shared infrastructure it needs. It sits beside the
project's original design baseline, which was archived to [`docs/legacy/design/`](../legacy/design/)
when this package was created. The numbered documents here (`01`–`11`) are reviewed copies of that
baseline and remain the authority for anything the thumbnail program does not override.

## Read this first

- [`overview.md`](overview.md) — goals, non-goals, the eight-family target roster, component map, and
  the prerequisite chain this program starts with.
- [`testing-strategy.md`](testing-strategy.md) — how the provider is built, hosted, fuzzed and
  qualified: COM host harness, golden images, surrogate soak, clean-machine verification.

## Security hardening program

The v0.5.0 security audit and the remediation tasks it produced live in [`../security/`](../security/)
([`ROADMAP.md`](../security/ROADMAP.md), [`PROGRESS.md`](../security/PROGRESS.md),
[`INDEX.md`](../security/INDEX.md)). The design documents here are updated to match the controls that
program lands; when a control is still owed to a task, the document names that task rather than
claiming it.

## Normative documents

1. [`01-product-scope.md`](01-product-scope.md) — users, platform, requirements, exclusions, release acceptance.
2. [`02-system-architecture.md`](02-system-architecture.md) — process/thread boundaries, dependencies, ownership, data flow.
3. [`03-file-formats-and-ingestion.md`](03-file-formats-and-ingestion.md) — exact format subsets, limits, errors, budgets.
4. [`04-rendering-and-streaming.md`](04-rendering-and-streaming.md) — viewer GPU path (context only; the provider has no GPU device).
5. [`05-thumbnail-provider.md`](05-thumbnail-provider.md) — the provider contract: COM classes, stream ingestion, sampler, CPU rasterizer, isolation, bitmaps.
6. [`06-application-lifecycle-and-ipc.md`](06-application-lifecycle-and-ipc.md) — viewer lifecycle and IPC (context for what the provider must never do).
7. [`07-user-experience.md`](07-user-experience.md) — viewer UX (context only).
8. [`08-installation-and-registration.md`](08-installation-and-registration.md) — COM registration identities, conflict/repair/uninstall rules.
9. [`09-quality-performance-and-security.md`](09-quality-performance-and-security.md) — test matrix, performance method, fuzzing, threat controls, release gates.
10. [`10-delivery-plan.md`](10-delivery-plan.md) — the original Gate 6/7 delivery sequence and definition of done.
11. [`11-decisions-and-risks.md`](11-decisions-and-risks.md) — decisions and risks, including R-08, R-15, R-21 and Spike 8.

## Format adapter briefs

Per-family thumbnail adapter requirements, carried over from the family plans:

- [`adapters/fbx-008-thumbnail.md`](adapters/fbx-008-thumbnail.md)
- [`adapters/3mf-thumbnail.md`](adapters/3mf-thumbnail.md) (see the "3MF-008" section)
- [`adapters/usd-010-thumbnail.md`](adapters/usd-010-thumbnail.md) (see the "USD-010" section)
- [`adapters/step-009-thumbnail.md`](adapters/step-009-thumbnail.md) (see the "STEP-009" section)

Tier A adapters (glTF/GLB, STL, PLY, OBJ) have no separate family brief: their contracts are in
[`05-thumbnail-provider.md`](05-thumbnail-provider.md) and [`03-file-formats-and-ingestion.md`](03-file-formats-and-ingestion.md).

## Precedence

`MUST`, `SHOULD` and `MAY` are normative. If documents conflict, the new ADRs in [`adr/`](adr/) and
[`overview.md`](overview.md) win over the archived baseline; otherwise the baseline precedence in the
archived `design/README.md` applies. Changing scope requires updating the affected requirement IDs,
format matrices, tests, risks and the roadmap together.

The accepted ADRs `0001`-`0007` are binding for this program. In particular,
[ADR-0007](adr/0007-provider-program-scope-and-installer.md) records the installed-scope and
NSIS-now/MSI-later vehicle decision and partially supersedes [ADR-016](11-decisions-and-risks.md)
(its per-archive exclusions stand; its "the whole MVP is portable-only" framing does not).
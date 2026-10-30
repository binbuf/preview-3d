# ADR-0075 — 0.7.0 qualification evidence and release-claim boundary

- **Status:** Accepted
- **Date:** 2026-10-30
- **Context:** T19 closes the 0.7.0 format-breadth and Studio-lighting slice. The
  suite has to state what was observed without turning per-adapter tests into an
  open-rate claim, and without letting a deferred feature silently weaken a gate.

## Context

The T01 corpus (`tests/compat/manifest.json`) is manifest-driven and runs the
real broker path, but it was seeded with the baseline families and was not
extended by the breadth tasks. The breadth families have their own fixtures and
tags, while the Studio lanes are visual and app-level. A release document that
reports one aggregate "open rate" across a cherry-picked fixture set would be
misleading; so would relaxing the `[compat]` assertion or a visual tolerance to
make the slice look green.

## Decision

1. The `[compat]` assertion remains the release gate. It must pass, or every
   remaining failure must be typed in the manifest with an owner. A passing
   corpus does **not** by itself claim open rates for families the manifest does
   not contain.
2. Qualification evidence is recorded per lane in
   `docs/0.7.0-QUALIFICATION.md` with the command, build id, configuration and
   raw report path: corpus, unit, import-isolation, visual goldens, app smoke,
   installer/catalog drift, release metadata, dependency closure and
   performance/memory.
3. Visual and smoke tolerances are not relaxed during qualification. A lane
   that cannot pass is recorded with its failure and a follow-up; it is not
   removed from the claim.
4. Explorer thumbnails remain a separate provider program. A format that ships
   viewer support (including the Assimp family and the new CAD formats) does not
   thereby claim an Explorer thumbnail; the installer registers only extensions
   with a provider adapter.
5. The viewer must import no parser DLL. Static closure is checked with
   `dumpbin /dependents`; OCCT appears only under `StepHost\`, OpenUSD only under
   `OpenUsdHost\`, and the general worker remains the only home of the Assimp,
   glTF, FBX, 3MF and TinyUSDZ readers.

## Consequences

- Later format additions must update both the catalog and the corpus (or state
  explicitly that the corpus does not cover them) before they join a release
  claim.
- A green `[compat]` run is necessary but not sufficient: the app-smoke and
  golden lanes are part of the release evidence, and a flaky lane is recorded,
  not hidden.
- Deferred work is enumerated in the T19 progress notes and the design ADRs;
  it does not weaken the acceptance criteria of the shipped slice.

## References

- ADR-0056 compatibility corpus scoreboard
- ADR-0057 format catalog and bounded sniff routing
- ADR-0059 compatibility feature ids and report chunk
- `docs/0.7.0-QUALIFICATION.md`

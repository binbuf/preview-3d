# T43 — Harden with fuzz, ASan, and hostile corpora

## Goal
Prove the provider cannot be crashed, hung or leaked by hostile input: a fuzz lane per adapter, an
adversarial corpus, and a surrogate soak that keeps Explorer stable.

## Context (read first)
- `docs/design/testing-strategy.md` — containment and hostile-input layers.
- `docs/design/05-thumbnail-provider.md` — "Security and robustness".
- `docs/design/09-quality-performance-and-security.md` — fuzzing and threat controls.
- `docs/tasks/17-provider-test-harness.md`, `docs/tasks/41-shell-registration.md`.
- `tests/fuzz/` — the existing fuzz-target conventions.

## Scope
- [ ] Add a fuzz target per family boundary (stream read → adapter → sampler) under ASan/libFuzzer, seeded from real fixtures; run a bounded Release smoke per family.
- [ ] Assemble an adversarial corpus: truncation, hostile counts, archive bombs (3MF/USDZ), traversal/ADS/UNC references, non-seekable streams, OOM injection, deadline overrun.
- [ ] Add a surrogate soak: many parallel apartments, repeated load/unload, GC of Explorer's thumbnail cache, asserting no crash, hang, persistent thread, GDI/User or private-byte growth.
- [ ] Re-run the isolation/hostile suite for every newly wired adapter (do not rely on a single global run).
- [ ] Fix findings rather than masking them; use the SEH boundary only as a last resort.

## Out of scope
- Parser bugs owned by upstream libraries that need a pinned-version fix (record and escalate via ADR if unfixable).
- Performance qualification (→ T51).

## Design notes
- Keep fuzz seeds immutable and output under an ignored directory; libFuzzer writes into the corpus directory it is given.
- A minimized crash becomes a permanent regression fixture.
- Record exact execution counts, runtime and peak RSS for each smoke.

## Done when
- [ ] Every family has a passing bounded Release fuzz smoke and an adversarial-corpus test.
- [ ] The surrogate soak reports no crash/hang/leak/persistent thread.
- [ ] Findings and any residual upstream issues are recorded in Hand-off (and an ADR if a limit changed).
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
# T51 — Qualify performance and memory against the provider budgets

## Goal
Demonstrate the provider meets its qualification targets on the reference systems with recorded evidence:
750 ms p95 / 2 s cooperative stop point, 192 MiB accounted scratch, 384 MiB measured private-commit increase above baseline, and bounded GDI/User
handles — across every family.

## Context (read first)
- `docs/design/09-quality-performance-and-security.md` — the thumbnail p95/stop-point gate and measurement method.
- `docs/design/05-thumbnail-provider.md` — deadlines, memory caps and stream budget.
- `docs/design/testing-strategy.md` — performance/memory method and determinism rule.
- `docs/tasks/17-provider-test-harness.md`, `docs/tasks/02-spike-rasterizer-prototype.md`.

## Scope
- [ ] Assemble a representative performance corpus per family (small, medium, complex/textured, and an over-budget case) with fixed hashes; do not commit multi-gigabyte binaries.
- [ ] Measure time-to-bitmap p50/p95/max and the fraction reaching or exceeding the 2 s stop point, including time spent inside uninterruptible parser calls, inside the real surrogate.
- [ ] Establish an idle, loaded surrogate baseline and sample peak process private commit, accounted parser scratch, GDI/User handles and thread count under single and concurrent requests. Report unaccounted library/module/GDI overhead; assert the 192 MiB accounted scratch cap and the 384 MiB measured process target separately.
- [ ] Verify determinism: repeated renders of the same input are identical; DPI/size changes resolution only.
- [ ] Publish a results table with raw data and fixture hashes in `docs/design/` and the task Hand-off.

## Out of scope
- Viewer startup/frame/input gates (viewer program).
- Adapter functional correctness (already covered by family tasks).

## Design notes
- p95 over repeated runs; keep maxima and exclusions visible.
- WARP or a dev machine cannot substitute for the reference-system run; record the machine used.
- If a family/subset exceeds the measured time or memory target, narrow its admission policy or disable that subset and remeasure before release. A generic-icon result after an overlong parser call does not erase the observed overrun.

## Done when
- [ ] The results table shows every family within the targets for its supported corpus and records all overruns/fallbacks; any failing family/subset is narrowed and requalified or the release scope is replanned through an ADR.
- [ ] Accounted memory limits and measured process/handle targets hold across the qualified corpus and concurrency.
- [ ] Determinism holds across runs.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_

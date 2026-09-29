---
timeoutMin: 300
verify: x64\Release\Tests.Unit.exe
---
# T34 — Implement the STEP/STP thumbnail adapter

> **E2E slice review point.** Completing this task closes the Tier B breadth slice. Run a fresh end-to-end review now; the pipeline may continue to the next task without waiting.

## Goal
Render self-contained `.step`/`.stp` visual geometry in Explorer through a separately built,
constrained OCCT adapter linked only into the thumbnail DLL — the last and heaviest family — and
qualify it before the full eight-family registration/release claim.

## Context (read first)
- `docs/design/adr/0002-occt-linked-into-thumbnail-adapter.md` — the linkage decision, ceilings and contingency.
- `docs/design/adapters/step-009-thumbnail.md` — the "STEP-009" section.
- `docs/design/05-thumbnail-provider.md` — stream ingestion and limits.
- `docs/tasks/06-budgets-deadlines-hresults.md` — provider ceilings (T34 uses the stricter provider values, not the host's).
- `docs/tasks/17-provider-test-harness.md`.

## Scope
- [ ] Route `.step`/`.stp` through the frozen STEP CLSID `{6EE961AC-AC3B-4958-A898-E30523FEE79D}`.
- [ ] Build a separately constrained OCCT static-library adapter and link it only into the thumbnail DLL; confirm the DLL imports no viewer/worker/host executable or library. The existing STEP host retains its own separate OCCT closure.
- [ ] Consume only `IInitializeWithStream` through a bounded seekable stream; run product-owned Part-21 admission before OCCT, rejecting external declarations, unsupported content and over-budget input.
- [ ] Use a fixed low-detail deterministic tessellation/sample policy: 256 MiB stream, 192 MiB accounted parser/tessellation scratch, 384 MiB measured process-commit increase target, reduced triangle cap, cooperative 2 s stop point / 750 ms p95. Measure actual OCCT-call overruns and unaccounted allocation.
- [ ] Preserve bounded shape colors and complete-assembly spatial representation where possible; never launch `Preview3DStepHost.exe`.
- [ ] Add fixtures and goldens: self-contained AP203/AP214/AP242 mesh/assembly, colors, and external/unsupported/over-budget/malformed cases; measure time and commit.
- [ ] If qualification fails, record raw timing/commit evidence and keep T34 incomplete. Replan via a scope-changing ADR and update the roadmap, registration and public release claims before proceeding; do not treat an always-generic STEP handler as completed thumbnail support.

## Out of scope
- STEP-008 viewer/host qualification (viewer program).
- External STEP documents (out of scope by recorded product decision).

## Design notes
- OCCT throws `Standard_Failure`, not `std::exception`; catch it explicitly at the adapter boundary.
- The provider must not create OCCT global state that outlives a call; keep per-call state bounded.
- This is the highest-risk family. A failed feasibility result is useful evidence, but it does not satisfy this full-scope task.

## Done when
- [ ] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes for supported self-contained STEP renders and safe fallbacks on unsupported inputs.
- [ ] Measured STEP corpus meets the time and process-commit qualification targets; otherwise the task is not done and the program is replanned.
- [ ] The closure check proves the constrained thumbnail OCCT adapter enters only the provider DLL, while the existing STEP host keeps its separate OCCT closure; viewer/general worker and provider-to-host imports remain absent.
- [ ] The family renders a model-derived Explorer thumbnail using the T22 procedure.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_

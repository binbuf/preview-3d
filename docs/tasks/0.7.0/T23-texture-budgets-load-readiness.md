---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T23 --configuration Debug --output TestResults/render-quality/T23
---
# T23 — Texture-budget accounting and complete-load readiness

## Goal

Open legitimate textured assets within the existing safety envelope, with honest final load status and predictable lower-detail behavior when full texture quality cannot fit.

## Context (read first)

- Depends on **T20, T21, T22**; execute in roadmap order.
- `TestResults/render-quality-review/preview-microscope-resource-limit.png` — fresh late ResourceLimit after visible geometry; this is a failed load, not a qualified Clay/Directional image.
- `GltfAdapter.cpp` texture decode/dedup, aggregate counters and progressive mip packaging; texture budget constants, broker/session status and shared-section emission.
- `tests/app-smoke/studio.py::wait_for_render`, T20 settled-capture contract and existing memory/security ADRs.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Reproduce the microscope late ResourceLimit with an instrumented Debug import/capture. Identify the exact producer/validator/broker limit and peak allocation; distinguish decoded pixel/byte totals, mip chains, image reuse across semantics, progressive duplicate copies, catalog/section limits, worker commit and GPU residency. Do not infer the cause from the generic overlay or compressed file size.
- [ ] Add bounded typed diagnostics for the limiting resource and requested/allowed quantities, with paths/model data excluded from Release logs. Make the user-facing degradation/failure state accurately reflect the stage and surviving geometry; a previously visible model cannot be treated as successful qualification after a terminal error.
- [ ] Fix duplicate charging/copying or decode-order dependence where present. Reuse immutable data safely, preserve distinct sRGB/data-map semantics and account for all simultaneously resident coarse/full mip allocations. Test shared images through multiple texture slots/materials.
- [ ] When optional texture quality exceeds budget, prefer a bounded lower-resolution mip decode or explicit missing-texture degradation if allowed by the established policy; geometry and safety limits remain hard. Plan sizes before allocations, use overflow-safe accounting, and preserve the worker Job/AppContainer/broker limits.
- [ ] Do not simply raise aggregate limits or bypass the sandbox. A necessary public-policy change requires measured legitimate/adversarial evidence and an ADR, with unchanged finite bounds and hostile cases. Avoid a full-resolution decode whose peak defeats the proposed lower-resolution fallback.
- [ ] Make capture readiness require terminal successful/degraded import, expected scene generation and required geometry/texture residency plus settled frames. Detect late errors after initial geometry; replace brightness-only visibility as a completion test.
- [ ] Add generated texture-heavy/reused-image/late-failure fixtures and boundary cases just below/above each touched limit; run malformed/oversized/decompression-bomb/cancellation tests. Review microscope, Camera and Car after the change and record final image/compatibility status.

## Verification

The front-matter command is a planned interface introduced by T20. Extend
its profile in this task; unimplemented profiles must fail. The wrapper must
run the task's meaningful unit/import checks and actual GPU image lane,
propagate failures and save exact commands/results. Build changed binaries
first. Keep import-worker suites serial. Image review and explicit reference
promotion are separate from ordinary verification; source-string checks and
self-created goldens do not prove rendering quality.

## Done when

- [ ] T23 verification includes exact-limit reproduction, budget-accounting/hostile import tests and success/degradation/late-error capture-contract tests.
- [ ] The microscope has a measured, truthful disposition and complete settled captures if it can open safely; an unexplained refusal or raised limit without evidence is not completion.
- [ ] Update texture-budget/readiness/compatibility documentation and hand-off; retain first-failure evidence.

## Out of scope

Weakening isolation, unbounded texture decodes, accepting partial geometry as a completed load, or promising every arbitrarily large model will open.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


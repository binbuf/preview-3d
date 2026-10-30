---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T26 --configuration Debug --output TestResults/render-quality/T26
---
# T26 — Bounded order-independent transparency

## Goal

Make overlapping and intersecting alpha surfaces stable when the camera or draw order changes.

## Context (read first)

- Depends on **T20, T22, T24, T25**; execute in roadmap order.
- `D3D12ViewerPath.cpp` draw sorting, opaque/blend PSOs, HDR/pick targets and resource retirement.
- F3D shipped preset's `blending: ddp`; `vtkF3DRenderPass.cxx` dual-depth-peeling construction.
- T20 glass/intersection fixtures and T27 transmission contract.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Implement bounded dual depth peeling or an evidence-backed alternative that resolves the specified layered-glass cases. Weighted blended OIT is acceptable only if it meets those cases; document its limitations and do not call approximate order independence exact.
- [ ] Keep opaque/masked depth semantics correct, composite transparent contributions in linear HDR before tone mapping, and preserve separate coverage/reflection/transmission inputs. Define how this pass hands off to T27; OIT alone does not provide refraction.
- [ ] Test intersecting triangles within one primitive, shuffled draw/primitive order, multiple nested front/back glass surfaces, alpha decals over glass/paint, cutouts and emissive blended geometry. Capture small orbit sequences; mesh-center sorting must no longer decide the answer.
- [ ] Keep selection/picking deterministic for transparent surfaces with a documented nearest-visible policy. Integer IDs must never be averaged; hidden opaque geometry and low-coverage overlays need explicit tests.
- [ ] Bound peel/layer count, render-target allocation and work. Record behavior when the limit is exceeded, use the existing GPU budget, and test resize/cancel/device loss/low-memory fallback. A sorted fallback must not silently become the usual high-quality path.
- [ ] Use Car windshield/side windows/headlights and Camera lens as paired visual cases; preserve the Camera behavior while fixing the car. Add a deliberately permuted-order regression test that fails the old implementation.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

## Done when

- [ ] T26 verification includes order-permutation/image/motion tests and depth/picking/resource-lifecycle checks.
- [ ] The report names the algorithm, caps, fallback and measured GPU/memory cost; reviewed crops show no order-dependent window popping within the supported layer range.
- [ ] Update transparency/resource design and hand-off.

## Out of scope

Ray tracing, unbounded per-pixel linked lists, and claiming that OIT alone solves physically based transmission.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


---
timeoutMin: 240
---
# T02 — SPIKE-8a: prototype the mesh and point CPU rasterizer

## Goal
Prototype on real hardware a bounded, product-owned CPU tile rasterizer that renders a framed
512 px thumbnail for triangle meshes and point clouds. T03 measures this prototype and bounded
compressed-glTF decode inside the real surrogate before foundation work commits to the approach.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — the CPU renderer contract (framing, isometric view, depth test, lights, transparency approximation, point splats, linear downsample, premultiplied BGRA).
- `docs/design/03-file-formats-and-ingestion.md` — the thumbnail budgets and scratch/commit caps.
- `docs/design/testing-strategy.md` — golden-image policy and the 750 ms/2 s targets.
- `docs/design/11-decisions-and-risks.md` — Spike 8 and R-15.

## Scope
- [ ] Build a reusable prototype library plus a standalone driver (no COM in this task) that takes triangle and point lists and renders to a premultiplied BGRA buffer at 32/64/256/512 px. Keep an entry point T03 can call inside its Shell-surrogate probe.
- [ ] Implement: verified-bounds framing with ~7% margin, fixed isometric view, double-precision transforms, near-plane clip, depth test, ambient + two fixed lights, opaque/masked triangles, weighted-opaque approximation for transparency, round depth-tested splats, linear-space downsample.
- [ ] Measure wall-clock time and peak private commit for a representative mesh and a multi-million-point cloud at the provider caps (2 M triangles inspected / 6 M points inspected / 250 k rasterized samples).
- [ ] Prepare a bounded compressed-glTF decode fixture/path (Draco or meshopt geometry plus an embedded compressed image where applicable) for T03 to measure end to end in the surrogate; record which decoder and dependency closure the spike actually exercises.
- [ ] Record golden images and a short Spike 8 results note in the task Hand-off (and, if it changes the contract, an ADR).

## Out of scope
- The COM provider shell, stream backing and adapter routing (→ T11–T13).
- Final family adapters (→ T21–T34).
- Byte-exact viewer parity — thumbnails are a distinct approximation by design.

## Design notes
- No GPU device, no D3D, no external renderer; this is a product-owned tile rasterizer.
- Sampling must be deterministic from a stable source-derived seed.
- If the deadline cannot be met at 512 px with 2× supersampling, record the failure and the reduced-supersampling fallback rather than exceeding the budget.

## Done when
- [ ] The prototype renders deterministic golden images for a mesh and a point cloud at all four sizes.
- [ ] Standalone time and measured process commit meet the prototype targets, or the shortfall is documented with a proposed narrowing; T03 remains the decisive in-surrogate feasibility gate.
- [ ] Commands and raw numbers are recorded in Hand-off; any contract change is an ADR.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_

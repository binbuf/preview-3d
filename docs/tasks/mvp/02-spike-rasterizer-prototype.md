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

### What landed
- `thumbnail-spike/rasterizer/ThumbnailRasterizer.{h,cpp}` — reusable, dependency-free prototype CPU
  tile rasterizer. Entry point `thumbnail_rasterizer::Render(GeometryView, Options, Image&, cancel)`.
  Implements: verified-bounds framing with 7% margin, fixed isometric orthographic view,
  double-precision transforms, view-space near-plane clip, depth test, ambient + two fixed lights,
  opaque/masked triangles, weighted-opaque transparency, round depth-tested point splats, soft neutral
  contact shadow on a transparent canvas, linear-space supersample resolve, premultiplied BGRA output.
- `thumbnail-spike/rasterizer/SceneFixtures.{h,cpp}` — deterministic golden mesh (icosphere) and point
  cloud, plus cap-sized generators with seeded reservoir sampling.
- `thumbnail-spike/rasterizer/PamImage.h` — lossless PAM golden I/O.
- `thumbnail-spike/driver/main.cpp` + `thumbnail-spike/RasterizerSpike.vcxproj` — standalone driver
  (`goldens`, `perf`), added to `Preview3D.slnx`.
- `thumbnail-spike/goldens/*.pam` — 8 committed deterministic goldens (mesh and points, 32/64/256/512).
- `tests/unit/ThumbnailRasterizerTests.cpp` — 9 Catch2 cases (determinism, premultiplied output, four
  golden sizes per scene, empty/invalid input, masked cutoff, weighted-opaque transparency, depth
  test, deterministic/bounded sampler, Draco decode → rasterize). Added to `Tests.Unit.vcxproj`.
- `thumbnail-spike/README.md` — results table, commands, gotchas, compressed-glTF fixture map.

### Commands and raw numbers (Release x64)
```
msbuild Preview3D.slnx /p:Configuration=Release /p:Platform=x64
x64\Release\RasterizerSpike.exe goldens thumbnail-spike\goldens
x64\Release\RasterizerSpike.exe perf
x64\Release\Tests.Unit.exe "[thumbnail-rasterizer]"
```
Perf (512 px, 2x supersampling; provider caps 2M tri / 6M pts / 250k rasterized):
```
supersample=2
  mesh   inspected=2000000 kept=250000 status=0 time=58.8 ms  private=59.1 MiB peakCommit=79.1 MiB
  points inspected=6000000 kept=250000 status=0 time=185.9 ms private=60.1 MiB peakCommit=80.1 MiB
supersample=1
  mesh   time=40.5 ms  points time=80.5 ms
cooperative-cancel status=3 (Cancelled)
```
Golden small-scene times: 0.20–32.16 ms. `Tests.Unit.exe "[thumbnail-rasterizer]"` → all 9 cases
pass (65628 assertions) in Debug and Release. Full `x64\Release\Tests.Unit.exe` → all 115 cases pass.

### Deviations and decisions
- Transparency: "weighted opaque" is implemented by blending the shaded color toward a neutral frost
  by material opacity and writing an **opaque** result with depth — no sorting or true blending. A
  transparent material therefore reads as opaque in the thumbnail, matching the contract's intent.
- Near-plane clip is implemented but rarely active: framing sets the near plane below the verified
  bounds, so only pathological/degenerate input exercises it. Kept as the required safety net.
- View is true orthographic isometric (not perspective); this keeps framing deterministic and stable.
- No ADR: the provider contract in `docs/design/05-thumbnail-provider.md` is unchanged and the
  prototype meets the targets without a narrowing, so no decision constrains later tasks.
- `Triangle.color`/`Point.color` are linear RGB (documented in the header and README).

### Compressed glTF for T03
The spike exercises the Draco decoder only (Catch2 `[draco]` case). Fixtures for T03's end-to-end
surrogate measurement are the existing corpus under `interactive-viewer/test-assets/corpus/`
(`draco_triangle.glb`, `draco_position_only.glb`, `basisu_textured_triangle.glb`,
`basisu_corrupt_ktx2.glb`, `meshopt.glb`, `webp.gltf` + `sample.webp`). Provider decoder closure per
ADR-0003: draco, ktx/basisu, meshoptimizer, libwebp.

### Remaining work / blockers
None block T02. T03 owns the decisive in-surrogate time/commit measurement and the image/meshopt
decoder paths; it should call `thumbnail_rasterizer::Render()` directly.

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

- [x] Implement bounded dual depth peeling or an evidence-backed alternative that resolves the specified layered-glass cases. Weighted blended OIT is acceptable only if it meets those cases; document its limitations and do not call approximate order independence exact.
- [x] Keep opaque/masked depth semantics correct, composite transparent contributions in linear HDR before tone mapping, and preserve separate coverage/reflection/transmission inputs. Define how this pass hands off to T27; OIT alone does not provide refraction.
- [x] Test intersecting triangles within one primitive, shuffled draw/primitive order, multiple nested front/back glass surfaces, alpha decals over glass/paint, cutouts and emissive blended geometry. Capture small orbit sequences; mesh-center sorting must no longer decide the answer.
- [x] Keep selection/picking deterministic for transparent surfaces with a documented nearest-visible policy. Integer IDs must never be averaged; hidden opaque geometry and low-coverage overlays need explicit tests.
- [x] Bound peel/layer count, render-target allocation and work. Record behavior when the limit is exceeded, use the existing GPU budget, and test resize/cancel/device loss/low-memory fallback. A sorted fallback must not silently become the usual high-quality path.
- [x] Use Car windshield/side windows/headlights and Camera lens as paired visual cases; preserve the Camera behavior while fixing the car. Add a deliberately permuted-order regression test that fails the old implementation.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

### Verification economy

The front-matter gate launches a real GPU viewer and takes minutes. Spend runs
deliberately:

- Run it in the foreground with an explicit long timeout (900000 ms or more).
  Do not background it and poll with short `Start-Sleep` loops; if the tool
  moves it to the background, wait once for completion instead of polling.
- Iterate with `--fast` (add `--no-build` once the binary is current). `--fast`
  keeps the build, generated fixtures and this task's focused lane, and skips
  the contract self-tests, external corpus, F3D references and review gate. It
  is iteration evidence, not acceptance evidence.
- Keep agent-run full gates to at most two: one early proof of the lane and
  one final after the last edit. The harness repeats the front-matter command
  once after you finish. Corpus or baseline re-promotion is one run, not a
  per-fix loop.
- Use focused unit/import filters between runs. Do not re-run a passing gate to
  "confirm" it, and do not repeat a failing configuration without a change.

## Done when

- [x] T26 verification includes order-permutation/image/motion tests and depth/picking/resource-lifecycle checks.
- [x] The report names the algorithm, caps, fallback and measured GPU/memory cost; reviewed crops show no order-dependent window popping within the supported layer range.
- [x] Update transparency/resource design and hand-off.

## Out of scope

Ray tracing, unbounded per-pixel linked lists, and claiming that OIT alone solves physically based transmission.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

### Implemented behavior

**Algorithm: bounded weighted blended OIT (WBOIT)**, not depth peeling and not
an exact per-layer stack. `D3D12ViewerPath` renders opaque/masked geometry first
with depth writes and the `R32_UINT` pick target. One transparent triangle pass
then writes:

- `RGBA16F` accumulation: `rgb = color * coverage * weight`, `a = coverage * weight`;
- `RGBA16F` revealage: `(coverage, 0, 0, coverage)` with
  `ZERO / INV_SRC_ALPHA` blending.

The weight is scene-relative (`gViewport.z` = eye distance) and clamped to
`[1e-2, 300]`. The full-screen resolve samples both targets and composites
`scene * reveal + averageColor * (1 - reveal)` in **linear HDR before exposure
and tone mapping**. Opaque/masked material writes force display alpha to 1
after the authored cutout; only BLEND writes carry coverage.

Caps and fallback:

- Supported validated overlap range: **8 layers** (`kOitSupportedLayerLimit`).
  Beyond it the result remains order-independent but is a weighted average, not
  an exact front-to-back composite. This is documented in the profile/report
  and ADR-0084; it is never called exact.
- Memory: two `RGBA16F` targets. Measured **18,219,008 bytes** at 1280×900
  (`OitTargetBytesForExtent`), included in `AccountedAllocationBytes` and the
  existing DXGI budget. No per-pixel linked list, peel count or per-layer
  target allocation.
- Work: one extra transparent geometry pass; no per-layer peel passes.
- Fallback: the historical stable-sorted straight-alpha path is used only when
  the OIT targets cannot be allocated (low memory/unsupported extent), an
  explicit Debug force is set, or a diagnostic channel is active. Fields
  243/244 report the effective algorithm and reason (0 none, 1 allocation,
  2 forced, 3 extent, 4 device, 5 diagnostic). A sorted fallback therefore
  cannot silently become the usual high-quality path.

Transmission/reflection hand-off to T27: coverage, interface reflection and
transmission stay separate material inputs. Diagnostic channel 22 still exposes
`(alpha, glassReflectance, transmission)`. OIT only stabilizes the composite;
it provides **no refraction**. T27 should add transmitted-scene sampling, tint
and rough refraction on top of this stable coverage/composite path.

Selection/picking: a separate nearest-visible pass writes only the integer pick
target and writes depth, reusing the material shader's discard. The pick id is
`instanceId ? instanceId : chunkId` (the reported id is the low 16 bits in the
current scene-snapshot namespace). IDs are never averaged. A low-coverage decal
and a covered cutout are selectable; hidden glass behind opaque paint and back
nested-glass sheets are absent from the pick census. Equal-depth ties use the
stable material/chunk draw order.

Bug fixed during T26: the shader forced base-color texture alpha to 1 for
non-layer materials, so textured `MASK` cutouts could never discard. It now
preserves sampled alpha for MASK/BLEND; the opaque/masked display alpha is
still forced to 1 after the cutoff.

### Verification (actual commands/results)

```powershell
# Full gate (final run, pass)
python tests/render-quality/verify.py --task T26 --configuration Debug --output TestResults/render-quality/T26
# status pass; 11 captures; 0 comparisons; external qualification available
# layeredTransparency: 72/72 checks pass; serial unit/validator/hostile lanes pass

# Focused lane (pass, 72 checks)
python -c "... layered_transparency.run(..., fast=False)"

# Unit/source contract
.\x64\Debug\Tests.Unit.exe "[layered-transparency]"
# 5 cases / 1235 assertions pass
```

Measured evidence from the final gate:

- OIT permutation MAE: **0.0**; sorted-fallback permutation MAE:
  **0.00023999** (the old implementation changes under the same permutation).
- OIT vs fallback (content-adaptive crops): Car front windshield luma 90.06 vs
  90.12; oblique 82.25 vs 82.21; Camera lens 25.47 vs 25.47; permutation MAE
  0.0 for both.
- Small orbit (-8°, 0°, +8°): all OIT/permuted MAE ≤ 1.2e-5; each frame
  non-blank.
- Resize: OIT target bytes 18,219,008 → 10,878,976 → 18,219,008.
- Device removal: recovered with the model and OIT targets rebuilt.
- Low-memory simulation (`PREVIEW3D_TEST_OIT_ALLOCATION_FAILURE=1`):
  algorithm 0, reason 1, zero OIT bytes, non-blank sorted fallback.
- Mean/p95 frame ms in the capture session: OIT 20.9/112.1,
  forced fallback 26.5/183.7 (not a reference-hardware benchmark).

### Images/review

- `tests/render-quality/profiles/T26.json`
- `tests/render-quality/review/T26-review.json` (image-capable agent,
  Symphony vision disabled, provider settings unchanged)
- `TestResults/render-quality/T26/images/` — layered OIT/permuted/fallback
  content crops, orbit frames, resize/device-reset/low-memory frames,
  Car front/oblique content+windshield crops, Camera content+lens crops.
- Observed evidence only; no accepted baseline or reference was promoted.

### Build/design changes

- `D3D12ViewerPath.{h,cpp}`: OIT targets/PSOs, resolve composite, transparent
  draw split, permutation toggle, nearest-visible pick pass, diagnostics,
  base-color alpha/cutout fix.
- `RenderThread.{h,cpp}`: atomic transparency/pick diagnostics, forced fallback
  and permutation controls.
- `Preview3D.cpp`: Debug test-control fields 243–258 (algorithm/reason/cap/
  draws/bytes, forced fallback, permutation, resolve diagnostic, pick id/
  completions/normalized/absolute pick, viewport rect). Guard remains
  `PREVIEW3D_ENABLE_TEST_CONTROL` + `--app-smoke`, max field 258.
- `tests/render-quality/{fixtures.py,layered_transparency.py,profiles/T26.json,
  verify.py,viewer.py}`; `tests/unit/LayeredTransparencyTests.cpp`.
- Design: ADR-0084; updated `04-rendering-and-streaming.md`,
  `03-file-formats-and-ingestion.md`, ADR-0074 consequence.
- No accepted baseline promotion. External assets remain restricted/local.

### Remaining work / T27 notes

- T27 must add transmitted-scene sampling, volume tint and rough refraction;
  do not reuse the WBOIT average as a refraction approximation.
- The validated layer range is 8; a future task may tune weights or add an
  exact peel path if evidence demands it, but must not silently replace WBOIT.
- The generic evidence capture helper still has a 150% DPI viewport-crop
  mismatch (logical PrintWindow crop vs physical swap chain). T26 content crops
  and the pick census work around it; T20/T33 should make the capture helper
  DPI-consistent.

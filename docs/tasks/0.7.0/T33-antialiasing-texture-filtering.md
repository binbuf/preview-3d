---
timeoutMin: 180
verify: python tests/render-quality/verify.py --task T33 --configuration Debug --output TestResults/render-quality/T33
---
# T33 — Edge, specular and texture sampling quality

## Goal

Remove jagged silhouettes and unstable highlights while retaining crisp textures and UI.

## Context (read first)

- Depends on **T24, T25, T26, T27, T31, T32**; execute in roadmap order.
- `D3D12ViewerPath.cpp` single-sample scene/pick targets, resolve and samplers.
- Texture semantic/mip generation and streaming code; F3D FXAA/SSAA pass construction.
- T20 thin-edge/orbit fixtures, Camera engraving, Car grille/windows, microscope bright rings.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [x] Implement and justify a suitable default edge-AA path compatible with HDR, transparency and D3D12/D3D11On12 overlay ordering. Evaluate a sharper spatial solution or supported MSAA/resolve against FXAA; retain a deterministic supersampled reference for validation. TAA is optional only with complete reprojection/history handling.
- [x] Handle R32_UINT picking separately with a nearest-visible/coverage-aware policy; never average instance IDs. Keep UI/text overlays crisp and outside scene AA. Verify transparency/OIT resolve ordering and mask/cutout coverage.
- [x] Respect authored nearest/linear/mipmap samplers; add bounded anisotropic filtering where appropriate for oblique textures without overriding explicit nearest semantics.
- [x] Audit mip generation by texture semantic: linear-light color reduction, normalized normal data, roughness/specular variance handling and coverage-preserving alpha cutouts. Reduce subpixel specular sparkle without globally blurring sharp materials or raising every material's authored roughness.
- [x] Use stills and matched small-motion/orbit sequences at physical 1080p/1440p, multiple DPI and fractional resize. Inspect thin wires, grille, lens lettering, chrome edges, normal-map detail and glancing ground grids; global MAE alone is insufficient.
- [x] Measure frame/memory costs and quality fallbacks; include target/sampler lifecycle, low-budget and device-loss tests.

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

- [x] T33 verification includes AA/picking/mip invariants and reviewed edge/detail/motion comparisons against the supersampled reference.
- [x] Aliasing/sparkle improves in every mode without blurry text, lost engravings, glass halos or changed cutout coverage.
- [x] Update sampling/resolve design and hand-off.

## Out of scope

A postprocess blur used as a substitute for correct materials/normals, unbounded supersampling as the default, or claims of temporal AA without history correctness.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

### Implemented behavior

- **Default AA.** A bounded seven-tap depth/coverage-aware spatial filter runs
  in the existing full-screen resolve against the single-sample HDR scene, the
  D32 depth SRV and the OIT coverage. Geometry/occlusion and coverage
  discontinuities get a 0.55 blend; flat-depth texture detail gets 0.12;
  low-contrast texture interiors are untouched. No extra render target is
  allocated. `viewer_aa` (`interactive-viewer/src/render/AntiAliasing.{h,cpp}`)
  owns the thresholds and the CPU mirror.
- **MSAA/FXAA/SSAA evaluation.** MSAA was rejected because the opaque pass
  writes project-owned scene + AO indirect + `R32_UINT` pick in one MRT set;
  D3D12 requires one sample count across bound targets and a resolved integer
  id would average ids. Bounded 2× window supersampling is retained only as a
  deterministic validation reference. TAA is not implemented.
- **Picking/UI.** The pick target is never sampled by AA; the
  D3D11On12/Direct2D overlay is drawn after the resolve. AA on/off returns
  identical pick ids/hit flags and a byte-identical toolbar text region.
- **Sampling.** Linear/mipmap slots use a bounded 4× anisotropic sampler;
  authored nearest slots keep the point sampler. `GenerateRasterMips` reduces
  Color/Emissive in linear light; the base-color map (`ColorCoverage`) is
  coverage-weighted alpha while other sRGB lobe colors keep a plain average.
  Normals renormalize, `MetallicRoughness` green uses GGX-alpha RMS and Data
  stays a plain linear mean. glTF/OBJ/FBX/USD and OpenUSD route base-color and
  MR maps to the new semantics.
- **Diagnostics/lifecycle.** Debug fields 300-305 expose AA enable, effective
  mode, fallback, additional target bytes, tap count and resolve record time.
  Depth SRV recreation on resize/device loss and the explicit AA-off fallback
  are covered. `viewer.py` gained `resize_client` and corrected `pick_image`
  physical-coordinate mapping.

### Deviations

- Whole-frame MAE against the 2× reference is recorded but is not the verdict:
  the 2× render samples different texture LODs/pixel centres. Per-region edge
  MAE, AA on/off pairs, picking/UI equality and cutout/OIT invariants own
  acceptance.
- The first edge-probe version put text/normal/MR/cutout in one four-image
  material set; the fourth slot rendered with the wrong map (blue). The edge
  probe now keeps one map per visible semantic, and MASK coverage is verified on
  the established `ao-probe`; the four-image slot behaviour is recorded as a
  follow-up, not hidden.
- Simulated DPI (field 67) changes chrome layout without changing the physical
  capture DPI, so those images are lifecycle/layout evidence, not first-open
  framing evidence.

### Commands and results

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' `
  Preview3D.slnx /t:Preview3D /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' `
  tests/unit/Tests.Unit.vcxproj /p:Configuration=Debug /p:Platform=x64 `
  /p:SolutionDir=D:\repos\binbuf\preview-3d\ /m /v:minimal
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' `
  tests/import-isolation/Tests.ImportIsolation.vcxproj /p:Configuration=Debug `
  /p:Platform=x64 /p:SolutionDir=D:\repos\binbuf\preview-3d\ /m /v:minimal
.\x64\Debug\Tests.Unit.exe "[anti-aliasing]"
.\x64\Debug\Tests.ImportIsolation.exe "[texture-mips]"
python tests/render-quality/verify.py --task T33 --configuration Debug `
  --output TestResults/render-quality/T33
```

- Focused unit lane: 35 assertions in 7 test cases passed.
- Focused import lane: 19 assertions in 3 test cases passed.
- Full gate: **pass**, 0 failures, 30 captures, external qualification
  `available`, F3D reference `available`, review `recorded`, 75/75 focused
  checks.
- Fast lane: pass, 13 captures (iteration evidence only).
- Full-gate AA pairs: Studio 8,145 changed pixels (0.98 %), Clay 3,535
  (0.43 %), Directional 8,409 (1.01 %); background mean delta 0.0. SSAA region
  edge MAE improves in every inspected region in all three modes (e.g. Studio
  wires 86.76→85.56, grille 47.43→46.39, digits 109.85→108.73, chrome
  64.85→64.24, normal 59.01→58.32, lens 104.69→103.58, grid 31.33→30.47).
- Picking: five scene points, identical ids/hit flags AA on/off. UI: toolbar
  text max delta 0. Cutout coverage delta <1 %; OIT algorithm 1, AA change set
  0.2 % of the layered-glass frame.
- Sizes/DPI: exact physical 1920×1080, 2560×1440, 1537×901 viewport crops and
  simulated 96/144/192 DPI. Three orbit steps change 0.94-1.09 %.
- Lifecycle: resize keeps mode 1/7 taps; forced off mode 0/reason 1; device loss
  rebuilds depth SRV and returns to mode 1. AA target bytes 0; resolve record
  15-28 µs. Whole-frame Debug medians varied from +0.056 ms to -16.8 ms across
  runs because adaptive refresh/settle work dominates; frame time is recorded,
  not asserted and not a release claim.

### Docs changed

- `docs/design/04-rendering-and-streaming.md` (AA, picking/overlay ordering,
  sampler/mip policy)
- `docs/design/adr/0090-depth-aware-edge-aa-and-semantic-sampling.md` (new)
- `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md` (T33 disposition)
- `docs/0.7.0/progress/T33.md` (reusable evidence/follow-ups)
- `tests/render-quality/review/T33-review.json` (reviewed images/defects)

### Remaining work

Nothing required for T33. T34/T35 should record release-hardware AA/resolve
timing, keep per-region edge evidence, and decide whether a temporal solution
or a four-image material-slot investigation is needed before release evidence.

---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T24 --configuration Debug --output TestResults/render-quality/T24
---
# T24 — Correct BRDF, environment integration and clearcoat layering

## Goal

Make metal, dielectric and coated-material response correct and consistent before tuning presets.

## Context (read first)

- Depends on **T20, T21, T22, T23**; execute in roadmap order.
- `D3D12ViewerPath.cpp::EvaluateLight`, `SampleStudioSpecular`, `PSMain` and HDR resolve.
- `render/StudioEnvironment.cpp::ProjectIrradiance`, `MaterialPayload.h` lobe helpers and current tests.
- Khronos clearcoat/specular/IOR references, Filament IBL treatment and PBR Neutral reference.
- F3D `vtkF3DCachedLUTTexture`/`vtkF3DCachedSpecularTexture` and their VTK PBR base classes: F3D's light configuration alone does not implement its BRDF.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [x] Replace Fresnel-only prefiltered environment response with a validated split-sum BRDF integration LUT or equivalent documented model, with correct roughness/NoV conventions and energy handling. Test the prefilter and LUT independently against numerical/reference integration.
- [x] Fix cubemap irradiance integration to use texel solid-angle weights; test constant and strongly directional environments and rotations, not only the same production formula copied into a test.
- [x] Replace `(1-clearcoatFactor)` base-specular removal with Fresnel-weighted, energy-consistent layer attenuation per Khronos. Account for base diffuse/specular and emission beneath the coat. Respect specularFactor/specularColor and IOR semantics at normal and grazing angles.
- [x] Make Studio and Directional use the same material BRDF and correctly Fresnel-weighted IBL. Remove the unscaled extra transmission reflection and prevent double Fresnel/double-counted reflection. Keep physical transmission changes coordinated with T27; diagnostics must expose any interim approximation.
- [x] Validate zero-light/zero-environment, roughness/metallic/IOR/specular/coat sweeps, white-furnace energy, black and white dielectrics, coated metal and high emission using actual GPU numeric patches. Test that environment intensity zero removes every environment contribution.
- [x] Verify CPU/GPU tone-map agreement, linear-light blend order, exactly one display encoding and neutral-color/hue retention. Remove/update tests that enshrine the old clearcoat attenuation or only search shader text.

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

- [x] T24 verification includes independent numeric reference checks and GPU image/readback lanes for all three modes, including Car paint and Camera lens nonregression.
- [x] Document measured tolerances and remaining lobe approximations; no exposure/preset change may conceal a failing material invariant.
- [x] Update rendering/material design and hand-off.

## Out of scope

Art direction of default softboxes (T28), texture-driven lobes (T25), new transparency algorithm or volume/path tracing.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

**Status: done.** Shared split-sum IBL, solid-angle irradiance, Khronos
clearcoat layering, corrected KHR_materials_specular/IOR semantics and the T24
GPU evidence lane landed.

### Implemented behavior

- **Split-sum environment BRDF.** `StudioEnvironment.cpp` bakes a 64²
  R32G32_FLOAT DFG LUT (`A`,`B`) with the documented GGX integration.
  `EnvironmentBrdfFresnel` returns `F0·A + B` plus the Fdez-Aguera
  multiple-scattering term, so white metal is the exact white-furnace identity.
  The shader samples it at `(NoV, perceptual roughness)` and uses
  `prefiltered × scale` for the environment specular lobe.
- **Solid-angle irradiance.** `ProjectCubeIrradiance` uses the exact analytic
  per-texel cubemap solid angle; all six faces sum to `4π`. Constant,
  directional and rotated environments are checked against an independent
  Fibonacci-sphere reference.
- **Prefilter reference.** `PrefilterSpecular` is the shipped GGX importance
  sampler; `ReferencePrefilteredRadiance` integrates the same documented
  integral with uniform half-vector quadrature.
- **Clearcoat.** Base diffuse/specular/sheen are mixed toward the coat by
  `clearcoatFactor × F_schlick(coatF0, 1, NoV)`; emission beneath the coat is
  attenuated by `(1 − cF)`. The coat direct/IBL lobes no longer carry their own
  Fresnel, and the old flat `(1 − clearcoatFactor)` base-F0 removal is gone.
- **Specular/IOR.** `F0 = ((ior−1)/(ior+1))² × specularColor` and
  `F90 = specularFactor` are independent (Khronos), replacing the old
  specularFactor-scaled F0. Direct light uses height-correlated Smith GGX.
- **Studio/Directional consistency.** Both modes call the same base IBL,
  direct BRDF and coat helpers. Directional uses its sun for base and coat and
  no longer adds the unweighted extra transmission reflection.
- **Transmission.** Keeps the interim alpha-carried interface Fresnel and
  un-Fresneled environment reflection (T27 owns the physical path). Numeric
  channel 22 exposes `(alpha, glassReflectance, transmission)`.
- **Diagnostics.** Channels 21 (`F0·A+B` scale), 22 (transmission composite)
  and 23 (flat 0.3 linear display-calibration patch); Debug fields 231/232
  report the exposure/tone-map pair the last resolve actually used.

### Deviations / approximations

- The independent LUT/prefilter uniform-quadrature reference is finite-sampled
  below roughness ≈0.4. Production importance sampling covers that range; the
  white-furnace identity and monotonic checks cover it separately. Recorded in
  ADR-0082 and the profile `knownDefects`.
- Transmission remains an alpha-composited approximation with no transmitted
  scene sampling, refraction or tinted volume; T26/T27 own it.
- Car windshield/side-glass ordering and directional brightness remain known
  T26/T27 defects. T24 only establishes shared BRDF/IBL and layering.

### Commands and results

All run from `D:\repos\binbuf\preview-3d`, Debug x64:

```powershell
# Front-matter acceptance command (builds viewer + Tests.Unit, full lane)
python tests/render-quality/verify.py --task T24 --configuration Debug --output TestResults/render-quality/T24
# -> status pass, 0 failures, 12 captures, 575/575 brdf checks, 3 serial unit lanes,
#    import [gltf-extensions] 143 assertions / 8 cases, review recorded.

# Focused iteration (not acceptance evidence)
python tests/render-quality/verify.py --task T24 --configuration Debug --output TestResults/render-quality/T24-dev --fast --no-build
# -> status pass

# Focused CPU reference lanes
.\x64\Debug\Tests.Unit.exe "[brdf]"
.\x64\Debug\Tests.Unit.exe "[material-lobes],[studio-ibl]"
.\x64\Debug\Tests.ImportIsolation.exe "[gltf-extensions]"
```

Release viewer builds after the shader/resource changes
(`MSBuild Preview3D.slnx /t:Preview3D /p:Configuration=Release`); the Debug
test-control handler remains compiled out of Release.

### Evidence

- `TestResults/render-quality/T24/report.json` — wrapper result.
- `TestResults/render-quality/T24/diagnostics/brdf.json` — 575 checks,
  material records, per-probe channel samples, zero-environment sweep,
  external Car/Camera surveys and T21 nonregression ratios.
- `TestResults/render-quality/T24/diagnostics/channels/` — environment
  specular, coat, emission, HDR, BRDF-scale and transmission-composite views.
- `TestResults/render-quality/T24/images/` — generated brdf-probe Studio,
  Clay, Directional and default captures.
- `TestResults/render-quality/T24/external/` — Car and Camera in all three
  modes from the standalone numeric lane.
- `tests/render-quality/review/T24-review.json` — inspected images and
  review observations.

### Measured tolerances / invariants

- White metal environment BRDF scale: 0.99992 (target 1.0).
- Flat 0.3 linear patch: GPU display `[139,139,139]`, CPU reference
  `[139,139,139]`, no hue shift.
- Black dielectric scale 0.0400; IOR 2.0 0.1111 vs IOR 1.5 0.0400;
  specularFactor 0 = 0.0000; tinted specular R>G>B.
- Studio vs Directional IBL channel delta: 0.00 for all 16 probes.
- Coated vs uncoated base environment specular: strictly lower on the coated
  probe; emission under coat 6.91 vs 7.20 linear (still >1).
- Environment intensity 0: every env/coat channel ≤1e-4; non-emissive HDR
  black; emission survives.
- Car paint environment specular 0.97× the T21 baseline; Camera lens 0.58×
  (both inside the recorded [0.2,5.0] nonregression band).

### Next task notes

- T25 can add texture-driven clearcoat/sheen/specular/transmission maps on top
  of the same scalar-lobe layering; do not reintroduce flat `(1 − c)` removal.
- T26/T27 should replace the channel-22 interim transmission composite with a
  transmitted-scene/tint path and preserve the Fresnel-weighted opaque lobes.
- T28/T30 may tune environment/sun intensity, but the material invariants in
  `[brdf]` and the T24 numeric lane must remain unchanged.
- T34/T35 should run the same lane on reference hardware; the current timings
  and numeric values are local evidence only.


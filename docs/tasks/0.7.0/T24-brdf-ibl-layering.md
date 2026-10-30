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

- [ ] Replace Fresnel-only prefiltered environment response with a validated split-sum BRDF integration LUT or equivalent documented model, with correct roughness/NoV conventions and energy handling. Test the prefilter and LUT independently against numerical/reference integration.
- [ ] Fix cubemap irradiance integration to use texel solid-angle weights; test constant and strongly directional environments and rotations, not only the same production formula copied into a test.
- [ ] Replace `(1-clearcoatFactor)` base-specular removal with Fresnel-weighted, energy-consistent layer attenuation per Khronos. Account for base diffuse/specular and emission beneath the coat. Respect specularFactor/specularColor and IOR semantics at normal and grazing angles.
- [ ] Make Studio and Directional use the same material BRDF and correctly Fresnel-weighted IBL. Remove the unscaled extra transmission reflection and prevent double Fresnel/double-counted reflection. Keep physical transmission changes coordinated with T27; diagnostics must expose any interim approximation.
- [ ] Validate zero-light/zero-environment, roughness/metallic/IOR/specular/coat sweeps, white-furnace energy, black and white dielectrics, coated metal and high emission using actual GPU numeric patches. Test that environment intensity zero removes every environment contribution.
- [ ] Verify CPU/GPU tone-map agreement, linear-light blend order, exactly one display encoding and neutral-color/hue retention. Remove/update tests that enshrine the old clearcoat attenuation or only search shader text.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

## Done when

- [ ] T24 verification includes independent numeric reference checks and GPU image/readback lanes for all three modes, including Car paint and Camera lens nonregression.
- [ ] Document measured tolerances and remaining lobe approximations; no exposure/preset change may conceal a failing material invariant.
- [ ] Update rendering/material design and hand-off.

## Out of scope

Art direction of default softboxes (T28), texture-driven lobes (T25), new transparency algorithm or volume/path tracing.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


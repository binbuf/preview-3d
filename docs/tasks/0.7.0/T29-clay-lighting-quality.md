---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T29 --configuration Debug --output TestResults/render-quality/T29
---
# T29 — Clay as a coherent opaque geometry-review mode

## Goal

Give Clay clean, readable form with consistent material override and correct visibility/depth behavior.

## Context (read first)

- Depends on **T22, T24, T26, T28**; execute in roadmap order.
- `D3D12ViewerPath.cpp::PSMain`, authored-alpha draw sorting and PSO selection.
- `render/Presentation.*`, Clay UI semantics and T20 Clay reference fixtures.
- Observed Car Clay seams and Camera/strap/STL/CAD geometry.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Derive a per-mode effective material/render bucket: opaque clay for ordinary mesh surfaces including authored glass, with depth writes/culling/sort semantics consistent with that override. Do not only force alpha to one in a shader while retaining the authored Blend PSO.
- [ ] Define alpha-cutout behavior explicitly: preserve actual geometric/foliage holes when appropriate by sampling coverage independently of gray shading. Do not unintentionally reveal interior meshes or fill modeled holes.
- [ ] Use a controlled neutral matte BRDF, soft form lighting and stable exposure; suppress authored color, metallic, coat, sheen, emission, transmission and normal-map surface decoration. Preserve actual geometric normals/hard edges and correct double-sided orientation.
- [ ] Review material-boundary neutrality, body/strap/part separation, cavities and silhouettes on Car, Camera, microscope, base/lid/cartridge STL and a STEP assembly. Keep shadows/AO interfaces ready for T31/T32, without using them to conceal depth defects.
- [ ] Verify switching Studio→Clay→Directional→Studio does not mutate imported data, lose texture residency, alter camera/user exposure, break picking or leave stale pipeline state. Test mirrored instances, transparent primitives and mixed line/point scenes.
- [ ] Capture controlled and first-open Clay views; make the Clay lane assess form readability and visibility, not PBR texture similarity.

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

- [ ] T29 verification includes effective draw/depth-state GPU fixtures, alpha-cutout policy, mode round-trips and reviewed corpus crops.
- [ ] Car Clay has no blend-order seams/interior leakage caused by the override; small geometry and curved form remain readable without washed-out whites.
- [ ] Update Clay semantics/render design and hand-off.

## Out of scope

Changing source material data, normal-map authoring, or making Clay another textured Studio preset.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


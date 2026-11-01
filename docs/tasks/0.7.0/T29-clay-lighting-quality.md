---
timeoutMin: 180
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

- [x] Derive a per-mode effective material/render bucket: opaque clay for ordinary mesh surfaces including authored glass, with depth writes/culling/sort semantics consistent with that override. Do not only force alpha to one in a shader while retaining the authored Blend PSO.
- [x] Define alpha-cutout behavior explicitly: preserve actual geometric/foliage holes when appropriate by sampling coverage independently of gray shading. Do not unintentionally reveal interior meshes or fill modeled holes.
- [x] Use a controlled neutral matte BRDF, soft form lighting and stable exposure; suppress authored color, metallic, coat, sheen, emission, transmission and normal-map surface decoration. Preserve actual geometric normals/hard edges and correct double-sided orientation.
- [x] Review material-boundary neutrality, body/strap/part separation, cavities and silhouettes on Car, Camera, microscope, base/lid/cartridge STL and a STEP assembly. Keep shadows/AO interfaces ready for T31/T32, without using them to conceal depth defects.
- [x] Verify switching Studio→Clay→Directional→Studio does not mutate imported data, lose texture residency, alter camera/user exposure, break picking or leave stale pipeline state. Test mirrored instances, transparent primitives and mixed line/point scenes.
- [x] Capture controlled and first-open Clay views; make the Clay lane assess form readability and visibility, not PBR texture similarity.

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

- [x] T29 verification includes effective draw/depth-state GPU fixtures, alpha-cutout policy, mode round-trips and reviewed corpus crops.
- [x] Car Clay has no blend-order seams/interior leakage caused by the override; small geometry and curved form remain readable without washed-out whites.
- [x] Update Clay semantics/render design and hand-off.

## Out of scope

Changing source material data, normal-map authoring, or making Clay another textured Studio preset.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

### Implemented behavior

- **Effective bucket, not a shader alpha hack.** `D3D12ViewerPath::RenderFrame`
  derives a per-frame bucket in Clay. Authored BLEND and
  `kMaterialFlagTransmissive` triangles leave the transparent bucket and join a
  dedicated opaque overlay pass: depth test + write, `LESS_EQUAL`, a bounded
  negative depth bias and far-to-near ordering. Coplanar window/trim pairs
  resolve without z-fighting, hidden interiors are rejected, and the authored
  Blend/OIT PSO is not used. The imported material data is never mutated.
- **MASK cutouts stay holes.** Clay does not sample the base-color texture for
  shading, but it samples that texture's alpha solely for the MASK cutoff. The
  discard therefore survives the neutral material, and BLEND/transmissive
  coverage is forced to opaque.
- **Neutral matte.** Clay uses one gray diffuse albedo, zero specular F0/F90 and
  no environment specular, no clearcoat/sheen/emission/transmission maps, no
  normal or coat-normal maps, and a softened Studio key/fill/rim rig. Exposure,
  tone mapping, camera and the environment bake are untouched. Geometric
  normals, hard edges and double-sided back-face reversal are preserved.
- **Diagnostics.** Debug material fields 143-145 now report the effective
  bucket/depth/cull for the current mode; new fields 274-276 report the authored
  bucket/depth and whether Clay rewrote the draw. Records refresh once per mode
  change. The Debug-only message guard is now `wParam <= 276`.
- **Lane.** New `tests/render-quality/clay_mode.py` and `profiles/T29.json`
  cover the generated `clay-review` fixture (identity row, MASK cutout, glass
  hiding an interior, overlapping alpha sheets, opaque transmissive, small
  curved parts, mirrored instance), a line/point OBJ round-trip, a STEP
  assembly, Studio/Clay/Directional captures, and Car/Camera/microscope/base/
  lid/cartridge external crops. It asserts effective draw state, zero
  transparent/forward-transmission draws, neutral matte, cutout policy,
  interior occlusion, small/mirrored visibility, picking and imported-data/
  texture/camera/exposure stability across Studio->Clay->Directional->Studio.
- **Reference fixtures.** `clay-review` publishes real per-mesh POSITION bounds
  so automatic framing is centred; `line-point-scene.obj` is line/point-only
  because ufbx exposes OBJ `l`/`p` only through the adapter fallback; the STEP
  review uses the in-repository `tests/fixtures/stp-spike/assembly_ap214.stp`.

### Verification (exact commands and results)

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' `
  Preview3D.slnx /t:Preview3D /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
# -> Preview3D.exe built

python tests/render-quality/verify.py --task T29 --configuration Debug `
  --output TestResults/render-quality/T29
# -> status pass, 0 failures, 42 Clay checks, 38 captures,
#    externalQualification available, review recorded

.\x64\Debug\Tests.Unit.exe "[layered-transparency]"   # 1235 assertions / 5 cases, pass
.\x64\Debug\Tests.Unit.exe "[material-lobes]"         # 158 assertions / 7 cases, pass
.\x64\Debug\Tests.Unit.exe "[studio-lighting]"        # 79 assertions / 5 cases, pass
```

Iteration evidence: `verify.py --task T29 ... --fast` reached `pass`, 32 Clay
checks and 20 captures. Review record:
`tests/render-quality/review/T29-review.json`; controlled report:
`TestResults/render-quality/T29/diagnostics/clay-mode.json` and
`clay-mode-report.md`. Images inspected are listed in the review record.

### Deviations / limits

- The external Voron 241 MB STEP assembly exceeds the bounded STEP host budget
  in Debug (terminal `StepHostLimit`, stage ValidateSection, ~302 s). The CAD
  review therefore uses the small in-repository AP214 assembly; the external
  file stays a T34 corpus item, not a passing T29 fixture.
- Car still shows the pre-existing authored white speckles along windshield/
  roof edges in both Studio and Clay. They are not caused by the Clay override
  and are recorded as `car-authored-window-speckle` for T33/T34.
- Clay deliberately does not use authored ORM AO. Geometry-derived cavity and
  contact shading remains T31/T32 work.
- No accepted baseline was promoted. No per-asset/material-name exception was
  added.

### Next task notes

- T30 can build Directional on the same effective-state pattern but must not
  reuse Clay's opaque overlay: Directional keeps authored transparency and
  needs its own reflection/sun contract.
- T31/T32 can consume the Clay overlay bucket for depth/contact but must not
  re-enable authored AO as a substitute for geometry-derived occlusion.
- T33 should treat the Car window-edge speckle separately from Clay; the
  overlay bias is not meant to hide antialiasing/edge defects.
- T34/T35: the complete external matrix still needs a STEP budget/qualification
  decision for the Voron assembly.

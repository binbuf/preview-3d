---
timeoutMin: 180
verify: python tests/render-quality/verify.py --task T30 --configuration Debug --output TestResults/render-quality/T30
---
# T30 — Directional lighting, reflection and sun consistency

## Goal

Make Directional a controlled surface-inspection light with credible materials and a consistent visible sun direction.

## Context (read first)

- Depends on **T24, T27, T28, T29**; execute in roadmap order.
- `D3D12ViewerPath.cpp` Directional branch, clearcoat direct lights and coordinate transforms.
- `Preview3D.cpp` sun/gizmo azimuth/elevation, `render/GroundShadow.*`.
- T24 shared BRDF and T20 Directional scenes; observed bright Car trim/windows.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [x] Use one coherent sun/light transform for base and coat BRDFs, light gizmo and subsequent shadow passes. Define/document the coordinate convention; prefer a world-anchored sun and transform it correctly into camera space as the camera moves.
- [x] Remove leftover Studio coat/key directions and unweighted environment reflection. Keep a calibrated, Fresnel/BRDF-correct ambient reflection floor so metals remain visible without turning black paint/glass white.
- [x] Make light intensity zero and environment intensity zero behave correctly, including transmission/coat; maintain predictable exposure between modes. Test light azimuth/elevation sweeps and grazing angles without clipping, NaNs or black discontinuities.
- [x] Test camera orbit with a stationary sun, manual sun movement with a stationary camera and imported-up-axis changes. The gizmo, lit face, highlight and shadow input must agree.
- [x] Review Car paint/window/trim, Camera lens/black body, microscope brass/enamel and CAD relief at several sun elevations. Use local contrast/highlight crops and motion stability; keep dark-side detail without a flat ambient wash.
- [x] Preserve keyboard/accessibility and existing sun-control interactions; changing lighting mode must not reset user camera or persist an accidental per-asset light exception.

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

- [x] T30 verification covers directional transform/energy GPU invariants, sun-control state and reviewed still/orbit/azimuth sequences.
- [x] One documented light direction drives every Directional lobe and the T31 shadow interface; glass and black materials retain separation.
- [x] Update lighting/gizmo design and hand-off.

## Out of scope

Additional user-facing light editors, multiple imported light authoring controls or shadows whose source disagrees with the visible sun.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

### Implemented behavior

- **One documented world sun.** `interactive-viewer/src/render/LightRig.h`
  defines the viewer's right-handed Z-up convention: normalized azimuth 0..1
  around +Z (0 = +X, 0.25 = +Y, 0.5 = -X, 0.75 = -Y), elevation in radians
  clamped to 1.4835, and
  `direction = (cos(az)cos(el), sin(az)cos(el), sin(el))`.
  `viewer_lighting::DirectionalSunDirection` is the only constructor. The
  frame constant `gSunDirection`, the material shader, the compact legacy
  shader, `NavGizmo::ComputeSun` and `GroundShadow::ProjectShadow` all consume
  it (ADR-0087).
- **Correct frame.** The vertex path subtracts the camera origin in double
  precision but leaves positions/normals in the fixed world frame; the camera
  rotation is in `gViewProjection`. The material shader therefore uses the
  world sun directly. The old camera-space azimuth/elevation branch and the
  extra camera rotation were removed, so a stationary sun no longer orbits
  with the camera. The gizmo separately uses the camera orientation for its
  screen bearing/depth, which is presentation-only.
- **Base and coat agree.** Directional base and clearcoat direct lobes use the
  same `lightDir`; the old Studio key directions are gone from the Directional
  branch and the legacy shader. Directional keeps the T24 split-sum
  Fresnel/BRDF environment as its ambient reflection floor and adds no
  unweighted reflection. Sheen now scales with environment intensity instead
  of retaining a flat 0.35 ambient lift.
- **Shadow input.** `D3D12ViewerPath::RenderFrame` passes the Directional world
  sun to `ProjectShadow`; Studio keeps `kKeyLightDirection`. The analytic
  footprint is still bounds-based; T31 must reuse this direction for the
  geometry-derived shadow.
- **Sun state and intensity.** Debug fields 277/278 read/write the global sun
  azimuth/elevation as IEEE float bits (`lParam == -1` reads); the test-control
  guard was widened to 278. Mode switches and opening another asset keep the
  camera/sun state; no per-asset exception exists. Intensity zero removes
  direct, IBL, coat and the background-only transmission fallback; emission
  survives. The measured Studio/Directional mean-luma ratio is 1.095 at the
  fixed T30 camera (predictable exposure).
- **Preserved interactions.** The gizmo ring/sun drag, keyboard azimuth/
  elevation controls, accessibility controls and persisted preferences were
  not changed. The ground-axis command still refits the camera home framing;
  that is view state, not a light exception, and is recorded in the evidence.

### Verification

Exact commands and results:

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' `
  Preview3D.slnx /t:Preview3D /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' `
  tests/unit/Tests.Unit.vcxproj /p:Configuration=Debug /p:Platform=x64 `
  /p:SolutionDir=D:\repos\binbuf\preview-3d\ /m /v:minimal
.\x64\Debug\Tests.Unit.exe "[directional-light]"   # 380 assertions / 5 cases
.\x64\Debug\Tests.Unit.exe "[gizmo]"               # 830 assertions / 8 cases
.\x64\Debug\Tests.Unit.exe "[ground-shadow]"       # 150 assertions / 7 cases
python tests/render-quality/verify.py --task T30 --configuration Debug `
  --output TestResults/render-quality/T30
```

The final front-matter gate **passes**: 639/639 Directional checks, 18 captures,
external qualification `available`, reference `available`, review `recorded`,
no failures. Build/unit/import build exit codes were 0. The T30 focused lane
also passes standalone with `--fast` (471 checks, 9 captures); that is
iteration evidence, not the acceptance run. The review record is
`tests/render-quality/review/T30-review.json`.

### Evidence

- `TestResults/render-quality/T30/report.json`
- `TestResults/render-quality/T30/diagnostics/directional.json`
- `TestResults/render-quality/T30/images/directional-front-lit.png`,
  `directional-orbit-yaw-000/020/040/060.png`,
  `directional-azimuth-0750/0875/1000/0250.png`,
  `directional-elevation-015/050/120.png`,
  `directional-zero-intensity.png`, `directional-up-axis-y/z.png`
- `TestResults/render-quality/T30/external/{car,camera,microscope,stl}-e022/e062/e120-{front,back}.png`
- Generated capture lane:
  `TestResults/render-quality/T30/images/generated-directional-probe-*`,
  `generated-stl-box-directional-oblique.png`,
  `generated-step-assembly-directional-front.png`

Reviewed observations: the Lambert wall response stays 1.038 across the
0/20/40/60-degree orbit; azimuth moves the lit face and coat together; floor
response rises with elevation; zero intensity leaves only emission; gold metal
keeps 0.021 env-specular luma vs 0.0012 for black paint; glass remains separate;
Car paint/trim/windshield, Camera lens/black body, microscope brass/enamel and
STL/STEP relief keep dark-side detail and bounded highlights. The known Car
window/roof speckles are recorded as pre-existing. No accepted reference was
promoted.

### Deviations and remaining work

- None in scope. The analytic ground ellipse remains a T16/A-0072 limitation;
  T31 owns geometry-derived shadows and must call
  `viewer_lighting::DirectionalSunDirection` for the Directional input.
- The external microscope loaded completely in this run; a future terminal
  ResourceLimit remains the T23/T34 known-defect observation, not quality
  evidence.
- Performance: the CPU light-rig addition is a handful of trigonometric
  operations per frame; no new GPU allocation, target or pass was added. The
  full gate runtime remains dominated by the GPU capture lane.


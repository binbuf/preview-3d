# 0.7.0 rendering quality follow-up

Prepared 2026-10-30 after T19. This is an AI-assisted source review and local
visual investigation, followed by an executable task plan; the proposed fixes
have **not** been implemented. T01–T19, including T10b, remain completed.
T20–T35, plus the later user-requested appearance task T21a, extend the same
task set with a separate rendering acceptance gate.

## Recommendation

User appearance requirement (2026-10-30): restore the original Windows
Photos-style dark background by default. Add a Settings Background dropdown
with custom color selection and an independent Ground highlight toggle;
keep a subtle base highlight enabled by default. Both preferences persist
across models, restarts and lighting/presentation modes, and later quality
work must preserve them. [T21a](../tasks/0.7.0/T21a-native-background-settings.md)
runs immediately after the active T21, before further material/lighting work.

The current Dark background is lighter partly because `BackgroundLinear`
returns #1C1C1E's sRGB codes as linear values and the resolve then tone-maps
and sRGB-encodes them. Some presets also select MidGrey, and the ground shader
adds a broad bright plane. T21a fixes the display-color contract and exposes
the user choices; changing model-light intensity is not the solution.

Fix material interpretation, light transport, transparency and sampling before
polishing presets. Then qualify **Studio, Clay and Directional individually**
on real assets and controlled fixtures. A successful import, compiled shader,
or screenshot that differs from another mode cannot establish image quality.

The highest priorities are T20–T24 and T26–T27: trustworthy comparisons,
diagnostics, material semantics, correct BRDF/IBL layering, transparency and
transmission. T25 closes texture-driven material gaps. T28–T33 finish the
lighting modes, shadows, AO and sampling. T23 investigates a late resource-limit
failure; T34–T35 establish release evidence.

## Evidence and limits

- Preview3D source: `fd15e29`; Release viewer built 2026-10-30 08:09 local time.
  The inspection used a byte-for-byte executable copy named
  `Preview3DQualityReview.exe` beside the original, because the native capture
  helper confused the development executable with the installed app's shell
  identity. The copy changes no code, settings, worker boundary or parser.
- F3D source studied: `D:/repos/github/f3d`, commit
  `7f32e1f3e02fecbcc0cc2ba74a97bcb18cc28e58`, declaring 4.0.0.
- F3D binary actually rendered: `C:/Program Files/F3D/bin/f3d.exe`, **3.5.0**,
  VTK **9.6.1-1336-gdc285c2254**, build 2026-04-05. Do not attribute every
  source-main feature to this older installed binary.
- Fresh Preview3D captures inspected: Car in all three modes; Camera in Studio
  and front Studio/Clay/Directional; microscope during progressive loading and
  its subsequent ResourceLimit refusal. Existing T14 sphere
  images in all modes and T16 CAD/ground evidence were also inspected. This
  was **not** a complete qualification of the external model directory.
- Fresh F3D captures inspected: Car and Camera with the explicitly selected
  shipped preset, plus controlled no-config/IBL captures. The installed
  `share/f3d/configs/config.d/05_all.json` enables `hdri-ambient`,
  `tone-mapping`, `anti-aliasing: fxaa`, `blending: ddp`, grid and an oblique
  camera. An unconfigured CLI render is a different reference condition.
- These exploratory captures have different framing, environment, UI and
  physical resolution. F3D's DPI-aware preset produced 1920×1350 for a
  requested 1280×900 on this machine. They support qualitative observations,
  **not** cross-renderer MAE, brightness ratios or formal quality scores.

Local, ignored evidence is in `TestResults/render-quality-review/`:

- [Local side-by-side gallery](../../TestResults/render-quality-review/index.html)
  — original captures with conditions and limitations.
- [Capture/binary identity manifest](../../TestResults/render-quality-review/evidence.json)
  — hashes, renderer versions, GPU/driver and unmatched-camera disclosure.

| Preview3D | F3D reference | What to inspect |
| --- | --- | --- |
| `preview-car-studio.png` | `f3d-car-shipped-preset.png` | Busy, bright windshield and window edges; paint/trim highlights |
| `preview-car-directional.png` | Same reference, different rig | Reflection washout and loss of dark-material separation |
| `preview-car-clay.png` | T14 Clay spheres for mode context | Pale form and dotted/broken-looking seams on authored transparent surfaces |
| `preview-camera-studio.png`, `preview-camera-studio-front.png` | `f3d-camera-shipped-preset.png` | Preserve the dark lens with visible reflection/depth; F3D's lens appears as a flat pale disk here |
| `preview-camera-clay-front.png`, `preview-camera-directional-front.png` | Same asset, mode comparison | Separation of body, strap and small geometry; exposure and edge quality |
| `preview-microscope-studio.png`, `preview-microscope-resource-limit.png` | `f3d-microscope-shipped-preset.png` | Initial geometry/metal appearance, followed by a terminal ResourceLimit; the partial image is not successful-load evidence |

The external models and their screenshots stay out of the repository/package
until redistribution rights are established. Car metadata identifies a
CC-BY-NC-SA-4.0 Sketchfab asset; access to the local corpus does not license it
as a shipped sample. T20 records hashes, provenance and local references;
portable CI uses generated or appropriately licensed fixtures.

## Findings grounded in source

| Priority | Finding | Evidence | Follow-up |
| --- | --- | --- | --- |
| P0 | The visual lane can bless its own output. Missing goldens are written automatically; most T14 checks establish that modes/orbit change pixels. T18 tests include shader-string checks and compilation. | `tests/app-smoke/studio.py:314–335`, `studio-presets.py:200–218`, `tests/unit/MaterialLobesTests.cpp` | T20–T21, T35 |
| P0 | Alpha coverage and physical transmission are collapsed into one blend value; no transmitted scene sampling, tint or rough refraction. OPAQUE transmission is rewritten to Blend. | `import-worker/src/GltfAdapter.cpp:1300–1347`; `D3D12ViewerPath.cpp:533–544` | T22, T26–T27 |
| P0 | Mesh-center sorting cannot resolve interpenetrating surfaces or triangles within one transparent primitive. | `D3D12ViewerPath.cpp:2231–2241`, blend PSOs with no depth writes; F3D preset's `blending: ddp` and `vtkF3DRenderPass.cxx` | T26 |
| P0 | A fully coated material loses its entire base specular F0 through `(1-clearcoat)`, rather than Fresnel-weighted layer attenuation. The coat itself remains sharp, hiding the underlying lobe error. | `D3D12ViewerPath.cpp:522–547,582–594`; `MaterialPayload.h` helper; Khronos clearcoat layering | T24 |
| P0 | Directional adds environment specular without the material Fresnel/BRDF factor; its extra transmission reflection also omits environment intensity. Coat direct lights still use the Studio directions. | `D3D12ViewerPath.cpp:553–594` | T24, T30 |
| P1 | Prefiltered environment sampling is multiplied by Fresnel alone, without a split-sum BRDF integration term. SH projection uses equal cubemap texel weights, although cubemap texel solid angles differ. | `SampleStudioSpecular`, `PSMain`; `StudioEnvironment.cpp::ProjectIrradiance` | T24 |
| P1 | Core material information is discarded: normal scale is explicitly dropped; there is one shared UV transform and four texture slots; AO is read from MR red even though glTF AO is independently specified. | `GltfAdapter.cpp::ResolveMaterial`, `MaterialPayload.h`, shader texture slots and `OcclusionFactor` | T22 |
| P1 | ~~Clearcoat, sheen, specular and transmission texture inputs are warning-only. Constants are insufficient for many authored assets.~~ Resolved by T25: protocol v14 carries thirteen fixed material slots, the shader samples all eight extension maps with their own channel/color-space/sampler/UV metadata, and the T25 lane proves per-texel GPU variation. | T18 hand-off, `GltfAdapter.cpp`, material wire/dependency contract | T25 |
| P1 | Clay changes shader color/alpha, but draw sorting and blend/depth PSO selection still depend on the authored alpha mode. It also retains a reduced environment specular term. | `D3D12ViewerPath.cpp:480–509,573,2231–2268` | T29 |
| P1 | The ground shadow is a bounds-projected ellipse, not a projection of model geometry. AO means authored ORM modulation, not contact/cavity SSAO. | `render/GroundShadow.{h,cpp}`, ground shader and T16 hand-off | T31–T32 |
| P1 | Scene targets and PSOs are single-sample, and there is no edge-AA pass. Very bright, low-roughness highlights expose geometric and specular aliasing. | `D3D12ViewerPath.cpp` scene target/PSO creation and HDR resolve | T33 |
| P1 | Preset decisions use material counts, not visible surface contribution, and can select a checker or scan presentation unexpectedly. The procedural environment mixes warm/cool panels with a camera-relative direct rig. | `Presentation.cpp::SelectPresentationPreset`, `StudioEnvironment.cpp::ProceduralStudioRadiance`, shader direct lights | T28–T30, T34 |
| P0 | The microscope becomes visible before a terminal ResourceLimit overlay. Geometry visibility or a brightness threshold does not establish complete-load readiness. The limiting resource is not yet traced. | `preview-microscope-resource-limit.png`; `studio.py::wait_for_render`; glTF aggregate texture/progressive mip accounting | T20, T23 |
| P1 | T19 explicitly records an incomplete format corpus and no reference-hardware timing claim. New render resources also need the existing GPU budget and lifecycle protections. | `docs/0.7.0-QUALIFICATION.md`, T18 cache follow-up, rendering design | T22, T34–T35 |

These are mechanisms to fix or test, not proof that each one caused a specific
pixel in the exploratory captures. T21 isolates the contributions before
claiming a measured fix.

**T23 disposition (measured).** The microscope's late overlay was the broker's
absolute 300 s generation wall clock (`ImportStage::GenerationDeadline`,
`ResourceLimit`) applied to an idle background detail replay, not a decoded
byte/pixel, mip, catalog or GPU-residency limit: the complete set was
2048 px / 12 mips and used 134,742,016 GPU bytes against a 1,023,410,176-byte
detail target. T23 ends an idle replay cleanly, deduplicates identical image
sources (the diffuse map drops from four resident textures to three), and makes
capture readiness require terminal complete/resident state. See
[ADR-0081](../design/adr/0081-texture-budget-accounting-and-complete-load-readiness.md).

**T24 disposition (measured).** The split-sum DFG LUT, exact cubemap
solid-angle SH projection and Khronos clearcoat layering replace the
Fresnel-only environment response and the flat `(1 − clearcoatFactor)` base
specular removal. A white metal reads the white-furnace identity (0.99992), the
flat 0.3 linear patch displays exactly the CPU NeutralToneMap result
(`[139,139,139]`), Studio and Directional environment channels match with zero
delta across all sixteen probes, and environment intensity zero removes every
environment/coat channel. Car paint keeps 0.97× the T21 environment-specular
baseline; the Camera lens keeps a dark reflection/depth at 0.58×. Transmission
remains the documented alpha-carried interim composite (channel 22) until
T26/T27. See
[ADR-0082](../design/adr/0082-split-sum-brdf-and-clearcoat-layering.md).

**T29 disposition (measured).** Clay now derives an effective bucket instead of
forcing shader alpha: authored BLEND/transmission triangles move to an opaque,
depth-writing overlay pass (`LESS_EQUAL` plus a bounded depth bias, far-to-near)
and the Blend/OIT PSOs are not used; MASK cutouts keep sampled coverage. The
generated fixture proves every identity material becomes the same neutral matte,
a textured hole stays a hole, glass hides its interior, overlapping alpha sheets
show no tint/seam, small and mirrored parts stay readable, and
Studio->Clay->Directional->Studio leaves imported data, texture residency,
camera, exposure and picking unchanged. Car Clay windows are solid gray with no
interior leakage; the small window-edge speckles also appear in Studio and are
recorded as the pre-existing authored defect. See
[ADR-0086](../design/adr/0086-clay-effective-opaque-bucket.md).

**T31 disposition (measured).** The P1 bounds-ellipse ground shadow is replaced
for triangle content by a fitted 2048² orthographic depth map (1024² allocation
fallback) plus an R8 caster-attenuation target. The same-camera on/off lane in
Studio, Clay and Directional finds six to nine disconnected shadow regions, an
enclosed strap-loop/cutout hole, no broad floor sweep (about 1–2 % of viewport
in the generated fixture), and a weaker clear-glass shadow than the equal opaque
panel. MASK keeps its alpha cutout, mirrored/double-sided geometry culls nothing,
BLEND coverage surfaces are not occluders, and transmission uses the bounded
approximation `clamp(1 − 0.60 × transmission, 0.40, 1)`. The contact layer is
premultiplied black coverage in linear HDR; the Ground highlight stays the
independent display-space backdrop layer, and the grid stays separate. The
fitted map is texel-snapped and scale-invariant, costs 20,971,520 bytes at
2048², survives resize without reallocation, and is rebuilt on device loss.
The analytic ellipse survives only as the labelled allocation fallback and is
never used for point/line-only content. See
[ADR-0088](../design/adr/0088-geometry-derived-fitted-shadow-map.md).

**T32 disposition (measured).** Screen-space AO is a depth-only, indirect-only
layer with a fixed 12-sample spiral and a 5x5 depth-edge-aware filter. The
opaque material pass writes its environment/IBL/ambient share to a second HDR
target and the resolve subtracts `indirect * (1 - ao)` before OIT, exposure and
tone mapping; direct light, emission, glass reflection/transmission and the
display transform are not multiplied by the mask. The same-camera generated
pairs in Studio, Clay and Directional change 3.0-3.7 % of the viewport with
max darkening 23-49 display units and a whole-frame mean luma delta under 0.5;
emission, dark-body, transparent/cutout and background regions stay unchanged.
AO-only masks in all three modes show the inside-corner/recess, contact rings,
narrow gaps and the thin ring, while ring/cutout holes and background stay
bright. Perspective/orthographic, near/far zoom, an 8x scene-scale duplicate
and a three-step orbit are covered; the mask overlap is 0.66 per small orbit
step and the occluded fraction stays within a narrow band. Targets cost
10,747,904 bytes at 1280x900 and the GPU timestamp pass reads 304-312 µs on the
Debug workstation. Transparent/BLEND surfaces do not write the sampled depth or
the indirect target. The T27 transmission snapshot is taken before the resolve
subtraction, so a cavity seen only through glass keeps the unoccluded
transmitted colour; that recorded limitation is left for T34/T35 to extend if
release evidence needs it. See
[ADR-0089](../design/adr/0089-scale-aware-screen-space-ao.md).

**T33 disposition (measured).** The default AA is a depth/coverage-aware
seven-tap edge filter inside the existing full-screen resolve, not MSAA or
TAA. The opaque MRT writes the `R32_UINT` pick id alongside scene colour, so
resolving that integer target would average ids; bounded 2× supersampling is
kept only as a deterministic validation reference. AA on/off pairs in Studio,
Clay and Directional change 0.35-1.1 % of the viewport, leave the background
delta at 0.0 and keep engraved lettering at 100-103 % of its AA-off strong-edge
samples. Against the box-reduced 2× reference, every inspected region improves
in all three modes (wires 86.8→85.6, grille 47.4→46.4, digits 109.9→108.7,
chrome 64.8→64.2, normal detail 59.0→58.3, lens 104.7→103.6, grid
31.3→30.5); the whole-frame MAE is recorded but is not the verdict because the
2× reference samples different texture LODs and pixel centres. Picking with AA
on/off returns identical ids, the D2D toolbar text is byte-identical, the
ao-probe MASK hole and the OIT algorithm/coverage are unchanged. Linear/mipmap
slots use bounded 4× anisotropy while authored nearest slots stay point; mip
reduction is linear-light, alpha-coverage-weighted color, renormalized normals,
GGX-alpha RMS for MR and linear for AO/factor data. Physical 1080p/1440p,
fractional resize, simulated 96/144/192 DPI, three orbit steps and
resize/device-loss/forced-off lifecycle are covered. No extra target is
allocated; the Debug resolve record time is 15-28 µs. See
[ADR-0090](../design/adr/0090-depth-aware-edge-aa-and-semantic-sampling.md).

### Why Camera and Car differ

Camera has four materials; its glass uses transmission 1, base RGB about 0.8,
roughness 0 and default OPAQUE coverage. Car has 39 materials and combines
double-sided alpha BLEND, transmission, clearcoat, specular and emission.
Examples include black glass with coverage alpha 0.25 and transmission 1,
separate coated light glass, orange/red tinted glass, decals and glow layers.

Those exercise different semantics and ordering. Preserve authored coverage,
transmitted color and interface reflection independently. Do not fix Car by
turning all glass darker or fix Camera by making every translucent surface
more reflective. Do not add asset-name/material-name exceptions.

F3D is a useful visual and implementation reference, but its pale Camera lens
in this run is not the target. Verify glTF behavior against Khronos references
as well. Audit the current blanket IOR≈1 transmission suppression and the
degenerate-BLEND recovery heuristic against `Suit.glb` and controlled fixtures;
IOR matching removes interface reflection/refraction, not all transmission.

## Execution and acceptance contract

The task files are the implementation instructions. New verification commands
are **planned interfaces**: T20 creates `tests/render-quality/verify.py` and
the capture/manifest support; subsequent tasks add their registered profiles.
They are not commands that exist or have passed at planning time.

Each task's wrapper must run the relevant numerical/import checks and its
actual GPU image lane, propagate failures, reject empty test selections and
missing artifacts, and write machine-readable results. A future task/profile
that is unimplemented must fail, not silently pass. Keep worker suites serial.

### Verification economy

The GPU gate is the expensive part of every task; T21a spent roughly half its
session waiting on repeated full gates. Spend runs deliberately:

- Run the front-matter command in the foreground with an explicit long timeout
  (900000 ms or more). Do not background it and poll with short `Start-Sleep`
  loops; if the tool moves it to the background, wait once for completion.
- Iterate with `--fast` (add `--no-build` once the binary is current). `--fast`
  keeps the build, generated fixtures and the task's focused lane, and skips
  the contract self-tests, external corpus, F3D references and review gate. It
  is iteration evidence, not acceptance evidence. Task-specific lanes must
  honor the same switch (a `fast` argument or `RENDER_QUALITY_FAST=1`) so they
  can skip slow external sampling too.
- Keep agent-run full gates to at most two: one early proof of the lane and
  one final after the last edit. The harness repeats the front-matter command
  once after you finish. Corpus or baseline re-promotion is one run, not a
  per-fix loop.
- Use focused unit/import filters between runs. Do not re-run a passing gate to
  "confirm" it, and do not repeat a failing configuration without a change.

Compare two distinct things:

1. **First-open product quality:** default framing, ground, background and
   exposure in each app, with every effective option/version recorded.
2. **Controlled rendering:** equivalent source-space camera/FOV, physical
   viewport pixels, orientation, material, neutral environment and exposure.
   Exclude UI/cursor/tooltips. Compare our before/after pixels on one setup;
   use F3D/Khronos side by side with a defect rubric, not pixel identity.

The external matrix covers every root asset in
`D:/repos/binbuf/preview-3d-test-models`, resolving `.gltf` directories to their
entry files and preserving sidecars. All three modes get first-open/front/
oblique/grazing views, selected close-ups and an orbit sequence. T20 records
the initial inventory; T34 completes the matrix. Missing assets produce an
explicit unavailable/incomplete result, never a passing skip for release.

Required regions include Car windshield/side windows/headlights/paint,
Camera lens/black body/strap, microscope brass/enamel, Suit, STL cavities,
STEP assembly details, textured OBJ and USD assets. Generated tests cover
clearcoat and roughness sweeps, independent AO, thin/tinted/rough glass,
alpha cutouts, intersecting transparent triangles and mirrored tangents.

Image review must identify concrete defects: glass depth/tint/order,
highlight shape, black/white detail, geometry readability, contact, halos,
silhouettes and stability in motion. Record reviewer/tool identity and actual
images inspected. Use an image-capable reviewer; a text-only test summary is
insufficient. Current local Symphony `vision.enabled` is false: establish an
image-inspection route in T20 without silently changing provider settings.
Absent visual review stays `needs_visual_review`, not `done`. No mandatory
human approval is implied if an image-capable agent can perform the review.

Baseline promotion is explicit and reviewable: immutable reference, candidate,
diff/crops, reason and review record. Ordinary verification must not generate
accepted baselines or turn missing goldens into success. Global downsampled
MAE/SSIM supplement local regions, numerical invariants and motion checks;
they do not replace them. Preserve existing good Camera glass throughout.

All tasks preserve parser isolation, brokered I/O, typed degradation, fixed
budgets, feature-level 11_0 and Debug-only test control. Extra render resources
participate in resize/cancellation/generation retirement/device recovery and
the existing memory budget. Record fallback quality; do not make a worse
fallback the normal path on supported hardware.

## Sources to carry into implementation

Read F3D as a reference; do not import its trusted-process parser architecture
or depend on its runtime. If reusing code/assets, preserve their licenses and
record provenance. Source-main and installed-binary behavior are distinct.

- F3D: `resources/configs/config.d/05_all.json`,
  `vtkext/private/module/vtkF3DRenderPass.cxx` (depth peeling, SSAO, pass
  construction), `vtkF3DRenderer.cxx` (AA, NeutralPBR, lights/IBL),
  `vtkF3DMetaImporter.cxx` (imported lights/material conversion), and
  `vtkext/public/module/vtkF3DGLTFImporter.cxx`.
- F3D's core PBR shading delegates substantially to VTK. Follow
  `vtkF3DCachedLUTTexture`/`vtkF3DCachedSpecularTexture` into VTK's
  `vtkPBRLUTTexture`/`vtkPBRPrefilterTexture` and mapper shading when studying
  BRDF integration; reproducing only F3D's light configuration omits that
  rendering machinery. Cache identities must include the bake algorithm and
  source environment, with validation of any persisted dimensions/data.
- [glTF material/texture semantics](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html).
- [KHR_materials_transmission](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_transmission)
  and [KHR_materials_volume](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_volume).
- [KHR_materials_clearcoat](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_clearcoat)
  and [KHR_materials_specular](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_specular).
- [Khronos glTF Sample Viewer](https://github.com/KhronosGroup/glTF-Sample-Viewer)
  and [Sample Renderer](https://github.com/KhronosGroup/glTF-Sample-Renderer).
- [Filament material and IBL treatment](https://google.github.io/filament/main/filament.html).
- [Khronos PBR Neutral tone mapping](https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral).

Transmission, clearcoat, specular, Filament and the Neutral reference were
retrieved during this review. The remaining links are implementation reading
targets, not claims that those engines were run in this investigation.

## T34 acceptance record (2026-11-01)

T34 is accepted through the T34b completion lane. The implementation is
**6ea6b5a**; T34b added the shared corpus-root metadata filter, corrected the
Release probe and wrote the hand-off. The exact gate is:

```powershell
python tests/render-quality/verify.py --task T34 --configuration Debug --output TestResults/render-quality/T34
```

The passing report (`TestResults/render-quality/T34/report.json`) has `status:
pass`, `fast: false`, `captureOnly: false`, `failures: []`, external
qualification `available`, 24 corpus root entries with zero unaccounted, F3D
reference `available`, review `recorded`, 74/74 full-corpus checks and 226
captures. The first attempt failed only on the Release probe, which had launched
Release with the Debug-only `--app-smoke` flag; that is fixed and the
first-failure log is retained.

Observed matrix:

| Area | Evidence |
| --- | --- |
| External corpus | 19 normal assets captured or typed (18 images, mint FBX typed `MalformedData` with a failed-load image) + five 32 MiB-capped `resource-budget` dispositions; 24/24 accounted |
| Modes | Studio, Clay and Directional at front/oblique/grazing, close-ups and a Car orbit; mode pairs differ |
| Mandatory regions | Car windshield/paint and Camera lens/lettering present in all three modes (12/12 checks) |
| Portable formats | FBX binary/hierarchy, USDZ, USD crease, 3MF, textured OBJ and STEP captured in all three modes |
| Hostile input | 32000×32000 image degrades to a bounded fallback before upload; NaN/Inf UV rejected (code 3, stage 15) before a drawable scene |
| Lifecycle | Rapid open/cancel, stale generation, resize (1600×1000, 1024×768), 96/144/192 DPI, coarse-to-full, eviction, device loss/reopen, 32 MiB typed `ResourceLimit` |
| Release boundary | `--app-smoke` exits 2; control fields 0/105/107/300 return 0; dependency closure has no parser/codec library |
| References | F3D 3.5.0 Car/Camera shipped-preset and controlled renders, qualitative only |

Measured Debug cost (Windows 11 10.0.26300, RTX 4080, driver 32.0.16.1714):
Car 18.0 s load, 170.9 MB of a 1.02 GB detail target (0.167), mean/p95
5.03/6.32 ms; Camera 8.28 s, 239.9 MB (0.234), 6.46/8.51 ms; portable STEP
2.95 s, 100.3 MB (0.098), 5.46/4.64 ms. These are workstation Debug numbers,
not reference-hardware claims.

Known defects carried to T35 (recorded, not successful images): mint-swimsuit
FBX typed `MalformedData`; food-collection fitted framing for its extreme flat
aspect ratio; Car window/trim authored speckle; F3D 3.5.0's DPI-aware preset and
pale Camera lens. The restricted local corpus and its screenshots stay under
`TestResults/`.

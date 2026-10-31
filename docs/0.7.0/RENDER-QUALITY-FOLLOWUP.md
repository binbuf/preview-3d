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
| P1 | Clearcoat, sheen, specular and transmission texture inputs are warning-only. Constants are insufficient for many authored assets. | T18 hand-off, `GltfAdapter.cpp`, material wire/dependency contract | T25 |
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

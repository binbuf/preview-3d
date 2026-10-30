# Render-quality evidence lane

T20 introduced this lane for reproducible GPU evidence. T21 extends the same
lane with material/light diagnostics, raw HDR readback and a per-material draw
record. It is separate from the per-frame/import tests: it launches the Debug
viewer, waits for a complete and settled scene, drives the existing
`--app-smoke` control surface, captures the viewport, and compares only
same-camera images.

The front-matter commands are:

```powershell
python tests/render-quality/verify.py --task T20 --configuration Debug --output TestResults/render-quality/T20
python tests/render-quality/verify.py --task T21 --configuration Debug --output TestResults/render-quality/T21
```

The T20 wrapper builds `Preview3D` Debug, runs the negative capture-contract
self-tests, generates fixtures, inventories the external corpus, renders the
installed F3D reference conditions, captures 53 T20 cases, and compares the
generated-fixture cases against accepted regression references. T21 instead
builds Debug **and Release**, captures the material probe plus Car/Camera/Suit,
runs the diagnostic GPU channel/readback checks in `diagnostics.py`, verifies
Release rejects the whole test-control surface, and writes `cause-report.md`.
Both exit nonzero on any failure and write `report.json`, `captures.json`,
`build.log` and images under the output directory.

## What a capture must satisfy

A capture is not evidence until all of these hold:

1. The viewer reports `Ready`, the requested generation is the loaded
   generation, and `HasCompleteModel` is true.
2. Texture counters have stopped changing and the viewport has produced a
   stable, non-blank frame. T20 observed Car present a background-only frame for
   several seconds after it reported complete; the lane waits for visible pixels,
   so that gap remains a T23 defect instead of a silently accepted baseline.
3. The physical viewport, DPI, camera yaw/pitch/distance scale, projection,
   model transform inputs, exposure/environment/preset, source/binary hashes and
   model/sidecar hashes are recorded in `captures.json`.
4. UI is excluded: the top toolbar and bottom bar are cropped, and the Debug-only
   capture-clean flag suppresses the navigation gizmo, reset button, warning
   badge, loading card, HUDs, info panel, flyouts, tooltips and focus rectangle.
   The capture path never moves the mouse, so the cursor and tooltips are absent.

Blank/flat frames, unsettled or proxy-only frames, generation mismatches,
missing images, missing accepted references and camera/viewport changes fail the
lane. `selftest.py` proves each of those failures with synthetic images and
metadata; `report.json.contractSelfTest` records the result.

## Debug capture controls

The viewer exposes these only with `--app-smoke` in a Debug build
(`PREVIEW3D_ENABLE_TEST_CONTROL`). Release rejects the whole surface.

| Field | Meaning |
| --- | --- |
| 31 / 32 / 33 | set yaw / pitch (centidegrees) and distance (per-mille of home); `lParam == -1` reads |
| 35 / 36 | projection (0 perspective, 1 orthographic) / immediate first-open reset |
| 105 | capture-clean viewport (gizmo, reset, warning, loading, HUD, info, flyout, tooltip off) |
| 106 | force a render-thread pass and synchronous paint; returns presented-frame count |
| 107 | capability mask: 1 canonical camera, 2 capture-clean, 4 forced present |
| 108-123 | effective source-to-viewer transform, row-major float elements |
| 124 | Debug diagnostic channel (0 normal, 1-20 inspection views); `lParam == -1` reads |
| 125 | supported diagnostic-channel mask; UV1 (bit 5) is deliberately absent |
| 126 / 127 | numeric sample rectangle min/max (client pixels, packed 16-bit x/y) |
| 128 / 129 | request HDR scene-target readback / status (0 none, 1 pending, 2 ready, 3 failed) |
| 130-133 | sampled mean linear RGBA as float32 bit patterns |
| 134 / 135 | per-material draw-record count / selected index |
| 136-194 | selected record: ids, counts, bucket, depth-write, cull, wire material, UV and texture metadata |
| 195+ | reserved for later diagnostics; Release answers none of these |

Fields 37-39 remain the existing texture smoke values; do not reuse them.

## T21 material and light diagnostics

The textured PBR shader accepts a diagnostic channel in `gPresentation.w`. It
writes raw linear values; the full-screen resolve is the only clamp. Channels:

| Channel | Value |
| --- | --- |
| 1 | base colour (factor × texture × vertex colour) |
| 2 / 3 | geometric normal / shading normal, encoded `n*0.5+0.5` |
| 4 | UV0 |
| 5 | UV1 — **unsupported**, deliberate magenta marker |
| 6 / 7 / 8 | metallic / roughness / effective AO factor |
| 9 / 10 | effective coverage alpha / transmission factor |
| 11 | IOR (r = ior/4) and IOR-derived F0 (g) |
| 12 / 13 | clearcoat factor / sheen colour × weight |
| 14-19 | isolated direct diffuse, direct specular, environment diffuse, environment specular (including sheen), coat and emission |
| 20 | pre-tone-map linear HDR colour |

A 64×64 RGBA16F scene-target region is copied to a readback resource and
averaged on the CPU. Tests compare the numeric result with source-authored
values from `fixtures.py`, not with shader-source tokens. `diagnostics.py`
also reads glTF JSON for external assets only in test tooling; the viewer never
parses glTF.

The per-material record is a bounded snapshot of the validated wire material
plus the effective draw state: bucket (opaque/masked/blended/transmissive),
depth-write, cull mode, source/primitive/material ids, texture slot identity,
pixel format, colour space and sampler/UV flags. Unbound slots are reported as
unbound rather than as authored maps.

The T21 profile captures Car window/paint, Camera lens/body, Suit torso and the
six-material `material-probe` fixture. `diagnostics/diagnostics.json` holds the
numbers, `diagnostics/channels/` the channel images and `cause-report.md` the
short dominance report handed to T22-T27.

## Reference conditions

Two different cases are captured and are never mixed numerically:

- **First-open/default presentation** (`*-default.png`): the product's own
  initial framing, presentation preset, background, exposure and shadow/grid
  state, after the load is complete and settled.
- **Controlled comparison** (`*-<mode>-<view>.png`): home target centre, one of
  the profile's yaw/pitch/distance-scale views, perspective 45° FOV, Studio PBR
  preset, dark background, PBR Neutral tone map, exposure 1.0, Studio intensity
  1.0, grid and shadow on, and the selected Studio/Clay/Directional lighting.

The T20 views are front (yaw 0°, pitch 90°, fit distance), oblique (30°, 60°),
lens close-up (0°, 90°, 0.42×) and window close-up (18°, 70°, 0.48×), with a
four-step 0°/90°/180°/270° orbit at 75°. The camera target is the home bounds
centre. Regions in the profile are normalised viewport rectangles used for local
MAE checks.

F3D reference images are observations, not pixel-match targets:

- installed `C:/Program Files/F3D/bin/f3d.exe` is version **3.5.0**, VTK
  `9.6.1-1336-gdc285c2254`;
- `shipped-preset` uses `C:/Program Files/F3D/share/f3d/configs/config.d/05_all.json`;
- `controlled` uses `--no-config --up +Z --camera-view-angle 45`.
- The preset is DPI-aware and produced 1920×1350 for a requested 1280×900;
  the controlled run produced 1280×900. Do not compare those as matched frames.
- Local F3D source at `D:/repos/github/f3d` is commit
  `7f32e1f3e02fecbcc0cc2ba74a97bcb18cc28e58` declaring 4.0.0. No local 4.0.0
  binary exists on this workstation, so no image is attributed to it.
- Khronos glass route: render `glass-reference.glb` with the Khronos glTF Sample
  Viewer/`KHR_materials_transmission` + volume. No sample viewer is installed
  locally, so the lane reports `incomplete`. F3D's flat gray Camera lens is a
  known defect, not the accepted glass target.

The installed F3D version, preset hash, command lines, image hashes and actual
pixel dimensions are in `report.json.reference`.

## Generated fixtures and provenance

`fixtures.py` builds four deterministic fixtures from code:

| Fixture | Roles | Rights |
| --- | --- | --- |
| `sphere-grid.glb` | white/black/red dielectric, rough/polished metal, transparent dielectric | Generated here; redistributable with the test tree |
| `glass-reference.glb` | thin tinted transmission, rough transmission, clear lens | Generated here; Khronos-style material roles, no Khronos binary is redistributed |
| `material-probe.glb` | six constant-material probes: smooth dielectric, rough metal, clearcoat, blend+sheen+emission, transmissive IOR, masked cutout | Generated here; source values repeated in `fixtures.json.expectedMaterials` |
| `stl-box.stl` | hard-edged CAD-like surface and ground contact | Generated here |

The generated lane runs without `D:/repos/binbuf/preview-3d-test-models`. The
external lane inventories **every root entry** and resolves the two
directory-shaped glTF assets (`Camera_01_4k.gltf`, `vintage_microscope_4k.gltf`)
to their entry files. T20 seeds Car, Camera, microscope and `base.stl`; T34
completes the matrix. If the corpus or an asset is absent, external qualification
is `incomplete`, not `success`.

External models and screenshots are restricted local evidence. They are not
committed and must not ship until redistribution rights are established. Car
metadata indicates CC-BY-NC-SA-4.0. Only generated-fixture images are committed
as accepted references.

## Profiles and extension

`profiles/TNN.json` is the manifest. A later task adds a profile; it does not
edit `verify.py`. The profile declares:

```json
{
  "task": "TNN",
  "viewport": {"width": 1280, "height": 860},
  "modes": [{"id": "studio", "lighting": 0}],
  "views": {"front": {"yawDegrees": 0, "pitchDegrees": 90, "distanceScale": 1}},
  "regions": {"asset-id": {"region": [0, 0, 1, 1]}},
  "lanes": {
    "generated": {"required": true, "regression": true, "fixtures": []},
    "external": {"required": false, "root": "...", "assets": []},
    "reference": {"required": false, "f3d": {"assets": [], "configurations": []}}
  },
  "knownDefects": []
}
```

Add the same manifest fields the T20 profile uses: `orbit` steps, per-asset
`closeups`, `knownDefect`, and lane `regression`. An unknown task, a profile with
an empty required lane, or an unavailable required capability fails.

## Baseline promotion

Ordinary verification **never** creates an accepted reference. Bootstrap one
deliberately:

```powershell
# 1. Collect candidates only. This is not a passing verification lane.
python tests/render-quality/verify.py --task T20 --configuration Debug `
  --output TestResults/render-quality/T20 --capture-only

# 2. Review the images with an image-capable reviewer and write/refresh
#    tests/render-quality/review/T20-review.json (identity, tool, images,
#    defects, provider settings unchanged).

# 3. Promote one case explicitly. Repeat per case.
python tests/render-quality/compare.py `
  --case generated-sphere-grid-studio-front `
  --candidate TestResults/render-quality/T20/images/generated-sphere-grid-studio-front.png `
  --captures TestResults/render-quality/T20/captures.json `
  --reason "Reviewed current generated-fixture rendering; regression reference only." `
  --reviewer "DeepSeek V4.1 Flash coding agent (image-capable)" `
  --review tests/render-quality/review/T20-review.json
```

Promotion writes `baselines/accepted/accepted.json`, the immutable PNG,
`baselines/accepted/promotions/<case>.json` and a copy of the previous image.
The record includes the reason, reviewer, review path, candidate hash, before
hash, camera, viewport, regions and promotion statistics. A missing accepted
reference fails its required regression lane. Accepted references defend the
reviewed current behaviour; they are not evidence that Studio/Clay/Directional
meet the quality target.

Image comparison is:

- global downsampled MAE and SSIM;
- per-region MAE on the profile's crops (windshield, lens, glass panels, CAD
  edge);
- exact physical viewport and camera metadata checks before any pixel score;
- no cross-renderer MAE. F3D/Khronos images are side-by-side observations with a
  defect rubric.

## Image review

Current local Symphony `vision.enabled` is false. T20's review route is the
coding agent's image file reader: the reviewer opens the actual PNGs listed in
`review/T20-review.json` and records concrete defects. Provider settings are not
changed. A text-only summary is not sufficient; absent visual review stays
`needs_visual_review`.

## T20 known defects

Recorded in `profiles/T20.json` and `review/T20-review.json`:

- Car windshield/side glass: bright, dotted white edge speckles; coverage alpha
  and transmission are not separated (T22/T26/T27).
- Car roof/window interpenetration reads as one flat plate, without layered glass
  depth (T26).
- The generated Khronos-style tinted, rough and clear transmission spheres
  render almost identically clear; the transmission/volume/IOR roles are not
  differentiated (T22/T25/T26/T27).
- A verified complete model can present background-only frames for several
  seconds before the first drawable/textured frame (T23).
- The T19 microscope ResourceLimit did not reproduce in the T20 run; the label
  remains until a repeatable fix (T23).
- Installed F3D 3.5.0 gray Camera lens remains a reference defect, not a target
  (T26/T27).
- F3D preset DPI/resolution and per-machine tolerances are limitations, not
  quality scores (T34/T35).

## Commands used for T20

```powershell
python tests/render-quality/verify.py --task T20 --configuration Debug --output TestResults/render-quality/T20
```

Outputs: `report.json`, `captures.json`, `captures.json` copy beside the images,
`contract-selftest/contract-selftest.json`, `corpus-inventory.json`,
`fixtures/fixtures.json`, `reference/f3d-*.png`, `images/*.png` and
`build.log`. `TestResults/` is local and ignored; only the harness, generated
fixture code, review record and generated-fixture accepted references are
committed.

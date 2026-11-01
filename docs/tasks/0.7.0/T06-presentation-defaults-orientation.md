---
timeoutMin: 240
verify: x64\Release\Tests.Unit.exe "[presentation]"
---
# T06 — Per-format presentation defaults and orientation

## Goal
A file opens looking correct without manual setup: OBJ is treated as Y-up,
FBX keeps its authored axes/unit scale where ufbx exposes them, point clouds
default to the point preset, and CAD vs. PBR formats start with different
Studio presets. The defaults come from the format catalog, not another switch.

## Context (read first)
- `docs/proposal.md` §3.8 — the per-format defaults design.
- `shared/model-core/include/model_core/FormatCatalog.h` — T02 catalog and
  `PresentationProfile`.
- `import-worker/src/ObjAdapter.cpp:561` — currently reports
  `UpAxisId::Unknown`.
- `shared/import-broker/src/SharedSectionValidator.cpp:227-251` — per-format
  up-axis/unit rules; OBJ currently requires `Unknown`.
- `import-worker/src/FbxAdapter.cpp:1008, 1084-1088` — target axes and hardcoded
  Y/1.0 metadata.
- `interactive-viewer/src/app/GroundAxis.h:21-26` — `Automatic` maps `Unknown`
  to Z.
- `interactive-viewer/src/render/Renderer.h:36-43` — lighting modes.
- `interactive-viewer/src/app/Preview3D.cpp` / `RenderThread.cpp` — where the
  initial lighting/grid/background state is assembled.
- `plugins/native/configs/config.d/10_native.json`,
  `plugins/assimp/configs/config.d/10_assimp.json` in F3D — examples of
  per-format defaults, not code to copy.

## Scope
- [x] Add the `PresentationProfile` defaults to the T02 catalog for the current
  families:
  - glTF/GLB, FBX, USD → Studio PBR;
  - STL, 3MF, STEP → Studio CAD;
  - PLY mesh → Studio PBR; PLY point-only/3DGS → point preset;
  - OBJ → Studio PBR with Y-up.
- [x] Fix OBJ orientation: report `UpAxisId::Y` (or convert in the adapter if
  the product chooses); update `SharedSectionValidator` to allow Y for OBJ and
  update tests. Keep the toolbar ground-axis override and “show native
  orientation” behavior.
- [x] FBX: read `ufbx_scene.settings.axes.up` and the authored unit scale if the
  pinned ufbx exposes them; otherwise keep Y/1.0 and add a documented
  user override. Do not silently guess a unit when the file does not provide
  one.
- [x] PLY: detect point-only / 3DGS properties and select the point preset
  (unlit/vertex color/round splats); do not force it for triangle meshes.
- [x] Apply the profile at document open: initial lighting mode, grid, camera
  orientation/orthographic where relevant, background and point size. The user
  can override; the override wins for the session. Do not re-apply the preset
  on every frame or on reload.
- [x] Add a small “Presentation” readout in the info panel showing source
  up-axis/units and the active preset; keep it compact and localized.
- [x] Add tests:
  - `[presentation]` unit cases for catalog → preset mapping and OBJ Y-up
    validation;
  - `[orientation]` import-isolation case with a known Y-up OBJ asserting
    metadata/bounds orientation;
  - PLY point fixture selects the point preset;
  - a CAD fixture selects the CAD preset.
- [x] Update `docs/FORMAT-SUPPORT.md` with the orientation/unit assumptions.

## Out of scope
- Full material-aware presets and exposure (→ T15).
- Ground shadow implementation (→ T16); this task only carries the default
  flag.
- A new settings dialog/page; the existing info panel/toolbar is enough.
- Changing the user's persisted global lighting preference for files where
  they explicitly chose a preset.

## Design notes
- OBJ has no standard up axis. Y-up is the ecosystem default and matches F3D;
  document it and keep the override. Do not bake a rotation unless the adapter
  already owns one for all OBJ files.
- A source format's up axis is metadata, not a reason to re-parse. Keep the
  viewer's existing ground-axis transform.
- FBX unit scale is a fidelity improvement, not a hard requirement. If ufbx
  cannot expose it reliably, do not invent a number; warn in the hand-off and
  leave the override.
- Keep the preset selection deterministic and testable: a pure function from
  `FormatDescriptor` + imported summary to a `PresentationProfile`.
- Do not let a preset change the source materials; it changes view state only.

## Done when
- [x] `x64\Release\Tests.Unit.exe "[presentation]"` passes.
- [x] `x64\Release\Tests.ImportIsolation.exe "[orientation]"` passes.
- [x] A known Y-up OBJ appears upright; the toolbar override still works.
- [x] STL/STEP/3MF open in the CAD preset; glTF/FBX/USD in PBR; PLY point
  clouds in the point preset.
- [x] No existing persisted setting is overwritten without user action.
- [x] Hand-off records the exact OBJ/FBX/PLY defaults and any unit caveats.

## Hand-off

### Catalog defaults (`shared/model-core/include/model_core/FormatCatalog.h`)
- `PresentationProfile` is now `Default | StudioPbr | StudioCad | PointCloud`.
- glTF/GLB, FBX, OBJ, PLY (mesh), USD/USDA/USDC/USDZ → `StudioPbr`.
- STL/ASCII STL, 3MF, STEP → `StudioCad`.
- OBJ `defaultUpAxis` is now `UpAxisId::Y`; units stay `Unspecified`.
- `ResolvePresentationProfile(descriptor, {pointCount, triangleCount})` is the
  pure mapper: only a PLY with points and zero triangles becomes `PointCloud`;
  every other family keeps its declared profile.

### OBJ orientation and validator
- `import-worker/src/ObjAdapter.cpp` reports `UpAxisId::Y` (metres-per-unit
  remains 0/unspecified). No geometry rotation is baked; the existing ground
  transform turns Y into the viewer's Z-up world, and the toolbar override /
  "show native orientation" are untouched.
- Per-format unit/up-axis policy moved to the pure
  `shared/model-core/include/model_core/SceneMetadataRules.h`
  (`SceneUnitsAndUpAxisAllowed`). `SharedSectionValidator.cpp` calls it.
  OBJ accepts `Y` or legacy `Unknown` and requires `metersPerUnit == 0`; it
  rejects X/Z or any authored metre factor. glTF/FBX/STL/PLY/USD/STEP rules are
  byte-for-byte the same policy as before.

### FBX axes/unit findings
- Pinned ufbx (0.23.0) exposes `ufbx_scene.settings.axes.up` and
  `settings.original_unit_meters`, and keeps them after `target_axes` /
  `target_unit_meters` normalization.
- The product still normalizes geometry to Y-up/metres
  (`ufbx_axes_right_handed_y_up`, `target_unit_meters = 1.0`) and the wire
  `SceneMetadata` deliberately reports the delivered geometry. The adapter now
  reads the authored axis/unit (documented in place) but does not put them on
  the wire; no authored unit is guessed, and `SceneUnitsAndUpAxisAllowed` keeps
  FBX at `Y/1.0`.
- Caveat: the info panel therefore shows FBX as Y/metres, not the authored
  centimetre/Z-up values. Carrying those separately needs a future protocol
  field; the toolbar ground axis is the user override today.

### Preset application point
- `Preview3D.cpp` applies the resolved profile once at the first completed
  upload for a new document path (terminal, non-refinement). Refinements and
  reloads of the same path do not re-apply it.
- Mapping: `StudioPbr` → Studio shader + grid; `StudioCad` → neutral Clay
  material + grid + slightly lighter backdrop; `PointCloud` → existing unlit
  round colored splat path + no grid + darker backdrop.
- An explicit lighting-button or grid-toggle action sets a session override;
  later opens/reloads leave it alone. No persisted setting is written.
- Info panel appends a localized `Presentation` section (preset, source up
  axis, source units) after Scene Data, so existing section indices are stable.

### Tests and checks
- `tests/unit/PresentationTests.cpp` — `[presentation][format-catalog]`,
  `[presentation][ply]`, `[presentation][orientation]`,
  `[presentation][info-panel]`.
- `tests/import-isolation/ObjImportTests.cpp` — new
  "A known Y-up OBJ reports Y-up metadata and keeps its native bounds"
  (`[obj][orientation]`).
- `tests/import-isolation/PlyImportTests.cpp` — an imported point-only PLY
  resolves to `PointCloud`, an imported mesh PLY stays `StudioPbr`
  (`[ply-import][presentation][orientation]`).
- `tests/import-isolation/StlImportTests.cpp` — an imported STL fixture
  resolves to `StudioCad` (`[stl-import][presentation][orientation]`).
- Commands run in `x64\Release`:
  - `Tests.Unit.exe "[presentation]"` — 68 assertions, 4 cases, pass.
  - `Tests.ImportIsolation.exe "[orientation]"` — 70 assertions, 3 cases, pass.
  - `Tests.Unit.exe` — 136,177 assertions, 432 cases, pass.
  - `Tests.ImportIsolation.exe "[obj]"` — 97 assertions, 9 cases, pass.
  - `Tests.ImportIsolation.exe "[fbx]"` — 48,044 assertions, 31 cases, pass.
  - `Tests.ImportIsolation.exe "[ply-import]"` — 673 assertions, 29 cases, pass.
  - `Tests.ImportIsolation.exe "[recovery]"` — 2,327 assertions, 35 pass +
    3 fault-harness skips, pass.
  - Full Release solution build (`MSBuild Preview3D.slnx`) — success.

### Deviations / remaining work
- Studio CAD currently uses the existing Clay review material because a
  dedicated CAD lighting lobe is T14/T15 work; the profile value is the stable
  hook. Point size/roundness are the existing splat shader defaults; tuning is
  T15/T17.
- Background uses two simple backdrop tints (CAD lighter, point darker) rather
  than a user-selectable background mode; the full background/exposure control
  is T15.
- Language packs received the new English fallback keys in every pack. The
  repository's `generate-language-packs.ps1 -Verify` already reports unrelated
  pre-existing drift (`open.filter.*`, `compat.gltf.*`) and was not made a gate
  for this task.

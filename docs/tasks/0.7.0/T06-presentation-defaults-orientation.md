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
- [ ] Add the `PresentationProfile` defaults to the T02 catalog for the current
  families:
  - glTF/GLB, FBX, USD → Studio PBR;
  - STL, 3MF, STEP → Studio CAD;
  - PLY mesh → Studio PBR; PLY point-only/3DGS → point preset;
  - OBJ → Studio PBR with Y-up.
- [ ] Fix OBJ orientation: report `UpAxisId::Y` (or convert in the adapter if
  the product chooses); update `SharedSectionValidator` to allow Y for OBJ and
  update tests. Keep the toolbar ground-axis override and “show native
  orientation” behavior.
- [ ] FBX: read `ufbx_scene.settings.axes.up` and the authored unit scale if the
  pinned ufbx exposes them; otherwise keep Y/1.0 and add a documented
  user override. Do not silently guess a unit when the file does not provide
  one.
- [ ] PLY: detect point-only / 3DGS properties and select the point preset
  (unlit/vertex color/round splats); do not force it for triangle meshes.
- [ ] Apply the profile at document open: initial lighting mode, grid, camera
  orientation/orthographic where relevant, background and point size. The user
  can override; the override wins for the session. Do not re-apply the preset
  on every frame or on reload.
- [ ] Add a small “Presentation” readout in the info panel showing source
  up-axis/units and the active preset; keep it compact and localized.
- [ ] Add tests:
  - `[presentation]` unit cases for catalog → preset mapping and OBJ Y-up
    validation;
  - `[orientation]` import-isolation case with a known Y-up OBJ asserting
    metadata/bounds orientation;
  - PLY point fixture selects the point preset;
  - a CAD fixture selects the CAD preset.
- [ ] Update `docs/FORMAT-SUPPORT.md` with the orientation/unit assumptions.

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
- [ ] `x64\Release\Tests.Unit.exe "[presentation]"` passes.
- [ ] `x64\Release\Tests.ImportIsolation.exe "[orientation]"` passes.
- [ ] A known Y-up OBJ appears upright; the toolbar override still works.
- [ ] STL/STEP/3MF open in the CAD preset; glTF/FBX/USD in PBR; PLY point
  clouds in the point preset.
- [ ] No existing persisted setting is overwritten without user action.
- [ ] Hand-off records the exact OBJ/FBX/PLY defaults and any unit caveats.

## Hand-off
_(filled in by the implementing session: catalog defaults, OBJ validator change,
FBX axes/unit findings, preset application point, and test names.)_

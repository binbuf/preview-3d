---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[cad-iges],[cad-brep]"
---
# T08 — OCCT host: IGES and BREP

## Goal
`.igs`/`.iges` and `.brep` open through the existing isolated OCCT host with
bounded admission, colors where representable, and the same normalized scene
contract as STEP. This adds two common CAD interchange formats without a new
runtime or a new trust boundary.

## Context (read first)
- `docs/proposal.md` §3.3 — OCCT host breadth.
- `compatibility-host-step/src/StepHostImport.{h,cpp}` — host entry and typed
  error mapping.
- `compatibility-host-step/src/StepXdeAdapter.{h,cpp}` — XDE traversal,
  tessellation and normalized output.
- `compatibility-host-step/src/StepPart21Preflight.{h,cpp}` — the admission
  pattern; IGES/BREP need their own bounded preflight, not Part-21.
- `shared/model-core/include/model_core/FormatCatalog.h` — T02 catalog.
- `shared/import-broker/src/ImportSession.cpp` — host routing; currently
  STEP-specific.
- F3D reference: `plugins/occt/CMakeLists.txt:57-133` and
  `vtkF3DOCCTReader.cxx` for IGES/BREP reader behavior and options.

## Scope
- [x] Add `Iges` and `Brep` format IDs to the catalog and `SourceFormatId`
  (append-only), and extend the broker/bridge routing to the CAD host.
- [x] Generalize the host request with a `CadFormat` field (`Step`, `Iges`,
  `Brep`; leave `Xbf`/`FCStd` for T09) while keeping the existing STEP opcode
  behavior compatible. A distinct format must be validated by the host; the
  worker must never accept a CAD opcode. Keep the executable name/path
  (`Preview3DStepHost.exe` under `StepHost\`) stable unless the packaging and
  AppContainer ACL scripts are updated in the same change.
- [x] Implement bounded preflights:
  - **IGES**: verify the section-letter layout (S/G/D/P/T), line/record and
    entity/reference limits, size cap; reject control bytes/truncation.
  - **BREP**: verify the OCCT ASCII/binary header/size shape; reject malformed
    or over-budget input before `BRepTools::Read`.
- [x] Implement adapters:
  - IGES via `IGESControl_Reader` or `IGESCAFControl_Reader` (colors/XDE where
    available);
  - BREP via `BRepTools::Read` from the mapped stream;
  - reuse the existing STEP shape-to-normalized-chunk pipeline so transforms,
    bounds, per-face/shape colors and material grouping stay consistent.
- [x] Metadata: up axis `Unknown` by design; a verified positive metre factor
  where the format provides units, otherwise the documented default/warning
  (do not invent exact units). Keep the host’s commit/time budgets.
- [x] Extend `SharedSectionValidator` for the new format IDs: require
  `UpAxisId::Unknown` and a positive metre factor where the format provides
  units; add hostile-worker cases for wrong-axis/wrong-unit sections.
- [x] Update installer associations, open-dialog filters, `SourceFormatLabel`,
  docs and README for `.igs`, `.iges`, `.brep`. Use T03’s catalog-driven path,
  not a new hard-coded list.
- [x] Add fixtures:
  - `cheese.igs` / `f3d.igs` or equivalent small IGES;
  - `cheese.brep` / `f3d.brep` or equivalent small BREP;
  - malformed/truncated/over-budget cases;
  - color/assembly cases.
  Verify fixture licenses before committing; generate equivalents if needed.
- [x] Add `[cad-iges]` and `[cad-brep]` cases asserting typed outcomes, chunk
  counts, bounds and colors. Include a hostile/truncated case.
- [x] Update `docs/design/01-product-scope.md` (IGES/BREP in scope) and
  `docs/FORMAT-SUPPORT.md`.

## Out of scope
- XBF and FCStd (→ T09).
- STEP external/unit tolerance (→ T07).
- Shape healing beyond T07’s STEP policy; IGES/BREP get the same typed failure
  behavior.
- Wireframe/line output (→ T17).
- Native CAD formats such as JT, Parasolid or DWG.

## Design notes
- Do not link OCCT into the viewer, general worker or provider. The existing
  CAD host and its private payload remain the only OCCT consumer.
- IGES has no universal unit declaration; use the product default/warning and
  keep the value explicit in metadata. Do not silently label it as millimetres
  if the file says otherwise.
- BREP files can contain very large shapes; preflight and per-definition budgets
  are mandatory. Reject before an unbounded `BRepTools` allocation.
- Reuse the STEP tessellation profile and per-definition failure isolation so
  one bad shape does not fail an assembly.
- The host still receives only an inherited read-only handle, never a path.

## Done when
- [x] `x64\Release\Tests.ImportIsolation.exe "[cad-iges],[cad-brep]"` passes.
- [x] Positive IGES/BREP fixtures open through the isolated host; malformed and
  over-budget fixtures fail typed.
- [x] Colors/transforms/bounds match the STEP normalization invariants where
  the source supports them.
- [x] The installer and open dialog list `.igs`, `.iges`, `.brep`; the catalog
  is the source.
- [x] No OCCT import appears in `Preview3D.exe`, the general worker or the
  provider DLL.
- [x] Hand-off records the readers used, unit defaults and fixture licenses.

## Hand-off

### What landed
- `SourceFormatId::Iges = 14`, `Brep = 15`; catalog descriptors with
  `ImporterKind::CadHost`, extensions `.igs`/`.iges`/`.brep`; installer
  ProgIDs/associations; catalog-driven Open As/filters/`--format`.
- Generalized CAD route: opcode 22 `StartCadImportFromFile` and
  `ParseCadFileRequest` (seven STEP fields + closed `cadFormat` + reserved).
  STEP's opcode/request/executable stay unchanged. The host validates the
  format; the general worker rejects CAD formats and never accepts the opcode.
- `CadPreflight.{h,cpp}`: IGES fixed 80-column S/G/D/P/T admission (2 GiB,
  5M records, 5M entities, 100M parameter tokens, section/sequence/control-byte
  checks, global unit-flag extraction) and BREP `CASCADE Topology` V1-V3
  admission (optional `DBRep_DrawableShape`, 2M shapes, 1M locations, 5M lines,
  2 GiB, control/truncation checks) before any OCCT reader.
- `IgesStreamReader.{h,cpp}`: OCCT's IGES work library has no `ReadStream`, so
  admitted records are fed to the pinned `TKDEIGES` C parser and installed as
  an `IGESData_IGESModel` on the `IGESCAFControl_Reader` session. No temporary
  file or path is created.
- `StepXdeAdapter` dispatches Step/Iges/Brep into the same XCAF planner,
  tessellation profile, transforms, bounds, per-definition isolation and
  progressive emitter. `ScopedCoutSilencer` keeps OCCT kernel text off the
  binary control channel.
- `SharedSectionValidator`/`SceneMetadataRules` accept the new IDs: Iges
  requires `UpAxisId::Unknown` + positive metre factor, Brep requires `Unknown`
  + `0.0`. Negative hostile-section tests cover wrong-axis/wrong-unit cases.
- Fixtures in `tests/fixtures/cad-spike/` (generated from OCCT primitives by the
  checked-in test-only generator; no third-party model license) plus
  `[cad-iges]`/`[cad-brep]`/`[cad-validator]` cases.
- Docs: product scope, FORMAT-SUPPORT, file-format design, UX/installer design,
  README, ADR-0063, fixture README, `docs/0.7.0/progress/T08.md`.

### Readers, units, colors, licenses
- Readers: `IGESCAFControl_Reader` (via the in-memory model path above) and
  `BRepTools::Read` + `XCAFDoc_ShapeTool::AddShape`.
- Units: IGES reports the verified post-transfer `0.001` m/unit (OCCT system
  millimetre); absent unit flag → warning 3010, declared non-mm → warning 3011.
  BREP reports `0.0` (no authored unit) and warning 3012; up axis `Unknown` for
  both.
- Colors: IGES colors/transparency and subfigure assembly structure where the
  entities represent them (fixture asserts two materials and translated
  occurrences). BREP has no color/unit data and uses the neutral material.
- Fixture licenses: all CAD fixtures are generated from primitives constructed
  in `GenerateCadFixtures.cpp`; they contain no third-party CAD content.

### Deviations / residual limitations
- The OCCT overlay port now includes `TKDEIGES` (port-version 2). In this
  workspace the new toolkit was built incrementally in the existing vcpkg blds
  directories and copied into the gitignored installed closure to avoid a full
  multi-hour OCCT rebuild. A clean restore should rebuild it automatically; that
  path was not exercised here.
- IGES/BREP claim no thumbnail family; the provider's OCCT closure remains
  STEP-only.
- BREP truncation after a complete `TShapes` header may be detected by
  `BRepTools::Read` rather than preflight; it still maps to typed
  `MalformedData`.
- No IGES inch-authoring fixture was added, so the 3011 non-mm warning is
  unit-tested only through the reader's unit-flag logic, not an end-to-end
  fixture.

### Checks (Release x64)
- `x64\Release\Tests.ImportIsolation.exe "[cad-iges],[cad-brep]"` → 90
  assertions / 8 cases, pass.
- `x64\Release\Tests.ImportIsolation.exe "[step-002],[step-003],[step-004],
  [step-005],[step-006],[step-008],[step-tolerance],[cad-validator]"` → 901
  assertions / 52 cases + 1 expected skip, pass.
- `x64\Release\Tests.Unit.exe "[wire-format],[compatibility],[format-catalog],
  [presentation],[open-as],[format-installer]"` → 901 assertions / 32 cases,
  pass.
- Debug `Preview3DStepHost` builds successfully with the debug `TKDEIGES.lib`.
- No new OCCT linkage was added to `Preview3D.exe`, the general worker, or the
  thumbnail provider; only the dedicated host project/target changed.

### Remaining work
None blocking. The clean-restore validation and the inch-IGES fixture are
recorded as follow-ups in `docs/0.7.0/progress/T08.md`.

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
- [ ] Add `Iges` and `Brep` format IDs to the catalog and `SourceFormatId`
  (append-only), and extend the broker/bridge routing to the CAD host.
- [ ] Generalize the host request with a `CadFormat` field (`Step`, `Iges`,
  `Brep`; leave `Xbf`/`FCStd` for T09) while keeping the existing STEP opcode
  behavior compatible. A distinct format must be validated by the host; the
  worker must never accept a CAD opcode. Keep the executable name/path
  (`Preview3DStepHost.exe` under `StepHost\`) stable unless the packaging and
  AppContainer ACL scripts are updated in the same change.
- [ ] Implement bounded preflights:
  - **IGES**: verify the section-letter layout (S/G/D/P/T), line/record and
    entity/reference limits, size cap; reject control bytes/truncation.
  - **BREP**: verify the OCCT ASCII/binary header/size shape; reject malformed
    or over-budget input before `BRepTools::Read`.
- [ ] Implement adapters:
  - IGES via `IGESControl_Reader` or `IGESCAFControl_Reader` (colors/XDE where
    available);
  - BREP via `BRepTools::Read` from the mapped stream;
  - reuse the existing STEP shape-to-normalized-chunk pipeline so transforms,
    bounds, per-face/shape colors and material grouping stay consistent.
- [ ] Metadata: up axis `Unknown` by design; a verified positive metre factor
  where the format provides units, otherwise the documented default/warning
  (do not invent exact units). Keep the host’s commit/time budgets.
- [ ] Extend `SharedSectionValidator` for the new format IDs: require
  `UpAxisId::Unknown` and a positive metre factor where the format provides
  units; add hostile-worker cases for wrong-axis/wrong-unit sections.
- [ ] Update installer associations, open-dialog filters, `SourceFormatLabel`,
  docs and README for `.igs`, `.iges`, `.brep`. Use T03’s catalog-driven path,
  not a new hard-coded list.
- [ ] Add fixtures:
  - `cheese.igs` / `f3d.igs` or equivalent small IGES;
  - `cheese.brep` / `f3d.brep` or equivalent small BREP;
  - malformed/truncated/over-budget cases;
  - color/assembly cases.
  Verify fixture licenses before committing; generate equivalents if needed.
- [ ] Add `[cad-iges]` and `[cad-brep]` cases asserting typed outcomes, chunk
  counts, bounds and colors. Include a hostile/truncated case.
- [ ] Update `docs/design/01-product-scope.md` (IGES/BREP in scope) and
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
- [ ] `x64\Release\Tests.ImportIsolation.exe "[cad-iges],[cad-brep]"` passes.
- [ ] Positive IGES/BREP fixtures open through the isolated host; malformed and
  over-budget fixtures fail typed.
- [ ] Colors/transforms/bounds match the STEP normalization invariants where
  the source supports them.
- [ ] The installer and open dialog list `.igs`, `.iges`, `.brep`; the catalog
  is the source.
- [ ] No OCCT import appears in `Preview3D.exe`, the general worker or the
  provider DLL.
- [ ] Hand-off records the readers used, unit defaults and fixture licenses.

## Hand-off
_(filled in by the implementing session: host request shape, preflight limits,
reader choice, color support, installer entries and any residual IGES/BREP
limitations.)_

---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[cad-xbf],[cad-fcstd]"
---
# T09 — OCCT host: XBF and FCStd

## Goal
`.xbf` (OCCT binary XCAF) and `.fcstd` (FreeCAD Standard) open through the same
isolated CAD host, using the bounded ZIP/XML and binary readers already implied
by the OCCT dependency. This closes the FreeCAD/OCCT-native part of the CAD
long tail without adding a new runtime.

## Context (read first)
- `docs/proposal.md` §3.3 — OCCT host breadth.
- T08 — `CadFormat`, host routing, preflight and adapter pattern.
- `import-worker/src/ThreeMfOpcPreflight.{h,cpp}` and
  `import-worker/src/UsdZipPreflight.{h,cpp}` — bounded ZIP structure checks
  that FCStd can reuse (source, not process state).
- OCCT `BinTools`/`BinXCAFDrivers`, `BRepTools`, `XCAFDoc` APIs in the pinned
  closure.
- F3D reference: `plugins/occt/module/vtkF3DFCStdReader.cxx` for the FreeCAD
  document/Part/visibility/color cases; `plugins/occt/CMakeLists.txt` for XBF.
- F3D fixtures: `cheese.xbf`, `f3d.xbf`, `arch_floor.FCStd`,
  `partdesign_body.FCStd`, `link.FCStd`, etc. Verify licenses before copying.

## Scope
- [ ] Add `Xbf` and `Fcstd` to the catalog/`CadFormat` and host routing.
- [ ] XBF:
  - verify the OCCT binary magic/version and bound the file size;
  - read with `BinTools`/`BinXCAFDrivers` through the mapped handle;
  - preserve colors/assembly where present; up axis `Unknown`, verified unit
    factor where available.
- [ ] FCStd:
  - run a bounded ZIP preflight (reject encrypted/multi-disk/streaming-data
    descriptor entries; per-entry/aggregate expansion limits; required
    `Document.xml` and shape entries);
  - parse `Document.xml` with a bounded XML reader, enforcing node/depth/size
    limits and rejecting external/absolute paths;
  - read the common Part/PartDesign shape payload (`PartShape.brp` or the
    document’s equivalent), colors, visibility and placements;
  - start with the common single-part/PartDesign case; unsupported FreeCAD
    objects become warnings, not whole-document failures.
- [ ] Reuse T08’s normalized chunk writer and per-definition budgets. Do not
  create a second CAD-to-wire path.
- [ ] Extend `SharedSectionValidator` for `Xbf`/`Fcstd` using the same
  unknown-up/positive-unit rules as T08, and add hostile-worker cases.
- [ ] Update installer associations, open-dialog filters, labels, docs and
  README for `.xbf` and `.fcstd` through T03’s catalog path.
- [ ] Add fixtures:
  - positive XBF with colors and an assembly;
  - positive FCStd with a PartDesign body, a color/material case and a nested
    placement;
  - malformed/truncated XBF; malformed/oversized/zip-bomb FCStd; a document with
    an unsupported external link.
- [ ] Add `[cad-xbf]` and `[cad-fcstd]` cases asserting typed outcomes, chunk
  counts, bounds/colors and that no path/network access occurs.
- [ ] Update `docs/design/01-product-scope.md` and `docs/FORMAT-SUPPORT.md`.

## Out of scope
- Full FreeCAD feature support (PartDesign history, sketches, expressions,
  assemblies, FEM, CAM, external links). Render the evaluated Part shape only.
- XBF write/export.
- IGES/BREP (→ T08) and STEP tolerance (→ T07).
- Wireframe output (→ T17).

## Design notes
- FCStd is a ZIP package; reuse the existing bounded ZIP logic rather than
  trusting entry names or sizes. No extraction to disk.
- The `Document.xml` parser must not resolve external paths or FreeCAD object
  references outside the package. Unsupported objects are skipped with a
  bounded warning.
- Do not run FreeCAD macros or Python; this is a static shape read only.
- XBF is OCCT-native and may be large; preflight before `BinTools` and keep the
  existing job/commit ceilings.
- The host never receives a path; all reads come from the mapped handle.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe "[cad-xbf],[cad-fcstd]"` passes.
- [ ] Positive XBF/FCStd fixtures open through the isolated host; malformed,
  zip-bomb and unsupported-link fixtures fail or degrade with the documented
  typed outcome.
- [ ] Colors/transforms/visibility match the source where supported; no external
  path is opened.
- [ ] Installer/open dialog list `.xbf` and `.fcstd`; the catalog is the source.
- [ ] No OCCT import appears in the viewer, general worker or provider DLL.
- [ ] Hand-off records the FCStd subset, ZIP limits and fixture licenses.

## Hand-off
_(filled in by the implementing session: XBF reader, FCStd ZIP/XML limits,
supported FreeCAD object subset, installer entries and known omissions.)_

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
- [x] Add `Xbf` and `Fcstd` to the catalog/`CadFormat` and host routing.
- [x] XBF:
  - verify the OCCT binary magic/version and bound the file size;
  - read with `BinTools`/`BinXCAFDrivers` through the mapped handle;
  - preserve colors/assembly where present; up axis `Unknown`, verified unit
    factor where available.
- [x] FCStd:
  - run a bounded ZIP preflight (reject encrypted/multi-disk/streaming-data
    descriptor entries; per-entry/aggregate expansion limits; required
    `Document.xml` and shape entries);
  - parse `Document.xml` with a bounded XML reader, enforcing node/depth/size
    limits and rejecting external/absolute paths;
  - read the common Part/PartDesign shape payload (`PartShape.brp` or the
    document’s equivalent), colors, visibility and placements;
  - start with the common single-part/PartDesign case; unsupported FreeCAD
    objects become warnings, not whole-document failures.
- [x] Reuse T08’s normalized chunk writer and per-definition budgets. Do not
  create a second CAD-to-wire path.
- [x] Extend `SharedSectionValidator` for `Xbf`/`Fcstd` using the same
  unknown-up/positive-unit rules as T08, and add hostile-worker cases.
- [x] Update installer associations, open-dialog filters, labels, docs and
  README for `.xbf` and `.fcstd` through T03’s catalog path.
- [x] Add fixtures:
  - positive XBF with colors and an assembly;
  - positive FCStd with a PartDesign body, a color/material case and a nested
    placement;
  - malformed/truncated XBF; malformed/oversized/zip-bomb FCStd; a document with
    an unsupported external link.
- [x] Add `[cad-xbf]` and `[cad-fcstd]` cases asserting typed outcomes, chunk
  counts, bounds/colors and that no path/network access occurs.
- [x] Update `docs/design/01-product-scope.md` and `docs/FORMAT-SUPPORT.md`.

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
- [x] `x64\Release\Tests.ImportIsolation.exe "[cad-xbf],[cad-fcstd]"` passes.
- [x] Positive XBF/FCStd fixtures open through the isolated host; malformed,
  zip-bomb and unsupported-link fixtures fail or degrade with the documented
  typed outcome.
- [x] Colors/transforms/visibility match the source where supported; no external
  path is opened.
- [x] Installer/open dialog list `.xbf` and `.fcstd`; the catalog is the source.
- [x] No OCCT import appears in the viewer, general worker or provider DLL.
- [x] Hand-off records the FCStd subset, ZIP limits and fixture licenses.

## Hand-off

### What landed

- **Catalog/routing:** `SourceFormatId::Xbf = 16` / `Fcstd = 17`,
  `CadFormat::Xbf = 3` / `Fcstd = 4`; `FormatId::Xbf`/`Fcstd` descriptors use
  `ImporterKind::CadHost` and `Preview3DStepHost.exe`. Broker format mapping,
  expected-format checks, Tier-B classification, producer pairing and
  `SharedSectionValidator` all include both formats. `ProgIdForFamily` and
  `Preview3D.nsi` add `.xbf` / `.fcstd`; open-dialog/Open As stay catalog-driven.
- **XBF reader:** `CadPreflight` verifies `BINFILE` + binary-document version
  and the 2 GiB source cap. `StepXdeAdapter` opens the mapped stream through
  `TDocStd_Application` + `BinXCAFDrivers`, preserving colors, assemblies and
  placements. The XCAF length unit is reported when positive; absence becomes
  the documented `0.001` millimetre default with feature 3013. Up axis is
  `Unknown`.
- **FCStd reader:** `FcstdPreflight` rejects encryption, multi-disk archives,
  data descriptors, unsafe/duplicate names, overlaps, per-entry >256 MiB,
  aggregate >512 MiB and >200:1 expansion before decode. `FcstdReader` parses
  `Document.xml`/`GuiDocument.xml` with 8 MiB / 200k-node / depth-64 limits,
  rejects DOCTYPE/ENTITY and unsafe shape paths, and builds the evaluated
  Part/PartDesign `*.brp` shapes with colors, transparency, visibility, local
  links, link arrays and nested placements. External links (3015) and unknown
  objects (3016) degrade to warnings. Geometry is millimetres (3014), up axis
  `Unknown`.
- **Tests/fixtures:** positive `assembly.xbf` and `partdesign.FCStd`; negative
  truncated/malformed/missing-document/zip-bomb/oversized packages and
  cross-format reads. `[cad-xbf]`, `[cad-fcstd]` and `[cad-validator]` cases
  assert typed outcomes, format/axis/unit, counts, colors, bounds and
  visibility.
- **Docs:** `docs/FORMAT-SUPPORT.md`, `docs/design/01-product-scope.md`,
  `README.md`, `docs/design/adr/0064-cad-host-xbf-fcstd.md`,
  `tests/fixtures/cad-spike/README.md`, `docs/0.7.0/progress/T09.md`.

### FCStd subset and limits

- Supported: a Part/PartDesign object with an evaluated `Part file="...brp"`
  payload; `ShapeColor`, `Transparency`, visibility (Document.xml with
  GuiDocument.xml fallback), `DiffuseColor.bin` face colors, local
  `LinkedObject`, link arrays (`PlacementList`), and nested
  `App::Part`/`Assembly` placements.
- Not evaluated: sketches, PartDesign history, expressions, FEM/CAM, FreeCAD
  macros/Python, assemblies beyond nested placements, external documents,
  `ShapeAppearance`/`MaterialList` material lobes.
- ZIP limits: 2 GiB source, 4096 entries, 256 MiB/entry, 512 MiB aggregate,
  200:1 ratio, stored/Deflate only, single disk, no encryption/data
  descriptors. XML: 8 MiB, 200,000 nodes, depth 64, no DOCTYPE/ENTITY.

### Fixture licenses

All fixtures are generated from repository-owned OCCT primitives
(`GenerateCadFixtures.cpp`) or the repository's own `part.brep`
(`GenerateFcstdFixtures.py`). No third-party FreeCAD/XBF sample is copied, so
the repository test-data terms apply.

### Checks (Release x64)

- `x64\Release\Tests.ImportIsolation.exe "[cad-xbf],[cad-fcstd]"` — 7 cases /
  101 assertions, **pass**.
- `[cad-iges],[cad-brep],[cad-validator],[cad-xbf],[cad-fcstd]` — 17 cases /
  211 assertions, **pass**.
- `[step-002]…[step-tolerance]` + CAD filters — 68 cases / 1098 assertions
  + 1 expected skip, **pass**.
- `Tests.Unit.exe "[format-catalog],[format-installer],[open-as],
  [wire-format],[compatibility],[presentation]"` — 33 cases / 986 assertions,
  **pass**.
- `Preview3D`, `Preview3DStepHost`, `Tests_ImportIsolation` and `Tests_Unit`
  Release builds succeed.

### Deviations / remaining

- The StepHost manifest now declares `zlib`, but `VcpkgManifestInstall` stays
  false; this workspace copied `zlib.h`/`zconf.h` and `zs.lib` from the root
  vcpkg tree into the StepHost installed tree. A clean restore should be
  exercised (recorded as a follow-up).
- XBF header admission cannot detect a file truncated after a valid 15-byte
  header; the OCCT reader returns typed `MalformedData`.
- Thumbnail families remain `None`; provider support is a later task.
- No real FreeCAD-authored fixture was added; the synthetic package covers the
  documented subset deterministically.

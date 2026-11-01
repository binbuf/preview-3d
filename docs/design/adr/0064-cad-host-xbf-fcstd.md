# ADR-0064 — XBF and FreeCAD Standard reuse the OCCT CAD host

**Status:** Accepted

## Context

The dedicated zero-capability CAD host already owns the OCCT closure, bounded
admission, the XDE planner, tessellation budgets and the normalized wire
writer for STEP, IGES and BREP. Two OCCT-native long-tail formats remain:
`.xbf` (OCCT binary XCAF documents) and `.fcstd` (FreeCAD Standard packages
containing evaluated Part/PartDesign shapes). Adding a second runtime or a
second CAD-to-wire path would duplicate the containment and scene contract.

## Decision

- `CadFormat::Xbf = 3` and `CadFormat::Fcstd = 4` extend the existing closed
  discriminator and `StartCadImportFromFile` request. `SourceFormatId::Xbf =
  16` and `Fcstd = 17` are append-only wire identities. Both descriptors use
  `ImporterKind::CadHost`, `ThumbnailFamily::None`, and the catalog-owned
  installer/open path.
- XBF admission verifies the fixed `BINFILE` magic plus the supported
  little-endian binary-document version and the existing source byte cap. The
  document is opened through `TDocStd_Application` with `BinXCAFDrivers` from
  the inherited mapped handle. Colors, assembly labels and placements are
  preserved by the existing XDE planner. The XCAF length unit is reported when
  positive; absence uses the documented OCCT millimetre default with a
  compatibility warning. Up axis is always `Unknown`.
- FCStd is never extracted to disk. A host-local bounded ZIP scanner rejects
  encryption, multi-disk archives, data descriptors, unsafe/duplicate names,
  overlapping entries, per-entry >256 MiB, aggregate >512 MiB and >200:1
  expansion before decoding. It requires `Document.xml` and at least one
  `*.brp` entry.
- `Document.xml`/`GuiDocument.xml` are parsed by a bounded reader (8 MiB,
  200,000 nodes, depth 64). DOCTYPE/ENTITY declarations are rejected; shape
  file references must be safe package-relative paths. No FreeCAD macro,
  Python, expression engine or external document is opened.
- The supported FreeCAD subset is the evaluated Part/PartDesign shape payload,
  shape/face colors, transparency, visibility, local links and link arrays,
  and nested `App::Part`/`Assembly` placements. External links and unknown
  objects are skipped with bounded compatibility warnings, not document
  failure.
- FCStd geometry is millimetres by FreeCAD convention: the wire reports
  `metersPerUnit == 0.001` and `UpAxisId::Unknown`, and adds a compatibility
  note rather than implying authored unit metadata. XBF requires a positive
  metre factor; FCStd requires exactly `0.001` in `SceneMetadataRules`.
- Both formats reuse `StepXdeLimits`, the per-definition mesher/emitter and
  the compatibility-report chunk. No new wire layout, second reader path or
  OCCT import in the viewer, general worker or thumbnail provider is added.

## Consequences

- XBF and FCStd inherit the existing job/CPU/wall-clock ceilings, cancellation,
  per-definition isolation and section validation. The ZIP/XML limits are
  additional admission policy, not a general archive extractor.
- FCStd renders only the stored evaluated Part shape. Sketches, PartDesign
  history, expressions, FEM/CAM, assemblies beyond nested placements, and
  external links remain unsupported and are documented.
- The clean-clone STEP-host manifest now includes `zlib`; the current workspace
  reused the repository zlib import library, matching the T08 evidence
  approach for the incremental OCCT closure.
- Thumbnail families remain `None`; adding them later is a provider task and
  does not change this host contract.

# ADR-0063 — IGES and OCCT BREP reuse the dedicated CAD host

## Status
Accepted (T08, 0.7.0).

## Context
T02 reserved `FormatId::Iges`/`Brep`, and the STEP host already owns the only
OCCT closure in the product: a zero-capability process that receives an
inherited read-only handle, runs product-owned admission, and emits normalized
wire records. IGES and OCCT BREP are common CAD interchange inputs that fit the
same normalized scene contract but need different readers and their own bounded
admission. Adding a second OCCT runtime, or linking OCCT into the viewer/worker/
provider, would duplicate the trust boundary and the large kernel payload.

OCCT's IGES work library implements only `ReadFile`; `IFSelect_WorkLibrary::
ReadStream` returns failure. The host has no path authority, so a temporary file
or a path-based fallback would either add a filesystem side effect or require a
new sandbox ACL.

## Decision
- Keep one executable and payload (`Preview3DStepHost.exe` under `StepHost\`).
  Add `SourceFormatId::Iges` (14) and `Brep` (15), both owned by
  `ImporterKind::CadHost`, and route them through the broker's existing
  `ImportProducer::StepHost` process identity.
- Keep the STEP opcode/request unchanged. Add `ControlOpcode::
  StartCadImportFromFile` and `ParseCadFileRequest` (the seven STEP fields plus
  a closed `CadFormat` and reserved zero). Only the dedicated host accepts it;
  the general worker rejects CAD formats before launch and the host validates
  the discriminator before admission.
- Admit IGES as a complete 80-column S/G/D/P/T record stream: bounded bytes,
  records, entity records and parameter tokens; increasing sequence numbers;
  section ordering; no control bytes or truncation.
- Admit BREP as a `CASCADE Topology` V1-V3 shape set (optional
  `DBRep_DrawableShape` prefix): bounded bytes, lines, `TShapes` and `Locations`
  tables; reject malformed/truncated input before `BRepTools::Read`.
- Read IGES by feeding the admitted records to the pinned OCCT C record parser
  and installing the resulting `IGESData_IGESModel` on the XDE reader's session.
  No path or temporary file is created. BREP is read with `BRepTools::Read` from
  the same mapped stream and added to a fresh XCAF shape document.
- Reuse the STEP planner/tessellator/emitter. Up axis stays `Unknown`. IGES
  reports the verified positive post-transfer OCCT system millimetre (0.001
  m/unit); an absent global unit flag assumes millimetres with a compatibility
  warning. BREP reports `0.0` (no authored unit) and a warning; it is never
  silently labelled millimetres.
- Extend the catalog, installer ProgIDs/associations, open-dialog/`--format`
  lists and `SharedSectionValidator` through the existing catalog-driven path.
  The thumbnail provider keeps its STEP-only OCCT closure and claims no
  thumbnail family for IGES/BREP.

## Consequences
- The constrained OCCT port now includes `TKDEIGES` (and its transitive B-rep/
  IGES toolkits). The shipped CAD payload grows, but there is still exactly one
  OCCT consumer and one CAD sandbox identity.
- IGES/BREP inherit the STEP tessellation profile, per-definition failure
  isolation, commit/time budgets, cancellation and compatibility reporting.
- IGES colors/assemblies depend on what the IGES entities represent; BREP has
  no color or unit data. Both remain bounded Tier B formats with typed failures.
- A future format that needs OCCT (XBF/FCStd in T09) should extend the same
  `CadFormat` route rather than create another CAD process.

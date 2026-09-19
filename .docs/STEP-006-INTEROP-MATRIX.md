# STEP-006 interoperability matrix and self-contained scope acceptance

Status: **complete**
Completed: 2026-09-19
Design authority: [stp2.md](stp2.md) (STEP-006); [stp.md](stp.md);
[design/03-file-formats-and-ingestion.md](design/03-file-formats-and-ingestion.md)
Basis: [STEP-004-VERIFICATION.md](STEP-004-VERIFICATION.md),
[STEP-005-VERIFICATION.md](STEP-005-VERIFICATION.md)

This is the checked-in visual contract for the bounded static STEP/STP preview
subset. Every row is exercised end-to-end through the STEP-004/005 pipeline by
`[step-006][matrix]` in
[`tests/import-isolation/StepInteropTests.cpp`](../tests/import-isolation/StepInteropTests.cpp),
which names the exact typed outcome for each immutable fixture. Adding a fixture
without a row, or a row without a fixture, is a test failure.

## Supported visual contract

| Fixture | Schema / AP | Content | Expected result | Key asserted facts |
| --- | --- | --- | --- | --- |
| `part_ap203.stp` | AP203 `CONFIG_CONTROL_DESIGN` | B-rep analytic solid with through-hole, shape color | **Accept** | 1 geometry, 1 instance, 1 material, unknown up axis, positive metre factor |
| `part_ap214.stp` | AP214 `AUTOMOTIVE_DESIGN` | B-rep analytic solid, shape color | **Accept** | 1 geometry, 1 instance |
| `part_ap242.stp` | AP242 managed model based 3D engineering | B-rep analytic solid, shape color | **Accept** | 1 geometry, 1 instance |
| `tessellated_ap242.stp` | AP242 | B-rep **plus** an authored `TESSELLATED_SHAPE_REPRESENTATION` (`TESSELLATED_SOLID`/`TRIANGULATED_FACE`/`COORDINATES_LIST`) | **Accept** | 1 geometry, 1 instance, deterministic repeat output |
| `tessellated_only_ap242.stp` | AP242 | Tessellated-only representation (`TRIANGULATED_SURFACE_SET`), no B-rep fallback | **Accept** | 1 geometry, 1 instance |
| `assembly_ap214.stp` | AP214 | Shared box definition reused twice, cylinder, translated occurrence, transparent color | **Accept** | 2 reusable definitions, 3 occurrences, 1 transparent material, single root |
| `assembly_nested_ap214.stp` | AP214 | Nested sub-assembly reused twice | **Accept** | 2 definitions, 5 instances, recursive transforms, single acyclic root |
| `instance_color_ap214.stp` | AP214 | One definition placed three times; middle instance color overrides | **Accept** | 1 geometry, 3 instances, 2 materials without geometry duplication |
| `face_color_ap214.stp` | AP214 | Per-face subshape colors, no shape-level color | **Accept** | 2 seam-split geometry groups, 2 materials |
| `inch_part_ap214.stp` | AP214 `CONVERSION_BASED_UNIT('INCH')` | Inch-authored analytic part | **Accept** | `UpAxisId::Unknown`, positive verified metre factor describing the normalized geometry |

## Intentional exclusions

Every row below has an asserted typed outcome and never reaches a filesystem,
network, plug-in, or child-process call.

| Fixture | Family | Expected result | Why |
| --- | --- | --- | --- |
| `no_geometry_ap242.stp` | Geometry-free product metadata | `EmptyGeometry` | A syntactically valid Part-21 file with no transferable shape representation is not an empty success; the adapter classifies it before the broker can accept a scene with no geometry. |
| `unsupported_schema.stp` | Unknown/unsupported AP profile, geometry-free | `EmptyGeometry` | `FILE_SCHEMA` is classification/diagnostics only, never a trust decision; with no visual geometry it is the geometry-free result, not an extension or schema rejection. |
| `external_document_ap214.stp` | Required external document via `FILE_POPULATION` | `UnsupportedRequiredFeature` | The external-reference no-go (below). |
| `external_document_relative_ap214.stp` | Relative `DOCUMENT_FILE` reference | `UnsupportedRequiredFeature` | A relative path is still a path; the host never resolves one. |
| `external_document_absolute_ap214.stp` | Absolute, UNC, and URL `DOCUMENT_FILE` references | `UnsupportedRequiredFeature` | The declaration is rejected from the lexical token stream before any resolver could see it, so no path form can escape the sandbox. |
| `faceted_invalid_ap242.stp` | Authored faceted data with an out-of-range triangle index | `MalformedData` | Unvalidated faceted data is never trusted GPU input; invalid tessellation topology fails typed instead of surfacing a kernel exception. |
| Any non-`ISO-10303-21` bytes | Binary/XML/compressed/UTF-16 | `MalformedData` / `UnsupportedEncoding` | Physical-file admission rejects non-clear-text encodings before the OCCT reader is reachable. |

## External STEP documents: recorded no-go

The product owner accepts that **external STEP documents are out of scope** for
this slice, per the STEP-001 finding that OCCT's external-document resolver is
path-based and protected with no stream/callback hook:

- The dedicated host never opens a path, and there is no brokered
  stream-resolver protocol to request one. Supporting external documents would
  require a new brokered-resolver design and product-owner approval; the
  AppContainer/handle-only boundary is not weakened to gain it.
- Any `FILE_POPULATION` or `DOCUMENT_FILE` declaration is discovered by
  `StepPart21Preflight` and returns `UnsupportedRequiredFeature` before OCCT
  sees it. This is stated consistently in the code
  (`StepPreflightLimits::maxExternalDocuments = 0`), the tests, and the public
  documentation.
- `STEP-006` fuzzes declaration discovery (`StepFuzz`, domain
  `DeclarationDiscovery`) and every path form (relative, absolute, UNC, URL,
  case/whitespace variants) so the no-go boundary cannot be bypassed into a
  filesystem or network access.

## Healing decision

STEP-006 closes the STEP-001 healing comparison with an explicit,
product-approved decision recorded as `step_host::kStepHealingPolicy =
StepHealingPolicy::None` in
[`compatibility-host-step/src/StepTessellationProfile.h`](../compatibility-host-step/src/StepTessellationProfile.h):

- **Adopted: no healing.** Invalid geometry fails typed rather than being
  mutated, so the accepted subset is exactly the authored geometry and the
  visual result is deterministic and producer-independent. This is what the
  STEP-001 spike used.
- **Rejected for this slice: a narrowly pinned `ShapeFix`/`XSAlgo` sequence.**
  It had no measured deterministic visual benefit on the accepted corpus and
  adds a second OCCT algorithm surface with its own failure modes.
- **Rejected: broad automatic healing.** It silently mutates geometry, depends
  on unmeasured OCCT heuristics, and can turn a typed unsupported result into an
  apparently faithful but altered model.

Adopting the existing behavior changes no emitted byte, so it does not bump
`kStepTessellationProfileVersion`/`kStepImporterVersion`. A future task that
adopts a healing sequence must bump them because output could change.

## Fixture provenance

All fixtures live in [`tests/fixtures/stp-spike/`](../tests/fixtures/stp-spike/).
The analytic/assembly fixtures and the two tessellated fixtures were generated
with the OCCT XDE writer by `GenerateStepFixtures.cpp` (the product never links
the writer; the writer embeds a creation timestamp, so the committed bytes, not
the generator, are the durable artifact). `faceted_invalid_ap242.stp` is a
deterministic derivation of `tessellated_only_ap242.stp` with two triangle
indices changed to an out-of-range node. The external/geometry-free fixtures are
hand-authored clear-text Part-21.

| Fixture | Bytes | SHA-256 |
| --- | ---: | --- |
| `part_ap203.stp` | 21,168 | `EB53A75D416826582AE497FA2CA5E0AC1DAE14346D3D6A378AFE4C023A82C13A` |
| `part_ap214.stp` | 19,632 | `5A200F99284DBDD96471C4357B940CF7BC7BF69FD7985363B5D2EA3A498DE338` |
| `part_ap242.stp` | 19,664 | `FC8E09CC6FD3A583808D75864821F0518FA2AEE9C34025045EFAA9A031BB5697` |
| `assembly_ap214.stp` | 24,428 | `71FF7BC7CC514235E42F0619F9193CEBA54306FFF2EB2A2B7C8503F906B7E7EB` |
| `assembly_nested_ap214.stp` | 26,404 | `24EE7D3F47C73E49C1ADE45449F6CC9E8274B3980BD862AB9FC581CC73A590B5` |
| `instance_color_ap214.stp` | 19,794 | `7D7EC2B6DA83832FC67851AB6083C61718832DFDE8CE899B5B553A35D01A5079` |
| `face_color_ap214.stp` | 17,274 | `E79CADD677A9C2F1FCEB7004B240937E91ABBE16BDAE643DAE26028A33FF152D` |
| `inch_part_ap214.stp` | 19,809 | `24FD6BDC553FFCF5BE0DF036A7D963A7F81165D60749981BE1C4BC98F3FF4210` |
| `tessellated_ap242.stp` | 18,018 | `27B80AE303917A45465C4CB0648DDCB6D0EB9B2E374AFA9EAF7266F6B1AC7B2B` |
| `tessellated_only_ap242.stp` | 3,614 | `4DD565E1135624C5BF8A85AAC22EEEFC0B17E80FECE07AC2A46A41B34635F365` |
| `faceted_invalid_ap242.stp` | 3,616 | `C43DCDD616D3693D1813E1E2E63BE127506D6DFBF93BC1F15E4DC8E228F8439B` |
| `no_geometry_ap242.stp` | 1,285 | `D7551BD363A6A80987FA61F1A72ADA8EE1914AB526892C113ACE1E54895B3F23` |
| `unsupported_schema.stp` | 250 | `C9B137546CD0529B62826FD3DEDF0DEB0D565105B51440EE411AA6D12B5F503E` |
| `external_document_ap214.stp` | 322 | `21B26806B662598C67B300BBB3CA6AE0C27482256A75C146C5AF29F99170FFEB` |
| `external_document_relative_ap214.stp` | 287 | `7954FB073A9E735069DB8F75EAF157B2A18F92C8325C1C46BA8C69808CE90081` |
| `external_document_absolute_ap214.stp` | 489 | `FBBEA3B9C79D84B4543601997C6A96BA574175AFD342F459F6DA50D538C24816` |

## Verification

```text
x64/<Config>/Tests.ImportIsolation.exe "[step-006]"
```

`[step-006][matrix]` runs every row above through `RunImportSession`;
`[step-006][tessellated]` asserts deterministic repeated output for the
authored-tessellation fixture; `[step-006][healing]` pins the no-healing
decision; `[step-006][external][no-bypass]` and
`[step-006][external][fuzz]` prove declaration discovery across every path form
and malformed envelope. The standalone `StepFuzz` target (see
[`tests/fuzz/README.md`](../tests/fuzz/README.md)) fuzzes admission and
declaration discovery with sanitizers and no GPU.

## Known limits

- The tessellated fixtures are tiny synthetic parts; a genuine large AP242
  tessellated production part, official AP242 conformance examples, and golden
  render images remain STEP-008 corpus work.
- Layer/name data is accepted but intentionally not displayed; there is no CAD
  tree, layer browser, PMI/GD&T, measurement, or editing surface.
- The external no-go is a product scope decision, not a qualification gap; the
  AppContainer/handle-only boundary stays unchanged.

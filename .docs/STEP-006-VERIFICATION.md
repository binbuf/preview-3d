# STEP-006 verification: interoperability closure and self-contained scope acceptance

Status: **complete**
Completed: 2026-09-19
Design authority: [stp2.md](stp2.md) (STEP-006); [stp.md](stp.md);
[design/README.md](design/README.md)
Basis: [STEP-004-VERIFICATION.md](STEP-004-VERIFICATION.md),
[STEP-005-VERIFICATION.md](STEP-005-VERIFICATION.md)
Checked-in contract: [STEP-006-INTEROP-MATRIX.md](STEP-006-INTEROP-MATRIX.md)

## Result

STEP-006 closes the accepted static-preview contract without weakening the
handle-only/AppContainer boundary:

1. **Interoperability matrix.** Every supported and excluded family has an
   immutable fixture and a named typed outcome, exercised end-to-end through
   the STEP-004/005 pipeline. See
   [STEP-006-INTEROP-MATRIX.md](STEP-006-INTEROP-MATRIX.md).
2. **AP242 tessellated representations.** An AP242 fixture carrying both a
   B-rep and an authored `TESSELLATED_SHAPE_REPRESENTATION` and a
   tessellated-only fixture both import; invalid faceted topology fails typed.
3. **Healing decision.** No healing is adopted as an explicit versioned
   product constant (`step_host::kStepHealingPolicy`), after comparing no
   healing, a narrowly pinned `ShapeFix`/`XSAlgo` sequence, and broad automatic
   healing.
4. **External-document no-go recorded.** The product-owner decision that
   external STEP documents are out of scope is reflected in the design support
   matrix, ADR-017, the public limitations, and the tests; declarations still
   return `UnsupportedRequiredFeature` before OCCT.
5. **Declaration-discovery fuzz.** A standalone sanitizer target (`StepFuzz`)
   mutates admission and declaration discovery with no GPU, path, or process.

The route remains private to the broker with `ImportFormat::Step`. No public
picker, drag/drop, Open With, installer, or thumbnail path recognizes
`.step`/`.stp` yet; that is STEP-007.

## Work item 1: interoperability matrix

`[step-006][matrix]` in
[`tests/import-isolation/StepInteropTests.cpp`](../tests/import-isolation/StepInteropTests.cpp)
iterates `kMatrix` and asserts the exact outcome per fixture. The rows are
listed in [STEP-006-INTEROP-MATRIX.md](STEP-006-INTEROP-MATRIX.md):

- supported: AP203/AP214/AP242 B-rep, AP242 B-rep+tessellated, AP242
  tessellated-only, assembly/reuse, nested assembly, instance color, face-color
  seam split, inch-authored units;
- excluded: geometry-free product metadata, geometry-free unknown schema,
  `FILE_POPULATION`, relative `DOCUMENT_FILE`, absolute/UNC/URL
  `DOCUMENT_FILE`, and invalid authored faceted topology.

New committed fixtures (hashes in the matrix doc):

```text
tessellated_ap242.stp           AP242 B-rep + TESSELLATED_SHAPE_REPRESENTATION
tessellated_only_ap242.stp      AP242 tessellated-only (TRIANGULATED_SURFACE_SET)
faceted_invalid_ap242.stp       derived: out-of-range triangle index
no_geometry_ap242.stp           product metadata, no shape
unsupported_schema.stp          geometry-free unknown FILE_SCHEMA
external_document_ap214.stp     FILE_POPULATION required external document
external_document_relative_ap214.stp   relative DOCUMENT_FILE
external_document_absolute_ap214.stp   absolute/UNC/URL DOCUMENT_FILE
```

## Work item 2: AP242 tessellated acceptance and fallback

The tessellated fixtures were generated with the OCCT XDE writer by
`GenerateStepFixtures.cpp` using `write.step.tessellated=On` (the default
`OnNoBRep` suppresses tessellated output when a B-rep exists) and the AP242
schema. The pinned reader translates the tessellated representation by default
(`read.step.tessellated=On`); the adapter's `AllowQualityDecrease=false`
preference keeps an authored triangulation instead of regenerating one.

Two adapter hardening changes landed with this work:

- **Geometry-free classification.** A valid Part-21 file with no transferable
  shape representation previously surfaced as `MalformedData` because OCCT
  reports it as a generic transfer failure. The adapter now checks
  `NbRootsForTransfer()` after `ReadStream` and returns `EmptyGeometry`, so the
  geometry-free family has a distinct typed outcome.
- **Kernel exceptions are malformed input.** OCCT reports invalid faceted
  topology (for example an out-of-range triangle index) by throwing
  `Standard_Failure`. `ReadStream`/`Transfer` are wrapped so those become
  `MalformedData`, and a boundary-level `catch (const Standard_Failure&)` maps
  any other kernel exception during traversal/extraction to `MalformedData`
  instead of `InternalImporterFailure`. No kernel message is copied.

The `faceted_invalid_ap242.stp` fixture exercises the second change: it fails
`MalformedData` instead of crashing or publishing invalid triangles.

## Work item 3: healing decision

`step_host::kStepHealingPolicy = StepHealingPolicy::None` in
[`compatibility-host-step/src/StepTessellationProfile.h`](../compatibility-host-step/src/StepTessellationProfile.h)
records the decision; `[step-006][healing]` pins it. The rationale is in
[STEP-006-INTEROP-MATRIX.md](STEP-006-INTEROP-MATRIX.md) and ADR-017. Adopting
the existing no-healing behavior changes no emitted byte, so it does not bump
`kStepTessellationProfileVersion` (still v3) or `kStepImporterVersion`.

## Work item 4: recorded self-contained scope

External STEP documents are out of scope by product decision:

- `StepPreflightLimits::maxExternalDocuments = 0`; any
  `FILE_POPULATION`/`DOCUMENT_FILE` declaration returns
  `UnsupportedRequiredFeature` from `StepPart21Preflight` before OCCT.
- The decision is recorded in
  [design/11-decisions-and-risks.md](design/11-decisions-and-risks.md) ADR-017,
  the [support matrix](design/03-file-formats-and-ingestion.md), the
  [product scope](design/01-product-scope.md),
  [design/README.md](design/README.md), the root
  [README.md](../README.md) limitations, and this verification.
- The route stays undiscoverable until STEP-007.

## Work item 5: declaration-discovery fuzz

`tests/fuzz/StepFuzz.cpp` (standalone, ASan/libFuzzer, no GPU) has four
domains: Part-21 admission (pure bytes and handle stream), declaration
discovery with the external ceiling raised, the production framed control
decoder with mutated `StartStepImportFromFile` request-flag masks and
`StepProgress` records, and the trusted normalized-output validator. Seeds are
materialized by `tests/fuzz/prepare_step_seeds.py`; see
[`tests/fuzz/README.md`](../tests/fuzz/README.md).

A 45-second Release smoke run over 40 fresh seeds completed **53,244
executions**, added 1,205 units, and reported **460 MiB peak RSS** with no
sanitizer finding or timeout.

## Verification

`x64/<Config>/Tests.ImportIsolation.exe "[step-002],[step-003],[step-004],[step-005],[step-006]"`
passes **35 cases / 706 assertions in both Debug and Release**. The new
`[step-006]` slice is **5 cases / 176 assertions**:

```text
[step-006][matrix]              every supported/excluded fixture produces its named
                                typed result through the real host
[step-006][tessellated]         AP242 authored tessellation imports deterministically
[step-006][healing]             the no-healing decision is explicit
[step-006][external][no-bypass] FILE_POPULATION/DOCUMENT_FILE path forms all reject
[step-006][external][fuzz]      malformed envelopes never produce a false accept
```

Release results and the exact `StepFuzz` command are recorded in the
[STEP-006-INTEROP-MATRIX.md](STEP-006-INTEROP-MATRIX.md) verification section.
The full `Tests.ImportIsolation` suite retains the pre-existing USD/FBX
failures documented for earlier tasks; none are STEP-related.

## Known limits and handoff

- **No genuine large AP242 tessellated production part, official AP242
  conformance examples, or golden render images.** The tessellated fixtures are
  tiny synthetic parts. The large-file corpus, published Tier-B budgets, and
  rendered golden images remain STEP-008.
- **No viewer exposure.** Extension discovery, activation, Open With,
  installer/portable payload, and documentation surfaces are STEP-007.
- **Healing remains off by decision.** A future pinned-healing sequence must
  bump the profile/importer version.
- **External STEP documents remain out of scope** and require a new
  brokered-resolver design plus product-owner approval before any revisit.

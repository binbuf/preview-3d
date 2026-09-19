# STEP-003 verification: self-contained XDE assembly scene adapter

Status: **complete — proceed to STEP-004**
Completed: 2026-09-19
Design authority: [stp.md](stp.md), [design/README.md](design/README.md)
Spike basis: [STEP-001-SPIKE-RESULTS.md](STEP-001-SPIKE-RESULTS.md)
Host basis: [STEP-002-VERIFICATION.md](STEP-002-VERIFICATION.md)

## Result

The bounded static STEP/STP preview subset now produces a real normalized
scene. `StepXdeAdapter` runs behind the existing
`StartStepImportFromFile` route, reads accepted ISO 10303-21 bytes exclusively
through the inherited read-only handle, transfers them with the pinned OCCT
7.8.1 `STEPCAFControl_Reader::ReadStream`, walks the XDE document, and emits
only the protocol-v10 node / reusable-geometry / mesh-instance / material
contract. The trusted viewer, the general worker, and the thumbnail provider
continue to link and load none of the OCCT closure.

No public picker, drag/drop, Open With, installer, or thumbnail path
recognizes `.step`/`.stp` yet. The route remains private to the broker with
`ImportFormat::Step`.

## What the adapter does

Production source: `compatibility-host-step/src/StepXdeAdapter.{h,cpp}`,
called by `RunStepHostImport` only after `StepPart21Preflight` admits the
physical file.

- **Enumeration.** One `NodePayload` per accepted occurrence and one reusable
  geometry record per definition/color seam. Assemblies are walked
  recursively; a component's `TopLoc_Location` becomes its node's local
  transform and nested occurrences compose in double precision using the same
  row-vector convention and `MultiplyAffine` semantics as
  `SharedSectionValidator`. Referenced simple-shape labels are built once and
  shared across every occurrence; geometry is never duplicated for reuse.
- **Validation.** Definition ancestry cycles, hierarchy depth over 256,
  singular/non-finite locations, non-finite vertices, and the function/node/
  material/triangle/vertex caps all produce typed failures. Orphan
  definitions (not reachable from a free shape root) are never drawn, and a
  document with no reachable supported geometry returns `EmptyGeometry`.
- **Transforms and bounds.** Node transforms are finite affine matrices
  (translation in elements 12..14, final column `{0,0,0,1}`). Instance world
  bounds are derived by transforming the referenced geometry's local AABB
  corners exactly as the broker independently recomputes them, so acceptance
  never depends on trusting the worker's own arithmetic.
- **Units and up axis.** `UpAxisId::Unknown` always; a positive verified metre
  factor always; absent or contradictory authored length units fail with
  `UnsupportedRequiredFeature` rather than assuming millimetres (see the unit
  finding below).
- **Appearance.** Instance/shape/subshape precedence: a component instance
  color overrides the whole occurrence without duplicating geometry;
  otherwise the shape-level color wins; otherwise per-face subshape colors
  split the definition into bounded seam groups; otherwise the neutral
  material (chunk id 0) is used. Colors are read as sRGB, converted to the
  linear `baseColorFactor`, and opacity below 1 maps to `AlphaModeId::Blend`.
  Materials are deduplicated by exact linear RGBA and capped by the Tier-B
  material limit.
- **Emission.** Nodes, materials, reusable geometry, and instances are written
  as one self-contained protocol-v10 section with disjoint deterministic chunk
  id ranges, a nonzero geometry source range (required by the broker), exact
  double origins, verified float bounds, per-chunk and section checksums, and
  complete dependency slots. Geometry is de-indexed with per-triangle flat
  normals and split at 65,536 triangles per chunk.
- **Diagnostics.** Every OCCT exception is caught at the adapter boundary and
  mapped to a closed product code; raw kernel messages, entity names, and
  source strings never cross the control channel. The optional-feature warning
  count is carried in a bounded `ImportStatus` record when nonzero.

The tessellation here is deliberately a fixed coarse policy
(`linearDeflection = 0.1`, relative; `angularDeflection = 0.5`). STEP-004 owns
the released coarse/display quality profile, progressive delivery, and the new
CAD-specific tessellation error codes.

## Unit finding (refines the STEP-001 handoff)

STEP-001 observed that OCCT normalizes transferred geometry to its Cascade
system unit and that only `FileUnits` exposes the authored unit *name*, then
recommended computing `metersPerUnit` from the authored unit explicitly. On
the pinned 7.8.1 build this verification measured the exact behavior directly:

| Fixture | `FileUnits` name | `LocalLengthUnit()` | `GetLengthUnit()` | Transferred bbox |
| --- | --- | --- | --- | --- |
| millimetre part (30 mm) | `millimetre` | 1.0 | 0.001 | 0..30 |
| inch-authored part (30 in) | `INCH` | 1.0 | 0.001 | 0..762 |

The reader normalizes coordinates to millimetres (`30 in -> 762 mm`);
`LocalLengthUnit()` and `SystemLengthUnit()` both report `1.0`, and
`GetLengthUnit` reports the metre factor of the *stored* (normalized)
geometry. Re-deriving an authored factor and rescaling rescale geometry would
need a separate Part-21 unit parse and, if inconsistent, would scale the
transferred geometry incorrectly. STEP-003 therefore reports the verified
factor that describes the geometry actually emitted (`0.001` for both
millimetre and inch-authored files), keeps `UpAxisId::Unknown`, and treats a
missing/contradictory/zero/non-finite authored unit as a typed failure. This
is physically identical for the viewer (metres rendered are correct) and
removes a factor/geometry mismatch risk.

## Fixtures

All fixtures are checked in under `tests/fixtures/stp-spike/`. They were
generated with the OCCT XDE writer by `GenerateStepFixtures.cpp` (the product
never links the writer); the generator gained a nested assembly, an
instance-color assembly, and a face-color part for this task. The committed
bytes are the frozen inputs; the generator is not byte-reproducible because
the writer embeds a creation timestamp.

| Fixture | Bytes | SHA-256 | Coverage |
| --- | ---: | --- | --- |
| `part_ap203.stp` | 21,168 | `EB53A75D416826582AE497FA2CA5E0AC1DAE14346D3D6A378AFE4C023A82C13A` | AP203 analytic solid with through-hole, shape color, millimetre |
| `part_ap214.stp` | 19,632 | `5A200F99284DBDD96471C4357B940CF7BC7BF69FD7985363B5D2EA3A498DE338` | AP214 analytic solid, shape color |
| `part_ap242.stp` | 19,664 | `FC8E09CC6FD3A583808D75864821F0518FA2AEE9C34025045EFAA9A031BB5697` | AP242 analytic solid, shape color |
| `assembly_ap214.stp` | 24,428 | `71FF7BC7CC514235E42F0619F9193CEBA54306FFF2EB2A2B7C8503F906B7E7EB` | Shared box definition reused twice, cylinder, translated occurrence, transparent color |
| `assembly_nested_ap214.stp` | 26,404 | `24EE7D3F47C73E49C1ADE45449F6CC9E8274B3980BD862AB9FC581CC73A590B5` | Nested sub-assembly reused twice over shared box/cylinder definitions |
| `instance_color_ap214.stp` | 19,794 | `7D7EC2B6DA83832FC67851AB6083C61718832DFDE8CE899B5B553A35D01A5079` | One definition placed three times; middle instance color overrides the definition color |
| `face_color_ap214.stp` | 17,274 | `E79CADD677A9C2F1FCEB7004B240937E91ABBE16BDAE643DAE26028A33FF152D` | Per-face subshape colors with no shape-level color (material seam split) |
| `inch_part_ap214.stp` | 19,809 | `24FD6BDC553FFCF5BE0DF036A7D963A7F81165D60749981BE1C4BC98F3FF4210` | `CONVERSION_BASED_UNIT('INCH')` authored length unit |

## Evidence

`x64/<Config>/Tests.ImportIsolation.exe "[step-003]"` passes 9 cases / 191
assertions in Debug and Release; `[step-002]` remains green at 13 cases / 113
assertions. Combined `"[step-002],[step-003]"` passes 22 cases / 304
assertions in Debug.

```text
[step-003][ap]          AP203/AP214/AP242: one root, one definition, one
                        instance, positive metre factor, unknown up axis
[step-003][assembly]    two reusable definitions, three occurrences, one
                        geometry chunk referenced twice, transparent material,
                        single root
[step-003][nested]      five instances over two shared definitions, recursive
                        sub-assembly translation, single acyclic root
[step-003][color][instance]  instance color override without geometry duplication
[step-003][color][face]      per-face seam split into two geometry groups
[step-003][units]       inch-authored file keeps a positive normalized factor
[step-003][negative]    Part-21 with no supported visual geometry is typed
[step-003][determinism] identical chunk ids/sizes/checksums across imports; no
                        source strings in any payload
[step-003][cancel]      pre-signalled cancellation observed
```

The full `Tests.ImportIsolation` suite retains the pre-existing USD/FBX
failures documented for STEP-002/STEP-007 (`UsdSpikeTests`, `UsdProtocolTests`,
`OpenUsdHostSpikeTests`, `FbxImportTests`); none are STEP-related, and the
STEP changes touch no shared broker, wire, or renderer code.

## Known limits and STEP-004 handoff

- The tessellation is a single coarse fixed policy; there is no display pass,
  coarse/display split, progressive catalog, or CAD-specific tessellation
  error code yet. STEP-004 owns those and the versioned quality profile.
- All records are emitted in one 64 MiB section. A model whose normalized
  scene exceeds the section or the chunk-count cap fails as `ResourceLimit`;
  progressive `ChunkBatchSink` delivery is STEP-004 work.
- Instance color discovery relies on OCCT's SHUO structure
  (`XCAFDoc_ColorTool::GetInstanceColor`); a producer that stores component
  color differently falls back to shape/face colors.
- External STEP documents remain a documented no-go per STEP-001/STEP-005; any
  `FILE_POPULATION`/`DOCUMENT_FILE` declaration is rejected during admission.
- The test-only `--step-001-spike` route and `StepXdeSpike.*` remain for the
  feasibility harness; they are not part of the production pool contract.
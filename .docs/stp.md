# STEP/STP post-MVP work plan

Status: STEP-003 complete — proceed to STEP-004 with the self-contained XDE scene adapter.
The execution order from STEP-004 onward is superseded by [stp2.md](stp2.md); the
external-document resolver described below is a recorded no-go (STEP-006 / ADR-017)
and STEP/STP ships as the self-contained static preview subset only.
Prepared: 2026-09-18
Design authority: [design/README.md](design/README.md)
Spike results: [STEP-001-SPIKE-RESULTS.md](STEP-001-SPIKE-RESULTS.md)
STEP-002 evidence: [STEP-002-VERIFICATION.md](STEP-002-VERIFICATION.md)
STEP-003 evidence: [STEP-003-VERIFICATION.md](STEP-003-VERIFICATION.md)

## Decision

Add `.step` and `.stp` as a **bounded static CAD-preview family**, implemented
through Open CASCADE Technology (OCCT) in a dedicated, zero-capability
AppContainer import host. The viewer will accept ISO 10303-21 clear-text STEP
files containing the common visual product subset: AP203/AP214/AP242 product
and assembly structures, B-rep and faceted/tessellated shape representations,
assembly occurrence transforms, units, names, layers, and representable
colors/transparency. Exact B-rep data is translated and tessellated inside the
host; only the existing normalized mesh/instance wire contract reaches the
trusted viewer.

This is the appropriate meaning of "full STEP/STP support" for this product:
all visual geometry and assembly content in the published support matrix is
shown together and faithfully enough for inspection, subject to the existing
Tier-B resource ceilings. It is not a claim to be a general-purpose PLM,
manufacturing, validation, editing, or exact-CAD application.

The user-facing name is **STEP**. `.stp` is an equally supported conventional
extension, not a reduced format or an alias with different behavior.

### Included static preview contract

| Content | Required behavior |
| --- | --- |
| ISO 10303-21 physical files | Accept valid clear-text Part 21 files identified by bytes, not extension alone. Support the AP203/AP214/AP242 geometry and product-data mappings demonstrated by the pinned OCCT build. |
| Geometry | Transfer supported solids, shells, faces, wires/curves when OCCT can create a visible shape, faceted B-rep, and supported AP242 tessellated representations. Tessellate surfaces under a deterministic screen-preview policy. |
| Assemblies | Preserve XDE assembly/occurrence structure, local transforms, reuse of repeated definitions, and visibility. Emit reusable geometry plus node/instance records; never flatten a large assembly merely to simplify rendering. |
| Units and coordinates | Respect the imported length unit, report a verified `metersPerUnit`, retain author-space coordinates/transforms in double precision, and set `UpAxisId::Unknown`: STEP does not define a universal display-up axis. The existing ground-axis control remains a viewer presentation choice. |
| Appearance | Preserve shape/face/instance color precedence and representable transparency through the existing material payload. Map unsupported material semantics to a deterministic neutral/PBR approximation with a bounded warning, not a false exact-material claim. |
| Product metadata | Retain only bounded facts useful across formats (counts, units, dimensions, fixed warning categories). Names, layer labels, GD&T text, document properties, saved views, and source paths do not cross IPC or enter the derived cache. |
| External STEP documents | Resolve required, relative local STEP references only through the broker after STEP-005 proves the selected OCCT API can do so without giving the host path authority. No URL, absolute, UNC, device, or non-STEP dependency is permitted. |

### Deliberate exclusions

- ISO 10303-28 XML, ZIP/compressed wrappers, P21e3/XML hybrids, IFC, IGES,
  JT, Parasolid, ACIS, native CAD files, and arbitrary files merely named
  `.step` or `.stp` are not STEP input in this slice.
- Editing, repair/save/export, PMI/GD&T rendering or measurement, saved-view
  UI, layer browser, BOM/property browser, CAD constraints/kinematics,
  manufacturing process plans, animation, and exact-kernel operations in the
  viewer are out of scope.
- Schema-specific business data that has no supported visible shape is not
  translated. An unknown AP profile is not itself a reason to reject a file
  when the pinned reader can safely transfer the required visual product
  representation; no geometry must be invented from metadata.
- AP242 PMI, validation properties, and advanced material data may be read by
  OCCT but are not displayed as authoritative engineering information. A
  document whose only required visible content depends on an unsupported
  feature fails explicitly rather than appearing empty or complete by
  accident.
- External documents are not silently omitted. Until the brokered-resolver
  task is complete, a required external reference returns
  `UnsupportedRequiredFeature`; after it is complete, an unsafe, missing,
  cyclic, or over-budget reference returns the applicable typed error.

The README and release text must call this the **supported static STEP preview
subset**, not unqualified CAD/STEP authoring support.

## Why a dedicated CAD host

OCCT is the right technical direction: its STEP/XDE reader retains assemblies,
names, colors, layers and validation data, while `BRepMesh_IncrementalMesh`
turns a transferred B-rep into renderable triangulation. It is also materially
larger and more stateful than the ordinary mesh importers. Its transfer and
tessellation phases can require substantial intermediate topology and do not
provide the fine-grained cancellation guarantees of the product-owned binary
parsers.

Accordingly, this work adds `Preview3DStepHost.exe` rather than linking OCCT
into `Preview3D.exe`, `Preview3DImportWorker.exe`, or a graphics library. It
uses the existing broker protocol, suspended AppContainer launch, zero-
capability token, Job Object, explicit handle list, generation cancellation,
bounded sections, and copy-then-validate host acceptance rule. The separate
host is lazy and per-generation (or exits after a short, measured idle grace)
so OCCT global state and peak B-rep memory are discarded deterministically.

```text
Preview3D.exe (trusted broker)
  | duplicated read-only primary/dependency handles; bounded control pipe
  v
Preview3DStepHost.exe (zero-capability AppContainer + Job Object)
  Part-21 admission -> OCCT STEPCAF/XDE translation -> bounded tessellation
  -> normalized nodes/materials/reusable geometry/instances in shared sections
  v
broker copies descriptor and bytes to private memory, validates, then uploads
```

This is a fifth runtime component in the original design topology. The
architecture, packaging, ACL, threat-model, test, and SBOM documents must be
updated together when the host becomes real.

## Non-negotiable implementation rules

1. The trusted viewer never links OCCT, a STEP parser, B-rep kernel, or CAD
   tessellator. It receives only versioned normalized wire records and makes
   no rendering decision from OCCT objects or source bytes.
2. The host receives an already-opened read-only handle, never the selected
   filename. The chosen OCCT route must consume a product-owned seekable
   stream/callback over that handle. A temporary path, broad model-directory
   ACL, current-directory search, environment-selected resource/plugin, or
   `ReadFile(path)` workaround is not acceptable.
3. OCCT's XDE reader is required, not the shape-only reader: visual assembly
   occurrence, color, and unit fidelity is a core reason to add this format.
   The exact reader/API combination is selected and pinned by STEP-001.
4. The host may request only the broker-approved relative STEP dependencies
   allowed by STEP-005. It never opens a path, socket, URL, registry key, or
   child process itself. It cannot load model-selected plug-ins or codecs.
5. Product-owned preflight owns input type, byte, lexical-record, entity,
   external-reference, depth, and elapsed-work limits before OCCT performs
   semantic transfer. OCCT success is not acceptance: every emitted shape,
   transform, color, count, tessellation result, and normalized byte range is
   independently revalidated.
6. STEP is Tier B. It receives the current Tier-B primary-source (2 GiB),
   aggregate approved-source (4 GiB), normalized geometry (20 million
   triangles), vertex (60 million), object/node (50,000), material (32,768),
   and scratch/Job controls. STEP-001 establishes lower STEP-specific limits
   if OCCT's measured peak requires them; no multi-gigabyte Tier-A latency
   promise applies.
7. A per-definition or whole-generation timeout/cancellation miss terminates
   only the STEP host's Job Object after the documented grace interval. The
   UI, renderer, existing document, and subsequent import remain usable.
8. Every OCCT diagnostic is captured locally, classified into the closed
   product taxonomy, redacted, and bounded. Raw kernel messages, entity names,
   paths, or source strings never cross the control channel or become default
   UI/clipboard text.
9. No `.step`/`.stp` picker filter, drag/drop/activation route, Open With
   registration, installer association, README claim, or thumbnail CLSID is
   enabled before STEP-003 through STEP-005 pass their recovery/security
   evidence.

## Existing foundation and format-specific gaps

The repository already supplies the important reusable foundation: a
zero-capability AppContainer launcher, Job Object containment, brokered source
handles, generation cancellation, bounded shared sections, hostile-worker
tests, host-side copy-then-validate, protocol-v10 nodes/reusable geometry/mesh
instances, double-precision transforms and bounds, progressive upload, and
existing installer/portable staging surfaces.

The gaps are specific and substantial:

- there is no OCCT dependency/port, CAD host executable, AppContainer payload,
  or package/SBOM/license decision;
- `SourceFormatId` currently ends at `ThreeMf`; `ImportFormat`, control
  opcodes, request structures, worker dispatch, validators, cache-versioning,
  and source-format UI text contain no STEP route;
- the current importer model assumes direct mesh normalization. STEP requires
  a product-owned Part-21 admission boundary, OCCT XDE traversal, B-rep
  lifetime management, deterministic tessellation, and per-face extraction;
- `Preview3DImportWorker.exe` is not an acceptable place to accumulate the
  OCCT binary/dependency and global-state surface;
- there is no STEP corpus, fuzz target, quality oracle, tessellation
  determinism baseline, external-document policy, thumbnail adapter, or
  clean-machine evidence;
- the current supported-extension lists correctly omit `.step` and `.stp` in
  the app, shell-integration catalog, NSIS script, package scripts, tests, and
  public documentation.

## Input, output, and failure policy

### Part-21 admission

`StepPart21Preflight` is deliberately a small, bounded lexical admission
scanner, not a second STEP implementation. It reads only through the inherited
handle/mapping and must:

1. verify the ISO 10303-21 physical-file signature and bounded HEADER/DATA/
   ENDSEC/END-ISO-10303-21 structure before dispatching OCCT;
2. distinguish malformed physical syntax from a valid but unsupported
   representation; inspect `FILE_SCHEMA` only for classification/diagnostics,
   never as an extension-based trust decision;
3. count entity records, references, DATA sections, nesting, record/string
   lengths, and total lexed bytes with checked arithmetic before any source
   field sizes a product allocation;
4. reject binary/XML/compressed inputs, missing terminators, duplicate or
   impossible entity identifiers, unterminated strings/comments, control-byte
   encodings outside the selected physical-file policy, and source claims over
   the product limits;
5. discover external-document declarations sufficiently to apply the
   STEP-005 broker policy before OCCT sees them. It does not resolve a
   filesystem path itself.

The spike fixes the numeric lexical ceilings from measurements and adversarial
tests. Initial candidates are 5 million entity instances, 256 reference/depth
levels, and 1 MiB for one lexical record or decoded string; they are admission
limits, not a promise that an OCCT translation of every limit-sized file fits.

### XDE traversal and normalization

The adapter initializes an XDE document and uses the selected STEPCAF reader
to transfer accepted content. It then performs a product-owned bounded walk:

- enumerate free shapes, assembly definitions, components, and occurrences;
  validate XDE labels/locations and detect cycles before emitting nodes;
- create one stable `NodePayload` per accepted occurrence and one reusable
  geometry record per reusable definition where locations and face appearance
  allow it; use deterministic IDs derived from bounded traversal order, never
  exposed source labels;
- compose affine transforms in double precision, reject non-finite or
  singular transforms, enforce the protocol's 256 hierarchy depth and 50,000
  node/object caps, and derive instance bounds from the extracted geometry;
- preserve the visible assembly as authored. Do not add explode, hide, or
  selection semantics in this format task; document-level Fit and existing
  format-neutral picking behavior continue to apply;
- obtain source length units through the reader/XDE document, validate a
  positive finite conversion to metres, and use `UpAxisId::Unknown`. Mixed or
  unresolvable length-unit semantics are a typed unsupported/malformed result,
  not an arbitrary millimetre assumption;
- apply documented XDE color precedence (instance/shape/subshape/face) while
  extracting faces. Split only where material/color seams require it and cap
  material and normalized-vertex growth. Layers are retained only insofar as
  they affect color/visibility under the fixed policy.

Names, arbitrary attributes, validation-property text, PMI, and raw OCCT
handles are discarded before normalized output is produced.

### Tessellation and progressive delivery

There is no safe source-range re-decode shortcut for a B-rep face. The STEP
host retains the accepted XDE/B-rep only while it creates bounded output, then
destroys it. It must:

1. derive a deterministic coarse and display tessellation policy from verified
   definition bounds, unit scale, and a fixed release-quality profile. STEP-001
   chooses the relative/absolute linear-deflection floor, angular deflection,
   minimum edge size, parallelism policy, and quality corpus; values may not
   silently vary by model file or host hardware;
2. tessellate one bounded reusable definition at a time with the pinned
   `BRepMesh_IncrementalMesh`/approved OCCT API. Enforce face, edge, output
   triangle, normalized-byte, elapsed-work, and peak-commit budgets before
   publishing each definition;
3. extract only finite vertices and valid triangles from face triangulations,
   respect face orientation/location, produce outward/credible normals or the
   existing generated-normal fallback, eliminate degenerates, and preserve
   material boundaries and shared definition identity;
4. split output to the established 4-16 MiB / 262,144-triangle chunks with
   cluster-local float positions and double origins. Emit nodes/materials
   before dependent reusable geometry and instances; comply with the existing
   bounded progressive generation catalog;
5. first publish a complete, representative coarse catalog across every
   nonempty accepted component, then replace it with fence-complete display
   chunks. A CAD document may need its complete Part-21/XDE transfer before
   first geometry; that Tier-B limitation is measured and documented rather
   than disguised as streaming;
6. cancel between lexical blocks, XDE traversal units, definitions, face
   extraction blocks, chunk builds, and shared-section handoffs. When OCCT is
   inside an uninterruptible transfer or meshing call, the host Job Object is
   the hard backstop.

If an accepted visible definition cannot be tessellated within its explicit
budget, fail the document with a new typed `TessellationFailed` or
`CadKernelLimit` result (chosen in STEP-001); do not silently publish a
partial assembly as Ready. Recoverable face-level healing is permitted only
when its exact operations, post-heal validation, visual effect, and warning
policy are pinned and tested. The first release must not enable broad automatic
shape healing simply to increase apparent compatibility.

### External documents

STEP-005 is required for the full supported assembly contract, but only after
the dependency spike proves a stream/callback API that can retain XDE external
references without worker path access. The broker resolves only a normalized,
relative `.step` or `.stp` reference below the root's directory, opens and
canonicalizes it by handle, pins its identity, and duplicates that handle to
the host. It rejects remote/absolute/device/UNC/traversal targets, different
extensions, duplicate canonical references, cycles, more than 64 dependencies,
more than 256 aggregate document depth, or more than 4 GiB aggregate source
bytes. Each dependency runs the same Part-21 admission and has a bounded
independent source identity in the cache key.

If the selected OCCT release cannot provide this without a source path or
unbounded path callback, the spike is a no-go for external-reference support;
the implementation must not weaken the AppContainer boundary. The resulting
product can ship only the explicitly documented self-contained STEP subset
after the product owner accepts that scope change.

## Execution order

| Order | Task | Outcome | Depends on |
| ---: | --- | --- | --- |
| 1 | STEP-001 | Prove/pin OCCT, stream-only XDE input, resource/search lockdown, memory, cancellation, and tessellation fidelity | Current main branch |
| 2 | STEP-002 | Add the dedicated STEP host, Part-21 admission, closed protocol route, and containment tests | STEP-001 go decision |
| 3 | STEP-003 | Translate self-contained XDE assemblies, units, colors, and reusable scene instances | STEP-002 |
| 4 | STEP-004 | Produce bounded deterministic B-rep tessellation, coarse/display delivery, and CAD-specific errors | STEP-003 |
| 5 | STEP-005 | Add brokered local external STEP documents and AP/profile interoperability policy | STEP-002 through STEP-004; stream-resolver proof |
| 6 | STEP-006 | Enable viewer, activation, Open With, installer/portable payload, and documentation surfaces | STEP-003 through STEP-005 |
| 7 | STEP-007 | Complete corpus, fuzz, security, performance, clean-machine, and signed-release qualification | STEP-006 |
| 8 | STEP-008 | Add the original-MVP Explorer thumbnail adapter | STEP-007 and general thumbnail-provider foundation |

Focused tests and required documentation land with the implementing task.
Qualification is evidence for completed work, not a substitute for omitted
limits or tests.

## STEP-001 — OCCT, containment, and tessellation feasibility spike

### Objective

Remove the architectural unknowns before a large CAD kernel, a public file
claim, or a source-path exception reaches production.

### Work

1. Evaluate a current certified OCCT release, selecting an exact source
   archive/revision and source hash only after the spike passes. The candidate
   must include the STEP/XDE reader and the stream-capable data-exchange API;
   do not pin a release merely because its path-based `ReadFile` sample works.
   Add a constrained custom vcpkg port if the registry port cannot produce the
   required static/app-local dependency closure reproducibly.
2. Build the smallest required OCCT module set for the dedicated host (STEP,
   XDE, B-rep, meshing, and their actual transitive modules). Disable Draw,
   visualization, writers, non-STEP exchange formats, sample tools, optional
   plug-in discovery, Tcl, and any unused resource/runtime feature. Record
   every resulting EXE/DLL/resource and prove `Preview3D.exe` and the general
   worker load none of them.
3. Demonstrate end-to-end XDE import from the duplicated inherited handle by a
   seekable product-owned stream/callback. Prove AP203, AP214, and AP242
   fixtures retain assembly occurrences, definition reuse, colors, and units.
   Attempt path leakage, current-directory lookup, `CSF_*`/PATH injection,
   model-selected plug-ins, network access, child-process creation, and direct
   filesystem opens from the host; all must fail under the real AppContainer.
4. Prove or disprove an XDE-compatible, brokered external-reference resolver
   using only approved duplicated handles. This is a distinct result from
   basic stream import. A path-only API is a no-go, not a reason to give the
   host a model directory or temporary pathname.
5. Measure peak private commit, output triangles/bytes, elapsed time, and
   cancellation latency for: a simple analytic part; a trimmed/NURBS/fillet
   part; a face-color part; a deeply nested/reused assembly; a faceted AP242
   part; and a large industrial-style assembly. Measure both coarse and
   display tessellation, malformed transfer, and deliberately pathological
   trim/surface cases. Establish the CAD host's final Job commit ceiling,
   per-definition deadline, lexical/entity/face/edge caps, and mesh-quality
   parameters from this data.
6. Compare no healing, a narrowly pinned validation/healing sequence, and
   broad automatic healing. Adopt only operations that have deterministic
   visual benefit and bounded time/memory; otherwise fail invalid geometry
   rather than mutating it. Record all adopted OCCT configuration defaults so
   no process-global setting can drift across imports.
7. Verify one host/reader per process and one import at a time until the exact
   pin's thread-safety contract supports more. Reuse is optional; host exit
   after a generation is the default safety/reproducibility baseline.
8. Map OCCT status and captured diagnostics into `MalformedData`,
   `UnsupportedVersion`, `UnsupportedRequiredFeature`, `NoSupportedGeometry`,
   `ResourceLimit`, `Cancelled`, `ImportWorkerLimit`, `ImportWorkerFailure`,
   and the proposed CAD/tessellation code. No OCCT exception/message reaches a
   product boundary.
9. Write `.docs/STEP-001-SPIKE-RESULTS.md` with fixture provenance and hashes,
   exact toolchain/pin/license decision, module inventory, stream/resolver
   results, resource tables, cancellation observations, quality comparisons,
   rejected options, and a go/no-go decision for STEP-002.

### Exit criteria

- The real zero-capability host imports a self-contained assembly through the
  inherited handle without a usable source path or broad filesystem access.
- Assembly/reuse/unit/color evidence survives XDE traversal into the existing
  normalized scene concepts, and deterministic tessellation is within proposed
  output/quality budgets.
- A cancelled, timed-out, malformed, or Job-limited host is terminated and a
  later valid import succeeds without viewer restart.
- The selected dependency closure builds Debug/Release under repository
  hardening rules and has an approved LGPL-with-exception/commercial-license,
  notices, SBOM, vulnerability-watch, and servicing plan.
- Failure changes the scope/design. It does not relax handle-only input,
  AppContainer, Job Object, copy-before-trust, or path/network restrictions.

## STEP-002 — host, Part-21 admission, and closed protocol route

Status: complete (2026-09-19; private host route only, no extension enabled)

### Objective

Create the production containment and dispatch boundary without enabling the
extension or claiming a CAD scene adapter.

### Work

1. Add `compatibility-host-step/Preview3DStepHost.vcxproj` (or the agreed
   equivalent) and shared launcher configuration. Launch it suspended with a
   zero-capability AppContainer token, kill-on-close Job Object, measured
   commit/CPU/process limits, and only the control, cancellation, input,
   approved dependency, and output-section handles in
   `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` before it executes import code.
2. Add `SourceFormatId::Step`, `ImportFormat::Step`,
   `ControlOpcode::StartStepImportFromFile`, and fixed
   `ParseStepFileRequest`. Append closed enum/opcode values without changing
   protocol-v10 layouts where possible; otherwise bump the protocol and update
   every static assertion, fixture, validator, hostile-worker case, and both
   producer/consumer binaries together. A STEP generation may emit only
   `SourceFormatId::Step`.
3. Wire one-shot dispatch, expected-format validation, source-format text,
   cancellation, replacement/recovery, cache importer-version identity, host
   crash/timeout result mapping, and source/dependency identity plumbing in
   `ImportSession`, `SharedSectionValidator`, and the D3D12 import bridge.
4. Implement `StepPart21Preflight` and a test-only host route that performs
   admission then returns a bounded synthetic scene. The production OCCT reader
   cannot be reached before admission succeeds. Do not reuse `UsdZipPreflight`
   or an XML parser: STEP Part 21 has different physical syntax and threat
   boundaries.
5. Lock down OCCT's dynamic-module/resource/message behavior with absolute
   installed paths and a fixed product configuration. Clear or ignore ambient
   environment/configuration inputs, install only allowlisted signed payloads,
   and verify their hashes in release packaging. The host must have no current
   directory or model-selected DLL/resource search path.
6. Extend the actual AppContainer/Job/handle-denial suite and synthetic
   hostile-worker suite to `Preview3DStepHost.exe`: stale generations,
   malformed opcode/size, wrong source format, replayed batch, modified shared
   bytes after inspection, invalid layouts/ranges/checksums, limit termination,
   and host replacement all remain rejected/recoverable in Debug and Release.

### Exit criteria

- Valid Part-21 fixture bytes reach only the dedicated host; no public UI path
  recognizes STEP yet.
- The host has no path, network, child-process, plug-in, or unrestricted
  filesystem authority, and no OCCT module is loaded into the viewer/general
  worker.
- Malformed/oversized physical files fail before OCCT semantic transfer;
  protocol and hostile-worker defenses work identically to existing import
  processes.

## STEP-003 — self-contained XDE assembly scene adapter

Status: complete (2026-09-19; private host route only, no extension enabled)

### Objective

Map accepted self-contained STEP product structure into the existing
node/reusable-geometry/mesh-instance/material contract before fine CAD
tessellation and external-reference support are exposed.

### Work

1. Add `StepXdeAdapter` behind the STEP-002 route. Configure the pinned XDE
   reader for names, colors, layers, assemblies, and verified unit transfer;
   disable writing, external-path fallback, presentation/UI subsystems, and
   all product features not consumed by the static scene contract.
2. Enumerate only the reachable free-shape/assembly roots. Preserve all visible
   root occurrences together, nested assembly transforms, and repeated part
   definition reuse. Empty documents or roots with no reachable supported
   geometry return `NoSupportedGeometry`; orphan definitions are not drawn.
3. Validate each XDE label/shape/location before use: finite coordinates and
   transforms, no cycle, bounded depth/occurrences/definitions/subshapes, and
   exact double-precision world bounds. Generate deterministic opaque IDs and
   use `NodePayload` plus `MeshInstancePayload`; do not encode source names or
   flatten components into duplicated meshes.
4. Convert verified length units to metres and produce `UpAxisId::Unknown`.
   Add cross-format metadata only where useful rather than inventing a
   CAD-specific UI property. Define a fixed unit-error result for absent,
   contradictory, zero, or non-finite conversions.
5. Implement a color/material resolver with explicit instance/shape/subshape/
   face precedence, sRGB and opacity conversion, neutral fallback, seam
   splitting, material cap, and bounded optional-feature warnings. Treat
   layer/name/GD&T data as non-rendered metadata unless it changes the
   documented color/visibility result.
6. Validate required visual product representations. Transferable analytic,
   trimmed, solid/shell, faceted, and supported tessellated representation
   types feed STEP-004; unsupported required visual representation fails. A
   valid assembly may not be declared Ready until its complete accepted root
   graph and verified bounds have been established.

### Verification

- Immutable fixtures cover AP203/AP214/AP242 self-contained parts, nested and
  repeated assemblies, mirrored/location transforms, all accepted length
  units, face/part/instance color precedence, transparent color, faceted and
  analytic geometry, and source-order permutations.
- Normalized snapshots assert root occurrence count, hierarchy, definition
  sharing, matrices, metre conversion, unknown up axis, colors/material IDs,
  bounds, and no source strings.
- Negative fixtures cover empty/no-shape data, malformed/unsupported transfer,
  cycles, invalid locations, unit ambiguity, excessive labels/subshapes,
  material/vertex explosion, unrepresentable visible data, cancellation, host
  crash, and next-valid-open recovery.

## STEP-004 — bounded tessellation and progressive CAD delivery

### Objective

Turn accepted B-rep/tessellated definitions into deterministic bounded
rendering chunks without compromising the current renderer's proxy, residency,
or replacement guarantees.

### Work

1. Implement the STEP-001 tessellation-quality profile as versioned product
   constants: coarse/display deflection, angle, minimum size, parallelism,
   shape-scale clamps, face/edge limits, output triangle limits, and measured
   deadlines. Include it in importer/cache version evidence.
2. Tessellate and extract one reusable definition at a time. Handle local face
   location and orientation correctly, extract valid triangulations in bounded
   face blocks, use cluster-local positions/double origins, and produce
   normals/material seams with the same payload/validator rules used by mesh
   formats. Never retain all extracted CAD geometry solely to deduplicate it.
3. Build a complete coarse catalog across every nonempty component, bounded by
   the existing coarse limits and a CAD-specific quality floor validated by
   golden images. Publish it through normal `ChunkBatchSink`/validator/upload
   paths, then replace it with display detail only after copy-fence completion.
   Preserve the old document until a complete usable STEP coarse catalog is
   available.
4. Define the conversion for source tessellated shape representations: validate
   their topology/counts and use them only when they meet the pinned fidelity
   policy; otherwise tessellate the underlying B-rep when available. Do not
   treat unvalidated faceted data as trusted GPU input.
5. Enforce every applicable Tier-B and STEP-specific budget during traversal,
   meshing, extraction, normal generation, section creation, and host output.
   Backpressure shared sections and host-owned CPU data; a single complex face
   must not grow an unbounded vector or make the UI wait.
6. Emit new typed CAD errors/status flags and fixed host-owned user text for
   accepted-with-approximation, B-rep meshing failure, unit issue, and resource
   limit. Keep generic parser/kernel diagnostics out of the UI.

### Verification

- Golden readback tests cover cylinders/cones/spheres, trimmed NURBS,
  fillets/chamfers, holes/voids, seam/orientation cases, tiny/large unit scales,
  face colors, faceted AP242 geometry, repeated definitions, and large offsets.
- Compare repeated imports and Debug/Release builds against declared triangle,
  bounds, normal-direction, material-boundary, and image-tolerance baselines.
- Adversarial fixtures cover degenerate/invalid triangulation, extreme
  tolerances, pathological trimmed surfaces, high face/edge/triangle claims,
  NaN/Inf after extraction, cancellation in each boundary, timeout, Job-limit
  termination, delayed section handoff, and device-pressure/old-document
  survival.
- Record Tier-B first-coarse, Ready, peak-commit, host heartbeat, output-byte,
  cancellation, UI-frame, and input-latency measurements. Publish results by
  fixture class; do not present Tier-A large-asset targets as CAD guarantees.

## STEP-005 — brokered external documents and interoperability closure

### Objective

Complete the accepted multi-document assembly path without granting the CAD
host a filesystem resolver or claiming unsupported application protocols.

### Work

1. Implement the STEP-001-proven external-reference hook as a product-owned
   broker protocol. The host requests a relative reference token; the broker
   canonicalizes, admits, opens, pins, and duplicates only an approved local
   `.step`/`.stp` dependency. No raw path string, external URI, arbitrary
   extension, or cached path crosses to the host.
2. Walk dependencies breadth-first with checked count/depth/aggregate-byte
   caps and an identity-based cycle/duplicate map. Capture every dependency in
   the generation/cache identity before accepting a scene. Missing required
   content fails; optional nonvisual external data produces only the fixed
   warning policy approved by the fixture corpus.
3. Prove XDE retains cross-document definition reuse and occurrence transforms
   under this resolver. Normalize the resolved assembly exactly as a
   self-contained one; only the source provenance/cache key records the fact
   that more than one handle was used.
4. Establish the interoperability matrix from immutable fixtures: AP203,
   AP214, AP242 B-rep and tessellated representations; names/colors/layers;
   assemblies; external documents; and explicitly unsupported or
   geometry-free/AP-specific data. Require a named expected result for each
   matrix row.
5. Reject external URLs, absolute/UNC/device/traversal/case-colliding targets,
   reference loops, target changes after opening, unrelated non-STEP files,
   references above caps, and any OCCT fallback that tries to resolve a path
   outside the broker. Fuzz both declaration discovery and the host/broker
   request sequence.

### Exit criteria

- A legal local multi-document assembly retains visible placement and shared
  definitions without a direct host file open.
- Every unsafe/missing/cyclic/over-budget dependency has a typed recoverable
  outcome, leaves no unclosed handle or host, and cannot access any unrelated
  file or network resource.
- The checked-in matrix makes the supported AP/profile visual contract and all
  intentional exclusions testable.

## STEP-006 — product, package, and documentation integration

### Objective

Expose only the completed subset consistently across viewer, activation, and
distribution surfaces.

### Work

1. Add `.step` and `.stp` case-insensitively to command-line validation,
   file-open filters, drag/drop, secondary activation, retry, supported-format
   errors, and the title-bar Open With catalog. The dispatch sniff verifies
   ISO 10303-21 bytes before choosing `ImportFormat::Step`; an extension alone
   never bypasses STEP-002 admission.
2. Route the D3D12 bridge through the STEP host and present actual source
   format, phase, verified dimensions, units, geometry/node/instance/material
   counts, and fixed approximation warnings. Use the existing document-level
   Fit, shading, ground-axis, selection, and loading-state UX; do not add a
   CAD tree, layer browser, or edit command.
3. Stage the exact signed OCCT host payload, its real dependency closure, and
   resource configuration beside a dedicated `StepHost` directory. Extend
   portable and NSIS staging, manifest hashes, AppContainer ACL provisioning,
   uninstall, tamper tests, dependency notices, SBOM, and release signing.
   The viewer and general worker payloads remain OCCT-free.
4. Add `Binbuf.Preview3D.STEP.1` and both `.step`/`.stp` registration entries
   to the actual NSIS package without altering the user's default. Extend
   install/repair/upgrade/uninstall/conflict tests and the Default Apps
   capability map. Do not add thumbnail registration in this task.
5. Update README, portable/installer notes, About/help strings, supported
   extension tables, `.docs/TODO.md`, `.docs/PROGRESS.md`, design support
   matrix/architecture/package/threat documents, and public limitations. State
   the static preview subset, Tier-B limits, external-reference policy, and
   lack of PMI/editing/Explorer thumbnails.

### Verification

- Valid self-contained and approved multi-document STEP files open through
  every viewer activation route, remain responsive during import, and recover
  after malformed, over-limit, cancellation, host-crash, timeout, and
  replacement cases.
- `Preview3D.exe` and `Preview3DImportWorker.exe` do not load an OCCT DLL or
  link an OCCT import library. Packaging contains exactly the approved StepHost
  closure, license notices, hashes, ACLs, and uninstall behavior.
- Open With/Default Apps, portable launch, NSIS install/repair/upgrade/
  uninstall, clean standard-user startup, and handler-conflict tests cover
  both extensions without changing an existing user default.

## STEP-007 — qualification and hardening

### Objective

Produce the evidence required to call the STEP viewer slice complete and keep
its CAD-kernel attack surface serviceable.

### Work

1. Freeze a provenance-and-SHA-256 corpus manifest of official/open licensed
   OCCT STEP test data where redistribution permits, AP203/AP214/AP242
   conformance/interoperability examples, product-authored focused fixtures,
   and anonymized representative assemblies. Commit expected normalized facts,
   render tolerances, source/dependency maps, and supported/unsupported result
   classifications; never regenerate golden output implicitly.
2. Add standalone no-GPU sanitizer/libFuzzer targets for Part-21 admission,
   bounded external-reference discovery, XDE/OCCT stream-adapter boundary,
   B-rep triangulation extraction, STEP host control messages, and normalized
   CAD chunks. Include seeds for comments/strings, entity IDs/references,
   complex instances, malformed headers/data sections, trims/NURBS,
   assemblies/colors/units, and multi-document cycles.
3. Run the shared normalized-scene invariants, protocol/property suite,
   synthetic hostile-StepHost suite, AppContainer path/network/process denial,
   Job/timeout/worker replacement, cancellation/open storm, delayed-batch,
   GPU validation, device-pressure, cache-version, and source-change tests
   with STEP enabled.
4. Measure per fixture class: Part-21 admission time, XDE transfer time,
   coarse/display tessellation time, first coarse/Ready, peak host commit,
   output bytes/triangles, cancellation/termination latency, host heartbeat,
   UI/frame/input latency, and normalized-render comparison. Revisit numeric
   limits only through a documented design update, never through a silent
   release build setting.
5. Complete static analysis, dependency vulnerability/license review, Debug/
   Release reproducibility, 8-hour mixed-format soak, clean offline
   standard-user VM, portable/installer lifecycle, signed artifact/hash/SBOM
   inspection, and an OCCT update/rollback servicing rehearsal.

### Exit criteria

- Every promised visual capability has immutable normalized and rendered
  evidence; every unsupported/over-limit/malformed family has an asserted
  typed outcome.
- No corpus item crashes or hangs the viewer, bypasses the sandbox or
  copy-then-validate rule, creates an unbounded allocation, exposes a path or
  diagnostic, stalls UI/render, or prevents a later valid open.
- Performance reports meet Tier-B responsiveness and documented limits on the
  defined references, with raw traces and fixture hashes linked to the signed
  release candidate.

## STEP-008 — Explorer thumbnail adapter

Status: blocked on STEP-007 and the general Gate 6 thumbnail-provider
foundation. This completes the original-MVP STEP family; it does not block the
viewer slice.

1. Link a separately built, explicitly limited OCCT STEP/XDE/tessellation
   adapter only into the isolated thumbnail DLL/surrogate, never the viewer or
   either import host. It must consume only `IInitializeWithStream` through a
   bounded seekable stream and must prove no arbitrary path, sidecar, network,
   cache, process launch, D3D device, or persistent write occurs.
2. Support self-contained Part-21 visual geometry only under stricter provider
   ceilings: 256 MiB stream, 192 MiB parser/tessellation scratch, 384 MiB
   private commit above baseline, a much smaller inspected/rasterized triangle
   cap, and the general 2-second cutoff/750 ms p95 target. Required external
   documents, unsupported content, or budget pressure safely return failure so
   Explorer uses its normal icon.
3. Use a fixed low-detail deterministic tessellation/sample policy and the
   CPU rasterizer. Preserve bounded shape colors and complete-assembly spatial
   representation where possible; do not represent a source-prefix or launch
   `Preview3DStepHost.exe` to obtain a thumbnail.
4. Add one dedicated STEP thumbnail CLSID/family mapping and `.step`/`.stp`
   registration only after its isolated-surrogate tests pass. Add COM stream,
   malformed/parallel/unload, golden raster, deadline, actual-surrogate,
   `DisableProcessIsolation` absence, installer conflict, and lifecycle tests.

## Completion boundaries

- **STEP/STP viewer support:** STEP-001 through STEP-007 are complete, with
  external local-document support included only if STEP-005 passes the
  handle-only resolver proof.
- **Original-MVP STEP/STP family complete:** STEP-008 is additionally complete
  after the shared thumbnail-provider foundation exists.
- **Future CAD breadth:** IGES/IFC/JT/native CAD formats, a CAD tree/property
  browser, PMI/GD&T display, saved views, exact measurement, repair/editing,
  export, and arbitrary external references require separately approved plans.

## Primary references

- [OCCT STEP user guide](https://github.com/Open-Cascade-SAS/OCCT/blob/master/dox/user_guides/step/step.md)
- [OCCT XDE guide](https://github.com/Open-Cascade-SAS/OCCT/wiki/xde)
- [OCCT STEP/XDE overview](https://github.com/Open-Cascade-SAS/OCCT/wiki/step)
- [OCCT meshing guide](https://github.com/Open-Cascade-SAS/OCCT/wiki/mesh)
- [OCCT source, releases, and license](https://github.com/Open-Cascade-SAS/OCCT)
- [ISO 10303 STEP catalogue](https://www.iso.org/committee/54166.html)

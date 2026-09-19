# STEP/STP post-MVP work plan — revision 2 (render-time focused)

Status: STEP-003 complete, STEP-004 complete for the single-pass display slice
(coarse-catalog replacement delegated to STEP-005 item 5), STEP-005 implemented
for measurement, progress, input I/O, the corrected thread-pool decision, and
the versioned single-pass delivery-strategy choice; the genuine large-file
corpus and published Tier-B budgets remain for STEP-008 (see
[STEP-005-VERIFICATION.md](STEP-005-VERIFICATION.md)). This file supersedes
the remaining execution order in [stp.md](stp.md) from STEP-004 onward.
Prepared: 2026-09-19
Design authority: [design/README.md](design/README.md)
Spike results: [STEP-001-SPIKE-RESULTS.md](STEP-001-SPIKE-RESULTS.md)
STEP-002 evidence: [STEP-002-VERIFICATION.md](STEP-002-VERIFICATION.md)
STEP-003 evidence: [STEP-003-VERIFICATION.md](STEP-003-VERIFICATION.md)

The decision, supported subset, deliberate exclusions, non-negotiable
implementation rules, Part-21 admission contract, and containment architecture
in [stp.md](stp.md) remain authoritative and unchanged. Only the execution
order and the large-file performance work change.

## Why this revision

A 250 MB STEP file was observed to take a long time in Bambu Studio, which
shares our dependency (OCCT). The independent advice correctly identifies
OCCT's serialized Part-21 parse, pointer-graph resolution, and
tessellation/contiguity costs, and recommends coarse/display LOD, parallel
tessellation, memory-mapped input, and `fast_float`. The plan already covered
part of this, but three project-specific facts were missing:

1. **Time to first geometry is parse-bound, not tessellation-bound.** Our host
   cannot emit any normalized chunk until `ReadStream` parses the whole file
   and `Transfer` builds the XDE document. Coarse/display LOD (STEP-004) only
   optimizes the *meshing* phase; it does not make a 250 MB file appear sooner.
   No task currently measures or improves the parse/transfer phase, and
   performance work is otherwise deferred to qualification (old STEP-007).
2. **The "parallel tessellation" knob may be inert.** The pinned constrained
   port sets `-DUSE_TBB=OFF`, while `StepTessellationProfile.h` declares
   `parallel = true` and a comment implying parallel OCCT meshing. If the
   pinned build has no parallel `OSD_Parallel` backend, meshing runs serially
   and the profile comment is false. Either product-level per-definition
   parallelism or an explicit serial decision is required.
3. **The remaining plan is stale.** STEP-005 still plans brokered external STEP
   documents, but STEP-001/STEP-002 already concluded external references are a
   no-go (path-based, protected resolver; no stream hook). Continuing to budget
   STEP-005 for it wastes the largest remaining task.

The advice's other options (commercial kernels, Truck, GPU NURBS ray tracing,
skipping tessellation) are rejected for this slice: they would replace or
bypass the pinned OCCT kernel and violate non-negotiable rule 1 (the trusted
viewer never links a CAD kernel) and the handle-only normalized-wire
containment model. They are recorded as future breadth, not tasks.

### Advice triage

| Advice | Verdict for this project |
| --- | --- |
| Decouple ingestion from the UI thread | Already inherent: dedicated AppContainer host plus progressive `ChunkBatchSink`/upload. Keep. |
| Multi-tier coarse/display LOD | Partly in STEP-004, but cannot beat parse/transfer. STEP-005 measures whether two-pass beats single-pass per-definition streaming and picks one. |
| Parallelize tessellation | Directly relevant. Pinned port disables TBB; STEP-005 proves or implements product-level per-definition parallel meshing, or documents serial throughput. |
| Memory-mapped file + `fast_float` | Applicable inside the handle-only rule (map the inherited handle; no path). STEP-005 evaluates mapped streambuf and `fast_float` in `StepPart21Preflight`. Also removes the current preflight + OCCT double read of the whole file. |
| Avoid slicer manifold/repair passes | Already true: we are a previewer and do not run slicer-grade topology repair. This is why our pipeline should already beat Bambu on the same file; STEP-005 quantifies it. |
| Commercial kernels (ACIS/Parasolid/HOOPS) | Rejected: licensing, SBOM, and it replaces the pinned kernel. |
| Truck (Rust CAD kernel) | Rejected: replaces pinned OCCT, new dependency/servicing surface, not required for static preview. |
| Direct NURBS/Bezier GPU ray tracing | Rejected for this slice: requires the viewer to understand CAD primitives, violating the normalized-wire and no-kernel-in-viewer rules. Future separately approved plan only. |

## Revised execution order

| Order | Task | Outcome | Depends on |
| ---: | --- | --- | --- |
| 4 | STEP-004 | Bounded deterministic B-rep tessellation, LOD, progressive delivery, CAD errors (in progress; scope expanded below) | STEP-003 |
| 5 | STEP-005 | Measured and improved STEP render-time: parse/transfer, parallel/user-side meshing, input I/O, envelope, first-frame budget, progress | STEP-004 |
| 6 | STEP-006 | Interoperability closure, tessellated/faceted fidelity, healing decision, self-contained scope acceptance | STEP-004, STEP-005 |
| 7 | STEP-007 | Product, package, documentation, and activation integration | STEP-006 |
| 8 | STEP-008 | Qualification and hardening, including large-file performance regression gates | STEP-007 |
| 9 | STEP-009 | Explorer thumbnail adapter | STEP-008 and the general thumbnail-provider foundation |

Descoped/blocked work is listed at the end. Focused tests and required
documentation land with the implementing task; qualification is evidence for
completed work, not a substitute for omitted limits or tests.

## STEP-004 — bounded tessellation, LOD, and progressive CAD delivery

Status: complete for work items 1, 2, 4, 5, and 6 (single-pass progressive
display; see [STEP-004-VERIFICATION.md](STEP-004-VERIFICATION.md)). Work item 3
(coarse catalog replacement) and work item 7 (per-definition coarse/display cost
evidence) are delegated to STEP-005 item 5, which owns the two-pass-versus-
single-pass decision; the profile now truthfully records the pinned build's
serial meshing. This task is expanded by the performance findings so STEP-005
does not have to rebuild it.

### Objective

Turn accepted B-rep/tessellated definitions into deterministic bounded
rendering chunks without compromising the renderer's proxy, residency, or
replacement guarantees, and expose the LOD shape that STEP-005 will measure.

### Work

1. Finish the STEP-001 profile as versioned product constants: coarse/display
   deflection, angle, minimum size, shape-scale clamps, face/edge limits,
   output triangle limits, and measured deadlines, included in importer/cache
   version evidence. **Correct the `parallel` member**: it must describe the
   actual pinned build (see STEP-005), not an assumed TBB path. Bump
   `kStepTessellationProfileVersion` on any semantic change.
2. Tessellate and extract one reusable definition at a time, in bounded face
   blocks. Handle local face location/orientation, cluster-local positions with
   double origins, normals, and material seams with the same payload/validator
   rules as mesh formats. Never retain all extracted CAD geometry solely to
   deduplicate it.
3. Build a complete coarse catalog across every nonempty component bounded by
   the existing coarse limits and a CAD-specific quality floor validated by
   golden images. Publish through `ChunkBatchSink`/validator/upload, then
   replace with display detail only after copy-fence completion. Preserve the
   current document until a complete usable STEP coarse catalog exists.
4. Conversion for source tessellated shape representations: validate
   topology/counts and use them only when they meet the pinned fidelity policy;
   otherwise tessellate the underlying B-rep when available. Unvalidated
   faceted data is never trusted GPU input.
5. Enforce every applicable Tier-B and STEP-specific budget during traversal,
   meshing, extraction, normal generation, section creation, and host output.
   Backpressure shared sections and host-owned CPU data; one complex face must
   not grow an unbounded vector or make the UI wait.
6. Emit new typed CAD errors/status flags and fixed host-owned text for
   accepted-with-approximation, B-rep meshing failure, unit issue, and resource
   limit. Keep generic parser/kernel diagnostics out of the UI.
7. Measure per-definition coarse and display mesh cost and record it, so
   STEP-005 can decide two-pass vs single-pass from data rather than assumption.

### Verification

- Golden readback tests cover cylinders/cones/spheres, trimmed NURBS,
  fillets/chamfers, holes/voids, seam/orientation cases, tiny/large unit scales,
  face colors, faceted AP242 geometry, repeated definitions, and large offsets.
- Repeated imports and Debug/Release builds match declared triangle, bounds,
  normal-direction, material-boundary, and image-tolerance baselines.
- Adversarial fixtures cover degenerate/invalid triangulation, extreme
  tolerances, pathological trimmed surfaces, high face/edge/triangle claims,
  NaN/Inf after extraction, cancellation at each boundary, timeout, Job-limit
  termination, delayed section handoff, and device-pressure/old-document
  survival.
- Record Tier-B first-coarse, Ready, peak-commit, host heartbeat, output-byte,
  cancellation, UI-frame, and input-latency measurements by fixture class; do
  not present Tier-A large-asset targets as CAD guarantees.

## STEP-005 — STEP render-time performance and first-frame latency

### Objective

Make realistic large STEP files reach a usable first coarse frame and a ready
document in a measured, bounded time, and remove avoidable parse/transfer,
meshing, and input-I/O cost without weakening any containment or validation
rule. This is the task that answers the Bambu Studio observation for our own
pipeline.

### Work

1. **Build a large-file corpus and set explicit budgets.** Add immutable
   realistic fixtures (roughly 20 MB up to a genuine 100 MB+ assembly) with
   provenance and SHA-256: a large assembly with many reusable definitions and
   deep nesting; a high-face-count analytic/NURBS part; a large AP242
   tessellated part; a text-heavy file that stresses parsing; and one
   pathological trim/surface case. Use official/open-licensed OCCT data or an
   anonymized real assembly where redistribution permits. From this data set
   the Tier-B STEP **time-to-first-coarse and Ready budgets** and update
   [design/09-quality-performance-and-security.md](design/09-quality-performance-and-security.md);
   the current generic Tier-B "no multi-gigabyte promise" is not sufficient
   once a file-size claim is published.
2. **Instrument the real phases before optimizing.** Measure, on the corpus and
   in Release, the elapsed time and peak commit of each phase: Part-21 lexical
   scan, `ReadStream` parse, `Transfer`, per-definition
   `BRepMesh_IncrementalMesh`, face/triangle extraction, section/chunk
   serialization, broker copy-and-validate, and GPU upload to first presented
   frame. Publish where the time actually goes. Do not assume the advisor's
   parse/mesh split applies unchanged to our constrained build.
3. **Resolve and implement tessellation parallelism.** Determine whether the
   pinned `USE_TBB=OFF` build provides any parallel `OSD_Parallel` backend. If
   not, implement product-level per-definition parallel tessellation in the
   host: a bounded worker pool that meshes independent reusable definitions
   concurrently while the single reader/Transfer thread stays serial. Prove
   thread-safety of concurrent `BRepMesh_IncrementalMesh` on separate
   `TopoDS_Shape` instances on the pinned build. Bound pool width by the Job
   commit ceiling and measured CPU headroom. If concurrency cannot be proven
   safe and deterministic, keep meshing serial, fix the profile's `parallel`
   flag and comment, and record serial throughput as the documented limit.
   Determinism (chunk ids/sizes/checksums) must be identical with parallelism
   on and off.
4. **Reduce input I/O and parsing cost inside the rules.** Evaluate a
   product-owned streambuf over a read-only mapped view of the inherited handle
   (no path, no `MapViewOfFile` of a path) as a `ReadStream(std::istream&)`
   source, and `fast_float` in `StepPart21Preflight`. Evaluate eliminating the
   current double full read (preflight then OCCT), for example by fusing
   bounded lexical checks into one pass or by admitting the OCCT parse to
   consume the same mapped view. Adopt only measured, deterministic wins; the
   handle-only, AppContainer, and no-path rules are unchanged.
5. **Choose and version the delivery strategy.** Compare two-pass
   coarse-then-display against single-pass display streamed per definition.
   Pick the approach that minimizes time-to-first-usable-frame and total work
   on the corpus, make it a versioned product constant (not a per-file
   heuristic), and reconcile it with the ADR-015 rule that a complete coarse
   representation must exist before it replaces the old document.
6. **Validate and right-size the supported envelope.** Test the admission and
   normalized ceilings (5 million entity instances, 256 depth, 2 GiB source,
   20 M triangles, 60 M vertices, 50,000 nodes, 32,768 materials) against the
   corpus. A realistic large file must either import within budget or fail
   fast and typed before expensive OCCT work; a 250 MB text file can approach
   the entity cap. Adjust caps only through a documented design update, and
   keep an early preflight entity-count abort so over-cap files do not pay for
   a full scan.
7. **Make long loads legible and responsive.** Add a bounded, product-owned
   phase/progress signal (preflight bytes, parse/transfer in progress,
   definitions meshed N of M) so the viewer shows meaningful bounded
   provisional status rather than an apparent hang; never surface raw kernel
   text. Confirm section backpressure, heartbeat, and input/camera interaction
   remain within the loading latency gates while the host is CPU-bound.
8. **Re-prove cancellation and containment with the new machinery.** Exercise
   phase-boundary cancellation around the parallel mesher, and confirm the Job
   Object is still the only hard stop inside an uninterruptible parse/transfer.
   A cancelled/limited large import leaves the viewer and a later valid open
   working.

### Verification

- Large-corpus time-to-first-coarse, Ready, peak commit, output-byte, and
  UI/input-frame measurements by fixture class in Debug and Release, with
  before/after evidence for every adopted optimization.
- Determinism runs prove identical normalized output with parallelism on and
  off; the profile version changes if and only if output could change.
- Boundary fixtures just under and just over each effective cap import or fail
  fast with the correct typed result.
- Path/network/child-process denial and copy-before-trust remain green on the
  large corpus; no optimization weakens them.
- A cancelled, timed-out, or Job-limited large import recovers and a later
  valid import succeeds without viewer restart.

### Exit criteria

- Documented Tier-B STEP time-to-first-coarse and Ready budgets backed by
  evidence on at least one genuine 100 MB+ assembly and one high-triangle-class
  fixture, with the where-time-goes breakdown published.
- Tessellation parallelism is either proven deterministic and bounded on the
  pinned build or explicitly serial and documented with measured throughput.
- Over-cap and pathological files fail fast and typed, never hang the UI or
  exhaust the host.

## STEP-006 — interoperability closure and self-contained scope acceptance

### Objective

Close the accepted static-preview contract, make the external-document no-go
explicit and product-approved, and decide healing from evidence. This replaces
the former external-document implementation task, which STEP-001 already made a
no-go.

### Work

1. Establish the interoperability matrix from immutable fixtures: AP203,
   AP214, AP242 B-rep and tessellated representations; names/colors/layers;
   assemblies; and explicitly unsupported or geometry-free/AP-specific data.
   Require a named expected result for each matrix row, exercised through the
   STEP-004/005 pipeline.
2. Verify the acceptance and conversion of supported AP242 tessellated shape
   representations and the fallback to underlying B-rep; reject unvalidated
   faceted data.
3. Complete the healing decision left open by STEP-001: compare no healing, a
   narrowly pinned validation/healing sequence, and broad automatic healing.
   Adopt only operations with deterministic visual benefit and bounded
   time/memory; otherwise fail invalid geometry rather than mutate it.
4. Record the product-owner decision that external STEP documents are out of
   scope for this slice, per the STEP-001 no-go, and reflect it in the README,
   support matrix, design docs, and user-visible limitations. Until that
   decision is recorded, any `FILE_POPULATION`/`DOCUMENT_FILE` declaration
   continues to return `UnsupportedRequiredFeature`.
5. Fuzz declaration discovery and rejected external declarations so the
   no-go boundary cannot be bypassed into a filesystem or network access.

### Exit criteria

- The checked-in matrix makes the supported AP/profile visual contract and all
  intentional exclusions testable.
- Every unsupported, geometry-free, or external-declared family has an
  asserted typed outcome and no bypass of the host's path/network denial.
- The self-contained scope is stated consistently in code, tests, and public
  documentation.

## STEP-007 — product, package, and documentation integration

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
   format, phase/progress, verified dimensions, units, geometry/node/instance/
   material counts, and fixed approximation warnings. Use the existing
   document-level Fit, shading, ground-axis, selection, and loading-state UX;
   do not add a CAD tree, layer browser, or edit command.
3. Stage the exact signed OCCT host payload, its real dependency closure, and
   resource configuration beside a dedicated `StepHost` directory. Extend
   portable and NSIS staging, manifest hashes, AppContainer ACL provisioning,
   uninstall, tamper tests, dependency notices, SBOM, and release signing. The
   viewer and general worker payloads remain OCCT-free.
4. Add `Binbuf.Preview3D.STEP.1` and both `.step`/`.stp` registration entries
   to the actual NSIS package without altering the user's default. Extend
   install/repair/upgrade/uninstall/conflict tests and the Default Apps
   capability map. Do not add thumbnail registration in this task.
5. Update README, portable/installer notes, About/help strings, supported
   extension tables, `.docs/TODO.md`, `.docs/PROGRESS.md`, design support
   matrix/architecture/package/threat documents, and public limitations. State
   the static preview subset, the measured Tier-B time-to-first-coarse/Ready
   budgets, the external-reference no-go, and lack of PMI/editing/Explorer
   thumbnails.

### Verification

- Valid self-contained STEP files open through every viewer activation route,
  remain responsive during import, and recover after malformed, over-limit,
  cancellation, host-crash, timeout, and replacement cases.
- `Preview3D.exe` and `Preview3DImportWorker.exe` do not load an OCCT DLL or
  link an OCCT import library. Packaging contains exactly the approved StepHost
  closure, license notices, hashes, ACLs, and uninstall behavior.
- Open With/Default Apps, portable launch, NSIS install/repair/upgrade/
  uninstall, clean standard-user startup, and handler-conflict tests cover both
  extensions without changing an existing user default.

## STEP-008 — qualification and hardening

### Objective

Produce the evidence required to call the STEP viewer slice complete, keep its
CAD-kernel attack surface serviceable, and hold the STEP-005 performance gains
with regression gates.

### Work

1. Freeze a provenance-and-SHA-256 corpus manifest of official/open-licensed
   OCCT STEP test data where redistribution permits, AP203/AP214/AP242
   conformance/interoperability examples, product-authored focused fixtures,
   the STEP-005 large-file corpus, and anonymized representative assemblies.
   Commit expected normalized facts, render tolerances, source maps, and
   supported/unsupported classifications; never regenerate golden output
   implicitly.
2. Add standalone no-GPU sanitizer/libFuzzer targets for Part-21 admission,
   bounded external-declaration rejection, the XDE/OCCT stream-adapter
   boundary, B-rep triangulation extraction, STEP host control messages, and
   normalized CAD chunks, with seeds for comments/strings, entity
   IDs/references, complex instances, malformed headers/data sections,
   trims/NURBS, assemblies/colors/units, and external-declaration cycles.
3. Run the shared normalized-scene invariants, protocol/property suite,
   synthetic hostile-StepHost suite, AppContainer path/network/process denial,
   Job/timeout/worker replacement, cancellation/open storm, delayed-batch, GPU
   validation, device-pressure, cache-version, and source-change tests with
   STEP enabled.
4. Measure per fixture class: Part-21 admission time, XDE transfer time,
   coarse/display tessellation time, first-coarse/Ready, peak host commit,
   output bytes/triangles, cancellation/termination latency, host heartbeat,
   UI/frame/input latency, and normalized-render comparison. Enforce the
   STEP-005 budgets as release gates on the large corpus; revisit numeric
   limits only through a documented design update, never a silent release
   build setting.
5. Complete static analysis, dependency vulnerability/license review,
   Debug/Release reproducibility, 8-hour mixed-format soak (including large
   STEP files in the open/cancel/replacement mix), clean offline
   standard-user VM, portable/installer lifecycle, signed artifact/hash/SBOM
   inspection, and an OCCT update/rollback servicing rehearsal.

### Exit criteria

- Every promised visual capability has immutable normalized and rendered
  evidence; every unsupported/over-limit/malformed family has an asserted
  typed outcome.
- No corpus item crashes or hangs the viewer, bypasses the sandbox or
  copy-then-validate rule, creates an unbounded allocation, exposes a path or
  diagnostic, or stalls UI/render beyond the loading gates.
- Performance reports meet the STEP-005 Tier-B first-coarse/Ready budgets and
  documented limits on the defined references, with raw traces and fixture
  hashes linked to the signed release candidate.

## STEP-009 — Explorer thumbnail adapter

### Objective

Complete the original-MVP STEP family without blocking the viewer slice.

Status: blocked on STEP-008 and the general Gate 6 thumbnail-provider
foundation.

1. Link a separately built, explicitly limited OCCT STEP/XDE/tessellation
   adapter only into the isolated thumbnail DLL/surrogate, never the viewer or
   either import host. It consumes only `IInitializeWithStream` through a
   bounded seekable stream and proves no arbitrary path, sidecar, network,
   cache, process launch, D3D device, or persistent write occurs.
2. Support self-contained Part-21 visual geometry only under stricter provider
   ceilings: 256 MiB stream, 192 MiB parser/tessellation scratch, 384 MiB
   private commit above baseline, a much smaller inspected/rasterized triangle
   cap, and the general 2-second cutoff/750 ms p95 target. Required external
   documents, unsupported content, or budget pressure safely return failure so
   Explorer uses its normal icon.
3. Use a fixed low-detail deterministic tessellation/sample policy and the CPU
   rasterizer. Preserve bounded shape colors and complete-assembly spatial
   representation where possible; do not represent a source-prefix or launch
   `Preview3DStepHost.exe` to obtain a thumbnail.
4. Add one dedicated STEP thumbnail CLSID/family mapping and `.step`/`.stp`
   registration only after its isolated-surrogate tests pass. Add COM stream,
   malformed/parallel/unload, golden raster, deadline, actual-surrogate,
   `DisableProcessIsolation` absence, installer conflict, and lifecycle tests.

## Descoped and blocked

- **External STEP documents (former STEP-005 core).** No-go per STEP-001: the
  OCCT resolver is path-based and protected with no stream/callback hook.
  Supporting them requires product-owner approval, a new brokered resolver
  design, and a new plan; the AppContainer boundary must not be weakened to
  gain it. Tracked only as the scope-acceptance item in STEP-006.
- **Commercial CAD kernels (ACIS, Parasolid, HOOPS Exchange).** Rejected:
  licensing, SBOM/servicing cost, and replacement of the pinned kernel.
- **Truck (Rust) or another B-rep kernel.** Rejected: replaces pinned OCCT and
  adds a new dependency and servicing surface for no required capability.
- **Direct NURBS/Bezier GPU ray tracing and procedural primitives.** Rejected
  for this slice: the viewer must consume only normalized wire records and must
  not understand CAD primitives. Any future revisit is a separate approved
  plan and does not relax non-negotiable rule 1.

## Completion boundaries

- **STEP/STP viewer support:** STEP-004 through STEP-008 complete, with
  external local-document support explicitly **out of scope** by recorded
  product decision (STEP-006), not merely unproven.
- **Original-MVP STEP/STP family complete:** STEP-009 additionally complete
  after the shared thumbnail-provider foundation exists.
- **Future CAD breadth:** IGES/IFC/JT/native CAD formats, external references,
  a CAD tree/property browser, PMI/GD&T display, saved views, exact
  measurement, repair/editing, export, and GPU CAD-primitive rendering require
  separately approved plans.

## Mapping from stp.md

| stp.md | stp2.md |
| --- | --- |
| STEP-004 bounded tessellation | STEP-004 (same, expanded with profile-parallel correction and per-definition cost measurement) |
| STEP-005 external documents | Descoped; external-reference policy and scope acceptance folded into STEP-006 |
| — | STEP-005 (new) render-time performance and first-frame latency |
| STEP-006 integration | STEP-007 |
| STEP-007 qualification | STEP-008 (adds performance regression gates and large-file soak) |
| STEP-008 thumbnail | STEP-009 |
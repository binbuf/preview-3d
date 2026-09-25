# USD post-MVP work plan

Status: implementation in progress; USD-001 through USD-008 complete, viewer/distribution route exposed; USD-009 corpus/fuzz slice complete and environmental qualification pending

Prepared: 2026-09-17  
Design authority: [design/README.md](design/README.md)

## Decision

Design-compliant USD support is too large for one implementation session. It is
not one parser adapter: the accepted architecture requires a TinyUSDZ fast path
in the existing import worker, an independently sandboxed and lazily launched
OpenUSD compatibility host, a broker-only asset resolver, USDZ archive
preflight, normalized-result equivalence, product/installer integration, and a
separate Explorer-thumbnail boundary. Implement it through the tasks below.

USDC and USDZ are feasible targets, not optional stretch extensions. TinyUSDZ's
current release branch includes USDA, USDC/crate, and USDZ support, and OpenUSD
is the compatibility implementation already selected by ADR-013. The required
spikes still have to prove the exact pinned builds, memory/cancellation behavior,
resolver boundary, and deployable Windows payload before the product advertises
any USD extension.

In this plan, **USD support** means the original design's bounded static-preview
subset across `.usd`, `.usda`, `.usdc`, and `.usdz`. It does not mean complete
USD authoring fidelity or every OpenUSD schema/plugin. In particular, animation
playback, skeletal deformation, interactive variant selection, MaterialX, procedural
schemas, arbitrary renderer/file-format plugins, remote assets, and unrestricted
composition remain out of scope. Skeletal bindings are ignored and the authored
rest pose is previewed statically.

## Review findings

The design is internally consistent about the intended endpoint:

- `design/01-product-scope.md` FR-01 and FR-15 require all four direct
  extensions and OpenUSD fallback outside the TinyUSDZ subset.
- `design/02-system-architecture.md`, ADR-013, and ADR-014 require TinyUSDZ to
  stay in `Preview3DImportWorker.exe` and OpenUSD to stay in the separate
  zero-capability `Preview3DImportHost.exe`. Neither parser may link into the
  trusted viewer.
- `design/03-file-formats-and-ingestion.md` defines the static content,
  composition, archive, dependency, error, and Tier-B limit contract.
- `design/08-installation-and-registration.md` reserves the USD ProgID,
  extensions, second AppContainer identity, and private OpenUSD payload.
- `design/09-quality-performance-and-security.md` requires fast/compatibility
  overlap equivalence, archive/object-graph fuzzing, process isolation, package
  closure, and controlled compatibility-host limits.
- `design/11-decisions-and-risks.md` requires separate TinyUSDZ and OpenUSD
  validation spikes before either route can be treated as production-ready.

The product implementation still has none of the USD vertical slice. USD-001
added only a pinned dependency and test-only worker route:

- `vcpkg.json` and the checked-in overlay pin TinyUSDZ 0.9.1 for the completed
  feasibility spike. OpenUSD is not pinned and has no minimal build recipe.
- `compatibility-host/src/main.cpp` is an empty four-line process, and its
  project has no broker, model-core, OpenUSD, sandbox-client, or adapter code.
- Protocol v10 has no USD start opcode/request, `SourceFormatId`,
  `ImportFormat`, `UnsupportedComposition`, `CompatibilityHostFailure`, or
  `CompatibilityHostLimit` implementation.
- `WorkerRequestDispatch`, `ImportSession`, and `WorkerPool` know only the
  current glTF/STL/PLY/OBJ/FBX paths. The pool and profile lifecycle are specific
  to the general worker.
- The current sidecar broker is a useful base, but it has not been proven as a
  synchronous OpenUSD `ArResolver`/`ArAsset` boundary or for USD composition
  graphs and archive-contained identifiers.
- The viewer, active-instance validator, picker/drop strings, Open With cache,
  NSIS registration, package allowlists, documentation, fixtures, fuzzing, and
  app smoke tests all stop at FBX.
- The thumbnail provider is still a DLL-entry-point stub, so USD thumbnails
  depend on the format-neutral Gate 6 provider foundation just like FBX
  thumbnails do.

The design also leaves several choices that must become explicit before code is
enabled:

1. `.usd` is a family extension and must be classified by bytes as USDA or
   USDC; `.usda`, `.usdc`, and `.usdz` must reject mismatched/unsafe content
   rather than trusting only the suffix.
2. The deterministic static evaluation time, purpose/visibility policy,
   default-prim behavior, payload loading policy, interpolation rules, mesh
   orientation/subdivision behavior, and optional-versus-required omission
   policy need fixture-backed definitions.
3. USD supports X, Y, and Z up axes, while protocol v10's `UpAxisId` currently
   has only Y and Z. Units/up-axis handling must be fixed in the format-neutral
   contract before USD output is accepted.
4. A fast-path fallback must be atomic. TinyUSDZ may not publish candidate
   geometry and then ask OpenUSD to append to it. The broker either commits a
   completed TinyUSDZ result or discards it and starts the compatibility host
   for the same generation.
5. OpenUSD's plugin and environment-based discovery is incompatible with the
   product boundary unless the pinned payload, resolver registration, DLL
   search, `plugInfo.json` discovery, and all asset reads are explicitly locked
   to the installed host payload and brokered handles.

## Product target

The viewer target is:

- `.usd`, `.usda`, `.usdc`, and `.usdz`, case-insensitively, with byte-level
  identification and accurate source-format reporting;
- static `UsdGeomMesh` data with bounded triangulation, finite points, normals,
  common UV/color primvars, face material subsets, orientation, transforms,
  units, and up axis;
- hierarchy, reusable mesh instances, bounded point instancers, visibility,
  and verified double-precision world bounds through the existing protocol-v10
  node/instance contract;
- display color/opacity and a bounded USD Preview Surface subset mapped to the
  existing normalized base-color/metallic/roughness/emissive/normal material
  and texture contract;
- stream-contained USDZ assets and broker-approved local dependencies only;
- bounded local static composition through sublayers, references,
  inherits/specializes, authored default variant selections, and selectively
  loaded payloads in the OpenUSD host;
- typed warnings/failures for omitted optional or required content;
- Tier-B count, byte, archive-expansion, depth, scratch, elapsed-work, and Job
  Object limits; cooperative cancellation where available and bounded process
  termination otherwise;
- command line, dialog, drop, secondary activation, Open With/Default Apps,
  renderer/metadata/error UI, portable and installer payloads, and release
  evidence.

The Explorer target is a later, separate stream-only TinyUSDZ thumbnail adapter
for the fixed USD CLSID. It never invokes OpenUSD, the compatibility host, the
viewer, sidecars, or the derived cache.

## Execution order

| Order | Task | Outcome | Depends on |
| ---: | --- | --- | --- |
| 1 | USD-001 | Prove the pinned TinyUSDZ fast-path build, limits, cancellation, and subset | Current main branch |
| 2 | USD-002 | Prove a minimal pinned OpenUSD payload and broker-only resolver inside the compatibility sandbox | USD-001 contract decisions; may reuse its corpus |
| 3 | USD-003 | Add the USD-family protocol, format, error, axis, and atomic-fallback contract | USD-001 and USD-002 decisions |
| 4 | USD-004 | Import static USDA/USDC geometry and scene instances through TinyUSDZ, test-only | USD-003 |
| 5 | USD-005 | Add USDZ, Preview Surface, textures, approved dependencies, and the final fast-path decision | USD-004 |
| 6 | USD-006 | Build the lazy compatibility-host lifecycle and fallback orchestration | USD-002 and USD-003 |
| 7 | USD-007 | Compose and normalize the broader bounded static subset through OpenUSD | USD-005 and USD-006 |
| 8 | USD-008 | Enable viewer, activation, registration, packaging, and documentation surfaces | USD-007 |
| 9 | USD-009 | Complete corpus, fuzz, security, performance, package, and release qualification | USD-008 |
| 10 | USD-010 | Add bounded Explorer thumbnails | General Gate 6 thumbnail foundation and USD-009 |

The tasks are intentionally gated. USD-004 and USD-005 remain undiscoverable
test routes; the product must not advertise a fast-parser-only implementation
as general USD support. USD-008 may expose the four extensions only after the
typed fallback and compatibility host work end to end.

## USD-001: TinyUSDZ feasibility and policy spike

Status: complete (2026-09-17); proceed with the revised self-contained-layer fast subset in [USD-001-SPIKE-RESULTS.md](USD-001-SPIKE-RESULTS.md)

Depends on: none  
Unblocks: USD-002 and USD-003

### Objective

Satisfy required validation spike 5 for TinyUSDZ and turn the broad design text
into an exact, testable common-static-subset policy before wiring a production
opcode.

### Work

1. Create a repository overlay port pinned to an immutable TinyUSDZ commit.
   Use a production/minimal build: readers for USDA/USDC/USDZ and required
   composition/Preview Surface APIs only; disable tools, tests, Python, audio,
   OpenSubdiv, MaterialX, unrelated file-format bridges, built-in image codecs
   that bypass product decoding, and any other unused feature. Record license,
   transitive code, compile definitions, binary-size impact, and SBOM identity.
2. Read the installed pinned headers and implementation rather than coding from
   online examples. Prove byte/memory/custom-resolver entry points for USDA,
   USDC, and USDZ on MSVC x64, including production error behavior and the lack
   or presence of allocator/progress hooks.
3. Build a non-product AppContainer spike using brokered source/dependency
   handles. Measure parse, composition, render-data conversion, archive, and
   normalized-output peak private bytes independently. Exercise a low Job
   Object cap and any library memory limit; never infer safety only from source
   size.
4. Measure cooperative cancellation points. Where TinyUSDZ has a
   non-interruptible call, prove the existing 500 ms grace and worker
   terminate/replace path contains it and permits the next valid import.
5. Define the exact fast subset with small redistributable fixtures: USDA,
   USDC, `.usd` holding each encoding, USDZ, meshes/primvars, hierarchy,
   instances/point instances, units/up axis, display color, Preview Surface,
   contained textures, simple approved local references, and every composition
   feature that triggers fallback.
6. Decide deterministic static-time, purpose, visibility, prototype,
   interpolation, orientation, subdivision, material-subset, and unsupported
   schema policies. Classify each case as accept, bounded approximation with a
   warning, `UnsupportedRequiredFeature`, or `UnsupportedComposition`.
7. Preflight USDZ ZIP paths, duplicate/case-colliding entries, ZIP64, alignment,
   encryption, compression methods, entry sizes, aggregate expansion, and
   ratio before parser access. Confirm product code can enforce the same policy
   independently of TinyUSDZ.
8. Write `.docs/USD-001-SPIKE-RESULTS.md` with exact version/commit, build
   flags, fixture provenance/hashes, measurements, feature matrix, and an
   explicit proceed/revise decision. If the current design's fast subset or
   limits cannot be enforced, update the ADR/design and this plan first.

### Exit criteria

- USDA, USDC, and USDZ parse in the real worker sandbox with deterministic
  normalized hashes and no worker-originated filesystem/network access.
- Malformed/crate/archive/count/allocator failures are controlled, and a later
  valid request succeeds in the same pool or its bounded replacement.
- Cancellation and peak-memory evidence exist for Debug and Release.
- The immutable dependency and minimal feature surface are suitable for the
  release SBOM; no unreviewed parser/codec silently enters the worker.

## USD-002: OpenUSD host and resolver spike

Status: complete; proceed with the isolated monolithic compatibility payload

Depends on: USD-001  
Unblocks: USD-003, USD-006, and USD-007

### Objective

Satisfy required validation spike 6 before building the compatibility path.
Prove that a useful minimal OpenUSD build can run in the second AppContainer,
compose through brokered bytes only, and ship as a deterministic private
payload.

### Work

1. Pin an immutable OpenUSD release/commit through a reviewed overlay port or
   equally reproducible checked-in build recipe. Disable Python, imaging/Hydra,
   usdview, tools, examples/tutorials, tests, MaterialX, Alembic, Draco, RenderMan,
   and all unused third-party/file-format plugins. Compare monolithic and
   minimal shared builds for app-local deployment, startup, footprint,
   servicing, and required `plugInfo.json` resources; record the chosen form.
2. Build a temporary `Preview3DImportHost.exe` spike under a distinct
   `Binbuf.Preview3D.ImportHost` zero-capability profile and the lower of 4 GiB
   or 35% physical-memory commit limit. It must be launched/job-assigned before
   OpenUSD initializes and must receive only explicit pipes, events, sections,
   and read-only brokered handles.
3. Implement a spike resolver/asset layer that maps opaque product identifiers
   to parent-brokered handles or bounded byte stores. Prove sublayers,
   references, payloads, USDZ-contained assets, and textures can compose
   without exposing a model path/directory handle or permitting OpenUSD's
   default resolver to open arbitrary paths.
4. Lock DLL and plugin discovery before OpenUSD initialization. Ignore or
   replace environment-selected resolver/plugin paths, register only
   release-manifest-listed resources by absolute installed location, and prove
   a model cannot load a DLL, schema, file-format plugin, script, codec, or
   resolver from its directory or the environment.
5. Open stages with initial payload loading disabled. Discover/load required
   payloads breadth-first under dependency-count, byte, graph-depth, time, and
   memory limits. Measure cold/warm host startup, `LoadNone`, first geometry,
   cancellation, forced Job termination, and restart.
6. Run common fixtures through TinyUSDZ and OpenUSD and compare a canonical
   semantic digest for hierarchy, transforms, geometry, instances, materials,
   units/up axis, bounds, and warnings within documented numeric tolerances.
7. Prove host crash, hang, invalid output section, resolver abuse, and memory
   limit affect only the generation and that the viewer/general worker remain
   usable.
8. Write `.docs/USD-002-SPIKE-RESULTS.md` with payload inventory/hashes,
   build flags, plugin/resource search audit, resolver design, measurements,
   equivalence results, and an explicit proceed/revise decision.

### Exit criteria

- The compatibility host has no unbrokered file, directory, network, plugin,
  child-process, or cross-payload authority.
- `UsdStage::LoadNone` plus bounded incremental payload loading works for the
  target composition corpus.
- The minimal signed-payload shape is relocatable only in the documented
  package layout and rejects missing/modified/unlisted resources.
- Cancellation/termination and memory-limit recovery meet the design without
  relaxing the AppContainer or resolver boundary.

## USD-003: USD-family protocol and normalized contract

Status: complete (2026-09-17)
Depends on: successful USD-001 and USD-002 decisions (complete)
Unblocks: USD-004 and USD-006

### Objective

Add the smallest format-neutral protocol/broker changes both USD adapters need,
without yet exposing a USD extension to the viewer.

### Work

1. Add closed source-format identities for USD-family results. Preserve the
   actual detected container/encoding (`USDA`, `USDC`, or `USDZ`) even when the
   source suffix is `.usd`; add `ImportFormat::Usd`, a dedicated USD request
   opcode/struct, one-shot test mode, and pooled worker dispatch.
2. Add the missing typed errors from the design: `UnsupportedComposition`,
   `CompatibilityHostFailure`, `CompatibilityHostLimit`, and any archive-
   specific distinction actually required by UI/recovery policy. Update the
   known-code validator, redacted host mapping, fuzz inputs, and tests together.
3. Extend `UpAxisId` to include X and prove existing Y/Z behavior remains
   byte/semantically compatible. Define finite positive `metersPerUnit`
   handling and retain zero only for genuinely unspecified units.
4. Confirm protocol-v10 node/mesh-instance/material/image records can represent
   the approved USD subset, including point-instancer expansion and per-face
   material splits within Tier-B catalogs. If a wire layout changes, bump the
   protocol, reject v10, update static assertions/fixtures, and rerun every
   hostile-worker case; do not smuggle variable USD structures across IPC.
5. Specify atomic fallback state: a worker must classify composition before
   any candidate result becomes publishable. On the exact typed fallback only,
   discard all TinyUSDZ candidate sections/catalog state, preserve the source
   generation and broker authority, and begin a fresh compatibility-host result
   stream. Malformed, unsafe, archive-limit, OOM, and resource-limit results
   never retry through OpenUSD.
6. Extend strict source-format and Tier-B validation in `ImportSession`; USD
   scene records must obey the same copy-then-validate, backward-reference,
   hierarchy, transformed-bound, count, byte, and progressive batch invariants
   as FBX.
7. Add protocol/property/hostile tests for new enums/opcodes, spoofed formats,
   illegal axes/units, candidate publication before fallback, stale fallback,
   fallback loops, partial result mixing, and mutation after the host copy.

### Exit criteria

- Existing Debug/Release Unit and ImportIsolation suites remain green.
- The new worker route is reachable only from tests and returns typed results;
  no viewer extension list changes.
- One generation can choose exactly one committed producer—TinyUSDZ or
  OpenUSD—and can never publish a mixture.

### Completion record

Protocol v10 remains the active wire version. Its node, geometry, instance,
material, image, and scene-metadata records already represent the approved USD
subset: point instancers expand to ordinary bounded node/instance records, and
face-material subsets split into ordinary geometry sections that retain one
logical mesh identity. No USD-specific variable structure crosses IPC. For USD
geometry provenance, `sourceRangeOffset` packs the source object/prim ordinal in
the high 32 bits and the normalized point/index start in the low 32 bits;
`sourceRangeLength` is the point count for point lists and index count for
triangles. These rules are format-neutral after adapter normalization and fit
the existing Tier-B catalogs, so no layout or version bump was warranted.

The closed additive identities are `USDA`, `USDC`, and `USDZ`, with X appended
to `UpAxisId` so the existing numeric Y/Z identities do not move. USD results
must report X, Y, or Z and a finite positive `metersPerUnit`; zero remains legal
for non-USD formats whose units are genuinely unspecified. `.usd` trusts byte
sniffing, while `.usda`, `.usdc`, and `.usdz` put exactly one expected-encoding
bit in the dedicated 48-byte request. A suffix mismatch is terminal. USD uses
Tier-B validation and cannot return another family's source identity.

`UnsupportedComposition` is the sole fast-worker result that can select the
compatibility producer, and only before a fast batch has been accepted.
Malformed data, unsafe references, archive limits, unsupported encodings,
resource limits, cancellation, and importer failures are terminal. The
generation-scoped fallback state starts with no committed producer; stale
observations are ignored, while late/reverse fallback, producer mixing, and
fallback loops are protocol violations. USD-006 will connect this state to the
compatibility process lifecycle. Until then, `ImportSession` reports the exact
pre-publication decision through `compatibilityFallbackRequired` but never
starts a second producer.

Fast/compat equivalence uses one post-triangulation, format-neutral canonical
digest. USD-004 and USD-007 must serialize the same validated normalized scene
in this order: scene axis/units/counts and typed warnings; nodes by normalized
node ID with parent, local transform, visibility, and purpose result; geometry
by logical mesh ID and section/material slot with topology, vertex layout,
normalized vertex/index payload, and local bounds; instances by node and mesh
IDs; then materials, images, and their dependency IDs. Generation IDs, source
encoding (`USDA`/`USDC`/`USDZ`), section/chunk offsets, batch boundaries,
checksums, producer identity, and source-range diagnostics are excluded. Both
adapters must use the shared normalization rules before hashing: deterministic
triangulation/order, canonical positive zero, rejected non-finite values, and
the protocol's final stored scalar precision. Thus a digest mismatch denotes a
semantic normalization mismatch rather than a packaging or scheduling change.

The worker now has pooled and one-shot `--parse-usd` dispatch, but the route is
test-only. USD-004 replaced the one-point contract marker with normalized
USDA/USDC geometry, hierarchy, and instances. Valid USDZ remains deliberately
deferred to USD-005 after product-owned archive preflight. No viewer extension
classifier, picker, activation, registration, packaging, or thumbnail surface
changed.

## USD-004: sandboxed TinyUSDZ static scene adapter

Status: complete (2026-09-17)
Depends on: USD-003  
Unblocks: USD-005

### Objective

Import untextured static USDA/USDC scene geometry through TinyUSDZ and the
existing AppContainer worker, behind test-only routing.

### Work

1. Add `UsdAdapter`/`UsdImportWorker` around the pinned API. Map only the
   brokered primary handle, enforce the 2 GiB Tier-B source limit and approved
   spike budgets, catch library/allocation failures at the boundary, observe
   cancellation, and emit only typed phase/error facts.
2. Detect USDA versus crate bytes independently of `.usd`; reject mismatched
   `.usda`/`.usdc`, unsupported versions, malformed data, non-finite values,
   hostile counts, and empty stages before publication.
3. Traverse the approved static stage deterministically. Normalize finite mesh
   points, face counts/indices, orientation, normals, UV/color primvars and
   interpolation into bounded triangle chunks. Apply the documented
   subdivision/holes/degenerate policy without unbounded whole-stage remaps.
4. Emit protocol nodes and mesh instances for hierarchy, transforms,
   visibility, reusable prototypes, native instances, and bounded point
   instancers. Preserve double-precision transforms/origins and independently
   verified local/world bounds; cap prototype expansion and draw records.
5. Carry units and X/Y/Z up axis accurately. Use the approved deterministic
   static-time/purpose/default-prim policy and report bounded warnings for
   visible optional omissions.
6. Detect composition arcs/features before publish. Handle only the exact
   USD-001 common subset and return `UnsupportedComposition` for otherwise
   valid stages; never use fallback to hide malformed or unsafe input.
7. Add focused fixtures/tests for `.usd` ASCII/crate detection, explicit
   `.usda`/`.usdc`, topology/primvars, hierarchy, instance sharing, point
   instances, large origins, each up axis, units, visibility/purpose/time
   policy, progressive small sections, cancellation, limits, malformed input,
   fallback classification, pool reuse, and recovery.

### Exit criteria

- USDA/USDC geometry normalizes deterministically in Debug and Release through
  the real sandbox, and one uploaded geometry can serve compatible instances.
- Required visible data is never silently omitted; optional omissions have
  bounded host-owned warnings.
- No parser object, string, pointer, source mapping, or unvalidated section is
  retained by viewer/render code.

### Completion record

- `UsdAdapter` now consumes only the already-brokered mapped primary bytes,
  byte-detected as USDA or USDC, and keeps TinyUSDZ asset/composition loading
  disabled. The worker boundary catches allocation/library failures, observes
  cancellation throughout conversion/publication, and returns only typed
  phase/error facts.
- The adapter classifies sublayers, references, payloads, inherits,
  specializes, variants, clips, and instanceable composition before creating
  a chunk writer. Only `UnsupportedComposition` requests compatibility retry;
  malformed data, unsupported required geometry, limits, and archive policy
  remain terminal and publish no candidate batches.
- Tydra output is normalized into bounded deindexed triangle chunks with
  normals, UV0, display color/opacity, orientation, deterministic provenance,
  float-local positions plus double origins, nodes, reusable mesh instances,
  and independently transformed double world bounds. Purpose and inherited
  visibility are evaluated at authored `startTimeCode` (otherwise zero), X/Y/Z
  and finite positive units are preserved, and optional omissions produce one
  bounded host-owned status record.
- Point instancers evaluate prototype indices, IDs, positions, orientations,
  scales, and invisible IDs at the selected time. Prototype subtrees may
  contain multiple transformed meshes; emitted instances reuse their geometry
  chunks and remain bounded by the Tier-B object cap. Velocity/acceleration
  semantics that this static adapter cannot reproduce fail explicitly as
  `UnsupportedRequiredFeature`.
- TinyUSDZ 0.9.1's USDA reader had a reconstruction implementation for
  `PointInstancer` but omitted its registration in `USDAReader::Impl::Init`.
  The pinned overlay port carries a one-line patch; without it a valid USDA
  point instancer silently became a `Scope`. Exact type-ID checks remain in the
  adapter because TinyUSDZ's role-aware `Prim::as<T>` is intentionally looser
  than a concrete schema check.
- Focused real-AppContainer tests cover USDA/USDC and `.usd` byte detection,
  topology/UV/color normalization, large and prototype transforms, selected
  time, X-up/millimetre metadata, guide omission, visible/hidden shared point
  instances, deterministic output, progressive 4 KiB sections, cancellation,
  malformed recovery on a pooled worker, pre-publication composition fallback,
  unsupported point motion, and the USDZ handoff to USD-005. They pass in
  Debug and Release (56 USD-004 assertions; the adjacent USD-003 suite passes
  123 assertions in both configurations).

## USD-005: USDZ, materials, textures, and fast-path dependencies

Status: complete (2026-09-17)
Depends on: USD-004  
Unblocks: USD-007 and USD-008

### Objective

Complete the TinyUSDZ fast path with independently bounded USDZ ingestion,
Preview Surface conversion, contained textures, and the approved simple local
dependency subset.

### Work

1. Add product-owned USDZ central-directory/local-header preflight before
   TinyUSDZ sees archive content. Enforce normalized relative entry names,
   duplicate/case-collision policy, checked offsets/sizes, allowed compression,
   per-entry and 4 GiB aggregate expansion, 200:1 ratio, entry count/depth,
   encryption rejection, and cancellation. Archive-limit failure does not
   invoke OpenUSD.
2. Resolve contained layers/textures through an archive map with no extraction
   to disk. Resolve approved local fast-path references only through
   `RequestSidecarFile`; reject absolute, traversal, UNC, URL, ADS, reparse
   escape, unsafe package, and extension/type mismatch requests in the trusted
   broker.
3. Convert displayColor/displayOpacity and the approved USD Preview Surface
   inputs to existing material payloads: base color/opacity, metallic,
   roughness, emissive, normal, UV set/transform, alpha policy, and double-sided
   behavior. Split geometry for bounded `GeomSubset` material assignments.
4. Send encoded texture bytes through existing sniffed WIC/WebP/KTX paths and
   progressive mip publication. Do not use TinyUSDZ/OpenUSD image loaders or
   runtime-installed codecs. Apply semantic sRGB/linear policy, decoded-pixel
   and aggregate-byte caps, fallback warnings, and required-data failures.
5. Make the final fast/fallback decision before the first published geometry,
   material, or image batch. Test composed dependency discovery that begins in
   one layer and later reveals another; cap request count, aggregate bytes, and
   graph depth without partial commit.
6. Add USDA/USDC/USDZ material and dependency fixtures: contained PNG/JPEG,
   approved/missing/corrupt local textures, multiple UV sets, transforms,
   material subsets, unsupported shader nodes, nested packages, archive bombs,
   duplicate/traversal names, recursive composition, and aggregate pressure.

### Exit criteria

- The common static subset works across USDA, USDC, `.usd`, and USDZ with
  accurate materials/textures and recoverable optional fallbacks.
- Every file/archive dependency is brokered and budgeted; no source-derived
  path reaches worker filesystem APIs.
- Unsafe, malformed, and over-limit cases cannot become a permissive OpenUSD
  retry, and the next valid import succeeds.

### Completion record

- The product-owned USDZ inspector now validates EOCD and central/local-header
  agreement, stored-only entries, encryption/data-descriptor/ZIP64 rejection,
  normalized case-folded paths, duplicates, 64-byte data alignment, checked
  ranges with no overlap, per-entry and 4 GiB aggregate limits, CRC32, and
  cancellation. It returns bounded entry views backed by the brokered primary
  mapping; nothing is extracted, and every archive-policy failure is terminal
  `ArchiveLimit` before TinyUSDZ runs.
- TinyUSDZ's wildcard asset resolver is closed over that entry map and the
  existing synchronous `RequestSidecarFile` protocol. Relative image assets
  are normalized and budgeted (64 unique requests, 512 MiB retained encoded
  bytes); absolute, UNC, URL, ADS, traversal, package/layer, and unapproved
  extension requests fail as `UnsafeReference`. Case collisions and content
  type/extension mismatches are terminal, while missing or corrupt optional
  images receive a bounded warning and deterministic fallback image.
- The USD-001 boundary remains authoritative: the fast path accepts one
  self-contained root layer and image assets only. Sublayers, references,
  payloads, variants, and other composition still return pre-publication
  `UnsupportedComposition` for USD-006/007; USD-005 did not broaden local
  sidecars into a second layer loader.
- USD Preview Surface values now populate the normalized base color/opacity,
  metallic/roughness, emissive, normal, UV transform, alpha, and double-sided
  fields. Tydra material subsets are range/overlap validated and split into
  material-homogeneous geometry runs, so ordinary and point-instancer records
  bind the correct material chunk. Unsupported optional shader inputs and UV
  sets become bounded status warnings.
- Texture bytes remain encoded through TinyUSDZ. Product code sniffs them and
  uses the existing WIC, WebP, and KTX/Basis adapters with semantic sRGB/linear
  selection, per-image and aggregate decoded limits, cancellation, and small
  tail-mip-first publication where a mip chain exists. TinyUSDZ's built-in
  image loader and runtime-installed codecs remain disabled.
- Resolver/path/type/composition classification completes before a chunk
  writer exists. A terminal late decode/write failure still invalidates the
  generation in the broker; it can never request the more permissive OpenUSD
  retry. The next pooled import is independently classified.
- TinyUSDZ 0.9.1 requires `TimeCode::Default()` specifically for Tydra
  conversion of the canonical static USDZ fixture; numeric time zero makes
  that wrapper fail even though its contained crate converts directly. Stage
  policy/classification still uses the deterministic selected static time.
  Re-test this quirk when advancing the pin.
- Focused real-AppContainer tests cover brokered PNG materials, material
  subsets, contained USDZ textures with no extraction, missing/corrupt
  fallback, extension/content mismatch, unsafe traversal, bounded archive
  views, CRC-backed preflight, and cancellation. Debug and Release pass all
  five USD-005 cases (the final suite has 91 assertions after transform and
  negative type-mismatch coverage); existing USD-003/004 cases remain green.
  Unit tests pass 98/98 in both configurations. Full Debug isolation passes
  271/275 with only the four previously documented FBX fixture-rewrite
  failures; full Release passes 263/275 with those four plus eight known
  randomized sidecar
  scratch-directory collisions. No full-run failure enters a USD route.
- The TinyUSDZ overlay is revision `0.9.1#2`. Its installed copyright now
  combines the upstream license with notices for every vendored component
  used by the minimal static library, and portable-package notice generation
  includes TinyUSDZ. The clean Release worker is 5,413,376 bytes, 27,136 bytes
  over USD-004 and 109,056 bytes over the post-USD-001 worker; TinyUSDZ still
  adds no runtime DLL. An unsigned engineering portable package passes
  dependency closure and stages `licenses/tinyusdz.txt` plus TinyUSDZ in its
  SBOM.

## USD-006: compatibility-host platform and fallback lifecycle

Status: complete (2026-09-17)
Depends on: USD-002 and USD-003  
Unblocks: USD-007

### Objective

Turn the OpenUSD spike into the production second import process and integrate
it with document generation, cancellation, validation, and recovery without
duplicating a weaker broker path.

### Work

1. Generalize/reuse `SandboxLauncher`, control framing, sidecar servicing,
   shared sections, copy-then-validate, cancellation events, and Job enforcement
   for a distinct host executable/profile/payload. Keep separate AppContainer
   SIDs and ACLs; a section/pipe for one import identity must not admit the
   other.
2. Add a lazy compatibility-host session manager. It starts only after the
   current generation's exact `UnsupportedComposition`, is never on startup or
   non-USD paths, handles at most the current USD work, and exits after the
   generation or a bounded idle grace. Closing the viewer closes its job.
3. Give the host its own request opcode/struct and CLI/pool loop while reusing
   the normalized wire schema. The broker supplies primary and dependency
   handles/opaque IDs, never paths; resolver requests use bounded typed frames
   and the existing canonical handle policy.
4. Implement atomic orchestration in the import bridge/broker: discard the fast
   candidate, launch the host on a background lane, preserve generation
   cancellation/staleness, and expose only host-owned validated batches.
   Prevent repeated or reverse fallback.
5. Map host launch, payload integrity, resolver, crash, timeout, commit-limit,
   cancellation, malformed protocol, and importer failures to the documented
   typed errors and redacted host-owned UI facts.
6. Extend hostile-process tests to the compatibility executable: stale/replayed
   generations, wrong opcode/format, invalid ranges/layouts/references,
   post-copy mutation, endless dependency requests/batches, crash, hang, child
   spawn, filesystem/network access, and cross-SID handle attempts.
7. Prove replace/open/close/shutdown never waits on UI/render, leaves no host or
   OpenUSD child process, and a later fast or compatibility import recovers.

### Exit criteria

- Both import processes pass equivalent sandbox/protocol/hostile-worker
  guarantees with separate least-privilege payloads.
- OpenUSD is absent from `Preview3D.exe`, `Preview3DImportWorker.exe`, and the
  thumbnail DLL dependency closures and is not loaded until fallback.
- Host failure affects only the document generation and never silently returns
  the TinyUSDZ candidate it replaced.

### Completion record

Protocol v10 remains unchanged. `StartOpenUsdImportFromFile=18` and its
separate 48-byte `ParseOpenUsdFileRequest` are additive control identities;
the host and fast worker cannot accept each other's start opcode. Both
producers use the existing bounded control frames, brokered raw source and
dependency handles, cancellation event, reused output-section window,
copy-then-validate catalog, and normalized wire schema.

`RunImportSession` now owns the atomic orchestration. It starts OpenUSD only
for a USD fast result whose exact first error is `UnsupportedComposition`,
whose accepted batch count and candidate chunk list are both zero, and for
which a compatibility executable was explicitly configured. The second
attempt gets a new section, catalog, handle duplication, and process lease for
the same generation. Its result carries a closed producer identity; host
failure clears the fallback marker and can never reveal or restore the
discarded TinyUSDZ candidate. Reverse fallback is a protocol failure.

The compatibility manager creates the distinct
`Binbuf.Preview3D.ImportHost` zero-capability profile lazily, grants it
read/execute only on the private `OpenUsdHost` payload directory, launches one
`--pool` host suspended and Job-assigned, and permits only the current
generation to lease it. The selected bounded-idle policy is deliberately the
strict endpoint of the design: a successful/typed-error host receives
`Shutdown` immediately after the generation; a crash, hang, cancellation, or
protocol fault drops the kill-on-close Job without replacement. A later
generation launches a fresh host. The commit ceiling is the lower of 4 GiB
and 35% of visible physical RAM; a non-default ceiling exists only for
qualification.

The bootstrap remains free of OpenUSD imports. Before reading requests it
locks DLL search and clears OpenUSD/plugin discovery variables. On the first
production fallback it loads `Preview3DOpenUsdCore.dll` by absolute private
path and hashes the closed 13-file resource inventory before any future stage
open. USD-007 now supplies bounded stage composition and normalized chunk
emission behind that lifecycle. Product extension discovery remains disabled
until USD-008 completes the viewer and distribution surfaces.

Focused USD-006 tests pass in Debug and Release (2 cases / 64 assertions).
They cover exact
lazy fallback, zero fast publication, separate producer identity, immediate
host exit, cancellation, crash, hang, stale replies, reverse fallback,
commit-limit mapping, terminal fast failures that never launch compatibility,
and later fast/host recovery. USD-002 through USD-006 focused coverage passes
in both configurations. Unit passes 98/98 (7,617 Debug / 7,529 Release
assertions). Full Debug isolation reaches 273/277 with only the four known FBX
fixture-rewrite failures; full Release reaches 271/277 with those four plus
two known randomized sidecar scratch-directory collisions. No full-run failure
enters a USD route. PE inspection confirms no OpenUSD dependency in the
viewer, general worker, thumbnail DLL, or bootstrap executable.

## USD-007: bounded OpenUSD static composition adapter

Status: complete (2026-09-18)
Depends on: USD-005 and USD-006  
Unblocks: USD-008

### Objective

Implement the design's broader local static composition subset in the
compatibility host and prove semantic overlap with the TinyUSDZ route.

### Work

1. Open the broker-backed root stage with initial payload loading disabled and
   the locked resolver context from USD-002. Discover sublayers, references,
   inherits/specializes, authored default variants, and required payloads
   breadth-first under per-layer/aggregate bytes, dependency count, graph depth,
   elapsed time, and host commit limits.
2. Reject remote/custom URI schemes, cycles/over-depth graphs, unsafe paths,
   environment-selected plugins/resolvers, unsupported file formats, dynamic
   procedural schemas, and required content the static contract cannot
   represent. Optional schemas may warn only when remaining visible geometry is
   coherent.
3. Apply the same USD-001 policy for static time, purpose, visibility,
   instances/point instances, topology/primvars, orientation/subdivision,
   transforms, units/up axis, material subsets, display color, Preview Surface,
   texture semantics, and warnings. Share product normalization helpers where
   that does not link OpenUSD into another process.
4. Emit bounded progressive batches through the same writer and validator used
   by the worker. Ensure composition discovery and fallback-sensitive metadata
   are complete before first publish; later payload loading must not invalidate
   accepted scene identity or earlier references.
5. Canonicalize both adapters' normalized semantic output and require
   equivalence on the overlap corpus. Compare hierarchy/instance references,
   transformed bounds, geometry/index hashes, material factors/texture pixels,
   units/up axis, counts, and warnings within explicit tolerances—not raw chunk
   order or importer-native IDs.
6. Add compatibility-only fixtures for nested local composition, authored
   variants, inherits/specializes, payloads, instances, missing optional assets,
   unsupported required schemas, malformed layers, recursion, dependency/commit
   pressure, cancel/replace, crash/restart, and later valid reopen.

### Exit criteria

- Documented compatible stages render through OpenUSD with bounded local
  composition and no unbrokered authority.
- Every overlap fixture is semantically equivalent to TinyUSDZ or the policy is
  revised explicitly before product exposure.
- Limits/cancellation/crash paths recover and preserve prior usable content in
  the viewer bridge tests.

### Completion record

- The audited `Preview3DOpenUsdCore.dll` now owns the production import entry
  point. It opens USDA, USDC, and USDZ roots with `UsdStage::LoadNone` through
  the USD-002 `preview3d://` memory resolver, loads payload sites in bounded
  breadth-first order, and obtains every local layer or image through the
  existing synchronous broker protocol. The broker allowlist now admits
  `.usd`, `.usda`, and `.usdc` layer sidecars; recursive `.usdz` sidecars remain
  closed. Absolute, URI, traversal, reparse-escape, missing required,
  malformed, cyclic, over-depth, over-count, over-byte, timeout, and Job-limit
  outcomes fail without granting the host filesystem or network authority.
- OpenUSD composition covers sublayers, references, payloads, inherits,
  specializes, authored default variants, native instances, and bounded point
  instances. The adapter samples the USD-001 static policy time, applies
  purpose/visibility and local/world transforms, validates axes and units,
  triangulates meshes/material subsets, normalizes normals/UVs/display color,
  emits transformed instance bounds, and converts Preview Surface factors,
  texture slots, UV transforms, alpha and double-sided state. Optional texture
  failures use the product WIC/WebP/KTX decoders and deterministic warning
  fallback; unsupported required schemas or subdivision features fail closed.
- Output uses the worker's `BoundedChunkWriter`, `ChunkBatchSink`, image decode,
  USDZ preflight, wire schema, and trusted broker validator rather than a
  second compatibility-only format. The overlap fixture now produces an exact
  canonical semantic fingerprint across TinyUSDZ and OpenUSD after excluding
  generation, encoding, source ranges, offsets, and checksums as specified
  above. Compatibility USDZ composition is also exercised through the same
  normalized route.
- Original fixtures record provenance, independent expected facts, and
  immutable SHA-256 values. Focused coverage includes every approved
  composition arc, default variants, point-instance masking, materials and
  textures, optional missing assets, malformed/recursive/unsafe/required
  failures, dependency pressure, cancel/replace, crash/hang/protocol/commit
  faults, and later fast/host recovery. Viewer discovery, extension
  registration, packaging, and all thumbnail behavior remain unchanged for
  USD-008/009/010.
- Debug and Release focused USD-002 through USD-007 coverage passes 25 cases /
  705 assertions; USD-007 contributes 5 cases / 186 assertions. Unit passes
  98/98 in both configurations. Full Debug isolation reaches 278/282 with the
  four known FBX fixture-rewrite failures; Release reaches 271/282 with those
  four plus seven known randomized sidecar scratch-directory collisions. No
  full-run failure enters a USD route.

## USD-008: viewer, activation, installer, and package integration

Status: complete (2026-09-18)
Depends on: USD-007  
Unblocks: USD-009

### Objective

Expose the completed dual-path USD family consistently through every current
viewer and distribution surface, without claiming Explorer thumbnails.

### Work

1. Add case-insensitive `.usd`, `.usda`, `.usdc`, and `.usdz` classification to
   `D3D12ImportBridge`, command-line/secondary activation, picker, one-file
   drag/drop, retry/open-another, help/About, supported-format/error strings,
   metadata labels, and app smoke fixtures. Report the detected encoding, not
   merely the suffix.
2. Route all four extensions to `ImportFormat::Usd`; consume only the existing
   validated geometry/material/image/node/instance path. Verify progressive
   rendering, framing from transformed verified bounds, accurate units/up axis,
   counts, warning badges, Copy-details redaction, and prior-content retention
   on fast/host failure or cancellation.
3. Add the four extensions to ShellIntegration and increment its bounded Open
   With catalog revision. Cover uppercase paths, spaces/Unicode/long paths,
   direct launch, secondary forwarding, picker, drop, replacement while fast
   parsing, replacement while OpenUSD is active, and immediate relaunch.
4. Add the stable `Binbuf.Preview3D.USD.1` ProgID to the current NSIS
   capabilities/OpenWith/SupportedTypes/uninstall/reset paths without writing
   `UserChoice`. Do not register the USD thumbnail CLSID yet.
5. Package `Preview3DImportHost.exe` and its exact private OpenUSD DLL/resource
   tree in portable and installer outputs. Provision the separate host profile
   ACL only for that payload. Extend PE dependency allowlists, release-manifest
   hashing, tamper/missing-resource rejection, licenses, notices, and SBOM
   inputs; neither payload may read/execute the other's private directory.
6. Update root/viewer/installer/portable documentation with the exact static
   subset, compatibility-host behavior, local-only dependency policy, Tier-B
   limits, known omissions, and explicit absence of Explorer thumbnails.

### Exit criteria

- All four extensions open through every activation surface, including stages
  that stay fast and stages that deliberately fall back.
- Debug/Release solution, Unit, ImportIsolation, applicable app smoke, portable,
  and NSIS engineering package builds pass with no unresolved/unlisted import.
- Install/uninstall exposes one USD family ProgID without taking defaults and
  installs no thumbnail handler.

### Completion record

- The viewer now classifies `.usd`, `.usda`, `.usdc`, and `.usdz`
  case-insensitively across direct/secondary command-line activation, the
  picker, drop, retry/open-another, diagnostics, About text, and Open With.
  All four route to `ImportFormat::Usd`. `.usd` remains byte-sniffed, while the
  information panel reports the validated USDA, USDC, or USDZ source identity.
  The app supplies the private compatibility-host path only for USD and keeps
  rendering through the existing validated mesh/material/image/node/instance
  contract. USD deliberately does not set the Tier-A coarse/detail-service
  request flags; its adapters publish their own bounded normalized batches.
- The real-app USD smoke passes eleven checks in both Debug and Release:
  uppercase USDA direct launch, Unicode/space paths, `.usd` sniffing through
  secondary activation, USDC picker, USDZ drop, OpenUSD composition fallback,
  an optional-texture warning, prior-content retention after fast and
  compatibility-host failures, replacement in both producer directions, and
  immediate compatibility relaunch. The product bridge test opens one fast
  and one compatibility stage in both configurations. Focused USD-002 through
  USD-008 coverage passes 26 cases / 711 assertions in Debug and Release.
- ShellIntegration's ten-extension catalog is revision 5. NSIS registers the
  four extensions through the one stable `Binbuf.Preview3D.USD.1` ProgID in
  capabilities, `OpenWithProgids`, and `SupportedTypes`, with symmetric reset
  and uninstall cleanup. It does not write `UserChoice`, a CLSID, `shellex`, or
  any thumbnail registration.
- Portable and installer stages now contain a closed 26-file `OpenUsdHost/`
  tree: the bootstrap/core, required runtime DLLs/app-local CRT, and exactly 13
  audited OpenUSD resources. Packaging rejects missing/unlisted host files,
  validates every PE dependency, hashes the complete tree in `MANIFEST.json`,
  and records OpenUSD/oneTBB licenses and SBOM components. Installer ACL
  provisioning removes broad package grants and gives the worker and host SIDs
  read/execute only on their respective private trees; cross-grants are
  rejected. A staged portable composed stage opened successfully.
- Debug and Release solution builds pass. Unit passes 98/98 (7,625 Debug /
  7,537 Release assertions). Full Debug isolation reaches 279/283 with the
  four known FBX fixture-rewrite failures; Release reaches 271/283 with those
  four plus eight known randomized sidecar scratch-directory collisions. No
  full-run failure enters a USD route. The portable engineering payload has 63
  staged files and SHA-256
  `eaba718e74b2681fa6d9d14aa76d66f9a0496b511ea9818d9a46da5df27c484d`;
  the 64-file NSIS stage builds warning-free and the unsigned installer SHA-256
  is `b8a25b44f2d0d2e0daeac5ae59492862e8c38deaa1d2ad790f8ce0de4248f4f8`.
  Clean-VM registration/upgrade/uninstall, signed-candidate, fuzz, performance,
  and broader corpus qualification remain USD-009; Explorer thumbnails remain
  exclusively USD-010.

## USD-009: corpus, hardening, and release qualification

Status: in progress (2026-09-18); corpus and standalone fuzz-smoke lane complete, release-machine gates pending
Depends on: USD-008  
Unblocks: the viewer USD support claim and USD-010

### Objective

Produce Gate 4 evidence for the complete viewer family. Missing corpus, fuzz,
security, performance, resolver, package, or clean-machine evidence must be
implemented and rerun; this is not a documentation-only signoff.

### Work

1. Check in a redistribution-compatible corpus with immutable hashes and
   independent expectations for USDA/USDC parity, `.usd` sniffing, USDZ,
   hierarchy/instances/point instances, axes/units/large coordinates,
   primvars/material subsets, Preview Surface/textures, fast/compat overlap,
   each supported composition arc, payloads/variants, warnings, and every
   unsupported/unsafe/over-limit outcome.
2. Add standalone no-GPU fuzz targets for USDA/USDC object graphs, USDZ archive
   directory/expansion, TinyUSDZ normalization, constrained virtual dependency
   maps, OpenUSD resolver/control frames, and compatibility-host normalized
   output. Run sanitizer smoke where the pinned dependencies support it and
   minimize discovered failures into regressions.
3. Rerun the complete protocol/hostile-process suite against both executables,
   including mutated sections, stale generations, graph/reference attacks,
   dependency abuse, archive paths, plugin/DLL search, direct file/network,
   child process, commit/CPU limit, cancellation, crash, and timeout recovery.
4. Measure B-each and USD-compat median/p95 cold/warm import, first geometry,
   UI/present heartbeat, cancel observation, TinyUSDZ worker peak private
   commit, OpenUSD host peak/limit behavior, startup/payload cost, dependency
   counts, and archive expansion. Do not claim Tier-A multi-gigabyte targets.
5. Verify clean offline standard-user install, direct/dialog/drop/secondary
   activation, Open With/Default Apps, repair/upgrade/rollback/uninstall, loaded
   host replacement, payload ACL separation, tamper detection, no defaults
   change, and no surviving worker/host after close.
6. Audit pinned commits, licenses, notices, SBOM transitive components, release
   hashes, Authenticode when a candidate certificate exists, OpenUSD plugin and
   DLL closure, and absence of dependency payload in the wrong process.
7. Review persistent-cache impact. If cache writes remain deferred, record that
   no USD entry exists and reserve the combined TinyUSDZ/OpenUSD/options/schema
   importer-version tuple. If cache exists, prove either backend produces the
   same keyed normalized contract and that dependency changes invalidate it.
8. Update `.docs/TODO.md`, `.docs/PROGRESS.md`, public format documentation, and
   `.docs/USD-009-VERIFICATION.md` with exact commands, case/assertion counts,
   hashes, measurements, limitations, and unverified environmental gates. Mark
   Gate 4 USD slices complete only when all required evidence passes.

### Exit criteria

- Valid fixtures match independent geometry/scene/material expectations and
  fast/compat overlap is equivalent under the documented canonical comparison.
- Malformed, hostile, unsupported, and over-limit files fail safely; the next
  open succeeds without relaunching the viewer.
- Both import processes remain contained and package search cannot be redirected
  by a model, working directory, user environment, or adjacent unsigned file.
- Full Debug/Release automated, package, fuzz-smoke, performance, and clean-VM
  evidence is recorded, or the support claim remains disabled.

### Progress record

- The immutable [USD-009 corpus manifest](../tests/fixtures/usd/manifest.json)
  now records 10 redistributable sources and 13 reproducible malformed,
  unsafe, unsupported, recursion, archive, and dependency-limit derivations.
  Its verifier checks decoded USDC/USDZ bytes, not their base64 transport, and
  the existing real worker/host tests remain the independent numeric oracle.
- A standalone no-GPU `UsdFuzz` target covers TinyUSDZ USDA object graphs and
  normalization, USDC classification, product USDZ preflight, trusted normalized-output
  validation, the compatibility resolver's identifier policy, and OpenUSD
  control frames. A 60-second Release
  ASan/libFuzzer smoke completed 38,814 executions without a finding or timeout
  from a clean 11-seed corpus. Mutated USDC allocation pressure is instead
  contained and recovery-tested behind the real worker's Job limit.
- Debug/Release solution targets and Unit pass; focused USD-002 through USD-009
  passes 28 cases / 756 assertions in each configuration and the hostile-worker
  lane passes 13 cases / 273 assertions. No USD
  persistent-cache write exists; the reserved backend/options/schema tuple is
  recorded in [USD-009-VERIFICATION.md](USD-009-VERIFICATION.md).
- Repeated performance/heartbeat evidence, full-suite closure,
  final package/tamper audit, clean-VM lifecycle and signed-candidate evidence
  remain required. Exact commands, results, limits, and unverified gates are in
  [USD-009-VERIFICATION.md](USD-009-VERIFICATION.md); Gate 4 is not yet marked
  complete.

## USD-010: Explorer USD thumbnail adapter

Status: blocked on the general Gate 6 thumbnail-provider foundation  
Depends on: USD-009 and a working bounded COM thumbnail provider  
Completes: original-MVP USD-family support

### Objective

Add stream-only USD-family thumbnails through TinyUSDZ after the shared COM,
sampler, CPU rasterizer, deadline, and actual-surrogate infrastructure exists.
OpenUSD and the compatibility host are never part of this path.

### Work

1. Implement the fixed USD CLSID
   `{E938BC70-4C08-4446-A15D-EE31576BFB48}` for `.usd`, `.usda`, `.usdc`, and
   `.usdz` through `IInitializeWithStream`/`IThumbnailProvider`.
2. Enforce the provider contract: 256 MiB stream maximum, 128 MiB contiguous
   backing maximum where required, 192 MiB scratch, bounded inspected/rasterized
   geometry and decoded pixels, 750 ms target, and 2 s hard cutoff.
3. Permit only stream-contained USDA/USDC and contained USDZ layers/textures.
   `.usd` is byte-sniffed. External references, payloads, sublayers, textures,
   and compatibility-only composition return a safe generic-icon fallback; no
   path recovery, broker IPC, host launch, cache read, or network access occurs.
4. Apply the same static scene/material policy within provider limits and feed
   finite sampled triangles to the shared deterministic CPU rasterizer. Never
   create a GPU device or claim success for partial required geometry.
5. Register the fixed CLSID without `DisableProcessIsolation`, following
   non-clobber conflict/repair/uninstall rules. Include the provider-local pinned
   TinyUSDZ dependency in license/SBOM/signature/package checks.
6. Add COM/provider tests for every extension/encoding, USDZ contained assets,
   unsupported composition fallback, malformed/oversized/archive/deadline/OOM,
   perceptual goldens at 32/64/256/512 px, parallel apartments, unload/leaks,
   and actual Explorer surrogate hosting.

### Exit criteria

- Stream-contained supported files produce deterministic thumbnails; every
  sidecar-dependent, compatibility-only, malformed, or over-budget file safely
  returns a null bitmap/error so Explorer uses its generic icon.
- The CLSID loads in the isolated thumbnail surrogate, not `explorer.exe`, and
  no registration disables Shell isolation.
- Viewer USD behavior is independent of thumbnail failure.

## Completion boundaries

- **Gate 4 USD viewer slices complete:** USD-001 through USD-009 are complete.
  All four extensions open through the dual-path viewer and installed product,
  with recorded Tier-B/security/package evidence.
- **Original-MVP USD family complete:** USD-010 is also complete after the
  general thumbnail-provider foundation. Until then, documentation must state
  that Explorer thumbnails are unavailable.
- **Not implied by either boundary:** animation playback, arbitrary OpenUSD
  schemas/plugins, remote/studio resolvers, full MaterialX/Hydra fidelity,
  interactive variants, authoring/edit/export, or unbounded composition.

## Cross-task rules

1. Never parse/decode USD or initialize OpenUSD in `Preview3D.exe`.
   TinyUSDZ belongs only in the general worker (and later the isolated Shell
   thumbnail DLL); OpenUSD belongs only in the compatibility host.
2. Neither import process receives a source path or directory handle. Every
   dependency is an opaque, bounded broker request resolved from a trusted
   canonical handle under the primary model directory or package map.
3. Only exact `UnsupportedComposition` from otherwise valid fast-path input may
   invoke OpenUSD. Malformed, unsafe, archive-limit, resource-limit, and parser
   failures are terminal for that generation.
4. Fallback is atomic. Never combine, retain, cache, upload, or display
   TinyUSDZ candidate chunks once the compatibility route is selected.
5. Preserve copy-then-validate. No viewer, cache, upload, or renderer code may
   retain or reread a child-writable shared section.
6. Check all source counts, byte arithmetic, graph/archive expansion, and
   allocations before use. Parser callbacks and Job Objects are independent
   layers, not substitutes for product limits.
7. Lock DLL/plugin/resolver discovery before OpenUSD initializes. A release
   manifest and absolute private payload are authoritative; working directory,
   model directory, `PATH`, and OpenUSD plugin environment variables are not.
8. Do not expose or register a USD extension until both fast and compatibility
   paths and their recovery/security tests pass through USD-007.
9. Any wire-layout change bumps the protocol and updates static assertions,
   fixtures, fuzz seeds, unknown-version rejection, and hostile-worker tests in
   the same task.
10. Existing source-format behavior and user changes remain intact. Extract
    tested format-neutral helpers instead of broadly rewriting working adapters.
11. Every committed fixture records provenance, redistribution permission,
    immutable hash, and independent expected facts.
12. A failed spike changes the design/ADR and this plan before implementation;
    it never justifies weakening isolation, path policy, memory, cancellation,
    or support wording.

## Upstream references for the spikes

- [TinyUSDZ repository and build/features](https://github.com/lighttransport/tinyusdz)
- [OpenUSD build configuration](https://github.com/PixarAnimationStudios/OpenUSD/blob/dev/BUILDING.md)
- [OpenUSD `UsdStage` loading API](https://openusd.org/release/api/class_usd_stage.html)
- [OpenUSD asset resolution](https://openusd.org/release/wp_ar2.html)
- [OpenUSD USDZ specification](https://openusd.org/files/USDZFileFormatSpecification.pdf)

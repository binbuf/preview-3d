# 3MF post-MVP work plan

Status: proposal  
Prepared: 2026-09-18  
Design authority: [design/README.md](design/README.md)

## Decision

Implement the original MVP's bounded, static 3MF preview subset as a sequence
of independently verifiable tasks. In this document, "3MF support" means the
subset already promised by the design:

- 3MF Core geometry, components, build items, transforms, units, and base
  materials;
- Materials and Properties colors, textures, composite preview colors, and the
  representable display-property subset;
- Production Extension model parts and cross-part object references;
- a bounded Beam Lattice preview;
- the existing Tier-B isolation, memory, cancellation, progressive delivery,
  renderer, activation, installer, and eventual thumbnail requirements.

It does **not** mean every 3MF extension. Slice, Secure Content, Volumetric,
Implicit, toolpath interpretation, printer/process settings, repair, slicing,
editing, or export remain outside the original MVP contract. A required
unsupported extension fails with `UnsupportedRequiredFeature`; optional data
may be ignored only when the standard build still has a coherent preview, and
then produces a bounded warning. Product and release documentation must call
this the "supported static 3MF preview subset," not unqualified "full 3MF."

## Build items are not slicer plates

The standard concepts need to remain distinct:

| Concept | Standard meaning | Viewer behavior |
| --- | --- | --- |
| Object resource | Reusable mesh or component definition; it need not be manufactured | Import only when reachable from the root build |
| Build item | An occurrence selected by the root model's single `<build>` section, with an optional transform | Emit a normal scene instance and display all build items together |
| Production model part | Another `.model` stream inside the same package, used to split a large or modular build | Resolve through package relationships; it does not create another plate |
| Slicer plate | A vendor/project grouping of instances for separate print beds | Not a Core or Production concept; support only through an explicit compatibility feature |

The Core specification defines one build section containing the items to
manufacture. The Production Extension can distribute resources among multiple
model parts, but explicitly says that only the root model's build is valid.
Consequently, a standards-conforming file can contain many objects and many
build-item occurrences without containing multiple plates.

Bambu Studio's multi-plate project information is carried in additional
package content such as `Metadata/model_settings.config`; its open-source
reader defines private `plate`, `plater_id`, `plater_name`, and instance
association fields. That is useful interoperability evidence, but it is not a
reason to reinterpret every standard build item as a plate.

### UX recommendation

For the baseline 3MF work:

1. Show every root-build item at the same time in its authored coordinate
   space, exactly as other multi-instance scenes are shown.
2. Fit/Reset, ground-axis controls, view mode, lighting, Info, Share, and Open
   With continue to operate on the document. There are no edit commands that
   need an "active plate."
3. Do not add a plate selector, carousel, synthetic bed spacing, or cycling
   command. Those controls would be meaningless for ordinary standard 3MF and
   would make vendor-private grouping look normative.
4. Do not expand the existing boolean whole-document selection as part of the
   parser task. The renderer already writes stable instance IDs into its pick
   target, but the app currently collapses the result to hit/no-hit and Fit
   still uses document bounds. Instance or group selection should be completed
   once as a format-neutral viewer feature if it is pursued.

An optional slicer-project compatibility task is proposed at the end. If it is
approved later, adapt the useful part of Bambu Studio's interaction: show all
recognized plates in one view, select one by clicking it, and provide
keyboard-accessible Next/Previous Plate actions. Cycling should be an
accessibility/fallback path, not the only way to discover the other plates.

## Existing foundation and gaps

The repository is substantially closer to this slice than the original design
documents imply:

- the reusable zero-capability AppContainer worker, pooled lifecycle, brokered
  source handle, cancellation event, bounded shared-section batches,
  copy-then-validate host path, and Tier-B limits are implemented;
- protocol v10 already carries validated nodes, reusable geometry,
  mesh-instance occurrences, double-precision transforms, materials, images,
  verified instance bounds, and stable picking IDs;
- OBJ/FBX established component/instance normalization and material/texture
  paths; USDZ established package preflight and contained-asset handling;
- command-line, dialog, drag/drop, secondary activation, Open With, portable
  packaging, and NSIS registration are working for the formats already shipped.

The remaining 3MF-specific gaps are material:

- `lib3mf` is not pinned, built, licensed, or linked;
- there is no 3MF opcode, request, source-format identity, dispatch route, or
  adapter;
- the existing `UsdZipPreflight` is deliberately USDZ-specific: it accepts
  stored entries only, requires 64-byte data alignment, and rejects ZIP64.
  Core 3MF permits stored or Deflate entries and requires consumers to handle
  ZIP64 and the ZIP streaming extension, so that implementation must not be
  renamed and reused unchanged;
- no 3MF corpus, fuzz target, format UI, package registration, or release
  evidence exists;
- the Explorer thumbnail provider is still only a later shared foundation.

## Non-negotiable implementation rules

1. `lib3mf`, ZIP/XML handling, image decode, lattice tessellation, and all
   normalization run only in `Preview3DImportWorker.exe`. The trusted viewer
   never links a 3MF, ZIP, or XML parser.
2. The worker receives the already-opened primary file handle, not a path. Use
   `lib3mf`'s read/seek callback API over the duplicated read-only handle; never
   call its filename API and never extract package entries to disk.
3. All 3MF dependencies are package parts. A 3MF adapter does not request local
   sidecars and cannot turn an OPC target into filesystem or network authority.
4. Product-owned package preflight runs before the general model load. It owns
   archive/path/count/expanded-byte/required-extension policy even when
   `lib3mf` also validates the same file.
5. Library success is not acceptance. Recheck graph depth/cycles, counts,
   indices, finite values, transforms, property references, normalized growth,
   and terminal catalog invariants before emitting a chunk.
6. Reuse protocol-v10 nodes and mesh instances; do not flatten component or
   build-item occurrences into duplicate geometry merely for convenience.
7. The host copies and validates every returned byte before upload. No library
   object, pointer, callback-owned buffer, or shared-section view crosses into
   Graphics or UI.
8. If a protocol layout changes, bump the protocol, reject older versions, and
   update static assertions, fixtures, fuzz seeds, broker validation, and the
   hostile-worker suite together. Appending closed enum/opcode values alone
   should not force a layout change.
9. Do not advertise `.3mf` in any user-visible or installer surface until the
   Core, Production, Materials, and bounded lattice policies are implemented
   and their recovery/security tests pass.

## Execution order

| Order | Task | Outcome | Depends on |
| ---: | --- | --- | --- |
| 1 | 3MF-001 | Prove and pin the `lib3mf` dependency, memory, cancellation, callback-I/O, and lattice approach | Current main branch |
| 2 | 3MF-002 | Add safe OPC/ZIP preflight and the closed 3MF protocol route | 3MF-001 decisions |
| 3 | 3MF-003 | Import Core and Production geometry, components, and all root build items | 3MF-002 |
| 4 | 3MF-004 | Normalize Materials and Properties plus contained textures | 3MF-003 |
| 5 | 3MF-005 | Add bounded Beam Lattice preview | 3MF-003 and 3MF-004 |
| 6 | 3MF-006 | Enable viewer, activation, Open With, installer, package, and documentation surfaces | 3MF-003 through 3MF-005 |
| 7 | 3MF-007 | Complete corpus, fuzz, security, performance, clean-machine, and signed-release qualification | 3MF-006 |
| 8 | 3MF-008 | Add the original-MVP Explorer thumbnail path | 3MF-007 and the general Gate 6 provider foundation |

The tasks are sequential at their boundaries. Focused tests and documentation
land with each task; qualification is not a substitute for tests omitted from
the implementing task.

## 3MF-001 — dependency and feasibility spike

Status: complete (2026-09-18). Proceed to 3MF-002 under the decisions and
limits recorded in [3MF-001-SPIKE-RESULTS.md](3MF-001-SPIKE-RESULTS.md).

### Objective

Resolve the unknowns called out by Gate 4 and ADR risk R-02 before a production
adapter or public extension claim is added.

### Work

1. Evaluate the current stable `lib3mf` release (2.5.0 is the candidate when
   this plan was prepared), select an exact source revision/release, and pin it
   through the repository's vcpkg policy. Record source hash, enabled features,
   transitive `libzip`/zlib surface, license texts, and static-versus-app-local
   runtime decision. Experimental/alpha builds are not release inputs.
2. Build only the reader functionality the worker needs. Disable writers,
   language bindings, examples, tests, crypto/Secure Content, and unrelated
   extension code where upstream options allow it. Verify that no `lib3mf` or
   transitive DLL is loaded by `Preview3D.exe`.
3. Create a spike worker path that reads from the inherited handle using
   `ReadFromCallback` with checked read/seek callbacks. Exercise sparse seeks,
   short reads, source-change behavior, cancellation, and files near the Tier-B
   primary-source limit. Do not use `ReadFromFile` even in the spike.
4. Determine exactly what the reader progress callback can cancel and how
   promptly. Measure cancellation during central-directory work, Deflate/XML
   load, model construction, large mesh extraction, and Beam Lattice access.
   Where a library call is not cooperatively cancellable, retain the existing
   short grace period followed by kill-on-close Job termination and worker
   replacement; document the measured boundary rather than claiming callback
   cancellation covers it.
5. Measure peak private commit and elapsed time for Core meshes, deeply nested
   components, Production multi-part packages, Materials/texture resources,
   and large beam/ball lattices. Confirm which allocations can be bounded by
   callbacks, which can only be bounded between phases, and which rely on the
   Job Object as the hard backstop.
6. Compare three Beam Lattice preview strategies: a valid authored
   `representationmesh`, product tessellation into bounded chunks, and reusable
   cylinder/sphere instance templates where they do not change frustum/cap
   semantics. Select the bounded strategy and record triangle/error behavior.
7. Map library exceptions/error codes and warnings into the existing closed
   product taxonomy. No third-party diagnostic string may cross IPC or become
   default UI/clipboard text.
8. Save results in `.docs/3MF-001-SPIKE-RESULTS.md`, including toolchain,
   fixture hashes/provenance, memory/time tables, cancellation observations,
   dependency decision, rejected alternatives, and go/no-go criteria for
   3MF-002.

### Exit criteria

- A Core file and a Production multi-part file load through callbacks inside
  the real AppContainer worker.
- A lattice fixture produces a bounded recognizable preview without an
  allocation or triangle count proportional to unchecked source claims.
- Job-limit, timeout, cancellation, malformed archive, and worker-replacement
  tests leave a later valid import usable.
- The selected pin builds Debug/Release with the repository warning/security
  policy and has an approved package/license plan.
- A failed result changes the design and this plan; it does not relax process,
  path, memory, or cancellation invariants.

## 3MF-002 — OPC preflight and protocol route

### Objective

Create the bounded package boundary and format-neutral control path that the
real adapter will use, without yet enabling `.3mf` to users.

### Work

1. Add a product-owned 3MF/OPC preflight component. It must parse checked local
   and central headers, ZIP64 records, streaming/data-descriptor forms, and OPC
   part/relationship names without extracting to disk. Permit only stored and
   Deflate methods; reject encryption, multi-disk archives, ambiguous or
   overlapping records, duplicate canonical part names, traversal, absolute
   filesystem syntax, invalid UTF, excessive path depth, and inconsistent
   header/directory metadata.
2. Enforce explicit entry-count, relationship-count, model-part-count,
   per-entry expanded-size, aggregate 4 GiB/200:1 expansion, compressed-byte,
   and elapsed-work limits before general model construction. Checked addition
   and multiplication are mandatory. The 2 GiB primary source and the worker
   Job/scratch limits remain independent caps.
3. Resolve `[Content_Types].xml`, root relationships, the single StartPart,
   model relationships, Production paths, and contained texture parts under
   OPC URI rules. Targets remain package identifiers, never broker sidecar
   requests. Reject external relationship targets for any resource needed by
   the preview.
4. Perform a bounded root-model header/namespace scan sufficient to enforce
   `requiredextensions` and `recommendedextensions` before `lib3mf` builds the
   model. Support only the Core, Materials and Properties, Production, and Beam
   Lattice namespace/version allowlist approved by 3MF-001. Unknown required
   prefixes fail; unsupported recommended/optional data produces a fixed
   warning only if omission leaves valid build geometry. DTD/entity content is
   rejected.
5. Append `ThreeMf` to `SourceFormatId` and `ImportFormat`; add a dedicated
   `StartThreeMfImportFromFile` opcode and 48-byte `ParseThreeMfFileRequest`;
   wire one-shot and pooled dispatch, request-size validation, source-format
   matching, Tier-B broker checks, cancellation, and recovery. Keep protocol
   v10 if no fixed layout changes.
6. Update `SharedSectionValidator`, `ImportSession`, source-format text helpers,
   unit tests, protocol property tests, unknown-enum/opcode cases, and hostile
   worker cases. A 3MF generation may return only `SourceFormatId::ThreeMf`.
7. Add an adapter cache/importer-version component for the eventual derived
   cache even though persistent cache production remains unfinished.

### Verification

- Valid stored, Deflate, ZIP64, and streaming-extension packages pass; the same
  model normalizes identically across legal container encodings.
- Bomb ratios, aggregate expansion, entry/model/relationship counts, encrypted
  or multi-disk input, duplicate/case-colliding names, percent-encoded
  traversal, backslashes/drive/UNC syntax, overlapping records, bad offsets,
  truncated descriptors, CRC/read failures, DTDs, and required unknown
  extensions produce the expected typed error before unbounded allocation.
- The worker cannot turn a relationship target into a file, URL, socket, or
  sidecar request.
- Unknown opcode/size/format, stale generation, replayed batch, mutated
  section, crash, timeout, and cancellation tests remain green in Debug and
  Release.

## 3MF-003 — Core and Production scene adapter

Status: complete (2026-09-18; private worker route only)

### Objective

Normalize the standard root build into the existing scene/instance contract,
preserving reuse and authored placement.

### Work

1. Add `ThreeMfAdapter` and `ThreeMfImportWorker` behind the 3MF-002 request.
   Enable strict library validation, then independently validate every value
   consumed by normalization.
2. Enumerate only root-build items. An empty build or a build with no reachable
   supported geometry returns `NoSupportedGeometry`; unreferenced resource
   objects are not displayed merely because they exist in the package.
3. Resolve Core component graphs and Production cross-part references with
   checked resource identity, a 256-level combined hierarchy cap, explicit
   cycle detection, finite/nonsingular transforms, and the 50,000 Tier-B
   object/node limit. Only the root model's build is authoritative; child-model
   build sections are ignored as the Production specification requires.
4. Emit each unique mesh resource once as reusable geometry when practical.
   Emit nodes and `MeshInstancePayload` occurrences for components and build
   items, composing transforms in double precision and retaining stable IDs.
   IDs should derive deterministically from package-part/resource/occurrence
   order but expose no source names or paths to diagnostics.
5. Accept build-reachable `model`, `support`, `solidsupport`, and `surface`
   triangle resources under the same geometry limits. Reject an illegal
   build-reachable `other` object. Do not add a hidden-support policy or new
   visibility control in the format adapter.
6. Validate vertices, triangle indices, properties, finite coordinates, and
   transformed bounds before emission. Generate normals through the existing
   bounded normal policy; split logical meshes into 4–16 MiB / at most 262,144
   triangle chunks without losing geometry, build-item, object, or material
   identity.
7. Map all six Core units to `metersPerUnit` and report the mandated +Z up axis.
   Keep source coordinates and transforms intact; the existing viewer root
   transform handles display orientation.
8. Feed scan/coarse/fine output through `ChunkBatchSink`, the Tier-B LOD/proxy
   path, generation catalog, and backpressure rules. Geometry may publish in
   bounded batches after `lib3mf` model construction, but Ready waits for the
   complete validated build, verified bounds, dependencies, and uploads.

### Verification

- Fixtures cover one mesh, several build items, repeated instances, nested
  components, every unit, reflected transforms, Production child model parts,
  surface/support types, and unused resources. Expected normalized snapshots
  assert hierarchy, occurrence count, transform, bounds, and geometry sharing.
- Negative fixtures cover missing resources/parts, illegal child-build use,
  bad indices, non-finite data, singular transforms, recursion/cycles, depth,
  object/vertex/triangle limits, empty build, and cancellation between batches.
- Rendering all build items together matches their authored relative
  placement. No cycling or synthetic plate offset is introduced.
- A replacement open preserves the prior usable model until a 3MF proxy is
  ready; failure or cancellation cannot publish an incomplete Ready scene.

## 3MF-004 — Materials, properties, and contained textures

Status: complete (2026-09-18). Proceed to 3MF-005; the format remains private
until 3MF-006.

The worker normalizes Core base materials, Materials color groups, composite
display colors, the bounded representable multi-properties subset, and
contained PNG/JPEG texture groups. It carries object and triangle/corner
properties through deindexed vertex colors/UVs, preserves sRGB vertex
interpolation and 3MF's lower-left UV convention explicitly, and emits bounded
image/material dependencies before geometry. The viewer material contract
represents independent tile modes, nearest filtering, and the alpha semantics
needed by a non-base texture layer.

Because pinned lib3mf 2.5 does not expose realistic display-property resources,
a product-owned, bounded, cancellable XML scan now extracts their associations
from every preflighted model part without creating filesystem authority.
Representable PB metallic values map directly; PB specular uses the documented
deterministic metallic/roughness preview conversion. Display-texture and
translucent groups retain their base appearance and emit one bounded warning.
The scan validates finite ranges, group/resource identity, cardinality, CRC,
expanded bytes, and cancellation before the adapter consumes an association.

Focused Debug/Release tests cover object and triangle defaults, per-corner
color/alpha, composites, multi-properties, texture decode/order/UV convention,
metallic and specular conversion, malformed numeric input, display cardinality,
unsupported translucent fallback, cancellation, and same-worker recovery. An
opt-in local-corpus test accepts `PREVIEW3D_MANUAL_3MF_DIR`; it was also run
against the three slicer files supplied under `test-models/` without adding
those local files to the repository.

### Objective

Map the supported 3MF appearance model into the renderer's existing material,
image, UV, and vertex-color contract with deterministic approximations where
the design permits them.

### Work

1. Support Core base materials and Materials Extension color groups at object
   and per-triangle/per-corner scope. Convert sRGB colors correctly, preserve
   alpha where the normalized renderer can represent it, and duplicate
   bounded vertices when property seams require different corner attributes.
2. Support `texture2d`/`texture2dgroup` PNG and JPEG package parts, UV
   coordinates, tile style, and nearest/linear intent. Read attachments as
   bounded package bytes, verify MIME by content, and pass them through the
   existing worker-only WIC/image policy. Never enumerate installed codecs.
3. Support composite-material preview by the specification's display-color
   mixing rule. Support multi-properties combinations that reduce to one base
   material plus one color and/or one texture layer using the documented
   `mix`/`multiply` behavior. Cache normalized property combinations under a
   bounded material-count map rather than creating one material per triangle
   without a cap.
4. Map representable PB metallic display properties to base color, metallic,
   roughness, and opacity. Define and test a deterministic preview conversion
   for the representable portion of PB specular. Volumetric attenuation,
   unsupported layered/translucent behavior, and other nonrepresentable
   optional display properties fall back with a warning; if required for a
   coherent appearance under a required extension, fail explicitly.
5. Apply object defaults and triangle `pid`/`p1`/`p2`/`p3` overrides exactly,
   including mixed property indices and per-corner interpolation. Invalid
   references/counts fail rather than selecting material zero.
6. Emit material/image dependencies before geometry that references them and
   preserve progressive batch/catalog ordering. Missing or corrupt optional
   texture data uses the existing neutral/checker fallback and bounded warning;
   required appearance data follows the extension policy from 3MF-002.

### Verification

- Golden fixtures cover base material, flat and interpolated colors, PNG/JPEG
  texture coordinates, wrap/mirror/clamp/none, nearest/linear, composite
  mixing, multi-property layering, object defaults, triangle overrides,
  metallic/specular preview, alpha, and two instances of shared geometry with
  different valid properties.
- Malformed property indices, cycles, cardinality mismatches, unsafe/missing
  parts, MIME mismatch, decompression/decoded-pixel limits, corrupt images,
  material explosion, and cancellation have typed, recoverable outcomes.
- Normalized snapshot and render-readback tests verify colors, UV orientation,
  material sharing, texture presence, warnings, and Info statistics in Debug
  and Release.

## 3MF-005 — bounded Beam Lattice preview

Status: complete (2026-09-18). Proceed to 3MF-006; the format remains private
until that task enables every product and distribution surface.

The worker now preserves Beam Lattice XML semantics that lib3mf 2.5 does not
expose (including lattice/beam/ball property references and beam sets), prefers
a validated authored representation mesh, and otherwise emits bounded tapered
frusta with exact butt/hemisphere/sphere end profiles plus explicit or inferred
balls. The normal high-quality radial policy has 1.92% maximum circle chord
error and degrades the complete lattice uniformly through fixed deterministic
levels to a 262,144-triangle per-lattice cap; it never keeps a source prefix.

The supported clipping subset is `inside` clipping by a closed, axis-aligned
8-vertex/12-triangle box with one uniform normalized appearance. Generated
surfaces are clipped and the cut loops are closed using that clipping-mesh
appearance. General/nonuniform `inside` clipping and all parametric `outside`
clipping require a valid authored `representationmesh`; otherwise they return
`UnsupportedRequiredFeature`. A representation or clipping mesh must be a
different, plain `model` mesh in the same model part and cannot itself contain
a lattice. The current normalized scene contract has only document-occurrence
instances, not nested reusable sub-mesh instances, so the narrow cylinder/ball
template shortcut cannot preserve both lattice-local and document-occurrence
transforms without multiplying instances. The bounded tessellation remains a
single reusable geometry definition per source mesh instead.

### Objective

Make beam/ball lattices recognizable without allowing compact parametric input
to expand into unbounded CPU or GPU geometry.

### Work

1. Validate lattice vertex/beam/ball/set counts, indices, radii, minimum
   lengths, cap modes, property references, clipping references, and finite
   values before calculating output sizes. Count generated triangles against
   the same 20 million Tier-B scene ceiling and a stricter per-lattice preview
   budget selected by 3MF-001.
2. Prefer a valid authored `representationmesh` for preview when present. It is
   explicitly intended for display and avoids needless parametric expansion;
   still validate that it satisfies the specification's reference rules and
   normal geometry budgets.
3. Otherwise tessellate cylinders/frusta and requested balls/caps in cancellable
   batches. Choose radial/ball subdivision from a bounded projected-error
   policy and the remaining triangle budget, with deterministic degradation so
   the complete lattice remains represented instead of truncating a source
   prefix.
4. Reuse instance templates for genuinely identical cylinders/balls where the
   current node/instance contract preserves exact transforms and materials.
   Tapered beams, clipping, or cap differences must not be approximated as an
   identical cylinder merely to gain instancing.
5. Implement the bounded clipping subset selected by the spike. If a lattice
   requires inside/outside clipping that cannot be honored and has no valid
   representation mesh, return `UnsupportedRequiredFeature`; do not display
   unclipped material as though it were faithful.
6. Carry beam/ball properties through 3MF-004's normalized material path and
   include lattice geometry in verified bounds, proxy construction, progress,
   cancellation, and cache/importer versioning.

### Verification

- Golden fixtures cover uniform and tapered beams, all cap modes, ball modes,
  per-element properties, sets, representation meshes, supported clipping,
  very thin/short beams, and lattices under several preview budgets.
- Adversarial fixtures cover invalid indices/radii, NaN/Inf, zero/overflow
  arithmetic, enormous compact counts, triangle-budget pressure, unsupported
  clipping, recursion through representation/clipping meshes, cancellation,
  timeout, and Job-limit termination.
- Image/readback and normalized-snapshot tests establish recognizable complete
  coverage, deterministic output, bounded error/triangle count, and no
  first-N/source-order bias.

## 3MF-006 — product and viewer integration

### Objective

Expose the completed subset consistently through every existing viewer and
distribution path, without inventing plate semantics.

### Work

1. Enable `.3mf` case-insensitively in initial command line, open dialog,
   drag/drop, secondary activation, retry, Open With discovery, and supported
   extension/error guidance. Add a dedicated picker filter.
2. Route the D3D12 bridge through `ImportFormat::ThreeMf`; report actual 3MF
   format, phase, fixed warning facts, units, verified dimensions, triangle/
   vertex/material/texture/node/build-occurrence counts where the existing
   metadata contract can represent them. Add new metadata only if it is useful
   across formats rather than a private plate field.
3. Render all standard build items simultaneously. Preserve stable occurrence
   pick IDs, but keep the current document-level selection/toolbar semantics.
   Fit frames the document until a separate format-neutral instance-selection
   task is approved and completed.
4. Add `Binbuf.Preview3D.ThreeMF.1` / `.3mf` Open With and Default Apps
   registration to the NSIS installer without changing the user's default.
   Update install/repair/upgrade/uninstall and handler-conflict tests.
5. Deploy the exact `lib3mf` and transitive runtime payload only with the worker
   as required; add license/notices, SBOM entries, release-manifest hashes,
   portable staging, installer staging/removal, and AppContainer payload ACLs.
6. Update README, portable/installer notes, About/help/supported-format text,
   `.docs/TODO.md`, `.docs/PROGRESS.md`, design drift found during the work, and
   public limitations. State explicitly that slicer-private multi-plate
   grouping/settings and Explorer thumbnails are not yet supported.

### Verification

- A valid 3MF opens through every activation path and recovers after malformed,
  over-limit, crash, timeout, cancel, and replacement cases without relaunch.
- Multiple build items appear together and maintain correct transforms through
  ground-axis changes, Fit/Reset, fullscreen, selection hit testing, wireframe,
  Info, resize, and loading-state transitions.
- Open With, registration, install/repair/upgrade/uninstall, portable package,
  dependency/license inventory, worker launch, and clean-machine smoke tests
  include `.3mf` and leave no orphan association or payload.
- No viewer process imports or loads `lib3mf`, libzip, or their parser code.

## 3MF-007 — qualification and hardening

### Objective

Produce the evidence required to call the Gate 4 3MF viewer slice complete.

### Work

1. Freeze a provenance-and-SHA-256 manifest containing applicable official 3MF
   Consortium Core, Materials, Production, and Beam Lattice conformance samples;
   product-authored focused fixtures; and representative files from major
   slicers. Do not silently regenerate expected outputs during tests.
2. Add malformed and adversarial corpora for ZIP/ZIP64/Deflate/OPC, XML and
   namespace handling, object/component/build graphs, Production parts,
   properties/textures, and lattices. Include Bambu/Orca/Prusa-style project
   packages to prove that private metadata is ignored safely while the
   standard build has the documented fallback result.
3. Add a standalone sanitizer/libFuzzer target for the product preflight and
   the narrow adapter boundary, plus bounded regression replay through the
   real AppContainer worker. Archive and parser findings must be fixed or the
   affected feature disabled before release.
4. Run the shared normalized-scene invariant suite, protocol/property tests,
   full hostile-worker suite, path/network/process denial, memory/time limit,
   worker-pool recovery, cancellation/open storm, delayed-batch, GPU validation,
   and device-pressure tests with 3MF enabled.
5. Record peak commit, first-geometry, total Ready time, cancellation latency,
   worker heartbeat, shared-section/output bytes, and UI/render/input latency
   for small/medium Core, textured, Production, and lattice fixtures. 3MF is
   Tier B: it does not inherit the multi-gigabyte first-geometry target, but it
   must honor the Tier-B caps and responsiveness requirements.
6. Complete static analysis, dependency vulnerability/license review,
   reproducible Debug/Release builds, 8-hour mixed-open soak, clean-VM install
   and portable checks, signed artifact/hash/SBOM inspection, and support
   limitations review.

### Exit criteria

- Every promised Core/Materials/Production/Beam fixture has deterministic
  normalized and rendered evidence; every unsupported required extension and
  every over-limit family has an asserted typed result.
- No known corpus item crashes, hangs, escapes the sandbox, bypasses
  copy-then-validate, produces an unbounded allocation, stalls UI/render, or
  prevents a later valid open.
- Signed Release evidence is linked to exact binaries and fixture hashes.
- Gate 4's 3MF **viewer slice** is complete. Explorer thumbnails remain open as
  3MF-008.

## 3MF-008 — Explorer thumbnail adapter

Status: blocked on the general Gate 6 thumbnail-provider foundation  
Depends on: 3MF-007 and a working bounded COM thumbnail provider  
Completes: original-MVP 3MF family support

### Work

1. Link the approved bounded 3MF reader only into the isolated thumbnail DLL,
   never the viewer. Read exclusively from `IInitializeWithStream`; launch no
   worker/process and perform no path, sidecar, network, cache, or persistent
   write.
2. Reuse the 3MF package/required-extension policy under provider ceilings:
   256 MiB source stream, 128 MiB aggregate expansion, 100:1 ratio, 192 MiB
   parser scratch, 384 MiB private commit, and bounded inspected/sampled
   geometry. Lower count/deadline limits are expected.
3. Sample the complete root build deterministically across build items and
   spatial regions. Include a bounded representation of supported lattice
   geometry and standard materials/colors; fail safely to the normal icon for
   content that requires unsupported data or exceeds the provider budget.
4. Add `.3mf` routing/CLSID/registration, COM harness, golden raster,
   malformed/parallel/unload soak, actual isolated-surrogate, GDI ownership,
   and install lifecycle tests.

Slicer-private plate metadata is not needed for a thumbnail. The thumbnail
represents the standard root build under its own limits.

## Optional follow-up — slicer-project plate compatibility

This is **not** part of 3MF-001 through 3MF-008 and should not block standard
3MF support. Approve it only after a versioned corpus establishes which vendor
schemas are common enough to maintain (initially likely Bambu Studio/OrcaSlicer
and any deliberately compatible PrusaSlicer metadata).

If approved:

1. Write a bounded, version-aware parser for the explicitly recognized private
   metadata parts after OPC preflight. Treat missing, unknown-version,
   contradictory, or malformed vendor metadata as an optional-feature warning
   and fall back to the standard root build; it must never invalidate otherwise
   valid Core geometry.
2. Define a format-neutral scene-group record mapping instance IDs to group ID,
   optional bounded display label, source bounds, and viewer-only layout
   transform. This is likely a wire-layout/protocol bump. Do not overload Node
   parentage or material IDs with plate identity.
3. Preserve source transforms separately from viewer-only plate spacing. Lay
   out all plates deterministically in a non-overlapping grid for overview, but
   make it visually clear that spacing is a viewer presentation and not authored
   manufacturing coordinates.
4. Complete application selection: retain the GPU-returned instance/group ID,
   compute selected group bounds, highlight the selected plate, make Fit target
   it, and show `Plate n of m`/recognized name. Keep ground axis, grid, view,
   lighting, Share, and Open With document-wide because this app does not edit
   plate contents.
5. Add mouse click plus keyboard/UIA Next Plate and Previous Plate actions.
   Clicking empty space clears the active group and Fit returns to all plates.
   A mouse-only selector is not acceptable.
6. Apply the selection/group feature consistently to other multi-instance
   formats where meaningful, or explicitly document why the vendor plate view
   is a compatibility presentation rather than a new general editing model.
7. Qualify supported vendor versions with immutable real-world fixtures,
   cross-slicer round trips, overlapping per-plate coordinates, deleted/reordered
   instances, duplicate IDs, huge plate counts, malicious private XML, and
   standard-only fallback behavior.

The preferred interaction is therefore **all plates visible plus direct
selection, with cycling as a keyboard fallback**. It should not be implemented
until the private format and the general selection contract are both explicit.

## Completion boundaries

- **Standard 3MF viewer support:** 3MF-001 through 3MF-007 are complete.
- **Original-MVP 3MF family complete:** 3MF-008 is also complete after the
  shared thumbnail-provider foundation exists.
- **Slicer-project plate compatibility:** separately approved optional follow-up;
  it is neither implied by Core/Production support nor required for the
  original MVP.

## Primary references

- [3MF Core Specification](https://github.com/3MFConsortium/spec_core/blob/master/3MF%20Core%20Specification.md)
- [3MF Materials and Properties Extension](https://github.com/3MFConsortium/spec_materials/blob/master/3MF%20Materials%20Extension.md)
- [3MF Production Extension](https://github.com/3MFConsortium/spec_production/blob/master/3MF%20Production%20Extension.md)
- [3MF Beam Lattice Extension](https://github.com/3MFConsortium/spec_beamlattice/blob/master/3MF%20Beam%20Lattice%20Extension.md)
- [`lib3mf` repository and SDK](https://github.com/3MFConsortium/lib3mf)
- [Bambu Studio 3MF implementation](https://github.com/bambulab/BambuStudio/blob/master/src/libslic3r/Format/bbs_3mf.cpp)

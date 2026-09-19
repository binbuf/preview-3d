# File formats and ingestion

## Purpose

This document defines what “supports a format” means, how untrusted files enter the process, and the normalized scene contract consumed by the renderer. It deliberately separates the large-file fast path from compatibility importers.

**Process boundary.** Every parser and decoder named in this document — fastgltf, the STL/PLY product parsers, ufbx, lib3mf, TinyUSDZ, the Draco decoder, the KTX/Basis transcoder, libwebp, and DirectXTex/WIC — executes only inside the zero-capability AppContainer `Preview3DImportWorker.exe` (for OpenUSD composition beyond the TinyUSDZ subset, `Preview3DImportHost.exe`; for the bounded static STEP/STP subset, `Preview3DStepHost.exe`), never inside the trusted `Preview3D.exe` process that owns the window, the D3D12 device, and the user's other open documents. Moving parsing to a background thread inside `Preview3D.exe`, as the current GLB-only vertical slice does, is a responsiveness measure only; it does not change the trust boundary and must not be treated as containment for a parser exploit. See [02-system-architecture.md](./02-system-architecture.md) and [ADR-014](./11-decisions-and-risks.md#adr-014-appcontainer-import-processes-are-the-parser-security-boundary-not-threads).

## Support matrix

| Input | Parser | MVP content | Deliberate limits | Performance tier |
| --- | --- | --- | --- | --- |
| .glb, .gltf | fastgltf + Draco + KTX/Basis | glTF 2.0 scenes, nodes, triangle meshes, PBR metallic/roughness and unlit materials, vertex colors, PNG/JPEG/WebP/KTX2 textures, `KHR_texture_transform`, `KHR_mesh_quantization`, `KHR_draco_mesh_compression`, `EXT_meshopt_compression`, `EXT_texture_webp` | No animation playback, skins, morph targets, lights, cameras, or advanced material lobes. External URIs must resolve to approved local sibling files. A single oversized compressed primitive/view may fail its decode-working-set limit. | A for ordinary/meshopt data; compressed primitives are bounded decode units |
| .stl | Product code | Binary and ASCII triangles; supplied normals or generated flat normals | No color/material dialect guarantee | A for binary; B for ASCII |
| .ply | Product code | ASCII and binary little/big-endian PLY mesh or point cloud; positions, normals, common UV/color properties, polygon faces triangulated on import | Unknown elements/properties are skipped within bounds; no volumetric, range-grid, material, or arbitrary custom-property visualization | A for supported binary layouts; B for ASCII |
| .obj with optional .mtl | ufbx OBJ loader | Faces, smoothing, normals, UVs, vertex colors where available, MTL colors and approved local texture maps | Triangulated on import; .mtl is never opened as a primary document | B |
| .fbx | ufbx | Binary/ASCII FBX, static hierarchy, instancing, polygon geometry, unified PBR materials, and a deterministic evaluated start pose including supported skin and blend deformers | No animation playback, geometry caches, dynamic constraints, NURBS/subdivision tessellation, cameras, or lights | B |
| .3mf | lib3mf | Core, Materials, Production, and bounded Beam Lattice preview subsets; components, build items, transforms, colors/materials, textures | Slice, Secure Content, Volumetric, and Implicit extensions are not supported; any unsupported required extension fails | B |
| .usd, .usda, .usdc, .usdz | TinyUSDZ fast path; OpenUSD compatibility host | Static meshes, transforms, instances/point instances, display color, bounded Preview Surface materials, packaged textures, and bounded local composition including sublayers, references, inherits/specializes, authored variant selections, and payloads | No animation playback, skeletal data, variants UI, MaterialX, remote assets, procedural schemas, or arbitrary renderer plug-ins | B |
| .step, .stp (private route until STEP-006) | OCCT in the dedicated STEP host | ISO 10303-21 clear-text AP203/AP214/AP242 product/assembly structure, B-rep and faceted geometry, occurrence transforms, units, representable colors/transparency | Self-contained documents only until STEP-005; no PMI/GD&T, editing, exact-kernel operations, or external STEP documents; `UpAxisId::Unknown` by design | B |

Tier A is the release-gated out-of-core path for the multi-gigabyte workload in [01-product-scope.md](./01-product-scope.md). Tier B formats are still MVP features, but their parser libraries may materialize more intermediate state and therefore have lower enforced limits. Draco primitives are independently bounded because their decoded topology cannot be demand-paged from the compressed bitstream. A format badge is shown when a file uses an ignored or approximated optional feature; unsupported required geometry/composition fails instead of silently presenting an apparently faithful model.

## Input boundary

All primary inputs are local, regular, non-empty files. The loader rejects:

- UNC paths, device paths, alternate data streams, URLs, and URI schemes other than local relative file references;
- reparse points that resolve outside the primary model's directory when following a sidecar reference;
- absolute sidecar paths and any normalized relative path that escapes the primary directory;
- files that change identity or size after opening;
- integer overflow, overlapping ranges where prohibited by the format, invalid UTF encoding, non-finite geometry, and counts that exceed the applicable budget.

Path authority lives only in the trusted process. `Preview3D.exe` canonicalizes a path with GetFinalPathNameByHandleW after opening it, holds the primary handle for the generation's lifetime with FILE_SHARE_READ only (preventing ordinary replacement, deletion, or writing while pointers are live), and opens/checks each sidecar independently. Security checks are performed on the final handle path, not only on user-provided text. `Preview3DImportWorker.exe`, `Preview3DImportHost.exe`, and `Preview3DStepHost.exe` never open a path themselves: the trusted process duplicates a read-only handle across the process boundary for the primary file and every approved local dependency, and a worker/host asset resolver may only request additional dependencies from the broker by relative reference, never by opening a path or URI itself. This is what makes "no network access, no path escape" an enforceable process-level property instead of a parser-callback convention: even a fully compromised worker holds no handle capable of opening an arbitrary file or socket.

No importer performs network access. Third-party parsers, wherever they run, allocate and resolve resources only through product callbacks that enforce the generation's cancellation token, byte budget, item-count limits, and path policy. OpenUSD internal allocation is additionally contained by the compatibility-host Job Object/commit budget; OCCT transfer and tessellation allocation is contained by the STEP-host Job Object/commit budget; the general-format parsers in `Preview3DImportWorker.exe` are contained the same way by its Job Object/commit budget. All import hosts receive no path authority: their custom resolvers request read-only dependencies from the viewer broker, which applies the same handle-based canonicalization before duplicating a handle or serving bytes.

## Mapped-file abstraction

Model Core exposes a read-only MappedFile and MappingLease abstraction, used inside `Preview3DImportWorker.exe`/`Preview3DImportHost.exe` against the handles the broker duplicated to them:

1. Open with CreateFileW using GENERIC_READ, FILE_SHARE_READ, and OPEN_EXISTING — or, for a duplicated handle received from the broker, reopen a mapping directly from that handle without a fresh CreateFileW call. Use a sequential access hint for STL, PLY, and packaged sequential scans, a random-access hint for glTF/USD offset graphs, and no hint when the access pattern is not yet known; the extension is only a cache hint, never a trust decision.
2. Query file size and identity with GetFileInformationByHandleEx and reject zero-length or non-regular inputs.
3. Create a PAGE_READONLY file mapping.
4. For a compact source, map the complete file. For very large sources and sidecars, map aligned windows on demand using the system allocation granularity.
5. Return a span of const bytes whose lifetime cannot exceed its MappingLease. No parser-owned raw pointer may survive a lease, and no such pointer or lease ever crosses the process boundary — only the normalized wire-format chunks defined below do.
6. Unmap windows only after every task using the lease has released it.

Mapping a file removes an eager full-file userspace copy; it does not make parsing allocation-free and does not make file bytes directly usable as GPU vertex data. The OS commits physical pages as they are touched. Normalized geometry, decompressed archives, decoded textures, acceleration data, upload staging, and GPU resources remain explicitly budgeted — and, for Tier A and Tier B alike, those budgets are now enforced against the import process's Job Object commit limit in addition to the in-process allocation callbacks.

Validated upcoming sequential ranges may be submitted to `PrefetchVirtualMemory` when profiling shows a benefit; correctness and responsiveness cannot depend on prefetch completion. For glTF BIN data, binary STL, and supported binary PLY layouts, adapters retain validated source offsets and decode bounded ranges into reusable scratch blocks. Fixed-width records and independent deindexed triangles may be normalized in parallel inside one such bounded block; cancellation and file-identity checks remain at block boundaries, and malformed STL blocks replay through the scalar compacting path so invalid-facet semantics do not change. Unknown fixed-width PLY properties advance the validated record stride without being converted. A parser that requires contiguous ownership receives a budgeted arena, never an unbounded vector sized from a file field.

## Wire format between the trusted process and an import process

Normalized output crosses the process boundary only as chunk descriptors over a bounded shared-memory section, per the copy-then-validate rule in [02-system-architecture.md](./02-system-architecture.md). The wire format is versioned and intentionally simple to validate:

- a fixed-width header (protocol version, generation ID, section length, chunk count, and a section-level checksum) at a fixed offset, read first and fully bounds-checked before any other field is trusted;
- one fixed-width descriptor per chunk (source/normalized range, topology, index/vertex counts, an explicit numeric vertex-layout ID drawn from a closed enumeration — never a raw stride/format the receiver must interpret unchecked —, LOD level, byte size, dependency IDs, and a per-chunk checksum), never a variable-length or self-describing record that the host would need to interpret before it can be bounds-checked;
- payload bytes placed at offsets the descriptor names, always validated against the section length and the chunk's declared byte size before the host will copy them;
- no embedded pointers, indices into host-process memory, or host-interpreted format strings anywhere in the section.

A new protocol version is a breaking change requiring updated fixtures, fuzz corpora, and an explicit compatibility decision — the host never attempts to interpret a section whose version it does not recognize. This wire format, not the shared-memory mechanism by itself, is what lets the host snapshot rule in [02-system-architecture.md](./02-system-architecture.md) be a cheap, bounded, fully-checkable copy rather than an open-ended deserialization of untrusted structure.

The viewer uses protocol v10: an 88-byte header, 160-byte chunk
descriptor, 40-byte scan-summary payload, and 168-byte detail request. A scan
summary retains counts, verified bounds, source provenance, layout and the
normalized-detail checksum without transferring a second copy of normalized
geometry. Protocol-v10 section and payload integrity uses XXH64 directly below
10 MiB and a split-invariant fixed-256-KiB-leaf XXH64 tree at or above 10 MiB;
this is an incidental-corruption check, not an authentication boundary. A
validated deindexed flag lets proxy sampling traverse identity-indexed triangle
ranges without repeatedly reading their index buffer. PLY descriptors include a
bounded fan offset within the first source face so a split polygon can be
re-decoded exactly. Detail replies must match the original verified scan's
geometry, provenance, checksum and metadata. They reuse pinned primary and
approved sidecar handles; new sidecar requests are forbidden during replay.
Viewer and worker binaries ship from the same build. Compatibility decision:
reject v9 and every other unknown version; there is no migration or mixed-
version fallback. Updated protocol fixtures and hostile-worker cases exercise
the v10 checksum, scan-summary, scene-record, flag and copy-then-validate paths.

Protocol v10 adds two fixed payloads. `NodePayload` is 144 bytes: a stable
nonzero ID, optional parent ID, closed visibility flags, and a finite
double-precision row-vector affine 4x4 local transform. `MeshInstancePayload`
is 80 bytes: a stable nonzero ID, node/geometry/material IDs, closed visibility
flags, and finite verified double-precision world bounds. IDs are unique in the
generation-wide chunk namespace. Node matrices must be nonsingular affine
matrices with an exact `{0,0,0,1}` final column; hierarchy depth is capped at
256. The host resolves the hierarchy, rejects cycles, and recomputes every
instance AABB from all eight corners of the referenced geometry's local bounds
and double origin before publishing it.

Dependency slots have closed meanings for scene records:

| Record | Dependency slots |
| --- | --- |
| Triangle/point geometry | Existing bounded association slots remain compatible; slot 0 may identify a legacy geometry material. Geometry marked `ReusableInstanceSource` is a resource template and is drawn only through instance records. |
| Material | Slots 0–3 are base-color, metallic/roughness, normal, and emissive image IDs; sparse slots are allowed and the populated count must be exact. |
| Node | Slot 0 is the optional parent Node ID; slots 1–3 are zero; count is 0 or 1. |
| Mesh instance | Slot 0 is required triangle/point geometry, slot 1 is an optional Material, slot 2 is required Node, and slot 3 is zero; count is 2 or 3. |

References within a section resolve against its copied descriptor table;
references to earlier progressive batches resolve only through the bounded
generation catalog. Scene references do not reach outside that catalog, and
the terminal generation check requires the accepted Node catalog to match the
declared node count. This makes cross-window edges strictly backward, so a
worker cannot hide a cycle or an unbounded late dependency graph.

## Import generations

Opening a file creates a monotonically increasing LoadGeneration with:

- a stop_source;
- source identity and mapping leases;
- byte, object-count, scratch-memory, archive-expansion, and elapsed-work budgets;
- a bounded event sink;
- provisional and verified bounds;
- a normalized chunk catalog;
- upload and residency state.

Opening another file requests stop on the prior generation and immediately makes its events stale. Consumers compare generation IDs before applying any event. Destruction occurs only after CPU tasks release leases and the direct/copy fence retirement points make associated GPU objects safe to release. Cancellation is cooperative at parser callbacks, scan blocks, triangulation batches, texture rows, simplification clusters, and upload chunks.

## Format pipelines

### glTF and GLB

fastgltf validates glTF 2.0 structure while the adapter owns URI resolution and data access. GLB headers, JSON/BIN ranges, buffer views, accessors, strides, sparse accessors, and component conversions are checked before use. `KHR_mesh_quantization`, `EXT_meshopt_compression`, `KHR_draco_mesh_compression`, `KHR_texture_basisu`, `EXT_texture_webp`, `KHR_texture_transform`, and `KHR_materials_unlit` are accepted when their required data passes validation. meshoptimizer, the pinned Draco decoder, KTX/Basis transcoder, and libwebp handle their corresponding payloads behind the same budgets. Unknown required extensions fail clearly. Unknown optional extensions are ignored only when core geometry remains valid and are reported in diagnostics.

Draco decode is scheduled per primitive inside the import worker and publishes nothing across the process boundary until the decoded attributes and indices pass count/range/finite validation there. A primitive whose decoded working set would exceed 512 MiB or 10 million triangles fails with `ResourceLimit`; the design does not claim out-of-core decoding within one Draco bitstream. Meshopt-compressed buffer views remain suitable for bounded range decode.

Accessor min/max values may produce provisional scene bounds quickly, but they are not trusted as verified bounds. A bounded position scan verifies them while geometry is normalized. If verified bounds materially differ and the user has not moved the camera, the app eases to the corrected frame. If the user has interacted, camera target and scale are corrected without overriding their orientation.

A .gltf may reference approved local `.bin`, `.png`, `.jpg/.jpeg`, `.webp`, and `.ktx2` siblings. Data URIs have encoded- and decoded-byte caps. Unsupported image MIME types use the material fallback only when the image is not required to recover valid geometry.

### STL

The binary adapter validates the 84-byte prefix and checked multiplication of the 50-byte facet count. Trailing bytes are tolerated and diagnosed. Facets are scanned in bounded ranges directly from the mapping. Degenerate/non-finite facets are dropped, normals are normalized when credible, and otherwise flat normals are generated.

The ASCII adapter uses a streaming tokenizer with fixed-size blocks, a token length cap, locale-independent number parsing, and no recursive grammar. It is Tier B because text parsing touches and converts the complete source.

### PLY

The product parser accepts PLY 1.0 ASCII, binary little-endian, and binary big-endian streams. The header has line, token, property, element, and byte limits. It must declare finite scalar `x`, `y`, and `z` vertex properties. Recognized optional properties include normals, common `u/v` or `s/t` texture-coordinate aliases, and RGB/RGBA colors in bounded integer or floating representations.

Face index lists are range-checked and triangulated with a bounded scratch polygon. Point-only input is valid and emits point clusters; mesh input emits triangles and may also retain unreferenced points only when the point budget permits. Unknown elements and properties are skipped using their declared checked scalar/list types. Unsupported scalar encodings, malformed lists, or an element whose byte extent cannot be proven fail before allocation.

Binary PLY uses format-aware sequential mapped windows. Before the complete scan, the adapter reads stratified validated record windows across the vertex/face ranges to build a source-order-independent representative proxy; every sampled record is fully bounds-checked, and verified bounds still require the complete scan. ASCII PLY is Tier B because tokenization converts the complete source.

### OBJ and MTL

ufbx imports OBJ geometry and, when present, the named MTL sidecar. The adapter supplies a constrained open callback so mtllib and texture references cannot escape the source directory. Polygons are triangulated; invalid faces are reported and skipped only when the remaining model is coherent. Relative indices and smoothing groups are supported.

Missing MTL files or textures do not fail valid geometry. They produce a warning and a neutral material. The Explorer thumbnail path does not resolve OBJ sidecars because IInitializeWithStream provides neither a trustworthy filesystem location nor an arbitrary-file capability.

### FBX

ufbx is configured for explicit allocation and temporary-memory caps, progress callbacks, cancellation, triangulation, and static scene evaluation. The normalized scene uses evaluated local/global transforms and preserves instances where deformation permits. If an animation stack exists, the preview pose is deterministically evaluated at the start time of the first authored stack; otherwise the file's default/rest evaluation is used. Supported skin clusters and blend-channel weights are baked into that static normalized pose. Animation curves are not retained and no playback is implied. Geometry caches, dynamic constraints, NURBS, and subdivision surfaces produce a warning or `UnsupportedRequiredFeature` when omitting them would remove required visible geometry.

ufbx's unified PBR material mapping feeds the normalized base-color/metallic/roughness/emissive/normal subset. Embedded and approved local texture blobs are subject to the same decoded-byte and MIME allowlist as external textures.

### 3MF

Before lib3mf reads the bounded OPC stream, product code preflights local and central ZIP records, OPC part/relationship names, archive entry paths, per-entry sizes, aggregate expansion, compression ratio, and the required-extension allowlist. The adapter then validates object/component recursion and build-item counts before allocating normalized resources. Components and all occurrences reachable from the root build use reusable geometry instances where possible; child-model build sections are not interpreted as additional plates. The allowlist covers Core, Materials and Properties, Production, and a bounded Beam Lattice preview subset. A lattice prefers its validated authored representation mesh; otherwise deterministic product tessellation emits the complete beam/ball set under a 262,144-triangle per-lattice ceiling. Parametric clipping is limited to `inside` clipping against a closed, axis-aligned box with uniform appearance; other clipping requires a valid authored representation mesh. Nonrepresentable optional display/translucent features preserve the supported base appearance with a bounded warning, while unsupported required extensions fail.

### USD and USDZ

TinyUSDZ runs inside `Preview3DImportWorker.exe` and first handles USDA, USDC, and USDZ files in the common static subset there. The fast subset is a self-contained root layer: every USD composition arc (including sublayers, references, payloads, inherits/specializes, variants, clips, and relocates) returns a distinct `UnsupportedComposition` result before candidate output is published. Direct image assets are dependencies rather than layer composition and may be read only as contained USDZ entries or broker-approved local bytes. Only `UnsupportedComposition` asks the broker to start the separate `Preview3DImportHost.exe` for the same generation; malformed data, unsafe references, archive violations, unsupported required schemas, and resource-limit failures do not receive a more permissive retry in either process. The exact static-time, purpose, visibility, topology/primvar, orientation/subdivision, instancing, material-subset, and omission policy is recorded in [USD-001-SPIKE-RESULTS.md](../USD-001-SPIKE-RESULTS.md) and is shared by both adapters.

TinyUSDZ 0.9.1 has no allocation, progress, or cancellation callback, and its memory-limit option is advisory. The adapter therefore preflights input/archive structure, checks product-owned counts and normalized-output growth between library phases, and relies on the import worker's Job Object commit ceiling as the hard allocation backstop. A cancellation observed outside a library call is cooperative; a call still running after the existing 500 ms grace causes the worker to be terminated and replaced. A successful parser return alone is never evidence that a stage belongs to the accepted subset.

The AppContainer compatibility host opens the stage through pinned OpenUSD with initial payload loading disabled. Its composition policy permits sublayers, references, inherits/specializes, and authored default variant selections; its brokered resolver admits only approved local asset dependencies. Discovery and payload loading proceed breadth-first under dependency-count, byte, depth, time, and memory limits. Payloads needed for the build/default stage are loaded incrementally. The host emits only the normalized static subset: meshes, transforms, instances/point instances, display color, and supported Preview Surface bindings. Animation, skeletal schemas, MaterialX, procedural schemas, renderer plug-ins, remote assets, and interactive variant selection remain outside MVP. USDZ archive entries use the same archive/path limits regardless of adapter.

## Normalized scene

Importer-specific objects never cross into Streaming or Graphics. Model Core produces immutable metadata plus chunk descriptors:

| Object | Required fields |
| --- | --- |
| SceneMetadata | generation, source format, source units/up axis, node/material/triangle counts, warnings, provisional/verified bounds |
| Node | stable ID, parent, double-precision local transform, visibility |
| MeshInstance | stable ID, node, reusable geometry, per-instance material, verified world bounds, visibility |
| Material | base color factor/texture, metallic, roughness, emissive, normal texture, UV transform, unlit, alpha mode/cutoff, double-sided flag |
| TextureSource | bounded encoded bytes or validated mapping range, MIME/container, mip metadata, color space, sampler |
| MeshCluster | mesh/node IDs, triangle-or-point topology, double-precision origin, local AABB/sphere, material, LOD descriptors |
| ChunkDescriptor | source/normalized range, topology, index/vertex counts, vertex layout, LOD, byte size, dependencies |

Vertices use cluster-local float positions relative to a double-precision cluster origin so large coordinates retain usable precision. Node transforms and bounds remain double precision on the CPU; the renderer produces camera-relative float transforms each frame. Normals and tangents use a tested packed signed format, UVs use half2 when representable and float2 otherwise, colors use normalized RGBA8 when lossless enough, and indices use 16 or 32 bits per chunk. Point clusters carry a bounded screen-space size policy rather than synthetic triangle expansion in CPU memory.

The normalizer deduplicates only within bounded clusters. It never builds a whole-scene hash table. Target detail chunks are 4–16 MiB of GPU payload and at most 262,144 triangles or 1,048,576 points. Logical primitives are split when needed while preserving material and instance identity.

## Bounds, proxy, and LOD

Verified bounds are reduced incrementally from finite normalized positions. A scene sphere is derived from the verified AABB and refined when economical. Empty or entirely invalid geometry fails with EmptyGeometry.

Each Tier A import creates:

- a coarse, always-resident proxy capped at the lesser of 2 million triangles/points, 5% of valid source primitives, and its reserved GPU budget;
- an intermediate target near 50% detail where meshoptimizer can simplify without unacceptable error;
- full-detail chunks.

The first proxy is produced from validated, stratified source ranges and early spatial clusters, then replaced by the complete coarse proxy. Sampling must not depend only on file order. It may appear before the entire source scan completes, but the UI labels it as loading until verified bounds and the full proxy catalog are ready. Sharp boundaries and material seams are protected; point clouds use deterministic spatial/reservoir sampling. If simplification fails a quality threshold, the cluster keeps its next coarser valid representation.

Tier B adapters feed the same normalized chunk contract and LOD builder when their intermediate representation fits the Tier B budget.

## Hard limits

Limits are checked before multiplication/allocation and are configurable only in developer builds. These are the in-process budgets enforced by parser callbacks or by product-owned preflight, phase-boundary, and normalized-output accounting inside `Preview3DImportWorker.exe`. They are backstopped by that process's Job Object commit ceiling, which Gate 2 measures and sets high enough to cover a worst-case combination of the scratch, texture, and archive-expansion rows below plus working-set overhead. Libraries with allocator callbacks use the in-process scratch limit as the first and cheaper line of defense. TinyUSDZ lacks such callbacks, so its Job ceiling is the only hard bound on allocations made inside a parser call; a Job failure remains a controlled current-document failure and replacement path rather than a reason to raise the process limit.

| Budget | Tier A viewer | Tier B viewer | Thumbnail host |
| --- | ---: | ---: | ---: |
| Primary source | 8 GiB | 2 GiB | 256 MiB stream |
| All local source/sidecar bytes | 12 GiB | 4 GiB | Stream only |
| Valid source triangles | 100 million | 20 million | 2 million inspected/sampled |
| Valid source points | 100 million | 20 million | 6 million inspected; 250,000 rasterized |
| Source vertices/accessor elements | 300 million | 60 million | 6 million sampled |
| Nodes/objects | 100,000 | 50,000 | 10,000 |
| Materials | 65,536 | 32,768 | 4,096 |
| Decoded texture pixels, aggregate | 1 gigapixel | 512 megapixels | 32 megapixels |
| Archive expansion (.3mf/.usdz) | N/A | 4 GiB and 200:1 ratio | 128 MiB and 100:1 ratio |
| Parser/normalizer live scratch | 1 GiB or 25% of physical RAM, whichever is lower | 1.5 GiB or 35%, whichever is lower | 192 MiB |
| OpenUSD compatibility-host private commit | N/A | 4 GiB or 35% of physical RAM, whichever is lower | N/A |
| One Draco primitive decoded working set | 512 MiB and 10 million triangles | 512 MiB and 10 million triangles | 96 MiB and 1 million triangles |
| Total provider private commit above host baseline | N/A | N/A | 384 MiB |

These are acceptance ceilings, not promises that all limit-sized scenes fit simultaneously. GPU detail is separately constrained by the live DXGI budget. Above-limit files receive ResourceLimit with the first exceeded named limit; the process remains usable.

## Texture policy

The texture layer accepts format/container combinations explicitly enabled by the source adapter:

- inbox WIC paths for PNG, JPEG, BMP, and TIFF;
- DirectXTex paths for validated TGA, HDR, and DDS;
- pinned libwebp for WebP;
- KTX-Software/Basis transcoding for KTX2 and `KHR_texture_basisu`.

It does not enumerate or invoke arbitrary installed WIC codecs. Encoded type is verified from bytes and declared MIME/container metadata rather than extension alone. Color textures are sRGB; data/normal textures remain linear. Dimensions, row pitch, mip/array/depth counts, multiplication, total pixels, and compressed input size are checked before decode or direct upload.

Validated DDS/KTX2 mip chains may upload directly when their DXGI formats are supported. Basis payloads transcode to a supported BC7/BC5/BC3/BC1 target selected by semantic and adapter capabilities, with RGBA8 fallback. Existing small mip levels are uploaded first. For ordinary raster images, decoder-native downscaling is requested when trustworthy and CPU mip generation otherwise happens in cancellable tiles. Tangents are generated only for visible geometry whose material actually requires tangent-space normal mapping. A missing, corrupt, or unsupported optional texture uses a deterministic checker/neutral fallback without hiding the mesh.

## Error taxonomy

Adapters return typed errors, not localized strings:

- FileUnavailable, FileChanged, UnsafeReference;
- UnsupportedVersion, UnsupportedRequiredFeature, UnsupportedEncoding;
- MalformedData, IntegerOverflow, ArchiveLimit;
- ResourceLimit, OutOfMemory, Cancelled;
- NoSupportedGeometry, TextureDecodeFailed;
- UnsupportedComposition, CompatibilityHostFailure, CompatibilityHostLimit;
- ImportWorkerFailure, ImportWorkerLimit, ImportProtocolViolation;
- InternalImporterFailure.

The application maps these to user text and preserves format, byte offset/object path where safe, and a correlation ID in diagnostic logs. Third-party exceptions never cross a module boundary.

## Verification

Every adapter requires valid, malformed, truncated, adversarial-count, cancellation, and limit fixtures. Tier A tests additionally assert bounded resident CPU memory, source-order-independent proxy representation, and that no source-sized heap allocation occurs. PLY fixtures cover ASCII and both binary endiannesses, meshes, point clouds, unknown properties, and hostile lists. glTF fixtures cover Draco/KTX2/WebP success and decoded-expansion limits. FBX fixtures verify deterministic static skin/blend evaluation. USD fixtures run both fast and compatibility paths and assert equivalent normalized output where their supported subsets overlap. The same normalized-scene invariant suite runs across all formats.

Every adapter's isolation, not only its parsing correctness, is verified: a synthetic hostile-worker build (deliberately mutating shared-section bytes after the host's first read, replaying stale generations, or lying about a chunk's declared layout ID) proves the copy-then-validate rule in [02-system-architecture.md](./02-system-architecture.md) actually rejects the mutation, rather than only proving that well-formed workers behave. This test exists once per wire-format version, not once per format, since the host's validator is format-agnostic — but it must be in place before any real parser adapter ships behind it, per Gate 2 in [10-delivery-plan.md](./10-delivery-plan.md).

Primary references: [Windows file mapping](https://learn.microsoft.com/windows/win32/memory/file-mapping), [fastgltf](https://github.com/spnda/fastgltf), [Google Draco](https://github.com/google/draco), [KTX-Software](https://github.com/KhronosGroup/KTX-Software), [ufbx](https://github.com/ufbx/ufbx), [lib3mf](https://github.com/3MFConsortium/lib3mf), [TinyUSDZ](https://github.com/lighttransport/tinyusdz), [OpenUSD](https://openusd.org/release/), [DirectXTex](https://github.com/microsoft/DirectXTex), and [meshoptimizer](https://github.com/zeux/meshoptimizer).

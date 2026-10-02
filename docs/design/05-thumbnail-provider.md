# Explorer thumbnail provider

## Scope

thumbnail-provider builds an in-process x64 COM DLL used by Windows Explorer to render model thumbnails. It shares validation, normalized math, material fallbacks, and bounded parsers with model-core, but it does not share the interactive renderer, open a D3D12 device, launch the viewer, start worker processes, or make network requests.

The provider implements:

- IInitializeWithStream to receive the Shell-managed content stream;
- IThumbnailProvider to produce a requested-size HBITMAP;
- IClassFactory plus DllGetClassObject and DllCanUnloadNow;
- explicit module/object/lock reference counts.

The DLL exports only `DllGetClassObject` and `DllCanUnloadNow`. It does **not** export
`DllRegisterServer`/`DllUnregisterServer`: registration is owned by the installer (ADR-0006, ADR-0007),
which is the only path that implements the non-clobber conflict/repair/uninstall rules. A regsvr32-style
self-registration export is deliberately omitted because it could write keys without that policy
(ADR-011 rejects self-registration as the vehicle).

The DLL has no registration side effects in DllMain. DllMain only records the module handle and disables unnecessary thread notifications.

## COM classes and extension assignment

Distinct CLSIDs let the factory select an adapter without sniffing every grammar. The values below are product identities and must not be regenerated.

| Family | Extensions | CLSID |
| --- | --- | --- |
| glTF | .glb, .gltf | {A592F425-EA68-4C88-BB96-020805D4BE56} |
| STL | .stl | {BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9} |
| PLY | .ply | {F4DC6119-E235-4BAC-8089-54EDD84F8492} |
| OBJ | .obj | {D4722752-C480-4D9C-BEBE-1A9B514A8846} |
| FBX | .fbx | {FBC218D4-FD2C-41DF-B168-7F3B9E53C84E} |
| 3MF | .3mf | {D8389A63-8526-454A-9892-72F3149484B9} |
| USD | .usd, .usda, .usdc, .usdz | {E938BC70-4C08-4446-A15D-EE31576BFB48} |
| STEP | .step, .stp | {6EE961AC-AC3B-4958-A898-E30523FEE79D} |

Each class is registered as an InprocServer32 with ThreadingModel=Apartment. T03 validates the
ShellEx association location with third-party defaults and per-user associations before T41 writes
the machine-level mapping (ADR-0006/0007); users remain in control of default open applications.

Registering a handler as InprocServer32 is necessary for Explorer to load it, but it is not what isolates it: by default the Shell loads thumbnail handlers into an isolated per-handler COM surrogate (normally `DllHost.exe`) rather than into `explorer.exe` itself, and that surrogate process boundary — not apartment threading, not the COM contract — is what contains a parser crash inside this DLL away from Explorer. (`Prevhost.exe` is a different, unrelated surrogate that Windows uses to host `IPreviewHandler` for the Preview pane; this product implements no `IPreviewHandler` and is never hosted by it — see [01-product-scope.md](./01-product-scope.md).) The installer, its registry entries, and any troubleshooting documentation MUST NOT set `DisableProcessIsolation=1` (or an equivalent per-handler opt-out) for any of the eight CLSIDs above, in the installer or in support guidance. A future change that enables in-process (`explorer.exe`-hosted) execution for performance reasons requires a new ADR, a revised threat model in [09-quality-performance-and-security.md](./09-quality-performance-and-security.md), and re-justifying every claim in this document that currently depends on Shell process isolation.

## Frozen specification

T01 freezes this contract as the single authority adapter work is scheduled against:

- **Roster.** The eight-family CLSID table above is final, including the STEP identity
  `{6EE961AC-AC3B-4958-A898-E30523FEE79D}`. The seven earlier identities are product
  identities and are never regenerated or renamed ([ADR-0001](adr/0001-eight-family-clsid-roster.md);
  [`adapters/step-009-thumbnail.md`](adapters/step-009-thumbnail.md)). `.mtl` remains a sidecar with no CLSID.
- **OCCT linkage.** The provider links a separately built, explicitly limited OCCT
  STEP/XDE/tessellation adapter into this DLL only, never into the viewer, general worker or
  either import host, and launches no process ([ADR-0002](adr/0002-occt-linked-into-thumbnail-adapter.md)).
- **Decoder scope.** The bounded decoders the provider links (Draco, meshoptimizer, KTX2/Basis,
  WebP) are those in [ADR-0003](adr/0003-provider-decoder-scope.md); this supersedes the
  scope-limited-MVP restriction recorded in [11-decisions-and-risks.md](./11-decisions-and-risks.md).
- **Registration vehicle.** Registration ships through the project's per-machine NSIS installer
  now, and the same component identities are adopted by the eventual MSI
  ([ADR-0006](adr/0006-registration-through-installer.md), [ADR-0007](adr/0007-provider-program-scope-and-installer.md)).
  The extension-level `ShellEx` location is provisional until T03 proves it with a third-party
  default ProgID and a per-user association.

The C++ adapter/sampler/rasterizer contracts and the exact shared `model-core`/parser source subset
the DLL compiles are frozen in [interfaces.md](./interfaces.md) (T04, ADR-0009); that document is the
signature-level companion to this one. The CLSID→family routing table lives once in
`thumbnail-provider/FamilyRouting.h` and is shared by runtime routing and installer registration
(T41).

T07 performed the source extraction: the format-agnostic STL/PLY parser primitives and the ASCII
tokenizer now live in `shared/parser-core/` and are compiled into both `Preview3DImportWorker.exe`
and `Preview3DThumbnailProvider.dll`, with no worker/broker/host/viewer header crossing the DLL
boundary ([ADR-0004](adr/0004-share-source-not-state.md),
[ADR-0012](adr/0012-provider-parser-core-extraction.md)). The worker's wire/streaming adapter shells
stay in `import-worker/`; T21–T34 add only their family parser core + `IFamilyAdapter`.

**Limit and error authority.** For the limits T06 encodes, the source of truth is the
*Thumbnail host* column of [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md)
together with the HRESULT table below. The accountable caps (256 MiB stream maximum, 128 MiB
contiguous backing, 192 MiB parser/normalizer scratch, per-component decode caps, and the
384 MiB product-owned allocation ledger) fail closed to the generic icon with the first exceeded
limit named. The **384 MiB total process private commit above the idle, loaded surrogate
baseline** is a measured release qualification target, not a hard ledger ceiling: it cannot be
enforced against library allocations without callbacks, DLL loading, or GDI/Shell-surrogate
overhead, and T51 measures the actual peak. The **2 s point is a cooperative stop check** at
bounded parser/sampler/raster intervals, not an interruptible wall-clock timeout for an opaque
third-party call.

The checked encoding of these limits and helpers is one source of truth owned by T06:
`thumbnail-provider/ProviderLimits.h` (the frozen constants and the file-derived checked
integer/range helpers), `thumbnail-provider/Deadline.h` (the monotonic 750 ms p95 / cooperative 2 s
deadline), `thumbnail-provider/AllocationLedger.h` (the process-wide product-owned allocation
ledger), and `thumbnail-provider/ProviderErrors.h` (the HRESULT mapping). `ProviderContracts.cpp`
compiles them under the provider's `/W4 /WX` policy, and `Tests.Unit.exe` covers every constant,
the aggregate/concurrent ledger, deadline arithmetic, overflow rejection and every mapping.

## Call contract

Initialize:

- accepts exactly one non-null IStream;
- takes an independent stream reference and rejects a second initialization;
- queries STATSTG when supported, but does not trust its size without checked reads;
- seeks only if the stream advertises it; adapters that need random access copy into the bounded backing store;
- never assumes a filesystem path or attempts to recover one.

GetThumbnail:

1. Treats `cx` as the caller's requested maximum physical-pixel dimension per [`IThumbnailProvider::GetThumbnail`](https://learn.microsoft.com/windows/win32/api/thumbcache/nf-thumbcache-ithumbnailprovider-getthumbnail) and rejects only the degenerate `cx == 0`; it never fails a call merely because `cx` exceeds today's common Explorer cache sizes, since the Shell's cached thumbnail sizes are documented as subject to change. The actual raster resolution is independently clamped regardless of `cx` (see CPU renderer below), so a larger future request costs no extra correctness risk.
2. Establishes a monotonic deadline: 750 ms p95 target and a 2 s cooperative stop point checked at
   bounded parser/sampler/raster intervals. A third-party library call on the Shell's calling thread
   cannot be interrupted mid-call. Count/byte preflight limits input size but does not prove an
   upper bound on that call's execution time or allocation. After an overrun returns, the provider
   fails to the generic icon and records the elapsed time. Each adapter must specify its in-call
   admission policy and measure the worst observed time on its adversarial and representative corpus.
   The 2 s value is not a guaranteed wall-clock timeout for arbitrary input.
3. Parses metadata and samples geometry within the thumbnail budgets in [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md).
4. Frames finite bounds, renders a deterministic CPU image, and returns a top-down 32-bit premultiplied BGRA DIB section.
5. Sets WTS_ALPHATYPE to WTSAT_ARGB.

Explorer owns the returned HBITMAP. The provider releases every other GDI object before returning. A failed call sets the output bitmap to null and returns a precise HRESULT.

## Stream ingestion

256 MiB is the maximum source data this provider will ever read or cache from the stream; it is not a product-wide maximum file size. A multi-gigabyte GLB or STL still opens normally in the full viewer ([03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md)) — it simply receives no Explorer thumbnail, and Explorer falls back to the generic file icon. When STATSTG reports a size over 256 MiB, the provider fails fast to that fallback rather than attempting a partial read, so an oversized file costs Explorer no more than a quick size check. The provider does not write a temporary or persistent file. Seek-capable streams under the budget are accessed through serialized, bounded range reads and a small block cache. If an adapter requires one contiguous buffer, the provider may create a checked in-process backing buffer up to 128 MiB; a larger input on that path returns the safe generic-icon fallback. Non-seekable inputs may use that same bounded backing buffer. Reads abort on deadline, limit, or short-read inconsistency.

The single implementation of `BoundedSource` is `thumbnail-provider/StreamSource.h`/`StreamSource.cpp`
(T12, [ADR-0014](adr/0014-bounded-stream-backing.md)): `IInitializeWithStream::Initialize` on the T11
`ProviderObject` adopts one non-null stream and rejects a second initialization; the source owns a
64 KiB × 8-slot block cache charged to the T06 ledger and materializes the 128 MiB contiguous backing
buffer on demand, also charged before allocation. `StreamSource.cpp` compiles without the provider
precompiled header so `Tests.Unit.exe` links the same source for its `[provider][stream]` coverage.

The per-component caps (128 MiB contiguous backing, 192 MiB parser/normalizer scratch, 32 MP decoded
texture, sampled geometry, raster targets) apply to allocations the provider can account for. A
process-wide ledger reserves product-owned allocations before they occur, including concurrent
GetThumbnail calls, and rejects a charge that would exceed 384 MiB. Allocator callbacks enforce
third-party allocations where available.

What the 384 MiB ledger does and does not charge:

- **Charged before allocation** (product-owned): the 64 KiB × 8-slot stream block cache and the
  128 MiB contiguous backing ([ADR-0014](adr/0014-bounded-stream-backing.md)); per-adapter parser
  and decoded-buffer scratch (for STEP, one checked reservation covering a full geometry cache plus
  one definition build, [ADR-0036](adr/0036-step-scratch-accounting.md)); the deterministic
  sampler's retained storage; the raster color/depth targets and the output bitmap; and any library
  allocation a decoder routes through an allocator callback.
- **Not charged** (library/OS-owned): library allocations without a callback (lib3mf, Draco, OCCT
  reader/mesher, fastgltf/TinyUSDZ internals), DLL/module loading, GDI/USER objects, and other
  Shell-surrogate overhead. The ledger therefore cannot enforce a hard ceiling on total process
  private commit.

The 384 MiB increase above an idle, loaded surrogate baseline is a measured release qualification
target. T51 records the actual peak process commit and any unaccounted excess (the measurement is
still open in this task set; link it here when it lands). A charge that would cross the 384 MiB
ledger ceiling fails closed to the generic icon with `LimitExceeded`. T06 defines accounting and
concurrent call behavior; T12/T14/T15 and each adapter charge all allocations they control. A
family/subset whose measured commit exceeds the target must be narrowed or disabled before a
release claims that budget.

There is no MapViewOfFile zero-copy guarantee for Shell IStream inputs. This is intentional: Shell isolation, bounded memory, and deterministic latency matter more than sharing the interactive viewer's source mapping implementation.

External dependencies are unavailable in the provider:

- .obj renders geometry without its MTL or texture sidecars;
- .gltf renders only when required geometry buffers are embedded as data URIs within limits; bounded Draco geometry and KTX2/WebP textures are allowed when their stricter provider decode budgets and deadline hold. A missing optional image uses the default material, while unavailable required geometry safely falls back to the generic icon;
- .ply and .stl are fully stream-contained and may use their bounded mesh/point sampling paths;
- .fbx may use bounded embedded geometry/textures;
- .3mf and .usdz may use contained package entries;
- .usd/.usda/.usdc do not resolve external references;
- .step/.stp are self-contained only: the bounded OCCT adapter decodes the bounded seekable stream under the provider ceilings, and external STEP documents (FILE_POPULATION/DOCUMENT_FILE) fall back to the generic icon (ADR-0002, ADR-017).

The full viewer retains the broader local-sidecar support defined in the format document.

## Geometry sampling

The provider must not normalize a multi-million-triangle model in full merely to draw a 256-pixel image. Each adapter enumerates triangles into a deterministic spatial/reservoir sampler:

- bounds are accumulated from finite vertices;
- at most 2 million input triangles or 6 million input points are inspected and at most 250,000 representative triangles/points enter rasterization;
- material boundaries and disconnected large components receive minimum representation;
- degenerate/non-finite triangles are discarded;
- a stable source-derived seed ensures Explorer cache consistency.

The inspect cap is a bound on how much geometry is read, not a licence to render a source-order
prefix. When a source exceeds the cap, a seek-capable stream uses declared/record geometry to place a
deterministic stratified sample across the whole extent (reservoir/spatial strata), so the result is
source-order independent and every nonempty region can still be represented. A non-seekable source
that exceeds the cap and has no trustworthy metadata from which to place strata returns the safe
generic-icon fallback instead of rendering a biased prefix. The "minimum representation" and
"source-order independent" guarantees therefore hold only within what the stream can actually be
sampled across, and the fallback covers the rest.

If trustworthy format metadata supplies bounds, it can guide sampling but is verified against sampled positions. For formats that cannot stream geometry safely under the limits, the provider stops and lets Explorer show its generic icon.

T14 implements this as `thumbnail-provider/DeterministicGeometrySampler.{h,cpp}` behind the
frozen `IGeometrySampler`: a deterministic min-hash priority reservoir (bottom-k) so a reordered
enumeration is byte-identical, plus one representative per occupied spatial cell and per material
so separated components and material boundaries survive the retained cap. Enumeration continues
to the inspect cap even when the retained reservoir is full, so the result is never a source
prefix. The single 250k sample budget is shared by triangles and points, retained storage is
charged to the T06 ledger before allocation, and the over-cap policy
(`GeometrySamplingPolicy.h`, `DecideGeometrySampling`/`StratifiedOffsets`) is the one place
adapters (T21–T34) consult before reading a source that exceeds the cap; an over-cap stream the
provider cannot position returns the safe fallback ([ADR-0016](adr/0016-deterministic-geometry-sampling.md)).

T23 implements the PLY adapter as `thumbnail-provider/PlyFamilyAdapter.{h,cpp}` (selected only by the
routed `Family::Ply` CLSID) and records its memory policy in
[ADR-0021](adr/0021-ply-adapter-stride-and-bounded-ascii.md). It parses ASCII and binary
little/big-endian 1.0 headers through `shared/parser-core`, requires finite scalar `x`/`y`/`z`,
carries optional `nx/ny/nz` and `red/green/blue[/alpha]` (or `r/g/b[/a]`) colors, and treats a `face`
element with an integer `vertex_indices`/`vertex_index` list as a fan-triangulated mesh and otherwise
as a point cloud. A binary mesh addresses each referenced vertex by its fixed record stride
(`vertexStart + index * stride`) with a small direct-mapped cache, so no source positions are
materialized; an ASCII mesh retains a bounded, ledger-charged vertex table. Per-face lists (≤255),
unknown list lengths (≤65 536), skipped elements (≤6 M records) and the vertex count (≤6 M) are
bounded, and every read polls the deadline. Out-of-range indices and non-finite positions drop the
affected triangle locally; a list-typed vertex element in a binary mesh falls back to the generic icon.

T25 implements the glTF/GLB adapter as `thumbnail-provider/GltfFamilyAdapter.{h,cpp}` (selected only
by the routed `Family::Gltf` CLSID) over the provider-local fastgltf static library, with the decoder
scope and external-access policy recorded in [ADR-0003](adr/0003-provider-decoder-scope.md) and
[ADR-0023](adr/0023-gltf-adapter-embedded-only.md). It parses with fastgltf `Options::None`, so only
the GLB BIN chunk and embedded `data:` URIs are resolved; any external `.bin`/image URI is rejected as
an unsafe reference and receives the generic icon without ever opening a path. Node instances from the
default scene (or the implicit roots) are traversed and each instance's double-precision world
transform is applied to the emitted triangles; `NORMAL` is transformed by the inverse-transpose and
left zero (the rasterizer derives a geometric normal) when absent, `COLOR_0` is carried, and UVs are
dropped because the frozen `VertexSample` has no UV channel. Materials carry the product-owned
base-color/metallic/roughness/emissive/unlit/alpha-mode/cutoff/double-sided values. Geometry decode is
bounded: uncompressed accessors through a meshopt-aware buffer adapter, bounded **Draco**
(`KHR_draco_mesh_compression`, 96 MiB / 1 million triangles) and bounded **meshopt**
(`EXT_meshopt_compression`) decode, each charged to the T06 ledger. Embedded **KTX2/Basis** and **WebP**
images are decoded under the 32 MP aggregate texture budget and validated but discarded, because the
frozen `MaterialPayload` has no texture slot and the T15 rasterizer samples no texture; a missing,
corrupt or over-budget *optional* image therefore uses the default material and never fails valid
geometry. `fastgltf`'s `FASTGLTF_ENABLE_DEPRECATED_EXT` define must match the vcpkg static library's
`INTERFACE_COMPILE_DEFINITIONS` in every consuming project or the `fastgltf::Material` layout differs
and corrupts the heap.

T24 implements the OBJ adapter as `thumbnail-provider/ObjFamilyAdapter.{h,cpp}` (selected only by the
routed `Family::Obj` CLSID) over the provider-local pinned ufbx static library, with the external
access policy recorded in [ADR-0022](adr/0022-obj-adapter-ufbx-isolation.md). Parsing forces the OBJ
grammar, disables format detection from content/extension, and sets `load_external_files = false`
plus a deny `open_file_cb`, so `mtllib` and every texture reference are ignored and an MTL/texture
dependency can never fail an otherwise valid mesh or trigger a file open. ufbx's OBJ defaults split
by object and group inhabitation into separate meshes; polygons are triangulated (per-face ceiling
65 536 triangles), missing normals are generated and normalized, and vertex colors (`v x y z r g b`)
are carried with a white base so `vertexColor * baseColor` preserves the source. The neutral
material is index 1. OBJ text is read in one contiguous pass — the frozen view when available,
otherwise a checked ledger-charged backing buffer bounded by the 128 MiB cap — and the ufbx
`progress_cb` plus the geometry loop poll the cooperative deadline.

T31 implements the FBX adapter as `thumbnail-provider/FbxFamilyAdapter.{h,cpp}` (selected only by the
routed `Family::Fbx` CLSID, the fixed identity `{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}`) over the same
provider-local pinned ufbx copy, with the static-pose and external-access policy recorded in
[ADR-0024](adr/0024-fbx-adapter-static-pose.md). The encoding (ASCII or binary) is detected from
stream content only — extension detection is disabled and a synthesized filename never reaches the
filesystem — and a stream whose `metadata.file_format` is not FBX is rejected with the generic icon.
`load_external_files = false`, `ignore_missing_external_files`, and a deny `open_file_cb` mean external
geometry caches and texture sidecars are never opened. Because `ufbx_evaluate_scene` has no progress
callback (FBX-001), the adapter preflights element/triangle/vertex counts **before** evaluation
(2 M triangles / 6 M vertices / 10 000 nodes, meshes, animation stacks, skin deformers and bones; 4 096
materials) and bounds the call with explicit temp/result allocator memory (96 MiB each) and allocation
limits; a call never runs unbounded merely because Shell uses a surrogate. The deterministic static
pose evaluates at the first animation stack's `time_begin` (or `scene->anim` at time zero when no stack
exists) with `evaluate_skinning = true` and `evaluate_caches = false`; `mesh.skinned_position` /
`mesh.skinned_normal` are authoritative, transformed by `node.geometry_to_world` (position) and its
inverse-transpose (normal) when `skinned_is_local` is set. Polygons are triangulated with
`ufbx_triangulate_face` (per-face ceiling 65 536 triangles), reversed winding is honored, vertex colors
are carried, and UVs are dropped. Each face resolves its material through `node.materials` then
`mesh.materials`; a face with no material uses index 0, except that a mesh with vertex colors uses a
registered white fallback. NURBS-only, subdivision-only, procedural-only or geometry-cache-only content
with no supported polygon remaining returns `UnsupportedRequiredFeature` (generic icon); when supported
polygons remain the unsupported features are omitted. Embedded images are structurally validated only
— the allowlisted container (PNG/JPEG/GIF/BMP/WebP) is sniffed, bounded by encoded bytes (32 MiB each)
and decoded pixels (32 MP aggregate), then discarded — because the frozen `MaterialPayload` has no
texture slot and the rasterizer samples no texture; an external texture therefore uses the neutral
fallback and never fails valid geometry.

T32 implements the 3MF adapter as `thumbnail-provider/ThreeMfFamilyAdapter.{h,cpp}` (selected only by
the routed `Family::ThreeMf` CLSID, the fixed identity `{D8389A63-8526-454A-9892-72F3149484B9}`) over
the provider-local pinned lib3mf 2.5 reader, with the package/required-extension and appearance policy
recorded in [ADR-0025](adr/0025-3mf-adapter-opc-and-lib3mf.md). The bounded product OPC/ZIP preflight
(`ThreeMfOpcPreflight.cpp`, compiled from the worker's source under provider ceilings: 256 MiB stream,
128 MiB aggregate expansion, 100:1 ratio, 4096 entries, 32-level paths) rejects unsafe packages before
lib3mf reads a byte; a bounded byte-level scan of every `.model` part then rejects an unsupported
`requiredextensions` entry (Core/Materials/Production/Beam-Lattice/Ball allowlist) and DTD/entity
content, which lib3mf's compatible reader mode does not enforce. lib3mf is statically linked and fed
the bounded in-memory bytes through read/seek callbacks with a deadline-aborting progress callback —
never its filename API — so the provider launches no worker and performs no filesystem, sidecar,
network or persistent access. The standard root build is traversed deterministically across build
items and component graphs (256-level depth cap, 10 000 occurrences, 2 M inspected triangles) with
checked double-precision row-vector transforms; bare-mesh occurrences emit a flat-normal triangle
sample with the object/triangle/per-corner color resolved to a linear vertex color over one registered
white material. Supported property types are base materials, color groups, texture-coordinate groups,
composites and multi-properties; a texture group is structurally validated against the 32 MP aggregate
budget but not decoded (the frozen `MaterialPayload` has no texture slot). A beam/ball lattice
occurrence prefers a bounded tessellation of tapered beams (Butt/Hemisphere/Sphere caps) and balls,
`inside`-clips against a closed axis-aligned 8-vertex/12-triangle box under a 262 144-triangle
per-lattice ceiling with deterministic radial degradation, and fails to the generic icon for `outside`
or non-box clipping without a representation mesh. An unsupported required extension, an over-budget
scene, an unclipped parametric lattice or a malformed package fails closed; a supported scene is never
rendered only in part.

T33 implements the USD/USDZ adapter as `thumbnail-provider/UsdFamilyAdapter.{h,cpp}` (selected only by the
routed `Family::Usd` CLSID, the fixed identity `{E938BC70-4C08-4446-A15D-EE31576BFB48}`) over the
provider-local pinned TinyUSDZ 0.9.1 static reader, with the stream-only/no-composition/no-external-
resolution policy recorded in [ADR-0026](adr/0026-usd-adapter-pinned-tinyusdz.md). The container is
byte-sniffed independently of the suffix (ZIP local header → USDZ, `PXR-USDC` crate magic → USDC, a leading
`#usda` after an optional BOM/whitespace → USDA, anything else malformed). A USDZ stream is validated by the
product-owned worker preflight (`import-worker/src/UsdZipPreflight.cpp`, compiled source-not-state into the
provider) under provider ceilings — 128 MiB aggregate expansion, 100:1 ratio, 4096 entries, 32-level
normalized paths, stored-only, checked offsets and CRC — and every byte stays in the brokered stream;
nothing is extracted. TinyUSDZ is fed the bounded in-memory bytes with `load_assets`, `do_composition`,
`load_sublayers`, `load_references` and `load_payloads` all disabled and a wildcard asset resolver that
resolves only names present in the USDZ entry map (any other request is `UnsafeReference`). Composition arcs
(sublayers, references, payloads, inherits, specializes, variants, clips, instanceable) are classified
before conversion and fail closed to the generic icon (`UnsupportedComposition`), as does any external
reference or texture (`UnsafeReference`); the provider never launches the compatibility host, reads the
cache, recovers a path or reaches the network. The static `UsdPreviewSurface`/display-color policy is
normalized into the frozen `MaterialPayload` (base color/opacity, metallic, roughness, emissive, alpha
mode/cutoff, double-sided) and finite triangle samples with double-precision world transforms,
purpose/visibility and bounded point-instancer expansion; skeletal bindings are stripped so the authored
rest pose previews. Contained textures are recorded but not decoded — the frozen `MaterialPayload` has no
texture slot — so an absent or external image never fabricates geometry. A `fast_float` ABI collision
between TinyUSDZ's vendored copy and lib3mf's vcpkg copy is removed by building TinyUSDZ against the
vcpkg-pinned `fast_float` in the overlay port (`0.9.1#3`, [ADR-0026](adr/0026-usd-adapter-pinned-tinyusdz.md)).

T34 implements the STEP/STP adapter as `thumbnail-provider/StepFamilyAdapter.{h,cpp}`
(selected only by the routed `Family::Step` CLSID, the fixed identity
`{6EE961AC-AC3B-4958-A898-E30523FEE79D}`) over a **dedicated static OCCT 7.8
closure** declared by the isolated manifest `thumbnail-provider/step-occt/` and
linked only into the provider, `Tests.Unit.exe` and `Tests.ProviderHost.exe`
([ADR-0027](adr/0027-step-adapter-constrained-occt.md)); the existing STEP host
keeps its own separate OCCT closure and the provider never launches
`Preview3DStepHost.exe`. The adapter reuses the STEP host's product-owned
Part-21 admission scanner (source-not-state, [ADR-0004](adr/0004-share-source-not-state.md))
before any OCCT call: it rejects external documents (`FILE_POPULATION`/
`DOCUMENT_FILE`), unsupported encodings and over-budget input (256 MiB stream,
2 M entity records, 20 M references, 4096 sections). OCCT then reads a bounded
seekable `std::streambuf` over the T12 `BoundedSource` — never a path — with
color/name/layer modes and a per-call `TDocStd_Document`; opaque OCCT calls run
under the T16 exception/structured-exception containment boundary so a malformed
authored tessellation is a typed failure, never a fabricated image. The XDE
assembly/instance hierarchy is walked with checked double-precision row-vector
transforms (256-level depth cap, cycle detection, 20 000 definitions) and a fixed
low-detail deterministic meshing policy (`IMeshTools_Parameters` relative
deflection `0.05` clamped to `[0.01, 5.0]`, angle `0.7`; 1 M triangles per
definition, 2 M inspected total) with shape/instance colors normalized to the
shared `MaterialPayload`. A non-contiguous or over-128 MiB stream is read through
bounded range reads; an over-`kAllocationLedgerMaxBytes` accounted scratch
reservation fails closed.

## CPU renderer

The thumbnail DLL uses a product-owned tile rasterizer; no GPU device or graphics queue is created inside Explorer.

- Render at min(cx, 512) for nonzero cx, with 2x internal supersampling only when the deadline
  budget permits; the returned bitmap fits within cx in each dimension, including 32 px requests.
- Use a transparent canvas, soft neutral floor/contact shadow, and the same neutral material palette as the viewer.
- Frame a fixed isometric view from verified bounds with 7% margin.
- Apply model transforms in double precision, clip against the near plane, depth-test tiles, and shade with ambient plus two fixed lights.
- Render opaque/masked triangles. Approximate transparent materials as weighted opaque color; exact order-independent transparency is unnecessary at thumbnail size.
- Render point-cloud samples as depth-tested round splats with deterministic size and source/neutral color.
- Downsample in linear space and convert to premultiplied BGRA.

T15 implements this as `thumbnail-provider/CpuRasterizer.cpp` behind the frozen
`ICpuRasterizer` (`CpuRasterizerImpl.h` names the entry point; `ThumbnailPipeline.cpp` calls it),
[ADR-0017](adr/0017-cpu-tile-rasterizer.md). Resolution is `min(cx, 512)`; 2x internal
supersampling is used only when `allowSupersample` is set and the cooperative deadline has at
least 250 ms of remaining headroom, otherwise 1x. A triangle's albedo is
`vertexColor.rgb * baseColorFactor.rgb`; `alphaMode` selects Opaque, Mask (discard below
`alphaCutoff`) or Blend (the weighted-opaque approximation), `kMaterialFlagDoubleSided` controls
culling, `emissiveFactor` is added after shading and `kMaterialFlagUnlit` skips the light rig.
Raster targets and the output buffer are charged to the T06 ledger before allocation (a charge
that would cross the ceiling returns `ERROR_FILE_TOO_LARGE` with no image), the deadline is polled
every 2048 work units, and `thumbnail-provider/goldens/` holds the 32/64/256/512 px mesh/point
plus alpha PAM goldens compared with the tolerant perceptual metric.

No text, file path, watermark, network content, or nondeterministic animation appears in the bitmap.

## Threading and unload

An object is apartment-affine. GetThumbnail performs work on the calling thread because the Shell owns call scheduling; it does not create a lasting pool. Parsing libraries are invoked with per-call arenas and no process-global mutable caches. A deadline check is included at bounded parser/sampler/raster tiles.

DllCanUnloadNow returns S_OK only when live objects, class-factory locks, and active calls are all zero. Destructors are noexcept and release stream/backing resources. Thread-local parser scratch cannot keep the module artificially alive.

The COM core is implemented in `thumbnail-provider/ComCore.h`/`ComCore.cpp` and
documented by [ADR-0013](adr/0013-provider-com-core-lifetime.md): one class factory per routed
CLSID (family chosen from the CLSID alone, never sniffed), explicit atomic
module/object/lock/active-call counts behind `DllCanUnloadNow`, and a `ProviderObject`
that implements `IInitializeWithStream` (T12, [ADR-0014](adr/0014-bounded-stream-backing.md))
over the bounded stream source; T13 adds `IThumbnailProvider` to the same object. `GetThumbnail`
routes the CLSID-selected family through `thumbnail-provider/ThumbnailPipeline.h` (the
`RunThumbnailPipeline` orchestration and its `IThumbnailDependencies` seam), consumes the linked
adapter from `FamilyAdapterRegistry.h`, converts the rasterizer's `RasterImage` to the returned DIB
in `RasterBitmap.h`, and sets `WTSAT_ARGB`; [ADR-0015](adr/0015-provider-thumbnail-pipeline-and-cx.md)
records the composition and the `cx == 0` -> `E_INVALIDARG` row. DllMain only records the module
handle and disables the unused thread notifications; it performs no COM, registration, library load
or thread work.

T16 implements this directly (see [ADR-0018](adr/0018-provider-threading-containment-and-diagnostics.md)).
The object/lock/active-call counters and the RAII `ActiveCallGuard` live in
`thumbnail-provider/ModuleLifetime.{h,cpp}` (PCH/COM-free, so `Tests.Unit.exe` proves the
active-call unload gate). Both `Initialize` and `GetThumbnail` hold an `ActiveCallGuard` for their
whole body, so releasing the last external object reference cannot unload the module out from under
an in-flight call; thread-local containment scratch is a trivial `std::uint32_t`, which registers no
TLS destructor and cannot keep the module alive. Work stays on the Shell's calling thread: no pool,
worker, process or GPU device is created and no process-global mutable cache is added.
`RunThumbnailPipeline` takes a cooperative `Deadline::Checkpoint()` between every bounded stage
(Initialize/Parse/EnumerateMaterials/EnumerateGeometry/Render), in addition to the per-unit polls
inside the T12 stream source and T15 rasterizer; the adapters (T21-T34) poll
`AdapterInput::deadline->Checkpoint()` inside their own long loops. Because those frozen adapter
methods are `noexcept`, each of their allocating stages also runs its body through
`thumbnail-provider/ContainmentStage.h` (`RunContainedStage`), so a product-owned `std::bad_alloc`
is translated to `ErrorCode::OutOfMemory` before it can reach a `noexcept` frame
([ADR-0034](adr/0034-provider-adapter-stage-containment.md)). The COM boundary runs the
pipeline and the DIB conversion through `thumbnail-provider/Containment.{h,cpp}` (`RunContained`),
the last-resort HRESULT boundary that translates a C++ exception (`std::bad_alloc` -> `E_OUTOFMEMORY`,
anything else -> `E_FAIL`) and a contained structured exception (`E_FAIL`) to the T06 table. A call
already in progress is never interrupted: `RunContained` records the real elapsed time and, if an
uninterruptible call returned after the cooperative 2 s stop point, rejects it afterwards as
`ERROR_TIMEOUT`. Stack overflow, breakpoint and single-step are deliberately not swallowed.
Diagnostics are `thumbnail-provider/Diagnostics.{h,cpp}`: numeric events only (no field a path could
travel in), emitted only while explicitly enabled or via
`PREVIEW3D_THUMBNAIL_DIAGNOSTICS=1`, and off by default.

## Security and robustness

The DLL is treated as hostile-input code executing in a sensitive host:

- compile with /guard:cf, /CETCOMPAT, /DYNAMICBASE, /NXCOMPAT, /sdl, and high warning level;
- use checked integer/range helpers at every file-derived allocation or offset;
- disable parser callbacks that open paths, URLs, plug-ins, scripts, codecs, or environment-selected resources;
- never start or communicate with the OpenUSD compatibility host, the general import worker, or the viewer, and never read the viewer's persistent derived cache;
- rely on Shell's default out-of-process surrogate hosting as the actual crash-containment boundary (see COM classes and extension assignment above); the compile-time hardening flags below reduce what a crash can do, they do not replace process isolation;
- treat that surrogate hosting as crash containment for Explorer only, not as the zero-capability AppContainer security boundary that [ADR-014](./11-decisions-and-risks.md#adr-014-appcontainer-import-processes-are-the-parser-security-boundary-not-threads) requires of the import worker: the surrogate still runs with the invoking user's own token and ordinary file-system/network access, so this DLL's actual safety against a hostile file comes from the bounded reads, checked parsing, and deadline/limit enforcement in this document, not from the process boundary;
- place third-party parser calls behind exception and structured-exception containment at the COM boundary where legally safe, while fixing ordinary memory faults rather than masking them;
- write no model-derived persistent cache;
- keep diagnostic events path-redacted and disabled unless troubleshooting is enabled.

T16 implements the exception/SEH boundary once in `thumbnail-provider/Containment.h`/`Containment.cpp`
(`RunContained`) and calls it at the COM boundary; adapters route their third-party calls through it
(their frozen methods are `noexcept`, so an uncontained throw would terminate). SEC-06 adds the
matching product-owned path: every family adapter routes its allocating lifecycle stage through
`thumbnail-provider/ContainmentStage.h` (`RunContainedStage`), so a `std::bad_alloc` from its own
containers is `ErrorCode::OutOfMemory` too ([ADR-0034](adr/0034-provider-adapter-stage-containment.md)).
A contained fault
returns the tabulated failure with a diagnostic event, never a fabricated success; the ordinary
memory fault behind it is still expected to be fuzzed and fixed (T43). Diagnostics are
`thumbnail-provider/Diagnostics.h`/`Diagnostics.cpp`: events carry only a stage, outcome, counters and
elapsed time — no string field exists, so a path cannot leak — and are disabled unless troubleshooting
is enabled.

An importer crash must be addressed by fuzzing/fixing; SEH containment is a last-resort HRESULT boundary, not a correctness mechanism.

## HRESULT mapping

| Condition | HRESULT |
| --- | --- |
| Bad pointer/invalid call order | E_POINTER / E_UNEXPECTED |
| Invalid argument (the degenerate `cx == 0`) | E_INVALIDARG |
| Unsupported stream behavior or format feature | HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) |
| Malformed or empty geometry | HRESULT_FROM_WIN32(ERROR_BAD_FORMAT) |
| Limit or deadline exceeded | HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE) / ERROR_TIMEOUT |
| Allocation failure | E_OUTOFMEMORY |
| Decoder/importer failure | E_FAIL, with diagnostic event |

Explorer is allowed to fall back to the generic icon. Returning a fabricated “success” bitmap for a failed parse would poison the Shell thumbnail cache and is prohibited.

## Tests

- COM identity, QueryInterface, aggregation rejection, refcount, lock server, and unload tests.
- One extension-routing test per registry entry and CLSID.
- PLY mesh/point-cloud golden and hostile-list tests; glTF Draco/KTX2 provider-limit tests.
- Golden images at 32, 64, 256, and 512 pixels with tolerant perceptual comparison.
- STA parallel-host stress using multiple COM objects.
- The COM host harness `Tests.ProviderHost.exe` (T17, [ADR-0019](adr/0019-provider-com-host-harness.md)) drives the Shell activation sequence per CLSID, compares a registered fixture rendered through the real pipeline against a tolerant PAM golden, and soaks repeated load/unload against GDI/User/private-byte/thread growth.
- Truncation, archive bomb, adversarial count, non-seekable stream, timeout, OOM injection, and fuzz corpora.
- Repeated Explorer surrogate load/unload with GDI/User handle and private-byte leak checks.
- Verification in the actual Windows thumbnail surrogate at 100%, 150%, and 200% DPI.
- Post-install verification, on a clean machine, that each registered CLSID is actually loaded into the isolated surrogate process (not `explorer.exe`) and that no installed registry value sets `DisableProcessIsolation`; this is a release-blocking check, not an optional audit.

Primary references: [Thumbnail provider guidance](https://learn.microsoft.com/windows/win32/shell/thumbnail-providers), [IInitializeWithStream](https://learn.microsoft.com/windows/win32/api/propsys/nn-propsys-iinitializewithstream), and [IThumbnailProvider::GetThumbnail](https://learn.microsoft.com/windows/win32/api/thumbcache/nf-thumbcache-ithumbnailprovider-getthumbnail).

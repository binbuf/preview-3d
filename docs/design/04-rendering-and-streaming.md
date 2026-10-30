# Rendering and streaming

## Goals

The renderer must keep accepting input and presenting frames while cache validation, CPU import (now brokered to the AppContainer import worker and, for USD composition, the compatibility host), PCIe transfer, shader-visible publication, LOD selection, and eviction happen concurrently. It must also render a useful approximation when the full model is larger than available video memory and exploit verified derived data on repeat opens without trusting it implicitly.

“Asynchronous” means there is no UI-thread or render-thread wait for a loading resource. It does not mean every adapter or machine has a physically independent DMA engine.

## Device and presentation

At startup Graphics:

1. Enables the D3D12 debug layer only in developer builds before device creation.
2. Creates a DXGI factory and enumerates hardware adapters in the operating system's GPU preference order.
3. Selects the first non-software adapter that supports D3D12 feature level 11_0 and presentation to the window.
4. Creates one direct command queue and one copy command queue. A developer-only command line can request WARP; WARP is not an automatic performance fallback.
5. Creates a three-buffer flip-discard swap chain using DXGI_FORMAT_R8G8B8A8_UNORM, a frame-latency waitable object, maximum frame latency two, and Present(1, 0).

The primary path uses the Windows 11 system D3D12 runtime; the MVP does not ship the Agility SDK. Shaders are built offline with a pinned DXC version and embedded as immutable assets. Runtime shader compilation is prohibited in release builds.

The window client size is converted to physical pixels using per-monitor-v2 DPI. Zero-area/minimized windows stop acquiring swap-chain buffers but do not stop import or residency management. Resize is coalesced and performed at a direct-fence-safe point; it never blocks the UI message pump.

## Ownership model

| Owner | Exclusive state |
| --- | --- |
| UI thread | HWND, message pump, input/capture state, accessibility commands, high-level AppState |
| Render thread | device, direct queue, swap chain, back buffers, render allocators/lists, render fence, pipelines, descriptor heaps, D3D11On12/D2D overlay bridge |
| Upload coordinator | copy queue, copy allocators/lists, upload-ring suballocation, copy fence |
| Residency manager | resource catalog and requested/committed/resident states; actions are executed through the owning GPU lane |

An ID3D12CommandAllocator is reset only after the fence value of its last submitted command list has completed. Direct and copy command lists are recorded by their owning lanes. No worker records into a shared list and no COM command object crosses ownership without an explicit immutable handoff.

All D3D12 objects are represented to other components by stable handles into Graphics-owned tables. Destruction enters a deferred-release queue tagged with the last direct and copy fence values that could reference the object.

## Frame loop

The render thread waits on the frame-latency handle plus a short event set for shutdown, resize, new scene data, and UI invalidation. A timer keeps animation/loading indicators alive; there is no unconstrained busy loop.

For every drawable frame it:

1. Drains a bounded number of fence-complete upload publications and retired resources.
2. Atomically acquires the latest immutable SceneSnapshot.
3. Updates camera-relative transforms, visible clusters, LOD requests, and constant buffers for the current frame context.
4. Records depth, opaque, alpha-mask, point-cloud, transparent, grid/background, and selection-independent UI composition in deterministic order.
5. Submits the D3D12 scene command list. If an overlay pass follows, it leaves the wrapped back buffer in the bridge's declared input state; otherwise it transitions the buffer to PRESENT itself.
6. When an overlay pass follows, uses the D3D11On12 bridge to acquire, draw Direct2D/DirectWrite content, release, and flush the wrapped resource into PRESENT state.
7. Signals the direct fence after both scene and overlay GPU work, tags resources and the snapshot with that fence, presents, and advances the frame context.

D3D11On12 is used only for small native UI/text overlays. Geometry, depth, materials, tonemapping, and loading proxy rendering remain D3D12. The bridge is created on the render thread over the existing direct queue and is never called from the UI thread.

## Render pipeline

The MVP uses a forward, rasterized PBR-lite pipeline:

- right-handed internal coordinates and camera-relative float matrices;
- reversed-Z depth with a floating-point depth buffer;
- one product-owned procedural studio environment baked once into a world-stable cubemap (SH9 diffuse irradiance plus a GGX-prefiltered specular mip chain) with a camera-relative key/fill light;
- base color, metallic, roughness, emissive, normal map and UV transform when present, vertex color, unlit mode, alpha mask, double-sided state;
- bounded point primitives rendered as depth-tested camera-scaled round splats with source color or neutral shading;
- opaque and masked geometry before sorted transparent draw groups;
- back-face culling unless a material is double-sided;
- a linear `R16G16B16A16_FLOAT` HDR scene target plus the reversed-Z depth
  buffer and the R32_UINT pick target; object shaders write linear light and
  never apply a display transform;
- a full-screen resolve pass that composites the background in linear space,
  applies exposure, the selected neutral/ACES/Filmic/None tone curve and the
  sRGB transfer, then writes the UNORM swap-chain back buffer;
- MSAA disabled by default; temporal effects, ray tracing, mesh shaders, shadow maps and post-process antialiasing are outside the MVP. An analytic soft contact shadow and ORM ambient occlusion are in the MVP (see below).

Materials are flattened into fixed-size GPU records. Feature level 11_0-compatible descriptor tables are used instead of assuming resource binding tier 3 or shader model 6.6 bindless behavior. Missing resources point at permanent neutral descriptors.

Direct descriptor-heap indexing ("bindless") requires both shader model 6.6 and resource binding tier 3, which the MVP's feature-level-11_0 floor does not guarantee. Descriptor tables are therefore the baseline rendering path for the life of the MVP, not a placeholder pending a bindless rewrite; a bindless path may be added later strictly as an optional, separately gated capability tier behind a runtime feature check, evaluated on its own performance evidence rather than adopted as a wholesale replacement.

Per-frame camera/pass data (view-projection, camera-relative origin, lighting) is bound through a per-frame constant buffer view — a root CBV or a root-signature-referenced table entry — not packed into inline root constants. Root signatures have a hard 64-DWORD budget and materially less native fast storage on typical hardware; root constants are reserved for small, genuinely per-draw values such as a material/instance/draw ID, not for data that is naturally a buffer.

The normalized-chunk vertex layout is a small, closed, product-enumerated set of explicit layouts (at minimum: a position-only layout for point clouds, a position+normal+UV layout for untextured/simple meshes, and a full PBR layout with tangents/vertex color for textured meshes), each with a stable numeric layout ID carried on the chunk descriptor. It is not one universal fixed-size vertex record that every format's normalizer is forced into: a struct sized to fit worst-case data (as in the advisor's illustrative 32-byte sample, whose declared members total 28 bytes and only reach 32 through tail padding, and whose UV field is full float2 rather than half-precision) wastes bandwidth on the common case and still cannot represent every source faithfully. Adding a layout to the enumeration is a normal chunk-schema change reviewed like any other in [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md); it is not a reason to add a second universal struct.

CPU frustum culling operates on cluster bounds. Draws are grouped by pipeline/material and compatible instances are batched. When measured visible draw count or CPU submission time crosses the Gate-3 threshold, a feature-level-11-compatible `ExecuteIndirect` path consumes product-generated commands after CPU cluster culling; the ordinary direct-draw path remains the correctness fallback. GPU occlusion culling and mesh shaders remain deferred. The normalized cluster size bounds CPU work in either path.

## Studio environment and color pipeline

The default Studio view does not read a user file or a shipped HDRI. A
product-owned procedural environment is generated at startup
([`StudioEnvironment.h`](../../interactive-viewer/src/render/StudioEnvironment.h)):
three rectangular softboxes, a soft overhead strip and a neutral floor/ceiling
gradient, evaluated analytically in direction space. The default bake is 128
texels per cube face, six mips, with a fixed GGX importance sequence; total
GPU storage is well under 4 MiB. `BakeOptions` clamps the face size to 4–256 and
the mip count to the full chain, so the startup cost is bounded and the output
is bit-identical across runs.

The bake produces three derived views of the same radiance:

- the base RGBA32F cubemap (Direct3D face order +X, -X, +Y, -Y, +Z, -Z);
- a GGX-prefiltered specular mip chain, where roughness 0 selects mip 0 and
  roughness 1 selects the last mip (`roughness * (mipCount - 1)`);
- SH9 coefficients of irradiance/π for diffuse lighting.

The environment is uploaded once on the direct queue during initialization and
never re-baked for a camera move or a frame. Its SRV occupies one slot appended
to each model's material descriptor heap: D3D12 permits only one shader-visible
CBV/SRV/UAV heap at a time, so a separate environment heap cannot be bound
alongside the material heap. The same heap slot is re-written when progressive
material descriptors are rebuilt.

The vertex shader keeps the camera-relative position/normal precision policy.
Before sampling the environment the pixel shader rotates the camera-space
normal and view direction back into the model/world frame using the
world-from-camera quaternion; the cubemap therefore stays attached to the model
while the camera orbits. The key/fill analytic rig remains camera-relative for
predictable object readability. Studio exposes intensity, yaw and exposure in
the per-frame constant buffer. Clay reuses the same environment at low
specular; Directional keeps its hard key and adds only a low-intensity
world-stable ambient/reflection term.

The object shader now writes linear HDR radiance into an
`R16G16B16A16_FLOAT` scene target; it deliberately no longer applies the
display transform. A full-screen resolve pass reads that target, composites the
background in the same linear space (dark UI-matched, neutral mid-grey,
transparent, or a checkerboard), multiplies by exposure, applies the selected
tone curve and writes sRGB into the swap-chain back buffer. Tone mapping after
blending is what makes alpha and transmission composite correctly: the old
per-object curve ran before the blend and could not account for what was
behind a transparent surface.

The default curve is still Khronos PBR Neutral (`startCompression` 0.76,
desaturation 0.15); ACES, Filmic and None are selectable, and non-finite scene
values are contained before the curve. Exposure is a view setting applied
before the curve. `[` / `]` change it, `\` cycles the background and `` ` ``
cycles the tone curve; exposure, background and tone-map choice are persisted
with the other view preferences. Because the background is cleared into the
same HDR target and tone-mapped with the scene, a dark backdrop never hides a
dark object's silhouette without the preset's rim/key contribution. Transparent
and checkerboard modes clear the target alpha to zero and preserve it through
the resolve, so a shell or screenshot path can use transparency.

Presentation presets are selected from a bounded summary of the imported
material catalog (base-color texture presence, metallic/roughness range,
alpha/transmission share, unlit flags, point-only geometry and CAD format).
Studio PBR, Studio CAD, Scan/Sculpt, Transparent and Point cloud each supply
environment intensity, direct key/fill/rim multipliers, exposure, background,
grid and ground-shadow defaults. The selection is a pure function in
[`Presentation.h`](../../interactive-viewer/src/render/Presentation.h); the
material loop is capped independently of the file. An explicit toolbar/preset
choice pins the session and wins over later catalog recommendations.
Environment intensity scales the direct rig and IBL together, matching F3D's
paired `render.light.intensity` / `IBLIntensity` behavior. The CPU reference in
`StudioEnvironment.cpp` and `Presentation.cpp` and their HLSL copies are kept
in lock-step by the `[studio-ibl]`, `[tonemap]` and `[studio-presets]` unit
tests, including pixel-shader compile checks at the feature-level-11_0
baseline.

## Ground contact shadow and ambient occlusion

Objects sit on a ground layer that is independent of the grid. The chosen MVP
path is the analytic soft contact footprint from ADR-0072, not a shadow map:
[`GroundShadow.cpp`](../../interactive-viewer/src/render/GroundShadow.cpp)
transforms the verified model bounds with the active ground-axis matrix, places
the plane at `min.z - 0.0015 × radius`, projects the eight bounds corners along
the stable studio key direction `(0.45, -0.55, 0.70)`, biases that projection
15 % back toward the base and adds a 0.60 × radius soft rim. `D3D12ViewerPath`
draws the plane and mask as a generated quad before the grid and the model,
into the linear HDR target. It writes no depth, so it cannot occlude the model;
the grid stays a separate lower layer at `min.z - 0.012 × radius`, so the two
coplanar layers cannot z-fight.

The shadow is one blended quad: there is no extra target, pass or GPU
allocation. Opacity is 0.78 with 0.50 softness and an opaque core. A grazing
key elevation is clamped to 0.08 and the footprint to 6 × radius, so a flat or
very directional light cannot produce an unbounded ellipse. `H`, the More menu
item and the persisted `groundShadowEnabled` preference toggle the layer. Studio
CAD/PBR/Scan enable it by default; Transparent and Point cloud disable it. A
manual toggle is independent of the grid toggle and survives later preset
applications. SSAO is deliberately deferred to a follow-up: the HDR target does
not yet expose a depth/normal chain, and the quality option must never gate the
contact shadow.

The metallic-roughness texture's R channel is ambient occlusion. The shader
applies the glTF strength formula `saturate(1 + strength × (sample - 1))`, with
sample = 1 when the map is absent and `strength` from the protocol-v12
`occlusionStrength` field (default 1). AO multiplies indirect diffuse,
environment reflection and ambient terms only; direct `EvaluateLight` key/fill/
rim specular is never AO-scaled, so metals do not read as dirty.
The transparent/checkerboard backdrop from T15 remains the single background
implementation; ground shadow defaults off there and for point clouds.

### Material lobes and transparency

The textured pixel shader reads the protocol-v12 lobe block as four additional
`float4` root constants (`gMaterialLobes0..3`), keeping peak-shader-model 5.1
and the existing 32-bit-constant path instead of a material buffer rewrite.

- **Specular / IOR.** Dielectric F0 is `((ior-1)/(ior+1))²`, multiplied by the
  authored `specularFactor` and tinted by `specularColor`; the metallic path
  still uses base color as F0. The old fixed 0.04 survives only as the default
  IOR 1.5 result.
- **Clearcoat.** A second dielectric GGX lobe with its own roughness is added
  over the base. The base specular layer is multiplied by
  `(1 - clearcoatFactor)` so the same reflected energy is not counted twice;
  the coat's Fresnel term is folded into its direct-light contribution.
- **Sheen.** A grazing-only `pow(1 - N·V, 3)` velvet term, strongest for a
  rough weave, added at low weight. It never applies to the clay mode.
- **Emissive.** `emissiveStrength` multiplies emissive radiance before the
  resolve pass, so an authored 5× sign enters tone mapping as linear HDR
  rather than clipping at 1.0.
- **Occlusion strength.** Applied through the same glTF formula above.

Transparency stays on the existing straight-alpha path: opaque draws first,
alpha-blend draws are stable-sorted back-to-front by view depth, the HDR scene
target keeps linear color, and T15's resolve composites the background,
exposure and neutral tone curve afterwards. This ordering avoids the old
dark-halo failure where tone mapping happened before blending. Weighted blended
OIT and depth peeling are **deferred**: the sorted path is correct for ordinary
layered glTF glass, but intersecting transparent surfaces and very dense
transmission scenes remain order-dependent. The transmission approximation
remains Fresnel-weighted alpha with suppressed transmitted diffuse; there is
no refraction pass.

### Line and point topology

`ChunkTopology::LineList` is an indexed pair list: `indexCount` is even and each
`{indices[2i], indices[2i+1]}` pair is one independent segment. There are no
strips or loops on the wire. glTF `LINES` maps directly; `LINE_STRIP` and
`LINE_LOOP` normalize to pairs; triangle strips/fans become indexed
`TriangleList` with degenerate stitches dropped. OBJ `l`/`p` and FBX
`line_curves` use the same pair contract. The validator rejects an odd index
count or an out-of-range index as `MalformedData`, and line payloads use the
triangle vertex/byte caps.

The viewer draws lines with a position-only vertex/pixel shader, a dedicated
`D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE` pipeline, negative depth bias and
`LESS_EQUAL` with depth writes off, so authored curves stay visible over
coplanar surfaces without occluding them. Lines participate in bounds,
visibility, LOD and the pick target. `PointList` remains unindexed and renders
as bounded round/colored point splats; the point pipeline keeps its own pick
identity so a point cloud can be selected even though it has no triangles.

## Upload ring

The upload lane owns a persistently mapped ID3D12Resource in an UPLOAD heap. Its initial size is 256 MiB, allowed to grow in 64 MiB increments up to the lesser of 512 MiB and its CPU/GPU memory budget. It is a logical ring of allocations aligned to D3D12 placement and copy requirements.

Each allocation records offset, size, generation, destination resource, and the copy fence value that retires it. Allocation follows these rules:

- reclaim from the tail only when GetCompletedValue covers the allocation's fence;
- wrap only into proven-free space;
- split input into bounded chunks rather than waiting for one huge contiguous segment;
- when full, only the upload coordinator may wait on the copy-fence event, with cancellation and shutdown also in its wait set;
- apply backpressure to the bounded ReadyForUpload queue before exceeding memory budgets;
- never expose the CPU pointer or upload resource to parser workers.

The coordinator copies normalized bytes into the ring, records CopyBufferRegion or texture footprint copies into DEFAULT-heap resources, closes and executes a batch, and signals a monotonically increasing copy fence. Default buffers are created in COMMON. On the copy queue they may be implicitly promoted to COPY_DEST; buffer resources decay back to COMMON after the ExecuteCommandLists boundary. Texture transitions obey the explicit copy-queue-compatible state rules and are finalized before publication.

The fast path uses batches large enough to amortize submission but small enough to publish progressively: normally 16–64 MiB or 2 ms of recorded copy work, whichever comes first.

## Fence-complete publication

Copy submission does not make a resource renderable. The coordinator maintains a min-ordered list of PendingPublication records. When the copy fence reaches a record:

1. Graphics validates that its generation is still current.
2. The resource moves from Uploading to Ready.
3. A new immutable SceneSnapshot references the ready mesh/texture descriptor.
4. The renderer may use the resource on its next frame, causing a valid COMMON-to-read promotion/transition on the direct queue.

An incomplete resource is absent from every renderable snapshot. Consequently, the direct queue does not issue Wait against a copy fence for ordinary streaming; it keeps drawing the previous proxy/LOD. An explicit cross-queue wait is reserved for a rare atomic dependency batch that cannot be represented by prior publication, and it must be justified by a performance test.

This is an epoch/handle swap, not an unowned atomic raw pointer. Old snapshots remain alive until the last direct fence that used them completes.

## Scene lifecycle

Resource states follow:

    Parsed -> ReadyForUpload -> Uploading -> Ready -> Visible
                                              |
                                              v
                                      EvictRequested
                                              |
                                              v
                                    Retired -> Released

Cancellation can divert any pre-visible state to Retired. A new file does not remove the current scene until the new generation has a complete coarse proxy, unless the user explicitly closes the current document. This prevents a bad or slow file from blanking useful content. After a successful proxy handoff, old resources retire behind their last direct fence.

Geometry and textures publish independently, with neutral material fallbacks until dependent textures are ready. A material snapshot changes only at a frame boundary.

## Progressive LOD

The renderer always selects from resources in Ready state:

- while early loading, draw the partial coarse proxy and an explicit loading treatment;
- once the complete coarse catalog is ready, draw it as the always-resident safety representation;
- request intermediate/full chunks by projected screen error, visibility, distance, and camera motion;
- keep a parent LOD visible until every child needed for the replacement region is ready;
- cross-fade or dither a replacement for a short fixed interval to avoid obvious pops;
- reduce detail rapidly during camera motion and refine after a short idle hysteresis.

Requests include generation, cluster, desired LOD, priority, and last visible frame. The scheduler favors visible center-screen clusters, then silhouette-relevant large projected error, then near-camera prefetch. A bounded cancellation-aware priority queue prevents a camera sweep from producing unbounded obsolete work.

## Video-memory budget and residency

IDXGIAdapter3::QueryVideoMemoryInfo is the authority for current local/non-local usage and budget. The app registers for budget-change notifications and also samples at a low frequency while streaming. On UMA, “local” memory is shared system memory and the CPU working-set limit also constrains allocations.

The reserved set contains swap-chain/depth resources, pipeline assets, permanent fallback textures, frame data, and the coarse proxy. The detail target is recalculated as:

- no more than 60% of the reported local budget for all app-managed model resources;
- at least 512 MiB of reported budget headroom when the budget permits;
- no more than the format generation's CPU/GPU policy cap;
- reduced immediately when the OS budget falls.

If the reserved set itself cannot fit with headroom, the renderer lowers proxy density and texture resolution. It never treats CreateCommittedResource success as evidence that future allocations are safe.

Fine chunks use an LRU weighted by visible recency, projected error, re-import cost, and dependency sharing. Eviction removes a chunk from a new snapshot first, waits only in deferred retirement for the last direct fence, and then releases or uses explicit heap residency APIs where profiling justifies them. The MVP uses D3D12 Memory Allocator (D3D12MA) for suballocation and budget telemetry, but its policy remains product-owned.

D3D12MA is configured as one device-wide allocator instance, not one physical DEFAULT heap: it intentionally manages a pool of separate heaps and must keep buffers, render-target/depth-stencil resources, and small vs. large textures in distinct heaps on Resource Heap Tier 1 hardware, where mixing resource categories in one heap is not supported. "One allocator" describes the object the renderer talks to, not a claim that model geometry, textures, and target resources ever share underlying heap memory.

The system does not promise full-detail simultaneous residency for an 8 GiB source. Its contract is a stable proxy plus view-prioritized refinement.

## Textures

HDR and EXR material textures arrive as CPU-tonemapped RGBA8 (color/emissive
use Reinhard + sRGB; normal/data stay linear), so the GPU format contract does
not grow for T13. A future IBL source uses the import worker's separate linear
equirect float environment hook (bytes-only, budgeted) rather than a material
image chunk.

Validated KTX2/DDS mip tails and transcoded Basis mip levels are uploaded before higher-resolution mips. Compatible precompressed BC resources avoid RGBA expansion and runtime mip generation. Texture resources are created with copy-compatible layouts and immutable final content per published resource. The MVP does not overwrite a texture while it is sampled; a more detailed texture is a new handle swapped into a later material snapshot.

Sampler count is bounded by a small static sampler set. Alpha mode, color space, normal/data usage, dimensions, source digest, transcode target, and transcode version are part of a texture cache key. Live GPU texture-cache scope remains one active document; eligible decoded/transcoded mip data may also use the persistent derived cache below.

## Persistent derived-data cache

The viewer maintains a best-effort per-user cache under `%LOCALAPPDATA%\Binbuf\Preview 3D\DerivedCache\v1`. It is enabled by default and may be disabled or cleared from the keyboard-accessible overflow menu. The enabled flag is the only persisted viewer preference and is stored in a bounded, versioned, atomically replaced settings file adjacent to—not inside—the cache. A missing/corrupt setting restores the default. The Shell thumbnail provider never reads or writes either location.

An opaque entry key covers primary and geometry/material dependency version evidence, parser/normalizer versions, enabled feature set, and cache schema. A cheap source preflight discovers and safely opens current dependencies before a hit is accepted. The fast identity path requires a stable local volume/file ID plus an accessible change-journal ID and per-file USN, all captured from the open handles together with length; last-write time alone is never sufficient. If that evidence is unavailable or the journal was reset, the reader must verify a full streaming content digest before using the entry, or treat it as a miss when hashing would exceed the current load budget. The entry stores only opaque identity digests, not dependency names or paths. An entry contains only normalized coarse proxy data, bounds/chunk catalog, eligible reusable fine chunks, and decoded/transcoded texture levels. It contains no original encoded file bytes, source path, node/material names, or author metadata. Geometry is GPU-independent; any texture variant is keyed by DXGI format and decoder/transcoder version.

Cache data is untrusted. Before any byte reaches an upload batch, the reader validates the manifest, version tuple, every current source/dependency version token or required content digest, entry and section lengths, checked offsets/counts, per-section SHA-256, normalized-scene invariants, and generation. An absent, stale, partial, corrupt, unsupported, or over-budget entry is a miss. Only then may a cache hit publish its verified coarse proxy. Optional background source verification may continue, but it is not a substitute for the identity/content validation required by the hit. Warm-cache performance gates apply to fixtures on volumes that provide the fast identity path and report the validation mode separately.

The default global soft cap is 10 GiB and no entry may exceed 2 GiB. The cache stops admitting writes and schedules LRU eviction when the cap would be exceeded or free disk falls below the greater of 10 GiB and 10% of the volume. Cache maintenance is low-priority, cancellable, rate-limited, and never delays first geometry, Ready, close, or shutdown. Writers create current-user-only, non-executable temporary files, flush a complete manifest, and atomically rename; abandoned temporaries are removed on the next launch. Clear cached previews closes active cache leases, deletes entries through the cache coordinator, and reports completion without deleting source files or the Windows thumbnail cache.

## Input and camera concurrency

The UI thread converts pointer, wheel, keyboard, and capture changes into compact input events. The render thread drains them before camera update. Move events may coalesce; button, key, and capture transitions may not. Camera math never reads HWND state directly.

Bounds events update camera scale and target through AppState. A provisional-to-verified correction auto-frames only if the camera's userInteractionEpoch has not changed since open. Near/far behavior derives from verified scene scale with finite clamps so millimeter and planetary coordinate ranges do not destroy depth precision.

## Device loss and recovery

Every device-facing call that can fail is checked. On DXGI_ERROR_DEVICE_REMOVED or RESET:

1. Stop accepting uploads and capture GetDeviceRemovedReason plus DRED breadcrumbs/page-fault data in the local diagnostic log.
2. Publish a non-blocking recovering state to the UI.
3. Retire the device lanes and rebuild the adapter/device/swap chain once.
4. Recreate the coarse proxy and then visible detail from retained normalized cache/source ranges.
5. If recovery repeats, preserve app responsiveness and show a stable GPU failure with Copy diagnostics action.

Recovery never retries in a tight loop. Unsupported hardware fails before file parsing begins.

## Shutdown

Normal frames never flush the GPU. On document close, generation cancellation and fence-based deferred release proceed asynchronously. On process shutdown only, the render and upload coordinators stop accepting work, signal their queues, wait with finite diagnostics timeouts for their last fence values, release D2D/D3D11On12 before D3D12, then close handles. A driver hang must not leave the UI thread waiting forever; shutdown escalates to process exit after diagnostics are recorded.

## Required assertions and telemetry

Developer builds assert queue ownership, generation identity, legal resource-state use, allocator retirement, ring non-overlap, descriptor lifetime, and snapshot fence coverage. ETW events record:

- parse/decode/normalize/simplify/upload bytes and durations;
- derived-cache lookup/validation/write bytes, hit/miss reason, eviction, and warm-open milestones;
- import-worker and compatibility-host start, brokered bytes, shared-section validation, peak commit, cancellation, and exit reason;
- ring occupancy/backpressure and copy batch sizes;
- fence submission/completion latency;
- per-frame CPU time, GPU time, present result, and queue wait source;
- proxy/full-detail readiness;
- local budget, app model bytes, eviction, and allocation failure;
- cancellation latency and stale events dropped.

Release logging contains no model data or full paths by default.

Primary references: [D3D12 fence-based resource management](https://learn.microsoft.com/windows/win32/direct3d12/fence-based-resource-management), [resource barriers](https://learn.microsoft.com/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12), [multi-engine synchronization](https://learn.microsoft.com/windows/win32/direct3d12/user-mode-heap-synchronization), [DXGI video-memory reservation](https://learn.microsoft.com/windows/win32/direct3d12/residency), [D3D12 root signature limits](https://learn.microsoft.com/windows/win32/direct3d12/root-signature-limits), [DirectX shader model 6.6 dynamic resources](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html), and [D3D12 Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator).

# Decisions and risks

## Decision record policy

These decisions are accepted for the MVP. A change that invalidates an architecture invariant, requirement, release limit, public registration identity, or parser/security boundary must update this document and the affected design/test evidence in the same change.

## ADR-001 — native Win32/C++ viewer

**Status:** Accepted by product pivot.

The interactive viewer is a standalone x64 Win32/C++20 application using Direct3D 12. Flutter, Dart isolates/FFI, Chromium, and a hybrid UI runner are removed from the target architecture. A separate zero-capability AppContainer native import host is permitted only for lazy OpenUSD compatibility work and has no UI or persistent lifetime.

Reason: startup, frame ownership, memory mapping, cancellation, copy-queue submission, residency, and device recovery all need one native lifetime/type system and explicit scheduling. This also removes the original rationale for a warm Flutter daemon.

Consequence: the product owns native window chrome, accessibility, layout/overlay, graphics synchronization, and crash handling. The Visual Studio wizard project is scaffolding, not the final structure.

## ADR-002 — separate UI, render, upload, and worker lanes

**Status:** Accepted.

The UI thread owns only HWND/message/input/AppState. The render thread owns swap-chain presentation and the direct queue. One upload coordinator owns copy submission/ring retirement. A bounded adaptive worker pool inside the trusted process owns cache validation, file-handle/mapping setup for handles it will duplicate outward, chunk-descriptor copy/validation, and LOD/residency scheduling — not third-party parsing (see [ADR-014](#adr-014-appcontainer-import-processes-are-the-parser-security-boundary-not-threads)). A loader-owned broker exclusively controls the import-worker and compatibility-host processes and their shared sections.

Reason: moving parsing off the UI thread is insufficient if Present, resizing, default-heap allocation, or a queue wait can still block it. Exclusive queue/list ownership also makes fence lifetime auditable. Separately, moving parsing off any thread of the trusted process — not just the UI thread — is necessary because a background thread shares the process's address space, window, and GPU device with everything else the user has open; see ADR-014.

Consequence: cross-lane communication uses bounded value/handle queues and immutable snapshots. Some additional latency of one frame is preferable to shared mutable scene state.

Rejected: running Present in the UI message pump; permitting arbitrary workers to record/submit copy lists; one coarse global mutex around a mutable scene; treating a background thread inside the trusted process as sufficient containment for a parser.

## ADR-003 — mapped input is a bounded I/O primitive, not a zero-memory promise

**Status:** Accepted.

Tier A sources use CreateFileMapping/MapViewOfFile and mapping-window leases. The implementation does not issue an eager source-sized read or retain unbounded normalized arrays.

Reason: mapping lets the OS page cache service validated ranges and avoids a redundant complete file buffer. However, touched pages still consume memory and most formats require normalized/decompressed CPU and GPU data.

Consequence: mapped bytes never go directly to a draw call. Budgets cover scratch, archive expansion, textures, upload heap, default heap, and cache separately. Raw source pointers cannot outlive a lease.

Rejected: std::ifstream/read-whole-file ingestion for large assets; describing MapViewOfFile as preventing all out-of-memory failures.

## ADR-004 — publish only fence-complete GPU resources

**Status:** Accepted.

A resource becomes part of a renderable SceneSnapshot only after the copy fence covering all of its bytes and state preparation has completed. The direct queue normally draws the prior LOD rather than waiting for an incomplete upload.

Reason: a direct-queue Wait on every streamed copy serializes visible progress behind transfer and can turn asynchronous ingestion into stutter. Fence-complete publication gives a simple lifetime invariant.

Consequence: parent/proxy LODs stay alive longer and snapshots/deferred release need dual-fence accounting. Rare unavoidable cross-queue waits require a measured exception.

Rejected: atomically swapping a raw MeshInstance immediately after copy submission; making the direct queue wait for the next detail chunk each frame.

## ADR-005 — proxy plus view-prioritized residency

**Status:** Accepted.

Tier A imports build an always-resident coarse proxy and bounded spatial detail chunks. Fine LOD/texture mips are requested by visible error and evicted against the current DXGI budget.

Reason: multi-gigabyte source data can exceed local video memory even when ingestion is perfectly asynchronous. Uploading the complete model is not a viable product contract.

Consequence: full detail may never be simultaneously resident. The UI accurately reports reduced/refining detail. Chunk-local origin rebasing and parent-until-child-ready rendering are required.

Rejected for MVP: fail when full geometry does not fit; automatic whole-model downsample only; virtual-texture/Nanite-like hierarchy, mesh shaders, DirectStorage, or an unbounded disk cache.

## ADR-006 — format-specific importer stack

**Status:** Accepted, with Gate 3/4 validation items below.

| Family | Choice | Rationale |
| --- | --- | --- |
| glTF/GLB | fastgltf + Google Draco + KTX/Basis + libwebp | glTF-specific validation/data sources, common geometry/texture compression, and mapped-file-friendly API |
| STL/PLY | Product parsers | grammars are bounded enough for controlled binary streaming, safe text tokenization, and representative proxy sampling |
| OBJ/MTL, FBX | ufbx | single bounded C implementation, explicit allocation/progress controls, static FBX and OBJ/MTL support |
| 3MF | lib3mf | official 3MF Consortium implementation/API with required preview extensions |
| USD/USDA/USDC/USDZ fast path | TinyUSDZ | small common static subset, packaged-format support, and memory budget |
| broader USD compatibility | OpenUSD in AppContainer host | materially broader composition while keeping weight, crashes, paths, and memory outside the viewer |
| expanded raster textures | DirectXTex/inbox WIC/libwebp | deterministic app-owned BMP/TIFF/TGA/DDS/HDR/WebP coverage without arbitrary installed codecs |
| LOD/mesh compression | meshoptimizer | bounded cluster simplification and supported EXT_meshopt decode |
| GPU allocation | D3D12 Memory Allocator | suballocation/budget utilities without ceding product policy |

Reason: no single broad importer has the best performance, limits, fidelity, dependency weight, and attack surface for every format.

Consequence: Model Core owns one adapter contract and consistent budgets/errors. Library-native objects and exceptions do not cross it. None of these libraries link into the trusted viewer process at all: every one of them, including the "fast path" adapters and meshoptimizer, links only into `Preview3DImportWorker.exe`, and OpenUSD links only into `Preview3DImportHost.exe`. Neither set of objects crosses either process boundary — only the normalized wire-format chunks in [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md) do. Each dependency gets its own fuzz/conformance gate, and compressed payloads have decoded-size limits independent of source size.

Rejected: Assimp as a universal runtime importer; Autodesk FBX SDK due package/redistribution/weight concerns; loading OpenUSD into the viewer or thumbnail provider; using arbitrary installed WIC codecs; loading any of these libraries directly into the trusted viewer process on the theory that a background thread is isolation enough (see [ADR-014](#adr-014-appcontainer-import-processes-are-the-parser-security-boundary-not-threads)).

Primary upstream references: [fastgltf](https://github.com/spnda/fastgltf), [Google Draco](https://github.com/google/draco), [KTX-Software](https://github.com/KhronosGroup/KTX-Software), [ufbx](https://github.com/ufbx/ufbx), [lib3mf](https://github.com/3MFConsortium/lib3mf), [TinyUSDZ](https://github.com/lighttransport/tinyusdz), [OpenUSD](https://openusd.org/release/), [DirectXTex](https://github.com/microsoft/DirectXTex), [meshoptimizer](https://github.com/zeux/meshoptimizer), and [D3D12 Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator).

## ADR-007 — CPU Explorer thumbnails

**Status:** Accepted.

The in-process COM provider renders a deterministic, sampled mesh/point-cloud thumbnail on the CPU and creates no D3D device. It may use the strictly bounded in-process decoders but never starts the import worker, the OpenUSD compatibility host, or accesses the viewer cache. It relies on Shell's default policy of loading thumbnail handlers into an isolated per-handler surrogate process, not into `explorer.exe`, as its actual crash-containment boundary; the installer MUST NOT set `DisableProcessIsolation` for any of its CLSIDs.

Reason: driver/device creation and asynchronous GPU lifetime inside Explorer's surrogate add failure modes disproportionate to a small thumbnail. CPU sampling has a clear deadline and memory cap. Running in-process (registered as InprocServer32) inside the Shell's surrogate, rather than inside `explorer.exe` itself, is what makes an unavoidable parser bug survivable — the same crash-containment reasoning as ADR-014, applied to a process the product does not own. It is not the same guarantee ADR-014 makes: the Shell surrogate is not a zero-capability AppContainer, so it contains a crash rather than bounding what a compromised parser could do with the invoking user's own privileges — the provider's defense against that case is the bounded reads, checked parsing, and no-network/no-path-authority rules in [05-thumbnail-provider.md](./05-thumbnail-provider.md), not the process boundary itself.

Consequence: thumbnail shading is an approximation and external sidecars are unavailable through IInitializeWithStream. Explorer may show a generic icon when safe bounded import is impossible. Release verification includes confirming, on a clean install, that each CLSID actually loads into the surrogate and that isolation was not disabled.

Rejected: launching the viewer to capture a frame; IPC to a background renderer; GPU creation within the provider; reading arbitrary siblings based on untrusted embedded paths; disabling Shell's process isolation for this handler for any performance reason.

## ADR-008 — no warm daemon or tray

**Status:** Accepted.

The native viewer cold-starts when needed and exits after the last window closes. It forwards opens only while a visible primary instance exists. The compatibility host starts only on demand for a current USD generation and is terminated at generation/idle/shutdown boundaries; it is not a warm daemon.

Reason: native startup removes the Flutter VM cold-start pressure that motivated the daemon. Zero background footprint and simpler update/security/lifecycle behavior outweigh a speculative few milliseconds.

Consequence: cold startup is a formal 150/200 ms performance gate. IPC remains for visible-instance reuse and replacement semantics, not persistence.

Rejected: hide-on-close, start-at-login, system tray exit menu, helper launcher service.

## ADR-009 — system D3D12 runtime and feature level 11_0

**Status:** Accepted.

The MVP targets Windows 11's system D3D12/DXGI runtime and hardware feature level 11_0 or newer. It uses conservative descriptor tables and offline DXC shaders.

Reason: this covers the target OS without shipping an additional runtime contract, and it avoids making bindless/mesh-shader-era hardware a minimum requirement. Direct descriptor-heap indexing needs both shader model 6.6 and resource binding tier 3, neither of which feature level 11_0 guarantees, so descriptor tables are the correct baseline rather than a stopgap.

Consequence: modern features that require the Agility SDK or later binding tiers are not assumed. Descriptor tables remain the rendering baseline for the life of the MVP; an optional bindless capability tier behind a runtime feature check is a possible later addition evaluated on its own performance evidence, not a planned replacement. A later feature need can revisit packaging with a new ADR. Per-frame camera/pass data goes through a per-frame CBV, with root constants reserved for small per-draw IDs given the 64-DWORD root-signature budget; the normalized-chunk vertex layout is a small closed enumeration of explicit layouts rather than one universal fixed-size struct (see [04-rendering-and-streaming.md](./04-rendering-and-streaming.md)). D3D12MA is one device-wide allocator instance but manages multiple underlying DEFAULT heaps segregated by resource class, as Resource Heap Tier 1 hardware requires.

Rejected for MVP: automatic WARP user fallback; Agility SDK solely for novelty; D3D11 primary renderer; adopting bindless resource access as the baseline path; one physical DEFAULT heap for every resource class; packing per-frame camera/pass data into root constants; a single universal vertex struct sized for the worst-case format.

## ADR-010 — D3D11On12/Direct2D for compact overlays

**Status:** Accepted. Required validation spike 1 has now been run and passed; see the evidence note below.

The render thread uses D3D11On12 plus Direct2D/DirectWrite over the same direct queue for title/status/error text and controls, after D3D12 scene commands and before final frame-fence signal/present.

Reason: native text layout, scaling, high contrast, and typography are costly to recreate correctly as a custom glyph engine. The overlay surface is small and changes infrequently.

Consequence: one render thread must own all interop contexts and follow Acquire/Release/Flush ordering. Gate 1 profiling may replace this with a product glyph atlas only if interop demonstrably violates frame/lifetime gates; accessibility remains UI Automation either way.

Evidence (spike 1, measured on the 143 Hz performance reference with a dedicated render thread, 2000 frames per configuration, first run after each build discarded and four runs averaged):

| Configuration | mean frame interval | p95 | overlay CPU/frame |
| --- | ---: | ---: | ---: |
| overlay off | 6.96 ms | 7.08 ms | — |
| D3D11On12 interop only | 6.95 ms | 7.06 ms | 0.09 ms |
| interop plus a full chrome's worth of Direct2D/DirectWrite | 6.94 ms | 7.24 ms | 0.93 ms |

The Acquire/Release/Flush handshake itself is ~0.09 ms; the Direct2D drawing accounts for the rest, so the overlay's cost is a function of what it draws rather than of the interop. p95 with the overlay is within NFR-04's ≤8.3 ms gate and the glyph-atlas escape hatch is not triggered — but the margin is not large: repeat runs of the chrome-scale case range roughly 7.2–9.0 ms p95, occasionally crossing the gate. The synthetic stand-in redraws all ~250 primitives with a changing brush colour every frame, which the real chrome does not; this must be re-measured against the ported chrome, and if that margin does not improve, dirty-tracking the overlay rather than redrawing it per frame is the first thing to try. Direct2D was also confirmed to attach to the `DXGI_FORMAT_R8G8B8A8_UNORM` swap chain specified in [04-rendering-and-streaming.md](./04-rendering-and-streaming.md) — no BGRA swap-chain format change is required. Resize and D3D12-debug-layer validation are covered by the automated overlay suite.

Rejected: rendering the 3D scene through D3D11; overlay calls from the UI thread; runtime HTML UI.

## ADR-011 — signed per-machine MSI, user-controlled defaults

**Status:** Accepted for the original MVP; distribution is superseded for the
scope-limited MVP by ADR-016.

The app and seven stable thumbnail CLSIDs are installed machine-wide by MSI. Capabilities, Open With ProgIDs, and thumbnail handlers are registered; setup never overwrites UserChoice/default-app selection.

Reason: COM shell registration needs stable installation paths and repair/uninstall ownership. Windows explicitly controls defaults.

Consequence: elevation is required, handler conflicts are preserved/reported, and install lifecycle receives its own VM gate.

Rejected: self-registration with regsvr32, application-side first-run registry mutation, forcing defaults, portable ZIP as MVP distribution.

## ADR-012 — bounded persistent derived-data cache

**Status:** Accepted for the revised MVP.

The viewer keeps a default-enabled, per-user, bounded cache of verified coarse proxies, normalized chunk/catalog data, and eligible decoded/transcoded texture levels. Entries are opaque, versioned, checksummed, source-version-bound, LRU-evicted, and revalidated as untrusted input. Fast hits require stable local file/change-journal version tokens; otherwise a full content digest is required or the entry is a miss. The user can disable and clear the cache. The thumbnail provider does not access it.

Reason: repeat parsing, simplification, texture decoding, and compatibility-host startup dominate reopen latency for medium/large assets. Caching derived data produces a larger real-world improvement than further optimizing already asynchronous UI startup.

Consequence: the viewer owns up to a 10 GiB soft-cap of renderable model-derived data under LocalAppData, must document that privacy/storage fact, stop admission under low disk space, survive corrupt/hostile entries, and preserve cache-schema compatibility deliberately. Cache writes are never on the critical path to first geometry or Ready.

Rejected: an unbounded cache; storing original source bytes or full paths/names; sharing writable cache state with Explorer; trusting cache records because the current user owns them; deleting arbitrary user-profile data from elevated MSI code.

## ADR-013 — hybrid USD import with a second, heavier AppContainer host

**Status:** Accepted for the revised MVP; superseded in scope by [ADR-014](#adr-014-appcontainer-import-processes-are-the-parser-security-boundary-not-threads), which makes AppContainer isolation the rule for every parser rather than an OpenUSD-specific exception.

TinyUSDZ remains the common-static fast path, but it now runs inside `Preview3DImportWorker.exe` alongside every other format adapter rather than in-process in the viewer. A typed unsupported-composition result from the worker may start the separate `Preview3DImportHost.exe`, which links pinned OpenUSD, runs under a zero-capability AppContainer token in a kill-on-close Job Object, resolves only parent-brokered read-only local assets, and returns bounded normalized chunks through revalidated shared sections using the same broker protocol as the worker.

Reason: generic USD claims are not credible with the TinyUSDZ subset alone, while loading the full OpenUSD stack into the same process as every other format adapter would increase startup weight, plug-in/path authority, memory, and crash impact for every format, not just USD. Keeping it a separate executable rather than folding it into `Preview3DImportWorker.exe` keeps the worker's steady-state footprint small for the common case.

Consequence: the installed product gains a second signed executable and app-local dependency payload beyond the general import worker, but shares that worker's protocol, wire schema, and validation code rather than inventing its own. A failure affects only the current document. USD remains a documented static preview subset rather than complete authoring fidelity.

Rejected: generic USD marketing backed only by TinyUSDZ; in-process OpenUSD in the viewer or Shell provider; giving the host unrestricted filesystem/network access; a permanent import daemon; treating OpenUSD as the only format that needed this level of isolation.

## ADR-014 — AppContainer import processes are the parser security boundary, not threads

**Status:** Accepted for the revised MVP, adopted from external security review.

Every third-party format parser and decoder — fastgltf, the STL/PLY product parsers, ufbx, lib3mf, TinyUSDZ, the Draco decoder, the KTX/Basis transcoder, libwebp, and DirectXTex/WIC, in addition to OpenUSD — runs only inside `Preview3DImportWorker.exe` or `Preview3DImportHost.exe`, both zero-capability AppContainer processes launched suspended or job-assigned before they process input, reachable from the trusted viewer only through a versioned wire-format protocol whose chunks the host copies to private memory and independently validates before trusting them. The `interactive-viewer` GLB vertical slice's current practice of parsing on a background `std::thread` inside `Preview3D.exe` is accepted as a prototyping shortcut for validating the responsive-shell/camera architecture, not as the shipped ingestion design, and must not be extended to additional formats before this ADR's architecture lands.

Reason: a background thread is a **scheduling boundary** — it stops a slow or blocking operation from freezing the UI/render threads — not a **security boundary**. A memory-corruption bug in a parser running on any thread of `Preview3D.exe` still executes with that process's full privileges: the same address space as the window, the D3D12 device, the active-instance IPC listener, and (once implemented) the persistent derived cache. Threading changes nothing about what a successful exploit can do; only a process boundary with a different, more restricted token does. This generalizes the isolation the design already required for OpenUSD (ADR-013) to every parser, on the same reasoning: OpenUSD was never uniquely dangerous, it was simply the first format whose composition complexity made the risk obvious.

Consequence: `Preview3D.exe` links no third-party format-parsing or decoding library. Format adapters, meshoptimizer, and texture decode/transcode all move out of the trusted process and into `Preview3DImportWorker.exe`; only OpenUSD gets its own separate, heavier process per ADR-013. The Gate 2 exit criteria in [10-delivery-plan.md](./10-delivery-plan.md) include proving this sandbox — launcher, protocol, and a synthetic hostile-worker acceptance test — before any real parser is wired to it, so a real parser's own bugs can never be mistaken for correct containment. NFR-14 and the invariants below are written against every import process uniformly, not against OpenUSD as a special case.

Rejected: treating "parsing happens off the UI thread" as satisfying NFR-14; treating a shared-memory section as trusted merely because it "belongs" to the host after the worker signals completion (a compromised worker keeps write access to a mapped section for as long as it stays mapped on either side); isolating only OpenUSD while leaving every other parser in-process; deferring this refactor until after broad format support ships, which would require re-touching every adapter's threat model a second time.

## ADR-015 — bounded coarse/full sampling and the mandatory coverage floor

**Status:** Accepted for TSK-206's explicitly permitted sampling/acceptance revision (2026-09-15).

The limited MVP uses a bounded representative preview, a complete coarse proxy,
and fence-complete full source regions. It constructs no intermediate LOD,
meshoptimizer hierarchy, cross-fade, or persistent derived cache. Preview source
strata contain at most 4,096 primitives / 1 MiB, with an independent 8 MiB GPU
allocation ceiling; they remain provisional and never
authorize replacement of an existing document. A full worker scan crosses the
copy-then-validate boundary with scan payloads that the upload lane discards.
Worker-owned spatial/source samples retain at most 64 MiB of geometry. Only after
all regions, source counts, bounds, dependencies and coarse totals are accepted
may a complete coarse representation replace the old document. Fine regions are
re-decoded through the same pinned primary and broker-approved sidecar handles;
their normalized checksums/counts/bounds must match the scan.

The original 5% ceiling and unconditional component coverage cannot both hold for
Draw-heavy's 2,048 separate single-triangle instances: 102 triangles cannot cover
2,048 nonempty instances. Define the mandatory coverage floor as one primitive
per nonempty bounded source region (including mesh/node/material occurrences).
For 20 or more source primitives the density ceiling is
`min(2,000,000, max(floor(valid / 20), nonemptyRegions))`; documents below 20 retain
all valid primitives. The 64 MiB reserved geometry ceiling includes allocation
alignment and remains hard. Coarse regions share vertex/index buffers within
each publication rather than allocating separate heaps for tiny samples.
Ordinary dense fixtures must still satisfy their original 5% manifest
ceiling; the exception applies only when that ceiling would omit an entire
region. If the mandatory set cannot fit, fail with a controlled resource limit.
This revises density acceptance, not isolation, path authority, allocation caps,
generation filtering, or fence safety. TSK-207 will admit fine detail against
live DXGI accounting and may reduce the reserved sampling density.

Protocol v5 closes geometry roles and validates preview/scan/coarse/full order,
stable identities, source provenance, subset bounds, exact completion totals and
terminal coverage. A fine region suppresses its coarse parent only after the
upload coordinator has completed its copy fence; both representations remain
available and fine eviction restores the parent at a frame boundary. Displaced
and evicted resources retain their applicable direct/copy fence lifetimes.

Revised acceptance is enforced by `CoarseProxyTests.cpp` in both test projects,
the reordered GLB/STL/both-endian PLY mesh/point manifest generated by
`tests/fixtures/coarse.py`, hostile-worker coarse catalog attacks, and
`tests/app-smoke/coarse.py`'s cancel/failure/handoff/refinement/eviction checks.
The performance-reference timing gates remain unchanged and unqualified here;
variable-length PLY faces still require a bounded record-offset scan for preview.

## ADR-016 — scope-limited MVP uses a signed portable archive

**Status:** Accepted for the scope-limited MVP (2026-09-16).

The limited MVP is delivered as one Windows 11 x64 portable ZIP. It contains
only `Preview3D.exe`, the general import worker, their resolved app-local DLL
closures, usage/support-limit documentation, notices/licenses, a CycloneDX
SBOM, and a hash manifest. Viewer and worker are Authenticode-signed for a
release candidate and the completed ZIP has an adjacent SHA-256. The MSI,
thumbnail provider, compatibility host, tests, symbols, debug runtimes,
persistent cache, registrations, and deferred format payloads do not enter the
archive. The pinned vcpkg baseline and installed ABI records are provenance;
the package target also rejects an unresolved non-system PE import.

The viewer stays at the archive root. The worker and its private DLL closure
live under `worker\`; first use creates or reopens only the current user's
`Binbuf.Preview3D.ImportWorker` zero-capability profile and grants that SID
inheritable read/execute on `worker\` only. It does not grant the profile the
package root, source/model directories, or user-data trees. Models and local
sidecars remain brokered read-only handles. Profile creation and ACL changes
need no administrator rights. Closing the viewer closes the kill-on-close jobs;
the included cleanup command removes the worker-directory ACE and current-user
profile, while deleting the extracted directory removes all package files.

Reason: the updated scope explicitly defers shell integration, thumbnails,
Tier B breadth, compatibility host, and persistent cache. An MSI would imply
installation/repair/registration behavior that is neither included nor tested.
The portable shape preserves the retained isolated-import boundary and makes
the much smaller runtime/dependency surface auditable without claiming the
original MSI gates.

Consequence: there are no file associations, Explorer thumbnails, repair,
upgrade, machine-wide install, or uninstall entry. The user launches the EXE,
uses Ctrl+O/drag-drop/a path, and may remove the extracted directory. Clean
standard-user/offline VM evidence and signatures still gate the final candidate;
an unsigned archive is labeled engineering-only. ADR-011 remains the decision
for any future return to the original installed product scope.

Rejected for this scope: shipping the stub MSI/shell projects; granting the
AppContainer the archive root or source tree; copying every DLL from the shared
build output; packaging as an ordinary build side effect.

## ADR-017 - STEP/STP is a bounded, self-contained static preview

**Status:** Accepted for the STEP viewer slice (STEP-006).

Decision: `.step`/`.stp` support is the **bounded static STEP preview subset**
implemented by the dedicated, zero-capability `Preview3DStepHost.exe` over the
pinned constrained OCCT 7.8.1 closure. It accepts clear-text ISO 10303-21
AP203/AP214/AP242 product/assembly structure, B-rep geometry, supported AP242
tessellated representations, occurrence transforms, representable
colors/transparency, and a verified metre factor with `UpAxisId::Unknown`.
Geometry is tessellated in the host under the versioned
`StepTessellationProfile`; only normalized wire records reach the trusted
viewer.

The slice is **self-contained only**. External STEP documents are explicitly
**out of scope**: STEP-001 established that OCCT's external-document resolver is
path-based and protected with no stream/callback hook, so supporting them would
require a new brokered-resolver design and product-owner approval. Any
`FILE_POPULATION`/`DOCUMENT_FILE` declaration returns
`UnsupportedRequiredFeature` from the product-owned `StepPart21Preflight` before
OCCT sees it; the AppContainer/handle-only boundary is not weakened to gain it.
`StepFuzz` fuzzes declaration discovery so the no-go cannot be bypassed into a
filesystem or network access.

Shape healing is **off**. STEP-006 compared no healing, a narrowly pinned
`ShapeFix`/`XSAlgo` sequence, and broad automatic healing. Only no healing has a
deterministic, producer-independent result: invalid geometry fails typed rather
than being mutated. A future pinned-healing decision must bump the
profile/importer version because output could change.

Consequence: public text must call this the supported static STEP preview
subset, not general STEP/CAD authoring support, and must state the
external-reference exclusion, the Tier-B limits, and the lack of PMI/editing/
Explorer thumbnails. IGES/IFC/JT/native CAD formats, a CAD tree/property
browser, saved views, exact measurement, and export remain separate future
plans. STEP-007 exposes the extension to the viewer, activation, and installer
(`Binbuf.Preview3D.STEP.1` for `.step`/`.stp`); Explorer thumbnails remain
STEP-009 and are deliberately not registered yet.

## TSK-209 compressed glTF dependency decisions

TSK-209 completes the R-22 checklist for the two compressed glTF dependencies
that now ship in the import worker. `meshoptimizer` 1.2 is retained under its
MIT license for `EXT_meshopt_compression`; `libwebp` 1.6.0#3 and its packaged
`libsharpyuv` runtime are retained under the upstream BSD-style license for
static `EXT_texture_webp`. Both versions remain pinned by the existing vcpkg
baseline. Their notices and generated SPDX records under the installed triplet
are inputs to the release SBOM/notice bundle. `directxtex` was not used by the
limited MVP after TGA/DDS/HDR stayed deferred, so it was removed from
`vcpkg.json` rather than carried as an unreviewed shipping dependency.

Both adapters link only into `Preview3DImportWorker.exe` and the isolation test
binary. Meshopt validates count/stride/mode/filter, checked decoded extent and
the smaller of its 512 MiB unit cap and remaining worker scratch allowance
before allocating or entering the pinned decoder. Decoded bufferViews are
charged for their full retained lifetime. WebP validates RIFF/WebP sniff and
declared MIME, rejects animation, decodes into caller-owned RGBA memory, caps a
single chain at 32 MiB / 2,048 pixels per axis, and shares the existing 256 MiB
encoded, 128 MiB decoded and one-billion-pixel generation budgets. The worker
Job Object remains the backstop for decoder-internal allocations. Cancellation
is checked before/after each library call and throughout mip generation; TSK-301
still owns interruption inside a library call.

Required meshopt data fails closed on an unavailable range, invalid stream or
limit. Optional meshopt data may use its specification-required core fallback;
missing fallback geometry still fails. WebP is an optional image dependency:
decode/MIME/container failure uses the existing bounded semantic fallback and
warning without dropping valid geometry. Unknown required extensions fail;
unknown optional extensions remain bounded warnings. External BIN/image data
continues through sibling-only handle brokering with no worker path/network
authority. Frozen valid/corrupt meshopt and WebP seeds, quantized accessors,
valid/invalid sparse accessors, data-URI limits, required-extension mutation,
and sidecar traversal/network/ADS fixtures cover the enabled attack surface.
The thumbnail provider does not link either decoder or claim compressed-glTF
thumbnail support in this limited MVP.

Performance classification remains Tier A for ordinary/meshopt glTF source
layout, with each compressed stream an independently bounded decode unit;
WebP is worker-decoded texture work with low-mip-first publication. The local
functional/performance smoke is evidence of bounded behavior, not the
reference-system startup/frame/input qualification reserved for TSK-302.

## Invariants

The following are release blockers if violated:

1. UI thread never maps/parses/normalizes/decodes/allocates GPU resources, submits queues, presents, waits on a worker/fence/device idle, or joins during visible operation.
2. Render thread never waits for ordinary in-progress content; it selects an already Ready representation.
3. Only the upload coordinator mutates the upload ring and submits the copy queue.
4. Only fence-complete resources enter renderable snapshots; old resources survive through last-use fences.
5. Every queue, allocation, archive, graph, image, and source has an enforced bound and cancellation path.
6. A source-derived path cannot escape the selected local model directory or trigger network access.
7. A multi-gigabyte Tier A model does not require source-sized private commit or full-detail VRAM residency.
8. Shell code launches no process, opens no sidecar/path, creates no GPU device, and writes no persistent model data.
9. Closing the visible app terminates the viewer and any import worker/compatibility host; install/uninstall never take control of user defaults.
10. Persistent cache data is never trusted without version, identity, length, checksum, and normalized-scene validation; a cache failure is equivalent to a miss.
11. No third-party format parser or decoder — general-format or OpenUSD — ever runs inside the trusted `Preview3D.exe` process, on any thread; every one runs only in a zero-capability AppContainer import process. All of its dependencies are parent-brokered, and every returned chunk descriptor and its bytes are copied to private host memory and revalidated before the shared section is treated as trustworthy or admitted to the upload/cache path.

## Risk register

| ID | Risk | Probability / impact | Mitigation and evidence | Trigger / contingency |
| --- | --- | --- | --- | --- |
| R-01 | Copy queue shares hardware with graphics, so DMA does not overlap | Medium / Medium | Correctness never assumes overlap; bounded batches; frame/copy ETW on discrete and UMA | If copy batches hurt frames, reduce batch/time slice and staging target; retain old LOD longer |
| R-02 | A Tier B library creates large intermediate state, inside the import worker's Job Object | High / High | Allocation callbacks plus the worker's Job Object commit ceiling, Tier B caps, early spike/peak-memory tests, transient normalized store | Lower per-format limit or replace adapter; never relax global memory invariant |
| R-02b | A memory-corruption bug in any format parser is reachable because it still runs on a background thread inside the trusted viewer, not in an AppContainer process | Medium / Critical | ADR-014 general import-worker refactor completed and proven (Gate 2 sandbox + hostile-worker suite) before Gate 3/4 wires real parsers to it | Do not extend the current GLB-in-viewer prototype to additional formats; block Gate 3 start until the sandbox exit criteria in Gate 2 are met |
| R-03 | TinyUSDZ subset misses required USD assets or differs from OpenUSD normalization | Medium / High | typed fallback, overlap corpus, explicit feature matrix, AppContainer OpenUSD host | Route supported composition to host; narrow documented USD subset if results cannot be made consistent |
| R-04 | Complete proxy misses 5-second target because bounds/simplification require full scan | Medium / High | source-order-independent stratified sampler, parallel bounded scan, early partial proxy telemetry | Tune proxy algorithm/chunking; maintain partial usefulness while treating gate miss as blocker |
| R-05 | Transient/persistent derived stores consume disk or add latency | Medium / High | delete-on-close transient cap; 10 GiB/2 GiB persistent caps, free-space floor, atomic writes, background LRU | Stop cache admission/trim; fall back to uncached or coarse-only without delaying the viewer |
| R-06 | OS video budget drops below reserved proxy set | Medium / Medium | Continuous budget notification, proxy density tiers, low-mip fallbacks | Rebuild smaller proxy and disclose reduced detail; fail only if minimum render set cannot fit |
| R-07 | D3D11On12 overlay introduces synchronization stalls | Medium / Medium | Same-thread ownership, correct final fence, Gate 1 PIX/validation | Replace overlay backend behind UI renderer interface via ADR update |
| R-08 | Parser defect crashes Explorer surrogate | Medium / Critical | stricter stream caps/deadline, CPU path, fuzz/ASan/actual-surrogate soak, no shared state | Disable affected extension handler in servicing release while viewer remains available |
| R-09 | Existing thumbnail handler or locked surrogate complicates MSI | Medium / Medium | non-clobber registration, Restart Manager, unloadable DLL, rollback VM matrix | Install viewer only for conflict; exceptional 3010 rather than terminate Explorer |
| R-10 | Reference corpus understates real CAD topology/material complexity | Medium / High | anonymized representative acquisitions, adversarial graph/layout fixtures, published dimensions | Revise workload and gate before claiming support; no benchmark-specific special case |
| R-11 | Huge coordinates lose precision after float GPU conversion | Medium / Medium | double CPU transforms/bounds, cluster origins, camera-relative matrices, precision fixtures | Reduce spatial cluster size or split transforms further |
| R-12 | Malicious geometry triggers TDR/very long GPU work | Low / High | bounded chunk draw counts, validated commands, proxy density, GPU timing and device recovery | Reduce draw batch complexity/detail; blacklist pathological chunk with visible warning |
| R-13 | Parser/dependency vulnerability or abandoned upstream | Medium / High | pin/SBOM/security watch, wrapper/fuzz coverage, replaceable adapter interfaces | Patch/vendor fix or disable affected format until signed update |
| R-14 | First-window 150 ms target is missed by signing/AV/device startup | Medium / Medium | show native brush before device, lazy nonessential init, measure cold with common security software | Move additional initialization behind first present; revise target only with product approval/evidence |
| R-15 | CPU thumbnail misses quality/time target on complex packages | Medium / Medium | representative sampling, fixed deadline, golden/perf corpus | Reduce triangle sample/supersampling; safely return generic icon |
| R-16 | OpenUSD payload size/startup or host composition misses medium-file targets | Medium / High | lazy launch, `LoadNone`, brokered incremental payloads, warm derived cache, exact performance corpus | keep common stages on TinyUSDZ; lower composed-stage limits or document slower compatibility tier |
| R-17 | Import worker or compatibility host escapes its file/network/process boundary, or a compromised one mutates a shared section after the host's first read | Low / Critical | zero-capability AppContainer, kill-on-close Job Object, broker-only resolver, copy-then-validate chunk acceptance (never re-reading a shared section after the host's copy), fuzz and penetration tests including the Gate 2 synthetic hostile-worker suite | disable the affected format/fallback in servicing release until containment is restored |
| R-18 | Persistent cache discloses derived model content or serves stale/corrupt geometry | Medium / High | current-user ACL, opaque keys/no names, file/change-journal version token or full digest, SHA-256 sections, normalized validation, user disable/clear | disable cache, invalidate schema, and purge affected entries on next launch |
| R-19 | Draco/KTX2/WebP or expanded texture decode creates an expansion/OOM attack | Medium / High | per-payload decoded limits, pinned hardened decoders, cancellation, fuzz corpora, no decoder-global cache | fall back only for optional textures; reject required geometry and disable affected decoder if needed |
| R-20 | PLY/point-cloud or Beam Lattice workloads create excessive primitives/draw cost | Medium / Medium | bounded list/tessellation, point proxy sampling, chunk draw caps, draw-heavy GPU timing | lower point/lattice density, preserve representative proxy, report reduced detail |
| R-21 | A thumbnail CLSID ends up loaded in-process in `explorer.exe` (e.g. a future `DisableProcessIsolation=1` added for a perceived performance win) and a parser crash takes down Explorer instead of a surrogate | Low / Critical | installer/registry review prohibiting `DisableProcessIsolation`, post-install verification that each CLSID loads into the isolated surrogate, documented in [05-thumbnail-provider.md](./05-thumbnail-provider.md) | Remove the offending registry value in a servicing release; treat any such regression as a release blocker, not a waivable perf tradeoff |
| R-22 | A pinned but unused dependency can enter the dependency/license/SBOM and vcpkg autolink surface before its feature has completed limits, error semantics, threat review, corpus, thumbnail, license and performance classification | Low / Medium after TSK-209 | TSK-209 completed that checklist for linked `meshoptimizer`/`libwebp` in the section above and removed unused `directxtex` (and its vcpkg transitive surface) while TGA/DDS/HDR remain deferred | Remove any future dependency when its consuming slice is deferred; treat a shipped binary containing a component without this checklist as a release blocker |

## Required validation spikes

These tasks validate implementation choices; they do not expand scope:

1. Before Gate 1 completion, measure D3D11On12 overlay ordering and cost at 144 Hz, including resize and GPU validation.
2. **Before Gate 2 is called complete** (moved earlier per ADR-014, not deferred to the OpenUSD slice), prove the general-purpose AppContainer import sandbox against a synthetic parser stand-in: suspended launch/creation-time job assignment, zero-capability token, `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` restricted handle inheritance, the versioned wire-format decoder, and the host's copy-then-validate chunk acceptance path, all defeating a synthetic hostile-worker build that mutates sections, replays generations, and lies about layout/offsets. Generic "the parser runs in a worker" must never be asserted before this spike passes.
3. Before Gate 3 implementation, confirm exact fastgltf API/version support for mapped sources, sparse accessors, `KHR_mesh_quantization`, and `EXT_meshopt_compression`; separately measure bounded Draco decode and KTX2/Basis transcode on the compressed corpus, all inside the sandbox proven in spike 2.
4. Before Gate 3 claims Tier-A PLY, spike binary little/big-endian sequential scanning, stratified proxy quality, point rendering, hostile list limits, and 2–4 GiB memory behavior.
5. At Gate 4 start, build capped-memory/cancellation spikes for ufbx static skin/blend evaluation, lib3mf Beam Lattice tessellation, and TinyUSDZ before connecting them to the worker.
6. Before the OpenUSD slice, prove the compatibility host's pinned minimal payload, `LoadNone` startup, brokered resolver, Job Object limits, cancellation/termination, shared-section validation (reusing the protocol/validator already proven in spike 2), and overlap normalization corpus against TinyUSDZ. Generic "USD support" must never imply all OpenUSD semantics.
7. Before enabling persistent writes, prove file/change-journal token availability and invalidation, journal-reset and full-digest fallback cost, corrupt-entry fuzzing, atomic crash recovery, LRU/free-space behavior, warm-open value, and Clear cached previews races.
8. Before Gate 6, benchmark mesh/point and compressed glTF CPU rasterizer prototypes inside the actual thumbnail surrogate, and confirm the surrogate loads out-of-process with no `DisableProcessIsolation` value present.
9. Before Gate 7, validate thumbnail-handler conflict, loaded-DLL/host upgrade behavior, signed/hash-verified OpenUSD payload search lockdown, and cache preservation/removal documentation on clean Windows 11 VMs.

If a spike fails, the team updates the corresponding ADR, product limits, and acceptance test before implementation continues. It may not quietly substitute an unbounded importer or a render-thread wait.

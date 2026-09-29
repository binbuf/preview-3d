# Delivery plan

## Delivery strategy

Build the MVP as measured vertical slices. The first proof is a responsive native D3D12 shell with a synthetic streaming workload; format breadth comes only after queue ownership, bounded memory, publication, cancellation, and telemetry are demonstrably correct.

No phase is complete because code compiles. Each gate has observable exit criteria and regression tests that remain enabled.

## Repository transition

The current interactive-viewer and thumbnail-provider projects are Visual Studio wizard scaffolds. The first implementation change should preserve the user's native C++ pivot while replacing scaffold globals/layout deliberately.

The root `Preview3D.slnx` below and empty `Preview3DImportWorker`, `Preview3DImportHost`, and `ModelCore` projects already exist as buildable placeholders (no parser/broker/AppContainer logic yet); Gate 2 fills them in rather than creating them (see [02-system-architecture.md](./02-system-architecture.md)).

Target root layout:

    Preview3D.slnx
    Directory.Build.props
    Directory.Build.targets
    vcpkg.json
    vcpkg-configuration.json
    .gitignore
    .docs/
      design/
    shared/
      model-core/
      import-broker/
      platform/
    import-worker/
      src/
    compatibility-host/
      src/
    interactive-viewer/
      src/
      shaders/
      resources/
    thumbnail-provider/
      src/
      resources/
    installer/
    tests/
      unit/
      integration/
      corpus/
      performance/
      fuzz/
    third_party/
      notices/
    scripts/

One root solution references:

- ModelCore static library with format-independent normalized-scene/wire-format contracts, and its format adapters (linked only by the two AppContainer import executables, never by the viewer or thumbnail DLL's own copies);
- Preview3D Windows subsystem executable, zero-capability AppContainer Preview3DImportWorker executable (general formats), and zero-capability AppContainer Preview3DImportHost executable (OpenUSD);
- Preview3DThumbnailProvider COM DLL;
- unit, headless adapter, cache, import-broker (worker + host), rendering, IPC, COM, fuzz, and performance executables;
- installer projects.

Only x64 Debug and Release are product configurations. Win32 configurations and user-specific .vcxproj.user/.vs/build-output files are removed from source control. C++20, Unicode, conformance mode, /permissive-, warning level 4, warnings-as-errors for product code, reproducible build options, security flags, and common output directories live in Directory.Build.props/targets instead of drifting between projects.

vcpkg uses a pinned baseline and manifest feature set for available dependencies. A dependency without an acceptable pinned port is vendored at an exact source commit with checksum, license, narrow build target, and update notes. CI restores dependencies into its cache; release builds do not fetch unpinned content.

## Gate 0 — foundation

Deliver:

- root solution/build policy and x64-only project graph;
- product-owned RAII wrappers for HANDLE, mapping/view, COM pointer, event, and checked arithmetic;
- Result/error type, cancellation/generation primitive, bounded queues, clock and allocation-budget interfaces;
- derived-cache schema/key/checksum interfaces and compatibility-host protocol/shared-section schema;
- test framework, fixture manifest, ETW event schema, build/version embedding;
- dependency lock/licensing skeleton;
- repository ignores and CI build/unit-test job.

Exit:

- clean clone builds Debug/Release with the pinned toolchain;
- empty unit suite and one test per foundation primitive pass;
- warnings/security flags are inspected in produced command lines and PE headers;
- dependency/SBOM draft resolves;
- no wizard global mutable window state remains in runtime entry points.

## Gate 1 — responsive native shell

Deliver:

- Win32 UI thread with DPI-v2, immediate #1C1C1E background, custom chrome/system behavior, input event queue, empty/loading/error AppState;
- render thread owning D3D12 device/direct queue/three-buffer swap chain/frame fence and resize;
- D3D11On12/Direct2D/DirectWrite overlay path;
- fixed-shader neutral triangle/cube/point cloud, mouse/keyboard/touch camera orbit/pan/dolly/fit/reset;
- device-start failure and one-shot device recovery surfaces;
- frame/input/heartbeat telemetry.

Exit:

- cold empty launch and file-launch-to-loading UI meet NFR-03 before any real parser exists;
- resize/orbit/close remain responsive during a synthetic CPU saturator;
- D3D debug/GPU validation suites report no error;
- 100%, 150%, 200% DPI, Snap Layouts, Alt+Space, keyboard, high contrast, and reduce-motion checks pass;
- render/UI thread ownership assertions pass under device removal and rapid resize.

## Gate 2 — decoupled streaming proof and import sandbox

Deliver:

- MappedFile/window lease implementation;
- worker pool and generation cancellation;
- upload coordinator, 256 MiB persistent ring, copy queue/fence, DEFAULT buffers;
- fence-complete publication, immutable snapshots, deferred resource release;
- synthetic mapped geometry generator emitting bounded clusters/proxy/three LODs;
- crash-safe bounded derived-cache prototype with hit/miss/corruption/eviction injection;
- DXGI budget monitor, view-priority requester, detail eviction;
- fault injection for delayed copy fences, OOM, budget loss, cancellation, and stale events;
- **the general-purpose AppContainer import sandbox itself**, before any real third-party parser is wired to it: `Preview3DImportWorker.exe`'s launcher (suspended launch or creation-time job assignment, zero-capability token, restricted handle-inheritance list), the broker protocol and versioned wire format from [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md), and the host-side copy-then-validate chunk acceptance path, all exercised against a synthetic in-sandbox generator standing in for a real parser;
- a synthetic hostile-worker build of that same sandbox that deliberately mutates shared-section bytes after the host's first read, replays a stale generation, lies about a chunk's layout/offset, or overruns its Job Object limits, used to prove containment before it can be obscured by a real parser's own bugs.

Exit:

- a synthetic 4 GiB source streams cold and reopens from a verified cache entry while the UI/render performance gates hold;
- ring model/property tests prove no unretired overwrite across wrap;
- direct queue records no ordinary load-time wait on the copy fence;
- a forced 2-second copy delay keeps the previous LOD presenting;
- budget reduction preserves/rebuilds a usable proxy and produces no allocation loop;
- open/close/reopen stress has no stale snapshot, leak, deadlock, or unbounded queue;
- the synthetic hostile-worker build is rejected by the host's validator on every injected fault above, with no corrupted bytes reaching the upload path and no elevation beyond the sandbox's zero-capability token;
- the import sandbox's launcher, protocol, and validator are frozen as the interface real format adapters plug into during Gate 3/4 — a later gate may add adapters behind this boundary but must not weaken it.

This gate validates the architecture, including the parser security boundary, before a real parser's own correctness bugs can obscure a containment failure. Gate 3/4 format work explicitly builds inside the sandbox this gate proves, not beside it.

## Gate 3 — Tier A formats

Deliver, all of it running inside `Preview3DImportWorker.exe` behind the Gate 2 sandbox boundary:

- normalized scene/chunk/material/texture contracts;
- fastgltf GLB/glTF 2.0 adapter and constrained sidecar resolver;
- product binary/ASCII STL and PLY mesh/point-cloud adapters;
- bounded Draco geometry decode and KTX2/Basis transcode paths;
- verified bounds, double-origin cluster normalization, normal/tangent policy;
- meshoptimizer cluster LOD/proxy builder;
- inbox WIC/DirectXTex/libwebp PNG/JPEG/BMP/TIFF/TGA/DDS/HDR/WebP and mip pipeline;
- source-order-independent stratified first-proxy sampling;
- persistent derived-cache production path (validated and written host-side from worker-produced chunks) plus transient Tier-A normalized store inside the worker where needed;
- draw sorting/instancing and measured direct-versus-ExecuteIndirect threshold;
- error/warning mapping and format diagnostics.

Exit:

- A-small, A-medium, A-large GLB/STL/PLY, compressed, warm-cache, draw-heavy, and adversarial-layout fixtures meet all applicable time/memory/frame gates;
- supported glTF feature corpus renders with expected counts/bounds/material snapshots;
- malformed/overflow/cancellation/sidecar security suites pass, including against the worker process rather than an in-process harness;
- no source-sized private heap allocation appears in A-large traces, in either the worker or the trusted process;
- models larger than the current video-memory target remain inspectable through proxy and view refinement;
- the Gate 2 hostile-worker suite is re-run against each newly wired adapter and still passes — a real parser's bugs must not create a path around the copy-then-validate rule.

## Gate 4 — Tier B format breadth

Deliver in independent vertical slices, the first four inside `Preview3DImportWorker.exe` behind the Gate 2 sandbox and the fifth as the separate, heavier `Preview3DImportHost.exe` that reuses the same broker/protocol/validator:

1. OBJ plus MTL through ufbx, including local texture policy.
2. FBX deterministic static start-pose evaluation through ufbx, including supported skin/blend deformation and unified PBR mapping.
3. 3MF Core/Materials/Production/Beam Lattice preview through lib3mf.
4. USDA/USDC/USD and USDZ common static subset through TinyUSDZ, still inside the general import worker.
5. AppContainer compatibility host, brokered resolver, and broader bounded local static composition through OpenUSD, started only on that worker's typed `UnsupportedComposition` result.

Every slice includes adapter wrapper, dependency allocation/I/O/cancel callbacks and Job Object limits, normalized output, unsupported-feature diagnostics, golden scenes, malformed corpus, fuzz seed, cache-version effect, license update, and a re-run of the Gate 2 hostile-worker suite against the newly wired adapter. The OpenUSD slice additionally includes AppContainer restrictions, Job Object enforcement, broker protocol, shared-section revalidation, host crash/timeout behavior, and signed/hash-verified payload tests — the same class of tests slices 1–4 already carry for the import worker, not a stricter bar reserved for OpenUSD alone.

Exit:

- every direct extension opens from command line/dialog/drop/secondary activation;
- conformance and product fixtures preserve expected hierarchy/instances/transforms within the documented subset;
- lower Tier B limits fail safely and do not affect the next open;
- archive bombs, recursive graphs, unsafe references, missing MTL/textures, deformed FBX poses, 3MF lattices, and unsupported/over-limit USD composition have specified outcomes;
- TinyUSDZ/OpenUSD overlap fixtures normalize equivalently, and neither the import worker nor the compatibility host can access an unbrokered file, network, plug-in, or child process;
- UI/render responsiveness and cancellation gates remain satisfied.

## Gate 5 — integrated viewer UX

Deliver:

- old-document-until-proxy handoff;
- progressive phase/progress/warning UI;
- complete camera/bounds correction behavior;
- complete mouse/keyboard/touch camera bindings and cache overflow commands;
- OLE drop and IFileOpenDialog filters;
- pressure/reduced-detail UI;
- actionable failures and redacted Copy details;
- UI Automation providers, focus/live-region behavior;
- session/user-scoped mutex/pipe forwarding and activation;
- asynchronous close with clean process termination.

Exit:

- all acceptance scenarios in [07-user-experience.md](./07-user-experience.md) pass;
- simultaneous launch/IPC spoof/open-storm/cancellation tests pass;
- there is no tray icon, startup task, hidden post-close window, or lingering process;
- automated UI heartbeat shows no load-time message gap above 100 ms;
- accessibility audit passes keyboard, 200% scale, high contrast, and screen-reader smoke.

## Gate 6 — Explorer thumbnail provider

Deliver:

- seven stable COM classes, including PLY, and class factory/lifetime exports;
- bounded IStream backing, adapter routing, geometry sampler;
- deterministic CPU rasterizer and premultiplied BGRA HBITMAP;
- triangle and point-cloud sampling/rasterization plus bounded Draco/KTX2 provider paths;
- Explorer-safe error/deadline/resource behavior;
- COM host harness, golden images, actual surrogate tests.

Exit:

- every extension routes to the intended CLSID;
- thumbnail target/cutoff and 192 MiB scratch cap hold;
- OBJ/glTF external sidecars are never opened from a Shell stream;
- every registered CLSID is confirmed, on a clean installed machine, to load into the isolated Shell thumbnail surrogate rather than `explorer.exe`, and no installed registry value sets `DisableProcessIsolation`;
- malformed/fuzz/parallel/unload soak produces no Explorer/surrogate crash, hang, handle leak, or persistent thread;
- DllCanUnloadNow semantics and GDI ownership tests pass.

## Gate 7 — installer and hardening

Deliver:

- WiX MSI, stable component identities, capabilities/ProgID/Open With/thumbnail registration;
- AppContainer compatibility-host/OpenUSD payload packaging, signing, ACLs, DLL/plug-in search lockdown, and rollback;
- install, repair, upgrade, rollback, handler-conflict, Restart Manager, notification, and uninstall logic;
- Authenticode signing pipeline, SBOM/notices, release manifests;
- ASan/static analysis/Application Verifier, D3D validation, fuzz and dependency-review closure;
- local redacted diagnostics and support bundle behavior.

Exit:

- full installer matrix in [08-installation-and-registration.md](./08-installation-and-registration.md) passes on clean VMs;
- install never changes the selected default app and uninstall removes only product-owned state;
- no normal scenario requires reboot or Explorer termination;
- signed installed hashes match the release manifest;
- threat-model controls and malformed corpus have no open critical/high defect.

## Gate 8 — release candidate

Deliver:

- feature-complete, signed candidate;
- performance/reference traces and compatibility evidence;
- 8-hour mixed-load soaks;
- final format/support/known-limit documentation;
- final cache storage/privacy/clear/uninstall documentation and compatibility-host support runbook;
- recovery/runbook and symbol archive.

Exit:

- all release acceptance checks in [01-product-scope.md](./01-product-scope.md) pass;
- all quality evidence in [09-quality-performance-and-security.md](./09-quality-performance-and-security.md) is linked to the candidate build and fixture hashes;
- no open blocker, security issue, data-corruption issue, Explorer stability issue, UI-thread wait, stale-generation render, or repeatable device-loss loop;
- product, engineering, security, and installer owners sign the same artifact manifest.

## Requirements traceability

| Requirements | Owning gates | Principal verification |
| --- | --- | --- |
| FR-01 formats | 3, 4, 6 | adapter corpus, extension routes |
| FR-02 thumbnails | 6, 7 | COM/surrogate/MSI tests |
| FR-03 viewer activation | 3–5, 7 | shell/dialog/drop/IPC scenarios |
| FR-04/FR-05 responsive isolation | 1–4 | heartbeat, frame traces, ownership assertions |
| FR-06 camera | 1, 5 | deterministic input/visual tests |
| FR-07 progressive safe chunks | 2–4 | fence delay/publication/LOD tests |
| FR-08 over-VRAM inspection | 2, 3 | pressure and A-large runs |
| FR-09 lifecycle | 5 | launch/forward/close stress |
| FR-10 errors/recovery | 3–5 | malformed/reopen/device tests |
| FR-11 static material subset | 3, 4 | golden normalized/render scenes |
| FR-12/FR-13 registration lifecycle | 7 | clean/repair/upgrade/uninstall matrix |
| FR-14 derived cache | 2, 3, 5, 7 | hit/miss/corruption/eviction/clear/privacy tests |
| FR-15 USD compatibility host | 4, 7 | composed corpus, restriction/broker/crash/limit tests |
| NFR-01/NFR-02 threads | 1, 2 | assertions and no-wait telemetry |
| NFR-03/NFR-04/NFR-05/NFR-06/NFR-07 performance | 1–4, 8 | reference benchmark suite |
| NFR-08 residency | 2, 3 | budget-reduction/large fixtures |
| NFR-09 Shell stability | 6–8 | fuzz, surrogate soak |
| NFR-10 accessibility | 1, 5 | UIA/manual audit |
| NFR-11/NFR-12 privacy/reproducibility | 0, 7, 8 | logs/build/signing/SBOM review |
| NFR-13/NFR-14 cache/host safety | 2–4, 7, 8 | cache fuzz, process restriction, protocol and soak evidence |

## Change control

Architecture invariants in [02-system-architecture.md](./02-system-architecture.md) and [04-rendering-and-streaming.md](./04-rendering-and-streaming.md) can change only through a recorded decision update in [11-decisions-and-risks.md](./11-decisions-and-risks.md) plus revised tests. Adding a format or enabled feature requires limits, error semantics, parser threat review, fuzz corpus, thumbnail decision, dependency/license entry, and performance classification.

## Definition of done

A work item is done when product code, tests, telemetry needed to diagnose it, malformed/cancellation paths, documentation, and dependency/license effects land together. A gate is done only with recorded evidence from the intended environment. Deferred work is explicitly placed outside MVP or recorded as a risk with owner/trigger; it is not hidden behind “best effort.”

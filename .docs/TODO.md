# Remaining work

Everything still open, by gate, measured directly against `.docs/design/10-delivery-plan.md`.

This is the forward-looking companion to [PROGRESS.md](./PROGRESS.md): that file records what
happened and what surprised us, this one records what is left. Neither replaces the design docs —
when they disagree, `.docs/design/` wins and this file is what needs fixing.

**Accurate as of `f0d2f86`** ("Let one generation hand over the output window more than once").

Two rules this list is written to, both from the delivery plan itself:

- `10-…:7` — "No phase is complete because code compiles. Each gate has observable exit criteria
  and regression tests that remain enabled." **Exit criteria are work items here, not paperwork.**
- `10-…:287` — "A work item is done when product code, tests, telemetry needed to diagnose it,
  malformed/cancellation paths, documentation, and dependency/license effects land together."

## Where we are

| Gate | Deliverables | Exit criteria | State |
| --- | --- | --- | --- |
| 0 — foundations | 4 of 7 partly open | 1 of 5 open | Primitives landed; CI, SBOM, ETW schema and fixture manifest never did |
| 1 — responsive native shell | 5 of 6 partly open | 5 of 5 open | Renderer built; shell obligations and all evidence outstanding |
| 2 — streaming proof + import sandbox | done | 2 of 8 open | Deliverables complete; three components never wired into the app |
| 3 — Tier A formats | 7.5 of 11 open | 6 of 6 open | **Current gate.** Both structural blockers now closed |
| 4 — Tier B breadth | 5 of 5 open | 6 of 6 open | Not started; no dependency pinned |
| 5 — integrated viewer UX | 10 of 10 open | 5 of 5 open | Not started |
| 6 — Explorer thumbnails | 6 of 6 open | 6 of 6 open | Stub DLL only |
| 7 — installer and hardening | 6 of 6 open | 5 of 5 open | Not started; no `installer/` directory |
| 8 — release candidate | 6 of 6 open | 4 of 4 open | Not started |

**Closing a blocker is not closing a gate.** Gate 3's two structural blockers — the 1 MiB output
section (`e7f99f4`) and the single-terminal-reply window (`f0d2f86`) — are both closed. That
removed what stood in front of the work; the work itself is below.

---

## Gate 0 — foundations

Seven deliverables (`10-…:67-73`). The primitives all landed; four deliverables have a half that
did not, and each is still owed.

- [ ] **CI build/unit-test job** (`10-…:73`). Nothing exists: no `.github/`, no pipeline file, no
      MSBuild test target, no ctest. Both Catch2 binaries are run by hand out of `x64\<Config>\`.
- [ ] **Licensing skeleton and SBOM draft** (`10-…:72`, exit `10-…:80`). No SBOM, NOTICE or LICENSE
      file anywhere. `vcpkg.json` pins versions, which is the dependency-lock half only.
- [ ] **ETW event schema** (`10-…:71`). `FrameStats.{h,cpp}` is a 240-sample mean/p95 ring and is
      explicitly not this. Everything that measures a frame gate downstream — Gate 1's NFR-03
      criteria, Gate 3's exit criterion 1, `09-…:87`'s present-event classification — needs it.
- [ ] **Fixture manifest** (`10-…:71`). The performance corpus it would describe does not exist
      either; see Gate 3 exit criterion 1.
- [ ] **Compatibility-host protocol / shared-section schema** (`10-…:70`). The derived-cache half
      of that line exists as a prototype; the compatibility-host half does not, and
      `compatibility-host/src/main.cpp` is a four-line `return 0;`. Gate 4 slice 5 depends on it.
- [ ] **Allocation-budget and clock interfaces** (`10-…:69`). Adapters enforce their own ad-hoc
      caps (`kMaxFacets`, `kMaxHeaderBytes`, `kMaxPolygonVerticesPerFace`, …) rather than sharing
      an interface, and there is no full Tier-A hard-limit table (`03-…:160-174`).

Done: build policy and x64-only project graph; RAII wrappers for HANDLE, mapping/view, COM pointer,
event and checked arithmetic; the cancellation/generation primitive (`platform/Generation.h`);
derived-cache schema/key/checksum interfaces; test framework; repository ignores. Exit criteria met
except the SBOM draft: clean-clone Debug/Release build on the pinned toolchain, one test per
foundation primitive, warnings/security flags inspected, no wizard global mutable window state.

---

## Gate 1 — responsive native shell

Six deliverables (`10-…:87-92`). Only the render thread and swap chain are fully done; the other
five each have an unbuilt half.

### Deliverables

- [ ] **Compact input event queue.** Not built — `RenderThread.h:68-72` says so in the source: the
      shared-camera mutex "is the interim."
- [ ] **Port the real chrome onto the D2D overlay bridge.** The bridge is built and correct
      (`D3D11On12Overlay.{h,cpp}`, `ID2D1Device` + a bitmap per back buffer, rebuilds on
      `D2DERR_RECREATE_TARGET`), but it only ever draws `--overlay-spike` stand-in primitives sized
      to approximate the real thing (`D3D12ViewerPath.h:76-79`). The ~750 lines of real drawing in
      `Renderer.cpp` are unported. **Consequence today: an import failure under `--d3d12` draws no
      error card at all** — which is also Gate 3 exit criterion 3 and FR-10.
- [ ] **Neutral point-cloud draw.** `D3D12ViewerPath.cpp:972` — `continue; // point clouds /
      unrecognized layouts: a later slice`.
- [ ] **Device-start failure surface and one-shot device recovery.** `RenderThread.cpp:291` —
      "Device-loss handling, which is the real answer, is a later chunk." No
      `DXGI_ERROR_DEVICE_REMOVED`/`RESET` path, no DRED capture.
- [ ] **Frame/input/heartbeat telemetry.** `FrameStats.{h,cpp}` is a 240-sample mean/p95 ring and
      is explicitly *not* the ETW present-event schema `09-…:87` specifies.

### Exit criteria — none currently demonstrable

- [ ] Cold empty launch and file-launch-to-loading UI meet NFR-03 (p95 ≤150 ms / ≤200 ms). No
      measurement harness exists.
- [ ] Resize/orbit/close stay responsive under a synthetic CPU saturator. No such test.
- [ ] D3D debug / GPU validation suites report no error. Partial only: one `[graphics]` case counts
      `ID3D12InfoQueue` ERROR/CORRUPTION messages across a mixed upload batch.
- [ ] 100% / 150% / 200% DPI, Snap Layouts, Alt+Space, keyboard, high contrast, reduce-motion.
      No audit.
- [ ] Render/UI thread ownership assertions under device removal and rapid resize. Device removal
      is unhandled, so this cannot run.

### Also blocking Gate 1 and Gate 3 evidence

- [ ] **Six dead `renderer.HasModel()` branches under `--d3d12`.** `HasNavigableModel`,
      `FrameSelectedOrAll`, `ToggleShowNativeOrientation`, `CancelOpen`, `ID_VIEW_RESET` and the
      bottom bar all ask a renderer that is never touched in that mode, so the bottom bar and info
      panel never appear — which is where counts and bounds would be read for Gate 3 exit
      criterion 2.

---

## Gate 2 — streaming proof and import sandbox

Every deliverable is done. What is open is evidence, plus three components that were built,
unit-tested, and then never added to `interactive-viewer/Preview3D.vcxproj`.

- [ ] **Wire `DxgiBudgetMonitor` + detail eviction into the app.** Needed for Gate 2's
      budget-reduction criterion *and* Gate 3 exit criterion 5 ("models larger than the current
      video-memory target remain inspectable"), which is unobservable without a live budget in the
      product. Also needs the Pressure fixture to demonstrate anything.
- [ ] **Wire `DerivedCache` into the app.** `DerivedCache.h:25` — "Not wired into
      `Preview3D.vcxproj`/`Preview3D.cpp` yet." Compiled only into `Tests.Unit`.
- [ ] **Wire `WorkerPool` into the app.** `D3D12ImportBridge.h:15-17` — the product still launches
      a fresh AppContainer worker per open, which lands inside the A-small/A-medium latency gates.
      Needs a way to duplicate a *new source-file* handle into an already-running pooled worker;
      only `DuplicateSectionIntoWorker` exists today.
- [ ] **A synthetic 4 GiB source streams cold and reopens from a verified cache entry** while the
      UI/render gates hold (`10-…:119`). Blocked on the derived cache above, the perf harness, and
      Gate 3's scratch-budget work.
- [ ] **Open/close/reopen stress with no stale snapshot, leak, deadlock, or unbounded queue**
      (`10-…:124`). **There is no automatable second-open path**: drag-and-drop needs a real
      `HDROP`, the dialog needs a person, and the singleton/pipe activation of `06-…` is Gate 5 and
      unbuilt. This also leaves `0048b56`'s displaced-model retire path covered by construction
      only, never by a real reopen.

Met: ring wrap/no-unretired-overwrite, direct queue records no load-time copy-fence wait, forced
2-second copy delay keeps the previous model presenting, hostile-worker rejection on every injected
fault, launcher/protocol/validator frozen as the Gate 3/4 interface.

---

## Gate 3 — Tier A formats (current gate)

Definition: `10-delivery-plan.md:130-153`. A flat list of 11 deliverables and 6 exit criteria —
**no slices**; the "Gate 3 slice N" vocabulary in git history was invented during implementation
and does not appear in the design docs.

### Deliverables

- [x] **1. Normalized scene/chunk/material/texture contracts** — *chunks only.*
  - [ ] **`SceneMetadata` does not exist as a type anywhere.** `03-…:131` requires generation,
        source format, source units/up axis, node/material/triangle counts, warnings, and
        provisional/verified bounds.
- [x] **2. fastgltf GLB/glTF 2.0 adapter + constrained sidecar resolver** — *GLB and `.gltf`+sidecars.*
  - [ ] `EXT_meshopt_compression` and `KHR_mesh_quantization` (`GltfAdapter.cpp:922-931` enables
        only `KHR_texture_transform`). Also closes validation spike 3.
- [x] **3. Binary/ASCII STL and PLY mesh/point-cloud adapters** — *import only.*
  - [ ] **PLY point clouds are never rendered** — `D3D12ViewerPath.cpp:972` skips
        `PointList`/`PositionOnly_F32`. `04-…:60` wants depth-tested camera-scaled round splats.
  - [ ] **Both emit exactly one chunk per file**, so neither can use progressive delivery and
        A-large-stl/ply stay unreachable until deliverable 5's splitting lands.
- [x] **4. Bounded Draco decode + KTX2/Basis transcode**
- [ ] **5. Verified bounds, double-origin cluster normalization, normal/tangent policy.** Not
      started, and the largest structural item left.
  - [ ] `ChunkDescriptor` carries no bounds and no origin (`WireFormat.h:53-85`,
        `static_assert(sizeof == 92)`). Needs a double-precision cluster origin and local
        AABB/sphere per `03-…:135`.
  - [ ] Packed normal/tangent format, half2 UVs where representable, RGBA8 colors, 16- or 32-bit
        indices per chunk (`03-…:138`).
  - [ ] The **full PBR vertex layout with tangents/vertex colour** — `04-…:72` requires at minimum
        three layouts; only `PositionOnly_F32` and `PositionNormalUv0_F32` exist.
  - [ ] **Cluster splitting** to `03-…:140`'s 4–16 MiB / ≤262,144 triangles / ≤1,048,576 points,
        preserving material and instance identity. Nothing splits today. This is what unblocks
        STL/PLY batching and any single oversized glTF primitive.
  - [ ] Tangents generated only for visible geometry whose material needs tangent-space normals.
- [ ] **6. meshoptimizer cluster LOD/proxy builder.** Not started; zero references to meshoptimizer
      in `import-worker/`. `lodLevel` exists on the descriptor and is always 0.
  - [ ] Three levels per `03-…:146-150`: an always-resident coarse proxy (≤2 M primitives, ≤5% of
        valid source, ≤ its reserved GPU budget), a ~50% intermediate, and full detail.
  - [ ] Quality-threshold fallback — a cluster that fails simplification keeps its next coarser
        valid representation.
  - [ ] Completes risk **R-22** for `meshoptimizer`, including the `10-…:283` change-control
        checklist.
- [ ] **7. Inbox WIC/DirectXTex/libwebp codecs and mip pipeline.** ~25% — `WicImageDecodeAdapter`
      handles PNG/JPEG/BMP/TIFF only.
  - [ ] DirectXTex paths for validated TGA, HDR, DDS (`03-…:183`).
  - [ ] Pinned libwebp for WebP; `EXT_texture_webp` in the glTF adapter.
  - [ ] Mip pipeline: direct upload of validated DDS/KTX2 mip chains, decoder-native downscaling
        where trustworthy, CPU mip generation in cancellable tiles otherwise, small mips first.
  - [ ] Semantic BC7/BC5/BC3/BC1 target selection (`TextureTranscodeAdapter.h:17` defers it).
  - [ ] Deterministic checker/neutral fallback for a missing or unsupported optional texture.
  - [ ] Completes **R-22** for `directxtex` and `libwebp`.
- [ ] **8. Source-order-independent stratified first-proxy sampling.** Not started (`03-…:101,152`).
      Sharp boundaries and material seams protected; point clouds use deterministic
      spatial/reservoir sampling. Closes validation spike 4.
- [ ] **9. Persistent derived-cache production path.** Prototype exists but is compiled only into
      `Tests.Unit`.
  - [ ] Wire it host-side from worker-produced chunks, and add it to `Preview3D.vcxproj`.
  - [ ] **Real `SceneSnapshot`/chunk-catalog serialization** — the prototype stores one opaque
        caller-supplied payload, so it proves cache *mechanics*, not the staleness guarantee.
  - [ ] **USN/change-journal identity capture.** Nothing in the repo touches
        `FSCTL_QUERY_USN_JOURNAL`/`FSCTL_READ_FILE_USN_DATA`, so a volume-serial/file-ID collision
        after a journal reset is undetected.
  - [ ] Gated by **validation spike 7** (`11-…:249`): token availability/invalidation,
        journal-reset and full-digest fallback cost, corrupt-entry fuzzing, atomic crash recovery,
        LRU/free-space behaviour, warm-open value, clear-cache races.
  - [ ] Transient Tier-A normalized store inside the worker where needed (`10-…:142`).
- [ ] **10. Draw sorting/instancing and a measured direct-vs-`ExecuteIndirect` threshold.** Not
      started — `D3D12ViewerPath.cpp:780` issues one `DrawIndexedInstanced` per chunk,
      `:352-356` sets `CullMode NONE`.
  - [ ] CPU frustum culling on cluster bounds (needs deliverable 5).
  - [ ] Grouping by pipeline/material, batching compatible instances.
  - [ ] Opaque and masked before sorted transparent groups; back-face culling unless double-sided.
  - [ ] A feature-level-11-compatible `ExecuteIndirect` path plus the **measured** crossover, with
        the direct path retained as the correctness fallback. Needs the Draw-heavy fixture.
- [ ] **11. Error/warning mapping and format diagnostics.** Not started.
  - [ ] `ImportError.h:12-25` has **7 of the ~20 codes** in `03-…:195-202`. Missing:
        `FileChanged`, `UnsupportedVersion`, `UnsupportedRequiredFeature`, `UnsupportedEncoding`,
        `IntegerOverflow`, `ArchiveLimit`, `OutOfMemory`, `Cancelled`, `NoSupportedGeometry`,
        `TextureDecodeFailed`, `ImportWorkerFailure`, `ImportWorkerLimit`, and the three
        compatibility-host codes.
  - [ ] **No warning channel exists at all** — needs a bounded warning list on the wire.
  - [ ] Preserve format, byte offset / object path where safe, and a correlation ID in diagnostic
        logs; map typed codes to user text app-side.
  - [ ] An ASCII-PLY file currently reuses `MalformedData`, a worse diagnostic than a dedicated
        unsupported-dialect code.

### Exit criteria — 0 of 6 demonstrable

- [ ] **1. The performance corpus meets all applicable time/memory/frame gates.** Blocked on
      infrastructure that does not exist:
  - [ ] **A perf and memory harness.** No `BENCHMARK`, no private-committed-bytes assertions, no
        ETW present-event frame classification (`09-…:87`).
  - [ ] **The fixture corpus** (`09-…:38-52`). `interactive-viewer/test-assets/` holds eleven ~KB
        triangle files. Needed: A-small (8 MiB), A-medium (350 MiB), A-large-glb (~4 GiB),
        A-large-stl (~3 GiB), A-large-ply (~3 GiB), A-adversarial-layout (2 GiB), A-compressed,
        Warm-cache, Draw-heavy, Pressure. `09-…:54` permits **generation recipes** rather than
        committing multi-gigabyte binaries — generate them.
- [ ] **2. The supported glTF feature corpus renders with expected counts/bounds/material
      snapshots.** No golden-scene infrastructure exists.
- [ ] **3. Malformed/overflow/cancellation/sidecar security suites pass against the worker
      process.** Malformed, overflow and sidecar suites exist and pass.
  - [ ] **Cooperative in-parse cancellation.** `ImportSession.h:123-128` states plainly that only
        host-side abandonment exists; `06-…:119` wants a cooperative cancel first with the Job
        Object as the post-grace fallback. Needs stop-token checkpoints threaded through the
        adapter loops.
- [ ] **4. No source-sized private heap allocation in A-large traces**, in either process. Needs
      the memory harness above.
- [ ] **5. Models larger than the current video-memory target stay inspectable through proxy and
      view refinement.** Needs deliverables 6 and 8, plus `DxgiBudgetMonitor` wired into the app.
- [x] **6. The Gate 2 hostile-worker suite is re-run against each newly wired adapter.** Standing
      practice, and extended in `f0d2f86` with seven progressive-delivery attack modes. **Keep
      extending it** — re-running is not enough when a change creates new attack surface.

### Blocking A-large specifically

Both structural blockers are closed and none of these is the output window:

- [ ] STL/PLY single-chunk output (deliverable 5's splitting).
- [ ] **The worker normalizes an entire model in memory before emitting any batch**, so the 1 GiB
      Tier A parser/normalizer scratch budget (`03-…:171`) binds. True streaming — parse, emit,
      free — is separate work and is what actually reaches a 4 GiB source.
- [ ] No proxy to send first, so "first useful partial proxy ≤2 s" has nothing to measure
      (deliverables 6 and 8).

### Open preconditions from the risk register

- [ ] **Spike 3** (`11-…:245`) — partial. Draco and KTX2/Basis are wired; `KHR_mesh_quantization`,
      `EXT_meshopt_compression`, and the compressed-corpus measurement are not.
- [ ] **Spike 4** (`11-…:246`) — "Before Gate 3 claims Tier-A PLY": stratified proxy quality, point
      rendering, and 2–4 GiB memory behaviour all still open.
- [ ] **Spike 7** (`11-…:249`) — gates deliverable 9.
- [ ] **Risk R-22** (`11-…:237`) — `meshoptimizer`, `directxtex` (+ transitive `directxmath`) and
      `libwebp` are pinned in `vcpkg.json` with nothing linking them, so they sit in the dependency
      and licence surface unreviewed. The register's own contingency: **if a library is still
      unwired when its slice is cut or deferred, remove it from `vcpkg.json`** rather than carrying
      it. Note vcpkg autolink puts the whole triplet's `lib\*.lib` on the link line, so an unwired
      dependency is not inert.

### Carried forward from `f0d2f86`

- [ ] **Progressive display.** `ImportSessionRequest::onBatch` exists and is proven by a test, but
      `D3D12ImportBridge` supplies none, so a batched import still accumulates host-side and
      uploads once. Getting the first geometry on screen before the last batch crosses needs the
      sink wired through `D3D12ImportBridge` → `RenderThread` → `D3D12ViewerPath`, with batches
      published additively rather than swapped once.
- [ ] **Optional hardening: bind a batch announcement to its content.** A worker that rewrites the
      window before its ack can have its own later section accepted — nothing invalid gets through,
      but "what the host accepted" is not provably "what was announced". A section checksum in
      `ChunkBatchReadyNotice` would close it against an honest worker's bug (not against a hostile
      one, which controls both halves). Only worth doing if a real bug motivates it.

---

## Gate 4 — Tier B format breadth

Five independent vertical slices (`10-…:155-174`), none started. **No Tier B dependency is pinned**
— `vcpkg.json` has no ufbx, lib3mf, TinyUSDZ or OpenUSD — and `compatibility-host/src/main.cpp` is
a four-line `return 0;`.

- [ ] **Slice 1** — OBJ plus MTL through ufbx, including local texture policy.
- [ ] **Slice 2** — FBX deterministic static start-pose evaluation through ufbx, including
      supported skin/blend deformation and unified PBR mapping.
- [ ] **Slice 3** — 3MF Core/Materials/Production/Beam Lattice preview through lib3mf.
- [ ] **Slice 4** — USDA/USDC/USD and USDZ common static subset through TinyUSDZ, inside the
      general import worker.
- [ ] **Slice 5** — the AppContainer compatibility host, brokered resolver, and bounded local
      static composition through OpenUSD, started **only** on the worker's typed
      `UnsupportedComposition` result. Additionally requires AppContainer restrictions, Job Object
      enforcement, broker protocol, shared-section revalidation, host crash/timeout behaviour, and
      signed/hash-verified payload tests.

Every slice carries the same bundle (`10-…:165`): adapter wrapper, dependency allocation/I/O/cancel
callbacks and Job Object limits, normalized output, unsupported-feature diagnostics, golden scenes,
malformed corpus, fuzz seed, cache-version effect, licence update, and a hostile-worker suite re-run
against the newly wired adapter.

- [ ] **Spike 5** (`11-…:247`) gates the start: capped-memory/cancellation spikes for ufbx static
      skin/blend evaluation, lib3mf Beam Lattice tessellation, and TinyUSDZ.
- [ ] **Spike 6** (`11-…:248`) gates the OpenUSD slice.

Exit (`10-…:167-174`): every direct extension opens from command line, dialog, drop and secondary
activation — **which needs Gate 5's activation work**; conformance fixtures preserve
hierarchy/instances/transforms; lower Tier B limits fail safely without affecting the next open;
archive bombs, recursive graphs, unsafe references, missing MTL/textures, deformed FBX poses, 3MF
lattices and over-limit USD composition all have specified outcomes; TinyUSDZ/OpenUSD overlap
fixtures normalize equivalently and neither process can reach an unbrokered file, network, plug-in
or child process; UI/render responsiveness and cancellation gates hold.

---

## Gate 5 — integrated viewer UX

Ten deliverables (`10-…:178-189`), none started.

- [ ] Old-document-until-proxy handoff.
- [ ] Progressive phase/progress/warning UI — needs Gate 3 deliverable 11's warning channel.
- [ ] Complete camera/bounds correction behaviour.
- [ ] Complete mouse/keyboard/touch camera bindings and cache-overflow commands.
- [ ] OLE drop and `IFileOpenDialog` filters.
- [ ] Pressure / reduced-detail UI — needs `DxgiBudgetMonitor` in the app.
- [ ] Actionable failures and redacted Copy details.
- [ ] UI Automation providers, focus and live-region behaviour.
- [ ] Session/user-scoped mutex and pipe forwarding and activation — **also unblocks Gate 2's
      open/close/reopen stress and Gate 4's activation exit criterion.**
- [ ] Asynchronous close with clean process termination.

Exit: all `07-user-experience.md` acceptance scenarios; simultaneous launch / IPC spoof /
open-storm / cancellation tests; no tray icon, startup task, hidden post-close window or lingering
process; automated UI heartbeat with no load-time message gap above 100 ms; accessibility audit
across keyboard, 200% scale, high contrast and screen-reader smoke.

---

## Gate 6 — Explorer thumbnail provider

`thumbnail-provider/dllmain.cpp` is a 21-line `DllMain` skeleton — no `IThumbnailProvider`, no
exports, no `.def`, no registration.

- [ ] Seven stable COM classes including PLY, plus class factory and lifetime exports.
- [ ] Bounded `IStream` backing, adapter routing, geometry sampler.
- [ ] Deterministic CPU rasterizer and premultiplied BGRA `HBITMAP`.
- [ ] Triangle and point-cloud sampling/rasterization plus bounded Draco/KTX2 provider paths.
- [ ] Explorer-safe error, deadline and resource behaviour.
- [ ] COM host harness, golden images, actual surrogate tests.
- [ ] **Spike 8** (`11-…:250`) gates it: benchmark the CPU rasterizer prototypes inside the actual
      thumbnail surrogate, and confirm out-of-process load with no `DisableProcessIsolation`.

Exit: every extension routes to the intended CLSID; the 750 ms p95 / 2 s cutoff and 192 MiB scratch
cap hold; OBJ/glTF external sidecars are never opened from a Shell stream; every registered CLSID
loads into the isolated surrogate rather than `explorer.exe` on a clean installed machine;
malformed/fuzz/parallel/unload soak produces no crash, hang, handle leak or persistent thread;
`DllCanUnloadNow` semantics and GDI ownership tests pass.

---

## Gate 7 — installer and hardening

No `installer/` directory exists.

- [ ] WiX MSI, stable component identities, capabilities/ProgID/Open With/thumbnail registration.
- [ ] AppContainer compatibility-host and OpenUSD payload packaging, signing, ACLs, DLL/plug-in
      search lockdown, and rollback.
- [ ] Install, repair, upgrade, rollback, handler-conflict, Restart Manager, notification and
      uninstall logic.
- [ ] Authenticode signing pipeline, SBOM/notices, release manifests.
- [ ] ASan, static analysis, Application Verifier, D3D validation, fuzz and dependency-review
      closure.
- [ ] Local redacted diagnostics and support-bundle behaviour.
- [ ] **Spike 9** (`11-…:251`) gates it: thumbnail-handler conflict, loaded-DLL/host upgrade
      behaviour, signed/hash-verified OpenUSD payload search lockdown, and cache
      preservation/removal on clean Windows 11 VMs.

Exit: the full installer matrix on clean VMs; install never changes the selected default app and
uninstall removes only product-owned state; no normal scenario requires reboot or Explorer
termination; signed installed hashes match the release manifest; threat-model controls and the
malformed corpus have no open critical or high defect.

---

## Gate 8 — release candidate

- [ ] Feature-complete, signed candidate.
- [ ] Performance/reference traces and compatibility evidence.
- [ ] 8-hour mixed-load soaks (`09-…:129` defines the scenario and its eight failure conditions).
- [ ] Final format/support/known-limit documentation.
- [ ] Final cache storage/privacy/clear/uninstall documentation and compatibility-host runbook.
- [ ] Recovery runbook and symbol archive.

Exit: all release acceptance checks in `01-product-scope.md`; all quality evidence linked to the
candidate build and fixture hashes; no open blocker, security issue, data-corruption issue,
Explorer stability issue, UI-thread wait, stale-generation render, or repeatable device-loss loop;
product, engineering, security and installer owners sign the same artifact manifest.

---

## Cross-cutting

- [ ] **No CI** (Gate 0). Everything below is run by hand, which is why regressions are caught by
      discipline rather than by tooling.
- [ ] **No perf or memory harness and no fixture corpus.** Blocks Gate 1's NFR-03 criteria and Gate
      3's exit criteria 1 and 4. Probably two chunks of work on its own.
- [ ] **`ModelCore.vcxproj` is a stub** — `shared/model-core/src/ModelCore.cpp` is nine lines whose
      only function returns 0. The shared "library" is in practice compiled per-consumer through
      relative-path `ClCompile`, which is an established and deliberate pattern here; the stub
      project is the leftover.
- [ ] **`compatibility-host` is a four-line stub** (Gate 4 slice 5).
- [ ] **`thumbnail-provider` is a 21-line stub** (Gate 6).
- [ ] **Validation spikes**: 1 done *with a caveat* — ADR-010's chrome-scale p95 ranges roughly
      7.2–9.0 ms across repeat runs and occasionally crosses NFR-04's 8.3 ms gate, and **must be
      re-measured against the real ported chrome**, with dirty-tracking as the first remedy; 2 done;
      3 partial; **4, 5, 6, 7, 8, 9 open**.
- [ ] **Broken tooling**: `interactive-viewer/tools/build-test-loader.ps1:14` references a
      `test-loader.cpp` that does not exist, and hardcodes an MSVC version path.
      `interactive-viewer/scripts/smoke-test.ps1` hardcodes the *project-level* Release path, which
      is not where a solution build puts the exe.

## Keeping this file honest

- Tick a box only when the work meets `10-…:287`'s definition of done — code, tests, diagnostics,
  malformed/cancellation paths, docs and licence effects together — not when it compiles.
- Record *what happened* in `PROGRESS.md` and *what is left* here. When a chunk lands, both change.
- Deferred work goes in the design docs' risk register with an owner and a trigger, per
  `10-…:287`; it does not get quietly dropped from this list.
- **Build `Preview3D.slnx`, never an individual `.vcxproj`** — `$(SolutionDir)` is undefined for a
  project-level build, and 85 of the import-isolation tests then fail in a way that looks exactly
  like a broken sandbox. See PROGRESS.md's "Flagged risks" section.

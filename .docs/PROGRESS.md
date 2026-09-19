# Progress notes

Running log of what's been built against `.docs/design/`, plus the Win32/MSBuild risks that were flagged before implementation and how they actually turned out. Intended for whoever picks up the next chunk of Gate 2 workstream A (or reviews this one) — not a replacement for the design docs, just the empirical record of what surprised us.

## Status

- **STEP-008 post-slice viewer regression (2026-09-19): the genuine corpus
  fails through the real viewer as `StepHostFailure`.** Opening
  `test-models/Voron_2.4r2_Assembly.step` in `Preview3D.exe` loads for several
  minutes and then reports "The STEP importer stopped unexpectedly." / "The
  isolated STEP host could not complete this model." It is a broker-side
  timeout, not a host crash: the D3D12 bridge never raises `replyTimeoutMs` for
  STEP, so the dedicated host runs under the shared
  `kWorkerReplyTimeoutMs = 120'000` backstop, and the host only sends control
  messages at phase boundaries (`StepHostImport` preflight; `StepXdeAdapter`
  Read/Transfer/Plan/Mesh/Emit). The long `ReadStream` and `Transfer` phases
  emit nothing, so the broker sees a >120 s silence, reports
  `ImportStage::ReplyTimedOut`, and `MapStepFailure` collapses that to
  `StepHostFailure` (26) — the same user text as a crash. The opt-in
  `[.][step-008-measure]` sets `replyTimeoutMs = 1'800'000` plus a 20-minute
  deadline, which is exactly why the qualification slice never saw this.

  Things the next STP/STEP task should not relearn:
  1. **The viewer needs a STEP-specific reply timeout, or the host needs a
     phase heartbeat.** A 120 s hang backstop is far below the genuine
     corpus's single-phase cost. Prefer a bounded heartbeat progress event
     during `ReadStream`/`Transfer` over a blanket longer timeout so a wedged
     host is still caught; either way, add a real-viewer regression for the
     large corpus, not just a `RunImportSession` measurement.
  2. **The Debug STEP host links the vcpkg Debug OCCT closure.** On the same
     241,522,213-byte assembly: Release broker Ready ~111 s with a largest
     phase gap of ~64 s (`Transfer`) and 2.28 GiB peak host commit; Debug
     broker Ready ~994 s (~16.5 min) with `Transfer` 445 s and 3.89 GiB peak.
     `x64/Debug/StepHost/TKernel.dll` is 2,964,992 B against Release's
     1,601,024 B. Debug is ~9x slower and ~1.7x the commit, so a Debug viewer
     run will always trip the 120 s backstop and is not performance evidence;
     the published Tier-B budget is Release-only.
  3. **`StepHostFailure` is overloaded.** `ReplyTimedOut`, a launch/payload
     fault, a host crash, and a broker validation failure all collapse to the
     same `StepHostFailure` text, so "stopped unexpectedly" must not be read as
     "crashed". Check `ImportSessionResult.stage`/`errorCode` (the bridge has
     them; the UI does not) before choosing a fix.
  4. **The STEP host commit ceiling is `min(4 GiB, 35% of physical RAM)`**
     (`CompatibilityCommitLimitBytes`), not the 4 GiB general-worker default.
     The Debug peak (3.89 GiB) already sits at that ceiling, so a lower-RAM
     machine's 35% branch can Job-kill the host; when the Job violation flag
     and the NT memory statuses are both missed that also surfaces as
     `StepHostFailure`, not `StepHostLimit`. Distinguish a genuine host defect
     from a machine-size limit before blaming the code.
  5. **Do not chase the per-generation caps for this failure.** The measurement
     uses `maxChunkBatchesPerGeneration = 4096` and `maxChunksPerGeneration = 0`
     (derive); the viewer uses `kTierACatalogLimit` (899,074) and
     `kTierABatchLimit`. The corpus delivers 4,589 chunks in 8 batches, so
     neither cap is binding and the STEP-008 `maxChunkCount` fix is not
     implicated.

- **STEP-008 qualification slice (2026-09-19): implemented; release
  qualification remains open.** A checked-in corpus manifest
  (`tests/fixtures/step/manifest.json`) freezes provenance, size, SHA-256,
  class, and expected outcome for the 16 committed fixtures plus 12 derived
  adversarial cases, and `tests/fixtures/step/verify.py` re-derives and checks
  them. A new `StepQualificationTests.cpp` gives every malformed/unsupported/
  over-limit admission family an asserted typed status, proves each cap exactly
  at its boundary, and adds a progressive-batch regression. `prepare_step_seeds.py`
  now emits the STEP-008 families as `StepFuzz` seeds (45-second Release ASan
  smoke: 64,569 executions, 456 MiB peak RSS, no finding).
  `[step-002]`..`[step-006]` plus `[step-008]` pass **38 cases / 746 assertions
  in Debug and Release**. The genuine 100 MB+ obligation is met: the real host
  imported a 241,522,213-byte AP214 assembly (4.06 M triangles, 1,314
  definitions) with time-to-first-coarse 66 s and Ready 95 s, peak host commit
  2.07 GiB, and the budget is published in
  [design/09-quality-performance-and-security.md](./design/09-quality-performance-and-security.md).
  See [STEP-008-VERIFICATION.md](./STEP-008-VERIFICATION.md). Still open: static
  analysis/license review, multi-run p95, the ~20 M-triangle fixture, an
  instrumented OCCT-boundary fuzzer, the 8-hour soak, clean-VM lifecycle, and
  signed-artifact/SBOM inspection.

  Things the next STP/STEP task should not relearn:
  1. **The STEP emitter must flush on the broker's `maxChunkCount`, not just on
     byte capacity.** `SceneEmitter::Add` only flushed when the next chunk
     exceeded the section byte window, so a large many-definition assembly whose
     geometry was small accumulated more descriptors than
     `request.maxChunkCount` and was rejected by the broker as
     `ResourceLimit`/`StepHostLimit` before a single batch was delivered. It now
     flushes-and-publishes at the chunk cap too (same pattern as the byte cap),
     with `[step-008][chunk-cap]` as the regression. If a future adapter grows a
     progressive emitter, honor every per-section cap the broker enforces, not
     only the byte one.
  2. **The genuine 100 MB+ corpus is `test-models/Voron_2.4r2_Assembly.step`**
     (241,522,213 bytes, AP214, self-contained, SHA-256 in the manifest). It is
     git-ignored and supplied locally; the opt-in measurement reads
     `PREVIEW3D_MANUAL_STEP_FILE` and is tagged `[.][step-008-measure]`. It is
     parse/transfer-bound: admission 3.2 s, `ReadStream` 12.2 s, `Transfer`
     50.8 s, mesh 24.3 s, emit 26.0 s. Do not expect a coarse second pass to
     beat the transfer.
  3. **The Part-21 scanner treats the newline after `DATA;` as the first byte of
     the next record.** Record-length boundary tests must build the record with
     no leading whitespace (`DATA;#1=...`) or they are off by one; a body of
     exactly `maxRecordBytes` is accepted and `maxRecordBytes + 1` is rejected.
     String-length counts only bytes inside quotes and is not affected.
  4. **The broker caps nodes and instances at the same Tier-B object limit
     (50,000).** The host planner caps nodes but not instances; a file with more
     than 50,000 occurrences fails during section validation, not planning.
     Treat the two as one envelope when publishing limits.
  5. **`_wgetenv` is banned under the test project's `/WX`.** Use
     `_wdupenv_s` and free the buffer. Also include
     `import_broker/SharedSection.h` for `kImportSectionBytes` /
     `kImportMaxChunkCount`; they are not in `ImportSession.h`.

- **STEP-007 product, package, and documentation integration (2026-09-19):
  complete.** `.step`/`.stp` now reach the existing dedicated
  `Preview3DStepHost.exe` route through every viewer activation surface:
  `ClassifyByExtension`, command-line validation, the Open dialog filters,
  drag/drop, secondary activation, Retry, supported-format errors, and the
  title-bar Open With catalog (revision 7). The bridge forwards the bounded
  `StepProgress` events, and the loading overlay shows product-owned phase text
  and N-of-M definition counts; `--app-smoke` fields 84/85/86 expose the phase
  and counts. The portable/NSIS payload stages the exact signed `StepHost\`
  OCCT closure, its license/SBOM entry, a closed payload allowlist, and the
  third `Binbuf.Preview3D.StepHost` AppContainer ACL; `Binbuf.Preview3D.STEP.1`
  registers both extensions without touching the user's default. `step.py`
  passes 11 checks in Debug and Release (including crash/timeout recovery and
  the Emit phase); `step_package.py` passes against both stages;
  `[step-002]`..`[step-006]` still pass 35 cases / 706 assertions. Thumbnails
  remain STEP-009. See [STEP-007-VERIFICATION.md](./STEP-007-VERIFICATION.md).

  Things the next STP/STEP task should not relearn:
  1. **The viewer groundwork for STEP already existed from STEP-005/006.**
     `SourceFormat::Step`, `ToBrokerFormat`, `ResolveStepHostExePath`,
     `SourceFormatLabel`, and the STEP-specific error text were all present;
     discovery was deliberately gated only in `ClassifyByExtension` and the
     extension allowlists. Enabling STEP-007 was four small list edits plus the
     Shell catalog revision bump. Do not rebuild the bridge route.
  2. **`StepProgressNotice`'s mesh counter is `definitionsMeshed`, not
     `definitionIndex`.** The callback runs on the import thread, so the viewer
     stores phase/done/total in `std::atomic<uint32_t>` and reads them on the UI
     thread; the completion message is always posted after the last progress
     event, so the Emit phase (6) is observable once the document is Ready.
  3. **The STEP host ignores `workerArgumentsOverride`.** Viewer fault injection
     for STEP must use `stepHostArgumentsOverride` with the host's own
     `--pool-crash`/`--pool-hang`/`--pool-overallocate` modes (and
     `stepHostCommitLimitBytes` for the memory case). The general-worker
     `--child-noop`/`--test-hang-import` overrides are inert for STEP and would
     make a fault test silently import normally.
  4. **OCCT has no root `vcpkg_installed` status entry.** It is installed only
     by `compatibility-host-step/vcpkg.json`, so the packaging script must copy
     `licenses/opencascade.txt` from
     `compatibility-host-step/vcpkg_installed/x64-windows/share/opencascade/copyright`
     and read its version/ABI from that tree's `vcpkg.spdx.json`. The root
     `Read-VcpkgStatus` path will not contain `opencascade`.
  5. **OCCT's `TKernel.dll` imports `WSOCK32.dll` and `TKService.dll` imports
     `WINMM.dll`.** Both are Windows system DLLs that the packaging PE-closure
     allowlist must list (alongside `ws2_32.dll`), or portable/installer staging
     fails with an unresolved-dependency error.
  6. **NSIS license removal is easy to forget.** Adding
     `licenses/opencascade.txt` to the stage requires a matching
     `Delete "$INSTDIR\licenses\opencascade.txt"` in the uninstall section; the
     `step_package.py` registration check catches the omission, and `makensis`
     is run with `/WX`.

- **STEP-006 interoperability closure and self-contained scope acceptance
  (2026-09-19): complete.** A checked-in interoperability matrix now names the
  exact typed outcome for every supported and excluded family and runs through
  the real host: AP203/AP214/AP242 B-rep, an AP242 B-rep-plus-authored-
  tessellation fixture, an AP242 tessellated-only fixture, assembly/reuse,
  instance/face colors, inch units, geometry-free product metadata, an unknown
  geometry-free schema, `FILE_POPULATION`, relative `DOCUMENT_FILE`,
  absolute/UNC/URL `DOCUMENT_FILE`, and invalid authored faceted topology. The
  external-document no-go is recorded in the support matrix, ADR-017, product
  scope, design README, and public limitations; the healing comparison adopted
  no healing as `step_host::kStepHealingPolicy`. A standalone `StepFuzz`
  (ASan/libFuzzer, no GPU) fuzzes admission and declaration discovery; a
  45-second Release smoke ran 53,244 executions with no finding. The route
  stays private to the broker; viewer/package exposure is STEP-007.
  `[step-002]`..`[step-006]` pass 35 cases / 706 assertions in Debug and
  Release. See [STEP-006-VERIFICATION.md](./STEP-006-VERIFICATION.md) and
  [STEP-006-INTEROP-MATRIX.md](./STEP-006-INTEROP-MATRIX.md).

  Things the next STP/STEP task should not relearn:
  1. **`Interface_Static` values are created with their defaults on the first
     STEP writer construction, overwriting any `SetCVal` made before that.**
     `GenerateStepFixtures.cpp` now constructs one throwaway
     `STEPCAFControl_Writer` before setting `write.step.schema` /
     `write.step.unit` / `write.step.tessellated`. Without it, the first case
     silently wrote AP214/OnNoBRep instead of the requested AP242/On.
  2. **`write.step.tessellated` is the writer switch, and the default is
     `OnNoBRep`, not `On`.** A shape that has a B-rep writes *no* tessellated
     representation unless it is set to `On`; a mesh-only shape (no surface)
     writes one with `OnNoBRep`. The AP242 B-rep+tessellated fixture uses `On`;
     the tessellated-only fixture is a `TopoDS_Compound` of
     `BRep_Builder::MakeFace(face, Poly_Triangulation)` mesh-only faces.
     Tessellated output requires the AP242 schema.
  3. **A valid geometry-free Part-21 file used to report `MalformedData`.**
     OCCT's `Transfer` returns a generic failure when there is no shape, so the
     adapter now checks `reader.ChangeReader().NbRootsForTransfer()` right
     after `ReadStream` and returns `EmptyGeometry`. Keep that ordering: the
     `FileUnits` check would otherwise run first and report
     `UnsupportedRequiredFeature`.
  4. **OCCT throws `Standard_Failure`, which derives from `Standard_Transient`,
     not `std::exception`.** A `catch (...)` therefore reported invalid faceted
     topology (out-of-range `TRIANGULATED_SURFACE_SET` indices) as
     `InternalImporterFailure`. `StepXdeAdapter` now wraps `ReadStream` and
     `Transfer` and adds a boundary `catch (const Standard_Failure&)` mapping
     to `MalformedData`. When adding OCCT code, catch `Standard_Failure`
     explicitly rather than assuming `std::exception`.
  5. **Declaration discovery is lexical and conservative.** The preflight scans
     each record's text for `FILE_POPULATION`/`DOCUMENT_FILE` case-
     insensitively, so case, whitespace, path shape, and even a keyword inside
     a quoted string all flag. That is intentional fail-closed behavior; do not
     "improve" it into parsing declarations without also proving no resolver
     can be reached.
  6. **A fuzz target that writes a pipe before reading it deadlocks above the
     pipe buffer.** `StepFuzz`'s handle-admission domain caps the payload below
     the 4 KiB pipe buffer (and passes the actual `written` count). The first
     draft used a 0-sized pipe and hung on a 26 KB fixture, which the fuzzer
     reported as a timeout — a harness bug, not a product finding.

- **STEP-005 render-time performance and first-frame latency (2026-09-19):
  implemented slice; large-file corpus and published budgets remain STEP-008.**
  The dedicated STEP host now measures and publishes every real phase
  (Part-21 lexical admission, `ReadStream` parse, `Transfer`, mesh-free
  planning, per-definition meshing, face/triangle extraction, window emission)
  through `StepPhaseTimings`, and emits a bounded `StepProgress` control message
  (opcode 21, `StepProgressNotice`, closed `kStepPhase*` Preflight/Read/
  Transfer/Plan/Mesh/Emit) that the broker validates, caps, forwards to an
  optional `onStepProgress`, and records on `ImportSessionResult`. Admission and
  OCCT transfer now consume one read-only mapping of the inherited handle
  (`CreateFileMappingW` + `MapViewOfFile`, no path), removing the former double
  full read. `StepTessellationProfile` v3 records
  `StepDeliveryStrategy::SinglePassProgressiveDisplay` and re-enables parallel
  meshing. `[step-002],[step-003],[step-004],[step-005]` pass 30 cases / 530
  assertions in Debug and Release. Details and the measured phase table are in
  [STEP-005-VERIFICATION.md](./STEP-005-VERIFICATION.md).

  Four things the next STEP task should not relearn:
  1. **STEP-004's `parallel = false` premise was wrong.** The pinned
     `USE_TBB=OFF` OCCT 7.8 port still parallelizes through
     `OSD_Parallel`'s built-in `OSD_ThreadPool` (`ToUseOcctThreads()` defaults
     true when no external library is enabled), and
     `IMeshTools_Parameters::InParallel` is documented as multi-thread on/off.
     The profile now sets `parallel = true` and
     `kImportRequestStepForceSerialForTesting` (bit 8) proves parallel and
     forced-serial output byte-identical. Do not repeat the TBB-only reading.
  2. **Map exactly the file size.** Mapping the source to end-of-file (or
     `MappedView::Map(..., 0)`) exposes the zero-filled tail of the final
     allocation granule; the Part-21 scanner correctly rejects NUL as a
     non-clear-text control byte, so every import failed `UnsupportedEncoding`
     until the view length was pinned to `GetFileSizeEx`. `CreateFileMappingW`
     works on the broker's `GENERIC_READ` duplicated handle.
  3. **The STEP host's request-flag mask had a latent precedence bug**:
     `flags & ~detail & ~coarse` is not `flags & ~(detail | coarse)`. It only
     tolerated the two original bits; any new test seam was rejected as
     malformed. The mask is now an explicit allowlist.
  4. **`StepProgress` is producer-bound.** The broker accepts it only from
     `ImportProducer::StepHost` and rejects it from any other producer; the
     per-generation cap (8192) maps to `ImportStage::StepProgressLimit` ->
     `ResourceLimit`. Progress is bounded to ~256 mesh events regardless of
     definition count, so a huge scene cannot flood the control channel.

  Open for STEP-008: the genuine 100 MB+ assembly and high-triangle fixture,
  the resulting Tier-B time-to-first-coarse/Ready budgets, measured thread-pool
  width against peak commit, progress UI wiring (STEP-006/STEP-007), and the
  two-pass-versus-single-pass recheck if a real benefit appears.

- **STEP-004 bounded tessellation and progressive CAD delivery (2026-09-19):
  complete for the single-pass display slice; coarse catalog delegated to
  STEP-005.** `StepXdeAdapter` is now two-phase: `ScenePlanner` walks the XDE
  document without meshing and builds nodes/definitions/materials/occurrences,
  then `SceneEmitter`+`DefinitionMesher` tessellate one reusable definition at a
  time under the new versioned `StepTessellationProfile` (v2) and write
  cluster-local float positions with exact double per-chunk origins. Geometry is
  no longer retained after it is written, and a window that fills is handed off
  through `ChunkBatchReady`/`ChunkBatchConsumed` via the existing
  `import_worker::ChunkBatchSink`, so a scene larger than the output window no
  longer needs one giant section. Authored AP242 tessellation is preferred by
  meshing with `AllowQualityDecrease = false`. A new typed
  `ImportErrorCode::TessellationFailed` (28) carries meshing/timeout/budget
  failures to fixed viewer text. `[step-002],[step-003],[step-004]` pass 26
  cases / 423 assertions in Debug and Release; STEP-003 golden counts are
  unchanged. Details in [STEP-004-VERIFICATION.md](./STEP-004-VERIFICATION.md).

  Five things constrain the later STEP work:
  1. The pinned constrained OCCT port builds with `USE_TBB=OFF`, so
     `BRepMesh_IncrementalMesh::InParallel` has no parallel backend. The profile
     now records `parallel = false` truthfully; STEP-005 must prove product-level
     per-definition parallelism or document serial throughput.
  2. The Tier-B broker computes `coarseProtocol = enableCoarseProxy &&
     !tierBFormat`, and `D3D12ImportBridge` deliberately leaves `enableCoarseProxy`
     and `nextDetail` unset for STEP. A `CoarseComplete` record is rejected and a
     `kCoarseLod` chunk would render *alongside* fine geometry, not replace it.
     The stp2.md two-pass coarse catalog therefore needs the scan/detail protocol
     enabled for STEP (a cross-cutting broker/bridge change) and is owned by
     STEP-005 item 5; STEP-004 ships single-pass progressive display.
  3. A single geometry chunk deliberately fails as `ResourceLimit` if it cannot
     fit one output window even after flushing; choosing a test window smaller
     than the largest chunk is not a progressive test. For the current fixtures
     the largest chunk is ~21.6 KiB and the whole nested scene ~27 KiB, so a
     24 KiB window exercises the batch path.
  4. `ImportStage::WorkerReportedError` is numeric value **16**, not
     `ValidateSection` (15); a host `GenerationError` therefore reads as stage 16
     and can be misread as a broker validation failure. The broker maps
     host-reported `ResourceLimit` to `StepHostLimit` (27).
  5. `StepXdeResult::meshMilliseconds` now accumulates per-definition
     `BRepMesh_IncrementalMesh` time for STEP-005's two-pass-versus-single-pass
     decision; it is not yet surfaced.

  Build note: after editing a source, MSBuild occasionally reported the affected
  project up to date and reused a stale `.obj` (the STEP host and the
  ImportIsolation test project both showed this). Deleting the specific `.obj`
  and rebuilding, or building through `Preview3D.slnx`, is required before
  trusting a test run. Building `Tests.ImportIsolation.vcxproj` directly without
  `/p:SolutionDir=<repo root>` bakes `$(SolutionDir)`-relative fixture/host paths
  into the test binary and makes every STEP import fail at `OpenSource`.

- **STEP-001 spike complete (2026-09-18); go for STEP-002 with a constrained
  OCCT port.** See [STEP-001-SPIKE-RESULTS.md](./STEP-001-SPIKE-RESULTS.md).
  OCCT 7.8.1 imported a self-contained AP214 assembly through
  `STEPCAFControl_Reader::ReadStream` over a product-owned seekable
  `std::streambuf` on the inherited read-only handle, inside the real
  zero-capability AppContainer and kill-on-close Job Object. XDE retained five
  instances, two reused definitions, nested transforms, three colors (two
  transparent), and the authored unit name; `BRepMesh_IncrementalMesh`
  produced 596 bounded triangles. Malformed bytes failed before OCCT, a
  pre-signalled cancellation was observed, a 4 MiB Job ceiling terminated the
  host without a false success and a replacement import recovered, and an
  authority probe under the same container was denied path, network, and child
  process. The registry `opencascade` port builds 48 DLLs / ~51.3 MB including
  visualization and every non-STEP exchange format; a constrained overlay port
  (`packaging/vcpkg-ports/opencascade`) and isolated
  `compatibility-host-step/vcpkg.json` are checked in. Two findings constrain
  later tasks: OCCT normalizes geometry to its system unit so the authored
  `metersPerUnit` must be derived from `FileUnits`, and OCCT's external
  resolver is path-based with no stream hook, so STEP-005 external references
  are a no-go without a product-owner scope change. No public opcode,
  `SourceFormatId`, extension filter, registration, or thumbnail behavior was
  added.

- **3MF-007 qualification started (2026-09-18); release gate remains open.**
  `tests/fixtures/3mf/manifest.json` freezes seven decoded sources and seven
  deterministic derived cases with independent hashes and expected outcomes.
  `verify.py` checks the hashes and proves the new static Production source is
  exactly the upstream required-Slice sample with `requiredextensions="s p"`
  changed to `"p"` in its three model parts. The upstream source must fail the
  shipping reader as `UnsupportedRequiredFeature`: Slice is outside the static
  viewer subset even though lib3mf's compatibility mode can load it. The
  optional-Slice derivative preserves two standard build occurrences and is
  now the Production viewer/app-smoke golden. Keep the upstream file only in
  the callback/API spike and negative tests; otherwise a test can silently
  bless an unsupported required extension.

  A real-worker regression exposed that the product XML scan did not enforce
  `requiredextensions` at all. It now resolves each declared root-model prefix
  against a closed Core/Materials/Production/Beam namespace allowlist before
  lib3mf model construction. Unknown and unsupported required extensions fail
  with the typed result; malformed OPC inputs still leave the next valid open
  usable. The ZIP64 expansion-ratio comparison now avoids untrusted 64-bit
  multiplication overflow. `ThreeMfFuzz` instruments product OPC/Deflate/XML
  boundaries with fresh immutable seeds; the final 11-second Release ASan
  smoke ran 145,971 executions without a finding (440 MiB peak RSS). It does not
  instrument the separately built lib3mf DLL or full adapter.

  Final Debug/Release focused real-worker tests pass 11 cases / 1,757 assertions
  each. Release 3MF spike plus hostile-worker tests pass 19 cases / 1,058
  assertions; Debug Unit passes 99 cases / 7,629 assertions; Release real-app
  smoke passes 12 checks using the new Production source. Three local slicer
  files pass the optional manual-corpus test but remain untracked. A broad
  Release security selector failed outside the 3MF route in OpenUSD/fixture
  tests, and a serial Release Unit rerun failed GPU device/queue creation after
  the app smoke; those runs cannot be claimed as green. A final Debug rebuild
  then hit `C1041` because Visual Studio's IDE build was concurrently compiling
  the same worker into `import-worker/x64/Debug/vc145.pdb`. Once that IDE build
  finished, a serial Debug rebuild and focused rerun passed; the test command
  that followed the failed build used an older binary and is excluded from
  final-source evidence. The product-owned OPC
  preflight still does not parse Content Types or relationship XML/targets,
  despite the design's stated requirement. Finish that boundary, official and
  licensed vendor corpus, numeric goldens, performance/soak, clean VM, and
  signing before marking 3MF-007 or Gate 4 complete. Exact commands and results
  are in [3MF-007-VERIFICATION.md](./3MF-007-VERIFICATION.md).

- **3MF-006 product/viewer integration (2026-09-18): implemented; 3MF-007
  qualification remains.** `.3mf` now reaches the existing `ThreeMf` broker
  opcode through case-insensitive command-line, dialog, drop, secondary
  activation, Retry, and Open With paths. The D3D12 bridge leaves both coarse
  proxy and detail-service request flags unset: the 3MF worker deliberately
  accepts no flags, so treating it like Tier A produces
  `ImportProtocolViolation` before parsing. Existing scene metadata reports
  3MF format, +Z source up, units, verified bounds, geometry/material/texture
  and node facts; all root-build occurrences retain authored transforms and
  document-wide Fit/selection. No private plate metadata or protocol layout
  change was needed.

  `Preview3D.exe` imports no 3MF/ZIP parser DLL. The Release worker closure
  observed with `dumpbin` is `lib3mf.dll` -> `zip.dll`/`z.dll`, and
  `zip.dll` -> `bz2.dll`/`z.dll`. `Create-PortableRelease.ps1` stages exactly
  those additional runtime DLLs under `worker/`, plus `bzip2`, `lib3mf`,
  `libzip`, and `zlib` license files and SBOM entries. Its PE dependency
  allowlist also needed the Windows system `xmllite.dll`, already linked by
  the worker for bounded XML scanning; omitting it caused package validation
  to fail despite a successful product build. NSIS uses the closed staged
  worker tree, grants the worker profile access through the existing ACL
  provisioning, registers `Binbuf.Preview3D.ThreeMF.1` for `.3mf`, and
  removes that entry plus exact payload/license files on uninstall. No
  thumbnail registration is installed.

  Debug and Release solution builds, focused 3MF-003..006 tests (10 cases /
  391 assertions in each), and full Unit suites (99 cases, 7,629 Debug /
  7,541 Release assertions) pass. The real-app 3MF smoke passes 12 checks
  in Debug and from the Release portable stage: command-line/picker/drop/
  forwarded opens, Production occurrences, Materials, Beam Lattice,
  ground-axis/Info/fullscreen/Fit/Reset controls, malformed/crash/timeout/
  cancellation recovery, replacement, and relaunch. Engineering portable and
  NSIS staging pass dependency closure with 73 and 74 files respectively;
  a separate package-contract check verifies every manifest hash, required
  worker DLL/license/SBOM entry, and symmetric `.3mf` registration/removal.
  These were unsigned builds, not signed release candidates. The upstream
  `beam-lattice.3mf.base64` fixture is an API/spike sample, not a production
  viewer golden: it reaches a typed failure in the shipping route that needs
  specific triage in 3MF-007. The app smoke instead builds a deterministic,
  supported parametric lattice package. 3MF-007 must freeze a provenance-
  and-hash-qualified lattice corpus and examine this upstream case, plus
  complete actual clean-VM install/repair/upgrade/uninstall and signing.

- **3MF-005 bounded Beam Lattice preview (2026-09-18): complete; private
  until 3MF-006.** The production adapter now recognizes lattice-only as well
  as mesh-plus-lattice objects, validates compact beam/ball/set input before
  output sizing, prefers a valid authored `representationmesh`, and otherwise
  tessellates every retained beam and ball in cancellable batches. The normal
  16-sided circle policy has a 1.92% relative chord error; fixed levels
  `16/12/8/6/4/3` degrade the whole lattice deterministically under a 262,144
  triangle per-lattice ceiling and the shared 20-million-triangle Tier-B scene
  ceiling. Sub-`minlength` beams are ignored as the specification requires;
  no source-prefix truncation is used. Importer/cache version is 4.

  A dependency limitation discovered here matters for future format work:
  lib3mf 2.5 exposes beam indices, radii, caps, balls, sets, clipping, and
  representation IDs, but its public Beam Lattice structs do not expose the
  standardized `pid`/`pindex`, beam `p1`/`p2`, or ball `p` associations. Its
  compatible reader also discards those attributes. The existing bounded
  XmlLite model-part scan therefore now retains the complete lattice source
  definition, keyed by canonical package part plus model-local object ID, and
  the adapter maps property ordinals back to lib3mf's opaque property handles.
  Keep that part-qualified key: Production parts may reuse local IDs.

  Butt disks and hemispheres are generated explicitly. A full-sphere cap on a
  tapered beam is not implemented as the spike's overlapping sphere shortcut:
  the frustum is trimmed at the analytic sphere/cone intersection and only the
  exposed spherical patch is emitted. Equal-radius sphere and hemisphere caps
  consequently produce the same exterior profile. Explicit balls and `all`
  mode endpoint balls share the same bounded sphere builder. Display-property
  gradients on beams fail as unsupported, per the extension; ordinary color
  and texture-coordinate endpoint properties flow through 3MF-004's material
  resolver and preserve authored interpolation.

  The bounded clipping subset is deliberately narrow and testable: `inside`
  against a closed axis-aligned box with exactly 8 corner vertices, 12 face
  triangles, no lattice, and one uniform normalized appearance. Triangle
  surfaces are clipped plane-by-plane; quantized boundary-edge loops close the
  cuts with the clipping mesh's appearance. General/nonuniform inside clips
  and parametric outside clips require a valid authored representation mesh or
  return `UnsupportedRequiredFeature`, never an unclipped preview. Referenced
  clipping/representation resources must be distinct plain `model` meshes in
  the same part and cannot recursively contain lattices.

  The scene contract can reuse the completed lattice geometry across normal
  object/build occurrences, but it cannot express a lattice-local cylinder
  transform nested beneath each document occurrence without multiplying the
  node/instance graph. The spike's template shortcut is therefore not exact in
  the current contract and was not used. Focused Debug tests pass 4 cases / 194
  assertions: tapered and uniform beams, all three cap modes, mixed balls,
  endpoint properties, sets, deterministic byte-identical output, authored
  representation preference, supported clipping bounds/caps, malformed
  values/indices/set references, sub-minimum beams, cancellation, unsupported
  clipping, budget pressure, and recovery. The combined 3MF regression set
  passes 15 cases / 1,097 assertions in Release; the earlier 3MF-001 worker
  tests continue to cover lattice cancellation plus Job-limit
  kill/replacement. Both full solutions build with zero warnings, and the full
  Unit suites pass (Debug 99/99, 7,627 assertions; Release 99/99, 7,539). The
  full Debug ImportIsolation run remains 285/303: all 18 failures are the
  already-recorded OpenUSD host/changed-USD-fixture and FBX fixture-rewrite
  failures, while every 3MF case passes. The locally supplied Bambu files
  remain untracked and were not added.

- **3MF-004 Materials/properties and contained textures (2026-09-18):
  complete; private until 3MF-006.** The adapter resolves
  Core base materials plus Materials Extension color, texture-coordinate,
  composite, and bounded multi-property resources at object, triangle, and
  per-corner scope. Geometry is deliberately deindexed at property seams and
  split into material-homogeneous chunks; a 32,768-entry normalized-material
  map prevents adversarial per-triangle combinations from growing without a
  cap. Image and material chunks are emitted before any geometry that depends
  on them, so the existing progressive catalog rules remain intact.

  Two format details cannot be left implicit in the general renderer. 3MF
  vertex colors interpolate in authored sRGB and become linear only after
  interpolation, while multi-property arithmetic is linear; a material flag
  now distinguishes those paths. 3MF UV `(0,0)` is lower-left, so texture
  materials carry the existing V-flip flag. Independent wrap/mirror/clamp/none
  and nearest intent are encoded in formerly reserved material flag bits and
  consumed by two fixed clamp samplers plus explicit shader addressing. For
  `none`, edge RGB with zero alpha is used only for a non-base multi-property
  texture layer; a standalone/base texture behaves as clamp and ignores image
  alpha, as required by the extension.

  Contained PNG/JPEG attachments are size-checked before `WriteToBuffer`,
  content-sniffed against the declared type, and decoded with the existing
  worker-only WIC allowlist under per-image and aggregate byte/pixel caps.
  Unknown/corrupt image bytes produce the bounded checker and warning; a
  positive PNG/JPEG signature that contradicts the declared MIME is malformed
  rather than silently decoded. The normalized image is tagged sRGB, allowing
  the D3D12 SRV to linearize samples before lighting or multi-property
  arithmetic.

  The pinned lib3mf 2.5 public bindings expose classic material/property
  groups but no realistic display-property resources or their
  `displaypropertiesid` associations. `ThreeMfDisplayProperties` therefore
  extracts already-preflighted `.model` parts in memory, verifies Deflate or
  stored bytes and CRC32, parses with XmlLite under 256 MiB/part, 512 MiB
  aggregate, depth, count, numeric, and cancellation bounds, and keys every
  association by canonical package part plus model-local resource ID. Never
  infer an association from a globally unique local ID: Production parts can
  reuse the same XML resource IDs.

  lib3mf property IDs returned by mesh/property APIs are opaque handles, not
  XML zero-based indices. PB display vectors must be indexed by the ordinal in
  each group's `GetAllPropertyIDs` result; treating the opaque value as the
  ordinal made valid property zero appear out of range. The adapter caches a
  bounded handle-to-ordinal map and independently checks display/property
  cardinality before normalization. PB metallic maps directly. PB specular
  converts its linear specular color and glossiness deterministically to the
  renderer's metallic/roughness model; unsupported translucent and display-
  texture groups preserve the ordinary base appearance and emit one fixed,
  bounded warning per group.

  lib3mf 2.5 strict mode rejects both valid Materials display-property XML and
  common current-slicer packages even though compatible mode constructs their
  standard scene. Production import therefore uses compatible mode only behind
  the product-owned OPC/XML boundary and still validates every value that can
  affect normalized output in the adapter. The older 3MF-001 strict spike stays
  strict so dependency behavior remains visible. Importer/cache version is 3.

  The opt-in `[.manual-3mf]` test reads `PREVIEW3D_MANUAL_3MF_DIR`. Of the three
  locally supplied Bambu files, `Ghosts.3mf` and the 948,263-triangle
  `leone-bambu.3mf` complete through the production worker (the latter via
  progressive batches). `Orbit Revolver Bambu Print File.3mf` is rejected as
  `UnsupportedRequiredFeature`: its printable model components recursively
  reference a Core `type="other"` mesh, while Core forbids `other` objects from
  entering the build recursively or directly. That is a deliberate Core policy
  result, not a material or archive failure. The three local files remain
  untracked. `.3mf` discovery and registration remain disabled; 3MF-005 is the
  next task.

- **3MF-003 Core and Production scene adapter (2026-09-18): complete;
  private only.** `Preview3DImportWorker.exe` now constructs lib3mf models
  through the same duplicated, read-only source handle used by the spike,
  with a progress callback for cooperative read-time cancellation. The strict
  reader claim from this slice is superseded by 3MF-004's documented
  compatible-mode boundary. Because lib3mf's callback ABI cannot report short
  reads or seek errors, the adapter records those facts out-of-band, zero-fills
  any unread callback destination, and rejects the generation after construction.
  The product OPC preflight still happens first; neither route uses a filename
  API or extracts package contents.

  The normalized scene contains only occurrences reachable from the root
  model's `<build>`: root build transforms and component transforms compose in
  double precision; a checked recursion stack enforces the shared 256-level
  cap; only `model`, `support`, `solidsupport`, and `surface` resources are
  accepted; and `other`/non-mesh resources fail instead of quietly appearing.
  Mesh resources are emitted once as deindexed, reusable geometry chunks and
  all occurrences reference them through normal node/instance records. This
  is important for Production packages: lib3mf resolves cross-part resource
  references, while the product deliberately does not enumerate child-model
  builds as extra plates. Units map to the six Core values and metadata is
  fixed at +Z up.

  The focused real-worker regression exercises Core, nested components, and
  Production multi-part fixtures through `ImportSession`; it asserts the
  `ThreeMf` scene identity, shared reusable geometry, Z-up metadata, and
  root-build occurrence counts. The route remains undiscoverable until
  3MF-004/005 implement appearance and lattice behavior, then 3MF-006 owns
  every viewer, activation, registration, package, and installer surface.
  OPC ZIP directory records are valid zero-byte structure: preflight validates
  their headers and ranges, but does not mistake them for package parts.

- **3MF-002 OPC boundary and protocol route (2026-09-18): in progress.**
  Protocol v10 can carry an additive `ThreeMf=12` source identity and a
  dedicated 48-byte `ParseThreeMfFileRequest`/opcode without changing any
  existing wire record. The route remains private: no viewer classifier,
  dialog, activation, registration, installer, package, or thumbnail path is
  allowed to discover `.3mf` before 3MF-003 through 3MF-005 have a complete
  scene contract. The broker accepts a 3MF generation only when the worker
  reports exactly `ThreeMf`, and treats it as Tier B.

  The product archive boundary is intentionally a new OPC checker rather than
  a renamed USDZ checker. USDZ's stored-only, 64-byte-aligned, no-ZIP64 policy
  is correct for USDZ but would reject valid 3MF containers. The new checker
  permits only stored/Deflate parts and begins enforcing central/local-header,
  ZIP64, duplicate canonical name, traversal, encryption, multi-disk,
  overlap, count, expanded-byte and expansion-ratio limits before any future
  lib3mf construction. `kThreeMfImporterVersion` is reserved now for the
  eventual viewer-owned derived-cache key; increase it for every normalization
  or preflight semantic change.

- **3MF-001 dependency and feasibility spike (2026-09-18): complete; private
  only.** The existing vcpkg baseline now pins lib3mf `2.5.0#1` (upstream
  2.5.0 commit `64bb454d1fcb53effa57d3cef752a10d740d41a2`) as an app-local
  worker dependency. Its Release closure is `lib3mf.dll`, `zip.dll`, `z.dll`,
  and `bz2.dll`; PE inspection confirms none enters `Preview3D.exe` or the
  thumbnail provider. Upstream provides no reader-only/writer-off build switch,
  so the product calls only reader APIs and depends on the sandbox and
  preflight policy rather than maintaining a source fork. Package notices/SBOM
  updates stay with 3MF-006, when the format actually ships.

  A private worker-pool route proves strict callback loading through the real
  AppContainer using only the duplicated source handle. Keep its exact-read
  pattern: lib3mf callbacks cannot return a byte count or error, so short
  reads, seek failures, and identity/size/last-write changes must be recorded
  out-of-band, the destination made deterministic, and acceptance denied after
  the call. A sparse valid package at 2 GiB minus 64 KiB read under 1 MiB.
  Product-owned OPC preflight is still mandatory; lib3mf is not the authority
  for ZIP64/streaming, relationships, paths, compression, expanded bytes, or
  required extensions.

  Progress callbacks cooperatively cancel package extraction, root/non-root
  model load, resources, and texture attachments with lib3mf error 10. They do
  not cover mesh getters, Beam Lattice access, or product normalization, so
  those loops need explicit event checks and the existing 500 ms kill/replace
  path remains the hard deadline. A 20 MiB Job pressure case surfaced only
  lib3mf generic error 5; Job/broker evidence must classify the limit and the
  process must be retired before reuse. Recovery passed after short read,
  source change, malformed input, cancellation, pressure, and forced
  replacement.

  For Beam Lattice, prefer a validated authored `representationmesh`; otherwise
  use bounded chunked product tessellation. Instance templates are exact only
  for equal radii, no clipping, and spherical caps. The spike's 12-segment,
  4,320-triangle fallback is intentionally approximate for hemisphere/clipping
  behavior, so 3MF-005 must supply the exact semantics before shipping. Debug
  and Release builds pass; `[3mf-spike]` passes 6 cases / 721 assertions in
  both. Full measurements, dependency hashes, fixtures, error mapping, and the
  3MF-002 go decision are in
  [3MF-001-SPIKE-RESULTS.md](./3MF-001-SPIKE-RESULTS.md). No extension filter,
  activation, installer, public protocol, viewer route, or thumbnail behavior
  changed.

- **USD-009 corpus and fuzz qualification slice (2026-09-18): complete;
  release qualification remains in progress.** A checked-in manifest now
  freezes 10 redistributable USDA/USDC/USDZ and composition sources plus 13
  deterministic malformed, unsafe, unsupported, recursion, archive and
  dependency-pressure derivations. Hashes cover decoded USDC/USDZ payloads,
  not base64 transport. The manifest records independent expected facts while
  existing real worker/host tests remain the numeric oracle, preventing either
  TinyUSDZ or OpenUSD from blessing its own output.

  The new standalone `UsdFuzz` target has five bounded no-GPU domains:
  TinyUSDZ USDA object graphs plus render-data normalization and USDC byte
  classification, product USDZ
  preflight, trusted normalized-output copy/validation, the host's exact pure
  identifier/anchoring policy, and production control framing seeded with
  OpenUSD-start and resolver-sidecar records. Its clean 11-seed Release
  ASan/libFuzzer smoke completed 38,814 executions in 61 seconds with no
  finding or timeout and 492 MiB peak reported RSS under a 1,024 MiB cap.
  OpenUSD itself is an ordinary pinned private DLL and is not falsely claimed
  as sanitizer-instrumented; real-host resolver/composition/Job/fault recovery
  stays in ImportIsolation.

  Two build details matter for the next fuzz target. MSVC ASan turns on STL
  string/vector/optional annotations, which cannot link to the ordinary pinned
  TinyUSDZ static library compiled without those ABI markers. Disable exactly
  those three annotations in the harness (while retaining ASan on harness and
  product boundary sources), or build TinyUSDZ with a dedicated sanitizer
  triplet; never force-link the mismatch. Keep standalone fuzz output
  project-private. An early draft pointed `UsdFuzz` at shared `x64/Release`;
  its correct incremental-clean pass removed sibling `fastgltf.dll` and
  `simdjson.dll`, making every Release worker request die before parsing until
  the product closure was rebuilt. Finally, libFuzzer grows the directory
  passed as its corpus, so always materialize seeds under ignored
  `TestResults/`, never point it at immutable source fixtures.

  TinyUSDZ's USDC memory option is advisory: a minimized mutated crate caused a
  multi-gigabyte allocation before that option reacted. Arbitrary USDC mutation
  therefore does not run in the long-lived in-process fuzz target. The input is
  frozen as a deterministic derived corpus case and a new 128 MiB Job-contained
  real-worker regression proves either controlled rejection or worker
  replacement, followed by a successful valid USDC import. The seed preparer
  now refuses non-empty destinations after a reused evolved corpus demonstrated
  why reproducible fuzz-smoke evidence must start from exactly the 11 seeds.

  A final sandbox/sidecar security selector exposed a test-only collision:
  `SidecarPathResolverTests` named scratch directories with process ID plus a
  stack address, which can repeat across processes and collide with stale test
  output. It now asks Windows for a unique temporary name before converting it
  to a directory. This did not change the production resolver, but keeps path
  containment evidence repeatable across Debug/Release and interrupted runs.

  Debug/Release solution targets and Unit pass (98 cases; 7,625 / 7,537
  assertions). Focused USD-002 through USD-009 passes 28 cases / 756 assertions
  in both configurations (the two new USD-009 regressions account for 45
  assertions), and the hostile-worker lane passes 13 cases / 273 assertions in
  both. There is no USD persistent-cache entry: the current cache is
  an unwired opaque-payload prototype. The reserved policy/wire/TinyUSDZ/OpenUSD
  tuple and exact commands are in
  [USD-009-VERIFICATION.md](./USD-009-VERIFICATION.md). Repeated p50/p95 and UI
  heartbeat evidence, full-suite closure, final package/tamper
  reruns, clean-VM lifecycle/loaded-host replacement, soak, and signed-candidate
  evidence remain open, so USD-009 and Gate 4 are not marked complete.

- **USD-007 bounded OpenUSD composition/normalization (2026-09-18): complete,
  still test-only.** The production compatibility core now opens broker-backed
  USDA/USDC/USDZ stages with `LoadNone`, loads payloads breadth-first, and
  normalizes the approved static scene into the same bounded progressive wire
  batches as the general worker. Composition covers sublayers, references,
  payloads, inherits, specializes, default variants, native instances, and
  point instancers. Geometry, transforms/bounds, material subsets, Preview
  Surface factors, WIC/WebP/KTX textures, warnings, and static-policy metadata
  all pass through the existing writer and trusted validator. No viewer,
  activation, association, package, or thumbnail surface changed; USD-008 is
  the next task and USD thumbnails remain USD-010.

  Several details are worth carrying forward. OpenUSD may call the resolver
  with an empty anchor for byte-backed composition arcs, so the private
  `preview3d://` resolver must explicitly anchor those at its namespace root;
  it must reject `..` before normalization or a traversal can disappear before
  the trusted broker sees it. The broker sidecar extension allowlist also had
  to admit `.usd`/`.usda`/`.usdc`—leaving it image-only made every legitimate
  composed layer look like `UnsafeReference`; `.usdz` is intentionally still
  absent so recursive packages remain closed. Identity-only xformables can
  leave `UsdGeomXformable::GetLocalTransformation`'s boolean false while the
  initialized identity matrix is valid, so matrix validity—not that boolean—is
  the useful normalization gate. Finally, OpenUSD's USDA parser is stricter
  than TinyUSDZ about metadata-block layout; multiline authored metadata in
  fixtures prevents a fast-parser-only test from masking malformed fallback
  input.

  The overlap corpus caught two tiny but user-visible normalization drifts:
  OpenUSD initially emitted a synthetic +X tangent where TinyUSDZ deliberately
  emits a zero tangent, and it marked constant display color as per-vertex
  color where the fast route does not. Aligning those rules now makes the full
  canonical chunk fingerprint exact after removing only the exclusions named
  in `.docs/usd.md` (generation/encoding/source offsets/checksums). Original
  USD-007 fixtures record SHA-256 and independent expected facts. Focused
  tests cover all approved arcs, USDZ composition, point-instance masking,
  exact fast/compat overlap, materials/textures and missing fallback, malformed
  and recursive layers, unsafe traversal, unsupported required content,
  dependency pressure, cancel/replace, crash/hang/protocol/commit faults, and
  later recovery. Adding WebP/KTX parity also means USD-008 must package and
  dependency-audit `ktx`, `zstd`, `libwebp`, and `libsharpyuv` beside the
  already-private OpenUSD host; they are not viewer/general-worker imports.
  Debug/Release solution builds are clean. Focused USD-002 through USD-007
  passes 25 cases / 705 assertions, including USD-007's 5 cases / 186
  assertions. Unit remains green
  at 98 cases (7,617 Debug / 7,529 Release assertions). Full Debug isolation
  reaches 278/282 with only the four pre-existing FBX fixture-rewrite failures;
  Release reaches 271/282 with those four plus seven already-documented
  randomized sidecar scratch-directory collisions. No full-run failure enters
  a USD route.

- **USD-006 compatibility-host lifecycle (2026-09-17): complete, still
  test-only.** The broker now consumes only an exact pre-publication
  `UnsupportedComposition` from TinyUSDZ, discards the fast attempt, and
  starts a fresh normalized session through the distinct
  `Binbuf.Preview3D.ImportHost` AppContainer. The host has its own additive
  opcode/request and closed producer identity while reusing the existing
  bounded framing, dependency servicing, output window, copy-then-validate,
  cancellation, and Job enforcement. It exits after its one generation; bad
  hosts are killed, not replaced/retried within that generation.

  The bootstrap locks DLL/environment discovery before input and remains free
  of OpenUSD imports. Its first production request loads the core by absolute
  private path and audits the 13-file resource inventory. At USD-006
  completion, composed-stage normalization was deliberately left to USD-007
  and the production host returned `CompatibilityHostFailure` after
  platform/payload validation rather than exposing USD-002 spike data. Viewer
  format discovery remains disabled.

  Debug/Release solution builds pass. Focused USD-006 passes 2 cases / 64
  deterministic assertions in both configurations, and USD-002 through
  USD-006 focused coverage is green. Unit passes 98/98 (7,617 Debug / 7,529
  Release assertions). Full Debug isolation reaches 273/277 with only the
  four known FBX fixture-rewrite failures; Release reaches 271/277 with those
  four plus two known sidecar scratch-directory collisions. PE dependency
  inspection confirms OpenUSD is absent from the viewer, general worker,
  thumbnail DLL, and bootstrap. USD-007 is now ready; USD remains undiscoverable
  until USD-008 and thumbnails remain USD-010.

- **USD-005 USDZ/material/texture fast path (2026-09-17): complete, still
  test-only.** The TinyUSDZ route now imports independently preflighted USDZ,
  brokered local or archive-contained image assets, USD Preview Surface
  materials, material subsets, and product-decoded WIC/WebP/KTX textures. The
  accepted fast boundary is unchanged: one self-contained root layer plus
  images; every composition arc remains an atomic `UnsupportedComposition`
  handoff for USD-006/007. No viewer, registration, activation, installer
  extension, or thumbnail behavior advertises USD yet.

  Debug and Release clean worker/test builds pass with warnings-as-errors;
  `[usd-005]` passes 5 cases / 91 assertions in each configuration, including
  contained USDZ assets, brokered PNG, subset bindings, archive cancellation,
  optional missing/corrupt fallbacks, extension/content mismatch, and unsafe
  traversal. TinyUSDZ's overlay is now `0.9.1#2`, the installed/portable
  copyright includes its enabled vendored components, and the Release worker
  is 5,413,376 B (27,136 B over USD-004; no new DLL). USD-006 is the next
  independently ready task; thumbnails remain USD-010.

  Unit passes 98/98 in both configurations (7,617 Debug / 7,529 Release
  assertions). Full Debug isolation passes 271/275 with only the four known
  FBX fixture-string rewrite failures. Full Release passes 263/275 with those
  four plus eight already documented randomized sidecar scratch-directory
  collisions; all focused USD cases are green in both runs. The unsigned
  engineering portable-package check passes dependency closure, stages the
  50,356-byte combined TinyUSDZ notice, and lists TinyUSDZ `0.9.1#2` in SBOM.

- **USD-003 USD-family protocol and normalized contract (2026-09-17):
  complete.** Protocol v10 remains byte-compatible. Closed source identities
  `USDA=9`, `USDC=10`, and `USDZ=11` and `UpAxisId::X=3` were appended without
  moving the existing Y/Z values or changing a wire record. The existing
  normalized node/geometry/instance/material/image catalogs can represent the
  approved subset: point instancers expand into bounded instances and
  per-face material subsets become ordinary geometry sections. The shared
  fast/OpenUSD canonical digest is now specified in `.docs/usd.md` over the
  deterministic post-triangulation semantic stream; encoding, producer,
  offsets, batch boundaries, and diagnostics are deliberately excluded.

  `ImportFormat::Usd`, a dedicated 48-byte request/opcode, pooled dispatch, and
  one-shot `--parse-usd` now form a test-only end-to-end route. `.usd` is
  byte-authoritative; explicit suffixes carry exactly one expected-encoding
  flag and mismatches are terminal. The worker independently preflights USDZ
  and maps archive-policy failures to the new terminal `ArchiveLimit` code.
  It emits a one-point contract marker because `ImportSession` correctly
  rejects metadata-only successful imports; USD-004 must replace that marker
  with real TinyUSDZ-normalized geometry. No viewer format classifier,
  activation, registration, package, or thumbnail surface advertises USD.

  USD validation is Tier B, requires an actual USDA/USDC/USDZ identity, accepts
  X/Y/Z only, and requires finite positive USD units. Geometry provenance packs
  the source prim/object ordinal into the high 32 bits of `sourceRangeOffset`
  and the normalized point/index start into the low 32 bits. Define
  `NOMINMAX`/`WIN32_LEAN_AND_MEAN` before TinyUSDZ headers: defining them after
  a project header that transitively includes Windows headers still permits
  `min`/`max` macro collisions.

  The new typed failures are `UnsupportedComposition`,
  `CompatibilityHostFailure`, `CompatibilityHostLimit`, and `ArchiveLimit`,
  with host-owned redacted messages. Only exact, pre-publication
  `UnsupportedComposition` sets `compatibilityFallbackRequired`; every other
  failure is terminal. A pure generation-scoped state machine begins with no
  producer and rejects late/reverse fallback, loops, or mixed producers while
  ignoring stale generations. USD-006 can now wire that contract to the lazy
  OpenUSD host without redefining its authority or publication semantics.

  Debug and Release focused USD-003 tests pass (6 cases / 119 assertions each)
  across pooled/one-shot format detection, suffix spoofing, archive policy,
  axes/units, hostile fallback, and state transitions. Unit passes 98 cases in
  both configurations (7,617
  Debug assertions; 7,529 Release assertions, with three existing unavailable
  debug-layer warnings in Release). Full Debug ImportIsolation reached 268
  cases / 101,139 assertions with only the four already documented FBX
  fixture-string rewrite failures. The first full Release run reached the same
  USD coverage; its unrelated failures were inherited FBX rewrites plus a
  stale sidecar scratch-directory collision from the existing PID/address
  naming scheme. USD-004 and USD-006 are unblocked; USD-010 remains the
  separate thumbnail task.

- **USD-001 TinyUSDZ feasibility and policy spike (2026-09-17): complete with
  a revised fast-path boundary.** The repository now pins TinyUSDZ v0.9.1 at
  commit `a04ee0bcbd1a930e30cc40938fcee3526a6fa8eb` through a checked-in static
  overlay. USDA, USDC, and stored/aligned USDZ load from broker-supplied memory
  and produce identical normalized hashes in Debug and Release inside the real
  zero-capability AppContainer worker. This is a test-only
  `--usd-spike-pool` route; no product opcode, extension, viewer route, or
  thumbnail behavior is exposed.

  TinyUSDZ has a usable whole-asset resolver callback but no ranged asset,
  allocator, progress, or cancellation callback. Its
  `max_memory_limit_in_mb` option is advisory: a roughly 4 MiB USDA load
  succeeded with a 1 MiB setting. A 16 MiB Job limit instead produced a
  controlled allocation failure and the same worker handled the next valid
  request. Noninterruptible work is contained by the existing 500 ms grace,
  terminate/replace path (515 ms Debug, 514 ms Release). Future adapters must
  preflight/account everything they own and treat the worker Job limit as the
  hard in-call allocation backstop; do not describe the library option as a
  budget. Define `NOMINMAX` before the installed TinyUSDZ headers on Windows.

  The exact fast policy is now deliberately a self-contained root layer. Any
  USD composition arc returns `UnsupportedComposition` before publication and
  is eligible for the OpenUSD host; image asset dependencies remain brokered
  bytes, not composition. Malformed, unsafe, archive-limit,
  unsupported-required-schema, and resource-limit failures never get a more
  permissive retry. Product code independently preflights USDZ EOCD,
  central/local agreement, normalized paths, case collisions, ZIP64,
  encryption, stored-only method/size, 64-byte alignment, entries, aggregate
  expansion, and ratio before TinyUSDZ access.

  The Release worker grew from 779,264 B to 5,304,320 B (4,525,056 B delta);
  no DLL was added. The upstream static archive is monolithic and includes
  vendored code even though codecs/bridges are disabled, so USD-005 must audit
  actual linked members and embedded third-party notices instead of shipping
  only the top-level Apache-2.0 file. The Visual Studio-bundled vcpkg
  `2026-07-27` is required for this repository baseline; the older standalone
  `2025-06` client fails in current helper scripts with unsupported CMake list
  operations.

  Focused USD passes 7 cases / 180 assertions in both configurations. MSVC
  14.51 LTCG ICEs after the TinyUSDZ archive is added to the already-large
  Release ImportIsolation harness, so WPO is disabled only for that test
  executable; product Release LTCG remains enabled. A full Debug isolation run
  reached 257 cases / 100,850 assertions with four independently reproducible
  pre-existing FBX fixture-string rewrite failures; the USD cases remain
  green. Full dependency, API, feature-policy, measurement, fixture, and
  follow-on details are in `.docs/USD-001-SPIKE-RESULTS.md`. USD-002 and
  USD-003 are unblocked; thumbnails remain a separate USD-010 task.

- **FBX-006 viewer, activation, and installer integration (2026-09-17):
  complete.** `.fbx` now routes case-insensitively through the existing
  AppContainer import bridge and normalized progressive renderer. Direct and
  secondary activation, the picker, one-file drop boundary, retry/replacement,
  metadata, Info UI, Open With discovery, portable distribution, and NSIS all
  agree on the format. Shell catalog revision 4 invalidates the previous
  five-extension Open With cache. NSIS owns `Binbuf.Preview3D.FBX.1`, advertises
  capabilities and SupportedTypes without touching `UserChoice`, and removes
  only product-owned state. The reset helper now also includes the formerly
  omitted OBJ ProgID. No thumbnail CLSID or `shellex` registration was added.

  The real-app static-pose check presents the combined skin/blend fixture as
  528 vertices, 176 triangles, 4 materials, 15 nodes, 6 meshes, 1 animation,
  1 skin, and 4 bones with verified bounds and 26 displayed GPU chunks. It
  revealed two reusable renderer lessons. First, progressive node records may
  be split across publications; the import bridge owns the generation-wide
  node catalog and stamps resolved instance transforms, so the GPU uploader
  must not reject a publication-local child merely because its parent arrived
  in an earlier batch. Second, the color MRT and integer `R32_UINT` pick-ID MRT
  require `IndependentBlendEnable`; inheriting alpha blend state into the
  integer target causes current D3D12 drivers to reject otherwise-valid grid
  and blended-material PSOs during startup.

  Product-facing bridge tests must call `EnsureImportSandboxPrepared()` before
  a filtered/standalone pooled `RunImport`; full-suite ordering can otherwise
  conceal a `LaunchWorker` failure. Real activation smoke uses a dedicated
  singleton-aware smoke flag because the general `--app-smoke` contract
  intentionally bypasses normal activation IPC. Cross-process tests also must
  not manufacture `HDROP` storage in the Python process: the bounded drop-path
  injection starts after the Shell-decoded one-path boundary and shares the
  production open path.

  Debug and Release Unit pass 98 cases / 7,612 and 7,524 assertions. Full
  ImportIsolation passed 250 cases / 100,704 assertions in both configurations
  before the final viewer-only corrections; focused FBX passes 26 / 47,977
  afterward. Activation, targeted metadata/static-pose, and malformed-FBX
  recovery smokes pass in both configurations with no leaked worker and no
  Debug D3D errors. Portable packaging passes with 34 files and SHA-256
  `28e8344f768581975afc019bf20ee89e843dd17d69e10c93826a26d13d6d69ee`;
  NSIS `/WX` passes with 35 files and setup SHA-256
  `375938af2dbd97ca6d7467785e1971d566329db75f1df9c05ca906b6fc8978f7`.
  The VS 18.10 Release LTCG linker intermittently ICEs on the unusually large
  ImportIsolation binary; a serial non-LTCG build and focused FBX run pass, and
  the Release viewer/worker rebuild cleanly through both package targets.
  FBX-007 is unblocked for corpus/fuzz/performance and clean-VM release evidence;
  Explorer thumbnails remain separately deferred to FBX-008.

- **FBX-005 unified materials and texture dependencies (2026-09-17):
  complete.** The static FBX adapter emits normalized material chunks from
  ufbx unified PBR maps with FBX diffuse/transparency/emission fallbacks,
  bounded non-finite clamping, alpha/double-sided/unlit state, and per-instance
  material selection without duplicating shared geometry. The material-factor
  conversion is a small common helper used by OBJ as well; the OBJ policy path
  deliberately retains its previous payload behavior.

  FBX enables ufbx embedded media and decodes embedded bytes directly in the
  AppContainer. External image references do not enable ufbx file access:
  they go through `RequestSidecarFile`, the existing pinned-replay client, the
  host containment checks, byte/request caps, byte sniffing, explicit WIC or
  WebP/KTX2 decode paths, and aggregate decoded-image budgets. Unsupported
  layered/procedural/shader texture graphs only accept one unambiguous file
  leaf (with a bounded approximation warning); otherwise the deterministic
  optional texture fallback is emitted. Unsafe sidecar results stay terminal
  and report the Sidecars phase. FBX remains disabled on viewer/Shell/product
  surfaces until FBX-006.

  The reported external-texture worker crash was a test-harness limit, not a
  worker/control-channel fault. `FbxImportTests` constructed requests with
  `maxSidecarRequestsPerGeneration == 0` and `maxSidecarFileBytes == 0`; the
  trusted broker therefore correctly stopped the first request at
  `SidecarRequestLimit`, and the one-shot job then closed. Production's
  `D3D12ImportBridge` already supplied nonzero limits. An untouched upstream
  external-texture FBX reproduced the same limit before the helper was fixed.
  Future format tests that expect sidecars must initialize both caps explicitly
  rather than diagnosing the expected one-shot exit as a worker crash.

  Embedded PNG/JPEG/WebP, approved local sidecars, sRGB/linear texture roles,
  material factors, alpha, emissive, normal/bump, UV transforms, deterministic
  missing/corrupt/byte-cap fallback, and hard path attacks are covered. The
  pinned upstream instanced-material fixture proves distinct instance material
  IDs keep one geometry resource; the layered-texture fixture proves ambiguous
  graphs retain visible geometry with bounded warnings. A test-only aggregate
  budget flag proves decoded texture pressure is a typed `ResourceLimit` and
  that the same pooled worker recovers, without changing the production 128 MiB
  limit. Progressive image/material dependency validation and cancellation /
  recovery coverage remain green.

  One additional harness lesson: the FBX scratch-directory name used only PID
  plus `GetTickCount64()`, which could collide when multiple fixtures stayed
  alive in one fast test. A monotonic process-local suffix now makes those
  directories unique independent of clock resolution.

  Debug and Release solution builds pass. Focused materials pass 12 cases / 284
  assertions; all FBX passes 25 / 47,968; Unit passes 98 / 7,609 Debug and
  7,521 Release; full ImportIsolation passes 249 / 100,693 in each
  configuration. OBJ regression parity passes 7 / 82 in both configurations.
  FBX-006 is unblocked; thumbnails remain separately deferred to FBX-008.

- **FBX-004 deterministic static deformation pose (2026-09-17): complete.**
  The AppContainer FBX adapter now evaluates the first authored animation
  stack at its authored start, or the default/rest animation at zero, and bakes
  supported linear, rigid, dual-quaternion, blended DQ/linear, blend-only, and
  combined skin-plus-blend deformation into the existing static scene/instance
  contract. `SceneMetadata` records animation-stack, skin-deformer, and bone
  counts without retaining names or curves. Load and evaluation have separate
  explicit temp/result allocation limits; caches and external files remain
  disabled.

  A crucial normalization detail for later FBX work: ufbx 0.23.0 exposes
  undeformed `skinned_*` attributes as local data (`skinned_is_local=true`) but
  may expose evaluated deformation in world space. Local data must use
  `node.geometry_to_node`; world data must be transformed by inverse
  `node.node_to_world` before it enters the node-local wire geometry, otherwise
  the retained node transform is applied twice. Normals use the matching
  inverse transpose. Blend results are already present in `skinned_*` after the
  one scene evaluation and must not be applied a second time.

  Deformed reuse compares the canonical node-local evaluated output, not mesh
  pointers or node transforms. The fingerprint includes source mesh identity,
  topology/attribute decisions and winding, is followed by exact comparison,
  and charges all scan/comparison work to the Tier-B index limit. This both
  shares equivalent results and prevents a hostile instance catalog from
  creating unbounded comparison work. Material bindings remain per instance
  for FBX-005.

  Cache-deformed/subdivision meshes are omitted rather than rendered at rest;
  caches, constraints, NURBS/trim objects, subdivision and procedural geometry
  warn only when independent visible polygons remain, while required-only
  unsupported geometry fails typed. The existing combined skin/blend fixture
  also contains cache-deformed meshes, making it a useful regression for the
  warn-and-omit branch. `nurbs-only-ascii.fbx` covers required failure and has
  SHA-256 `E62D8758117D22020552B9DD5D27BB93C1D81CFB1EE915C95479B89F972869CB`.

  ufbx still has no evaluation progress callback. Product cancellation checks
  bracket evaluation and continue through canonical comparison/normalization;
  cancellation inside evaluation relies on the already-proven 500 ms broker
  grace and worker replacement. A 1 KiB evaluator-limit seam proves typed
  `ScratchLimit` and same-worker recovery. Numeric 1e-6 wire goldens cover all
  supported deformation modes, stack/rest selection, transforms, normals,
  tangents, bounds, metadata, warnings and deformed sharing in Debug and
  Release. Focused FBX passes 13 cases / 47,670 assertions; full Unit passes 98
  cases / 7,609 Debug and 7,521 Release assertions, and full ImportIsolation
  passes 237 cases / 100,395 assertions in both configurations. FBX remains
  undiscoverable in viewer/Shell surfaces until FBX-005 and FBX-006.

- **Gate 4 Slice 1 OBJ/MTL product path (2026-09-16): implemented; release
  qualification remains open.** ufbx 0.23.0 is pinned through a repository
  vcpkg overlay and is linked only into the AppContainer import worker. Direct
  `.obj` opens now use the existing trusted primary-file handle and broker-only
  sidecar protocol: `.mtl` and supported local texture references are contained
  to the source directory, byte-capped, and never opened directly by the worker.
  MTL remains sidecar-only. The adapter imports polygon faces with deterministic
  triangulation, smoothing/generated normals, UVs, vertex colors and object/group
  mesh separation, then normalizes MTL colors, opacity, roughness/metalness,
  double-sided state, UV transforms and broker-approved base-color, normal/bump
  and emissive maps into the existing image/material/mesh wire chunks. Distinct
  scalar roughness and metalness maps currently produce an optional-feature
  warning rather than an invented packed texture.

  OBJ deliberately uses the bounded Tier B contract: 2 GiB primary source,
  4 GiB aggregate source, 20 million triangles, 60 million expanded vertices,
  50,000 objects and 32,768 materials. It emits progressively sized normalized
  batches but does not claim the Tier A representative-coarse/detail protocol.
  The viewer, secondary activation, Open dialog/drop copy, Information panel,
  installer/portable documentation and `.obj` shell registration are wired.
  Focused isolation coverage proves textured quad/material normalization,
  missing-MTL geometry fallback, escaping-sidecar rejection, multi-batch output
  through a 4 KiB section, object/material partitioning with generated normals,
  and malformed-then-valid worker recovery. Debug and Release solution builds
  pass; the full Unit suites pass 97 cases / 7,458 Debug and 7,370 Release
  assertions, and the full ImportIsolation suites (including hostile-worker
  coverage) pass 209 cases / 52,164 assertions in both configurations. The
  unsigned portable target passes closure and includes `licenses/ufbx.txt`; the
  unsigned NSIS installer target also builds with the new `.obj` registration.
  The remaining Gate 4 bundle is explicit in TODO: checked-in golden/malformed
  corpora, a fuzz seed, cache-version evidence and clean offline standard-user
  VM qualification are not claimed by this implementation pass.

- **Post-MVP ASCII STL/PLY amendment (2026-09-16): complete in the product
  import path.** The existing product-owned ASCII parsers are no longer hidden
  behind test-only worker switches. Format detection now selects a bounded
  Tier B path in normal one-shot and pooled workers, reports distinct ASCII
  source provenance, and emits progressive wire batches while deliberately
  bypassing the Tier A coarse-proxy/detail protocol. The broker independently
  validates Tier B geometry limits and source ranges. ASCII input is capped at
  2 GiB, 20 million triangles or points, 60 million expanded vertices, and the
  lower of 1.5 GiB or 35% of physical RAM for scratch. STL/PLY parser suites and
  the shipping pooled/coarse-request integration path pass; UI, picker, About,
  portable, installer, and active-scope copy now advertise ASCII support.

- **Post-MVP Open With catalog amendment (2026-09-16): complete.** The existing
  title-bar menu now promotes Windows-registered handlers from a curated list of
  common CAD, modeling, and 3D-printing tools into labeled groups, retains other
  Windows-recommended handlers, and keeps the system "Choose another app…"
  fallback. A bounded, fail-closed metadata cache under `%LOCALAPPDATA%` avoids
  repeated discovery; a dedicated Shell STA loads and incrementally refreshes it
  at most weekly, resolves/invokes selected handlers, and invalidates/rescans on
  launch failure without blocking the UI thread. It stores no model paths,
  launch history, or model-derived data and adds no association/installer state.
  Debug and Release solution builds pass with warnings as errors. The full Unit
  suites pass 96 cases / 7,398 Debug and 7,310 Release assertions, including 3
  new bounded-cache cases / 15 assertions in each configuration. A Release
  real-process lifecycle/cache smoke opened and closed cleanly and produced a
  bounded 1,132-byte cache.

- **Large scan-throughput optimization follow-up (2026-09-16): STL and PLY now
  meet the retained five-second complete-coarse target; GLB is narrowly above
  it, with early display and small-file behavior preserved.** Protocol v9
  replaces byte-serial wire FNV with XXH64 for payloads below 10 MiB and a
  fixed-leaf, split-invariant parallel XXH64 tree for larger payloads. The
  integrity checksum remains non-cryptographic; AppContainer isolation plus
  host copy-then-validate checks remain the security boundary. Coarse sampling
  hashes positions rather than unrelated vertex attributes, directly traverses
  validated deindexed geometry, and parallelizes large identity-indexed
  triangle regions. Large catalogs reserve descriptor tables once rather than
  repeatedly moving accumulated payload, while catalogs below 64 regions keep
  the original single-batch path.

  The Tier-A adapters retain their bounded-memory and replay contracts. Binary
  STL normalizes independent valid facets in parallel and replays a bounded
  block through the original scalar compactor if it contains an invalid facet.
  Fixed-width point-only PLY skips unknown property conversion, reads each
  contiguous record range once, and decodes XYZ plus bounds in bounded parallel
  blocks. The common GLB path uses direct tight accessors, validates monotonic
  index streams without retaining a duplicate index vector, uses bounded dense
  remaps for clustered non-monotonic indices, integrates rebasing/bounds into
  decode, and generates deindexed normals in parallel. Fine detail remains
  demand-driven with one outstanding request.

  Three fresh-process Release compatibility runs measured complete-coarse p95
  at 4,185.989 ms for the 3.00 GB STL, 4,667.902 ms for the 2.88 GB
  little-endian point PLY, and 5,217.987 ms for the 4.29 GB GLB. Their first-
  geometry p95 values were 539.466, 506.841, and 511.429 ms respectively. Three
  corpus A-small runs retained a 466.718 ms complete-coarse p95, and a separate
  8 MiB A-small copy completed at 491.910 ms p95. These are local
  Balanced/60 Hz compatibility results, not the unavailable performance-
  reference qualification. The GLB complete-coarse row remains 217.987 ms over
  target, and the STL compatibility runs still fail the separate frame-interval
  gate while their coarse scan completes under target; neither is waived.
  Both Debug and Release solution builds pass. The final Unit suites pass 92
  cases / 7,360 Debug and 7,272 Release assertions; the final ImportIsolation
  suites pass 200 cases / 52,068 assertions in each configuration, including
  current hostile-worker protocol coverage. No MVP format, limit, isolation,
  cache, or detail-residency scope was expanded.

- **TSK-305 blocker remediation follow-up (2026-09-16): the retained 8M-point
  timeout is fixed; multi-GiB early proxy publication is fixed; large complete-
  coarse throughput is still above target.** The pooled product worker now
  enters the same representative preview pass as one-shot workers (the request
  flag had enabled proxy mode without enabling `Preview()`), so the retained
  3 GB STL, 2.88 GB PLY points, and 4.29 GB GLB all publish representative
  geometry in roughly 0.42--0.56 seconds instead of showing no geometry for
  30 seconds. Protocol v8 replaces each scan region's duplicated normalized
  payload with a fixed 40-byte, section-checksummed catalog summary while the
  descriptor retains full counts, verified bounds, layout, source range, and
  normalized-detail checksum. The host validates those summaries and still
  validates every requested fine payload before upload. Product detail-service
  imports now terminate their initial response at the complete coarse catalog
  and decode fine regions only on request; the single reusable worker section
  admits one highest-priority detail at a time, eliminating a stale 18-request
  backlog across budget drops. Binary PLY fixed-width records are read once per
  record, position-only point clouds stay in the 12-byte layout, and full-layout
  point regions are capped at 8 MiB.

  The exact formerly failing 8,000,000-point little-endian PLY now reaches first
  geometry / verified complete coarse / first refinement at 424.325 / 1,526.166 /
  1,659.380 ms in a Release qualification run. Its pressure/eviction/source-
  pinned-recovery lane passes separately on both the discrete adapter and the
  simulated-UMA adapter, with 62 coarse regions, one admitted fine region, one
  eviction/recovery request, <=1 outstanding detail request, bounded queues,
  texture fallback, controlled resource error/reopen, zero surviving workers,
  and no 180-second timeout. The discrete run also passes the same full lane for
  the 60-million-triangle / 3,000,000,084-byte STL. Multi-GiB compatibility
  measurements now complete verified coarse for STL at 12,471.528 ms and PLY
  points at 11,438.074 ms; the 4.29 GB GLB publishes representative geometry at
  475.661 ms but still does not complete its scan inside 30 seconds. Therefore
  the named absence-of-proxy failures and 8M timeout are resolved, but the
  retained <=5-second large complete-coarse acceptance row remains blocking and
  the release is still not ready. After the protocol change, full Debug and
  Release suites pass: Unit 91 cases / 7,354 Debug and 7,266 Release assertions;
  ImportIsolation 200 cases / 52,068 assertions in each configuration.

- **Scope-limited MVP, Phase 3 / TSK-305 release acceptance baseline (2026-09-16):
  executed; release blocked.** At that acceptance commit, clean Debug/Release solution rebuilds pass. Unit
  passes 91 cases / 7,354 Debug and 7,266 Release assertions; ImportIsolation
  passes 200 cases / 51,943 assertions in each configuration, including the
  current hostile-worker/protocol boundary. Debug/Release activation,
  accessibility and lifecycle smokes pass, as do the final Release progressive,
  texture, recovery and coarse-handoff lanes. Three A-small compatibility runs
  pass with 456.796 ms complete-coarse p95, 4.391 ms frame p95 / 17.315 ms max,
  and 4.619 ms input p95. The 3 GB STL produced no geometry in 30 seconds,
  and an isolated 8M-point PLY reproducibly timed out at the budget lane's
  180-second complete-proxy bound in both discrete and simulated-UMA modes.
  Performance-reference, physical UMA/mixed-DPI/assistive-technology, signing,
  and clean offline standard-user VM evidence remain unavailable. The first
  post-TSK-304 package correctly failed because its strict system-DLL allowlist
  lacked the new inbox imports; adding `bcrypt`, `oleacc`, and
  `uiautomationcore` fixed the package closure. The final unsigned engineering
  ZIP hash is
  `c1938aeaf67d7792d2b27f5e50a7bd56383c7ca3c303ae6242b1e3994c582c7b`.
  All manifest hashes and nine extracted-package Tier A/compressed/Unicode-
  sidecar smokes pass with zero process remnants and idempotent profile cleanup.
  Exact commands, the retained acceptance matrix, material limitations and
  blocker handoff are in [TSK-305_VERIFICATION.md](./TSK-305_VERIFICATION.md)
  and `tests/fixtures/baselines/tsk-305/acceptance.json`. **The scope-limited MVP
  is not release-ready; no failed or unavailable retained row is waived.**

- **Scope-limited MVP, Phase 3 / TSK-304 (2026-09-16): complete.**
  Normal launches now elect one primary per interactive user/session and forward
  one bounded Open or Activate command over a local-only named pipe protected by
  an explicit current-user/System ACL, client SID/session authentication, strict
  versioned framing/UTF-8 JSON, and a bounded UI-thread notification queue.
  Later opens use the existing asynchronous generation replacement path; close
  leaves no daemon, and foreground-policy denial flashes the taskbar. Custom
  title, bottom-bar and gizmo controls now have keyboard navigation and native
  UIA fragments with names, roles, state, bounds and Invoke/Toggle behavior;
  native error buttons and dialog focus behavior remain intact. Load/error/warning
  changes are announced. High contrast uses system colors, and reduced motion
  removes transient animation/inertia while preserving the default camera feel.
  DPI relayout now includes both bars. Debug/Release solution builds pass with
  zero warnings/errors. Unit passes 91 cases / 7,354 Debug and 7,266 Release
  assertions; ImportIsolation remains 200 cases / 51,943 assertions in each
  configuration. Debug/Release activation, accessibility and lifecycle real-app
  smokes pass, including replacement during load, recovery after failure,
  malformed/oversized pipe frames, close/relaunch, UIA actions during work,
  Alt+Space/Snap/fullscreen behavior, native-dialog focus restoration, preference
  paths, and narrow 96/144/192-DPI layouts. The available two-monitor machine is
  150% on both displays, so physical unlike-DPI crossing and assistive-technology
  speech review remain explicit TSK-305 compatibility-matrix work. Exact behavior,
  commands, results and qualification notes are in
  [TSK-304_VERIFICATION.md](./TSK-304_VERIFICATION.md) and
  `tests/fixtures/baselines/tsk-304/`. **TSK-305 is next.**

- **Scope-limited MVP, Phase 3 / TSK-303 (2026-09-16): portable
  packaging implemented; signed clean-VM acceptance remains open.**
  `msbuild Preview3D.slnx /t:CreatePortableRelease
  /p:Configuration=Release /p:Platform=x64` is now a real solution-level
  target, builds only the Release x64 viewer/worker product graph, waits for
  app-local deployment, then runs one clean allowlisted packaging pass. The
  viewer prefers a packaged `worker\` subdirectory (with the existing shared
  output fallback for development), so first-use AppContainer provisioning
  grants read/execute only to the sandbox runtime payload rather than the
  archive root. The archive includes the exact viewer/worker PE closure,
  app-local MSVC CRT, pinned-version CycloneDX SBOM and vcpkg ABI/baseline
  provenance, installed-port license texts/notices, support/limit/cleanup docs,
  per-file hashes, and an adjacent archive SHA-256. It rejects missing or
  unresolved non-system imports, excluded binaries, debug runtimes, symbols,
  libraries, and stale shared-output files. Certificate-thumbprint signing
  signs and verifies staged viewer/worker copies before hashing/archiving;
  absent credentials produce an explicit unsigned engineering artifact.
  The final local ZIP hash is
  `156eed9a52b585f84f51c485e5ab0e69961c5a73130fc90709456deceaa07118`.
  A clean extraction passed GLB, local-sidecar glTF, binary STL, PLY mesh and
  PLY point benchmark smokes with no process remnants. The unelevated
  first-use check placed zero AppContainer ACEs on the package root and two
  read/execute ACE forms on `worker\`; cleanup removed both and the per-user
  profile and was idempotent. An ordinary Release solution build did not
  recreate the archive; Unit passed 87 cases / 7,239 assertions and
  ImportIsolation passed 200 cases / 51,943 assertions. The local executables
  are intentionally unsigned
  (`MANIFEST.json` says so), and no clean offline Windows 11 standard-user VM
  was available. Signing and that VM matrix remain required in TSK-305; the
  TSK-302 large-model failures also remain release blockers. Exact content,
  commands, timings, dependencies, signing procedure and open evidence are in
  [TSK-303_VERIFICATION.md](./TSK-303_VERIFICATION.md). TSK-304 is next.

- **Scope-limited MVP, Phase 3 / TSK-302 (2026-09-16): harness complete; retained large-model gates remain blocking.**
  `Preview3D.exe --benchmark=<fixture>` now sustains rendering on the dedicated
  render thread while keeping the UI pump live, with bounded duration/frame/repeat
  options, result-file or attached/allocated-console JSON, raw intervals and
  mean/median/p95/max/failure/exclusion data. It records background/loading UI,
  first geometry, complete coarse, verified bounds, refinement, synthetic
  input-to-present and heartbeat milestones. A background sampler reports viewer
  and pooled-worker private commit plus committed mapped views separately; worker
  private commit is also asserted as a labeled conservative scratch upper bound; queue
  and live/pending/retired GPU accounting and the actual configured general-worker
  Job cap are included. Applicable violated gates return nonzero. The opt-in
  PresentMon 2.x wrapper preserves ETW present intervals with automatic exclusion
  reasons and adds hashes plus design-doc-09 machine/run metadata. Warm cache,
  Tier B, thumbnails and MSI are absent by design.
  Qualification exposed and fixed an immediate command-line-open race that had
  captured a zero CPU cap before asynchronous renderer initialization. Debug and
  Release solution builds pass; Unit now passes 87 cases / 7,327 Debug and 7,239
  Release assertions, and ImportIsolation passes 200 cases / 51,943 assertions in
  each configuration. Three Release compatibility A-small runs, Draw-heavy and
  delayed-copy Pressure pass applicable gates. A-small coarse p95 is 430.259 ms,
  frame p95 4.406 ms / max 14.549 ms, input p95 3.656 ms; viewer/worker peaks are
  290,263,040 / 3,612,672 bytes. Worker-Job and occlusion negative controls fail
  explicitly as intended. Existing generated multi-GiB GLB/STL/PLY runs do **not**
  pass: GLB/STL present no proxy within 30 s and PLY fails validation with a named
  resource limit. A-medium and real ETW remain unrun locally. Exact commands,
  applicability, artifacts and limits are in
  [TSK-302_VERIFICATION.md](./TSK-302_VERIFICATION.md) and
  `tests/fixtures/baselines/tsk-302/`. These failures are not waived and block the
  scope-limited MVP exit criteria; TSK-303 is next, but packaging cannot establish
  release readiness while these retained gates remain red.

- **Scope-limited MVP, Phase 3 / TSK-301 (2026-09-16): complete.**
  Product imports now use a two-process asynchronously provisioned AppContainer
  pool. Each generation receives newly duplicated read-only primary/sidecar,
  output-section and cancellation-event handles; all request state/handles are
  closed before reuse, while detail service pins its worker to one generation.
  Per-request flags prevent coarse/detail mode leakage. Cooperative checkpoints
  cover parse/normalize/decode and blocked progressive/detail waits; cancellation
  is acknowledged within a 500 ms grace or the slot is terminated/replaced.
  Import threads are owned/joined, close wakes queues and performs bounded pool
  shutdown, and stale generation publications remain filtered. D3D12 and sandbox
  startup no longer block `WM_CREATE`; asynchronous failures use the existing
  error surface. Device removed/reset/hung results stop uploads, rebuild the
  graphics lanes once, and reopen the retained source for coarse/detail
  reconstruction; repeated failure is terminal rather than a retry loop.
  Debug/Release solution builds pass with zero warnings/errors. Unit: 85 cases,
  7,318 Debug / 7,230 Release assertions. ImportIsolation: 200 cases / 51,943
  assertions in each configuration, including stable-PID real `.gltf` sidecar
  reuse and progressive cancellation followed by same-worker reuse. Lifecycle
  and recovery app smokes pass in both configurations, with zero surviving
  workers and one injected device-recovery/reconstruction. Commands, results,
  measurements and qualification limits are in
  [TSK-301_VERIFICATION.md](./TSK-301_VERIFICATION.md) and
  `tests/fixtures/baselines/tsk-301/`. No path-authority, dependency, persistent
  cache, or shared-section wire-version change. Reference p95/ETW/memory
  qualification and physical device-loss soak remain TSK-302. **TSK-302 is next.**

- **Scope-limited MVP, Phase 2 / TSK-209 (2026-09-15): complete.**
  Completed the documented static glTF subset. Quantized POSITION/TEXCOORD and
  normalized NORMAL/TANGENT accessors now convert through the existing bounded
  layouts. Required `EXT_meshopt_compression` bufferViews decode once through a
  checked 512 MiB/remaining-scratch unit boundary; optional streams retain their
  core fallback. This is compressed-data decode only, not meshoptimizer LOD.
  Valid zero-base sparse/non-indexed accessors work in streaming and legacy
  single-section paths, while strict sparse index/range checks remain.
  `EXT_texture_webp` uses a sniff/MIME-validated static libwebp adapter with
  external RGBA output, bounded scaling and the existing cancellable semantic
  mip/fallback path. Unknown required extensions fail; optional extensions warn.
  Local BIN/image sidecars remain sibling-only and handle-brokered, and corrupt
  required geometry never becomes an incomplete Ready scene.
  Meshoptimizer 1.2 (MIT) and libwebp 1.6.0#3/libsharpyuv (BSD-style) now link
  only into the sandboxed worker/test boundary. Unused DirectXTex was removed
  from the manifest while TGA/DDS/HDR remain deferred. Exact limits, malformed
  seeds, threat review, performance classification and SBOM/thumbnail decisions
  are in [TSK-209_VERIFICATION.md](./TSK-209_VERIFICATION.md) and the updated
  R-22 record. Debug/Release builds pass. Unit: 85 cases / 7,318 Debug and 7,230
  Release assertions. ImportIsolation: 198 cases / 51,933 assertions each.
  Real-app lifecycle now opens sparse, meshopt and WebP fixtures; lifecycle,
  texture, progressive and recovery smokes pass in both configurations, with
  zero Debug texture-layer errors. Reports are under
  `tests/fixtures/baselines/tsk-209/`. No persistent cache or new path authority.
  **Phase 2 is complete; TSK-301 is next.**

- **Scope-limited MVP, Phase 2 / TSK-208 (2026-09-15): complete.**
  Protocol v7 adds a bounded 64-byte position/normal/UV/tangent/color vertex
  layout. glTF/Draco/PLY preserve authored attributes; missing normals are
  generated in the worker and tangents only when a normal-mapped primitive
  requires them. STL remains neutral and node identity, transforms and double
  origins are preserved. Point-only PLY now reaches Ready through a direct
  depth-tested `PointList` path: the GPU emits camera-scaled 2–12 px round
  splats for neutral or colored points, with no CPU triangle expansion.
  The D3D12 material path consumes base color, metallic/roughness, normal and
  emissive maps/factors, vertex color, unlit, UV transforms, alpha cutoff/blend
  and double-sided culling. Opaque draws group by material; blended draws follow
  in far-to-near depth order. Progressive batches atomically rebuild one
  model-wide four-slot descriptor catalog, retain old heaps behind direct
  fences and charge neutral resources once in budget admission.
  Debug/Release builds pass with 0 warnings/errors. Unit: 85 cases / 7,318
  Debug and 7,230 Release assertions. ImportIsolation: 193 cases / 51,826
  assertions each. Progressive, texture and point-required lifecycle checks
  pass in both configurations; the forced catalog split retains its textured
  draw and Debug reports zero D3D12 errors. Commands, reports and qualification
  limits are in [TSK-208_VERIFICATION.md](./TSK-208_VERIFICATION.md). No
  dependency/license, cache or path-authority change. **TSK-209 is next.**

- **Scope-limited MVP, Phase 2 / TSK-207 (2026-09-15): complete.**
  Wired live DXGI budget sampling and notifications into destination admission,
  publication and eviction. The model ceiling is at most 60% of local budget,
  keeps 512 MiB headroom when possible and applies bounded CPU/UMA caps. Actual
  aligned geometry, texture/descriptor, target/fallback, pending and retired
  allocations are charged; deferred releases remain charged through direct and
  copy fence completion. The complete coarse proxy remains resident. Pressure
  sheds view-prioritized fine geometry and restores immutable texture mip tails;
  a reserved set that cannot fit reaches the controlled resource card and can
  reopen successfully.
  Verified double-origin bounds now drive conservative CPU frustum culling and a
  32-id view-priority queue. One request owns the reused section at a time. The
  zero-capability worker re-decodes validated GLB/glTF, STL and both-endian PLY
  ranges through its pinned primary and already-approved sidecar handles. The
  broker revalidates complete descriptors, checksums, scene metadata and file
  identity. Protocol v6 adds a fixed 168-byte detail request and a 160-byte
  descriptor whose bounded PLY fan offset permits exact replay inside a polygon.
  No normalized scene, source-sized private copy or persistent cache is retained.
  Debug/Release builds pass. Unit suites: 85 cases / 7,317 Debug and 7,229 Release
  assertions. ImportIsolation: 193 cases / 51,575 assertions each, including
  repeated source-range replay, invalid ids, PLY polygon-fan continuation, CPU
  refusal, verified-bound priority and real dual-fence retirement accounting.
  On this desktop, NVIDIA discrete and physical AMD UMA pressure runs preserve
  proxy/count coverage, reduce 2048-wide textures to retained 64-wide tails,
  recover detail after a camera move/cap restoration, report zero D3D12 errors
  and leave zero workers. Release also passes every two-/eight-million primitive
  split fixture and a 3,000,000,084-byte, 60-million-triangle STL: its 916 coarse
  chunks remain usable at a 60 MiB target while six fine chunks retire and detail
  recovers. Existing coarse, progressive, texture, recovery and point-required
  lifecycle smokes pass in both configurations. Frozen commands, hashes, reports
  and qualification limits are in [TSK-207_VERIFICATION.md](./TSK-207_VERIFICATION.md).
  No dependency/license change, persistent cache or additional path authority.
  Reference-system resident memory and startup/frame/input p95 remain TSK-302.
  **TSK-208 is next.**

- **Scope-limited MVP, Phase 2 / TSK-206 (2026-09-15): complete.**
  Added deterministic source/spatial sampling, a bounded provisional preview,
  a complete coarse catalog and stable coarse/full region relationships.
  Preview covers source/occurrence strata before full normalization; complete
  sampling protects occupied spatial cells and boundaries while retaining
  attributes, winding, material/node identity and source ranges. Full scans
  cross host validation without GPU upload and provide accurate counts/bounds.
  Worker-owned coarse samples stay bounded; packed GPU buffers enforce a
  64 MiB reserve including allocation alignment. Preview has independent
  4,096-primitive / 1 MiB payload / 8 MiB GPU limits. Protocol v5 preserves wire
  structure sizes and rejects inconsistent roles, regions, provenance, totals,
  dependencies and terminal coverage. Fine re-decode uses pinned primary and
  broker-approved sidecar handles and must match validated scan checksums.
  Replacement handoff waits for the complete usable coarse set's copy fences;
  earlier cancel/failure preserves the prior document and metadata. Fine draws
  suppress only their ready coarse parents; fence-safe eviction restores them
  without gaps or duplicate surfaces. Intermediate LODs/cross-fades remain
  deferred. [ADR-015](./design/11-decisions-and-risks.md#adr-015-bounded-coarsefull-sampling-and-the-mandatory-coverage-floor)
  records the explicit density exception: one primitive per nonempty source
  region when 5% cannot cover all regions, demonstrated by Draw-heavy's 2,048
  single-triangle instances. The 2-million primitive / 64 MiB hard limits remain.
  Debug/Release builds and unit suites pass: 82 cases each / 7,291 Debug and
  7,203 Release assertions. ImportIsolation: 189 cases / 51,433 assertions each.
  Twelve original/reordered GLB/STL/both-endian PLY mesh/point fixtures pass
  862 explicit assertions per configuration, including first-publication and
  complete eight-component coverage. Six 2–4 GiB previews pass 141 assertions:
  8–64 representative primitives arrive before full normalization in local
  measurements of 15–1,250 ms. These are single-run broker timings, not reference
  p95 presentation qualification. Coarse app checks pass in both configurations:
  cancel/late failure preserve prior content, complete handoff remains refining,
  fine suppression/eviction preserve counts, reserved allocation is 64–512 KiB,
  zero D3D12 errors and zero surviving workers. Larger split imports, progressive,
  textures, recovery and point-required lifecycle checks also pass. Frozen hashes,
  fixture manifests, commands and limits are in
  [TSK-206_VERIFICATION.md](./TSK-206_VERIFICATION.md).
  No dependency/license changes or persistent cache. Live GPU detail admission,
  automatic eviction/re-requests and pressure/UMA qualification remain TSK-207;
  full multi-GiB coarse timing and startup/frame/input gates remain unqualified.
  **TSK-207 is next.**

- **Scope-limited MVP, Phase 2 / TSK-205 (2026-09-15): complete.**
  Replaced product whole-model normalization with bounded glTF primitive,
  binary STL and both-endian PLY mesh/point clusters. Nonlocal PLY vertices use
  indexed mapped windows; variable-width records retain sparse offset checkpoints.
  Remapping, sparse accessor validation, polygon fans and unknown lists are
  bounded. Geometry publishes progressively with acknowledgement backpressure;
  normalized CPU payloads are released after upload. Source/count/scratch and
  independent Draco limits precede allocations, including a bounded JSON
  preflight before fastgltf reserves asset arrays. Catalog/batch ceilings derive
  from split expansion and occurrence tails. The broker rejects hostile source
  ranges and changed files; immutable model/GPU records retain full primary
  identity and source descriptors for later worker re-decode. Protocol v4 and
  wire layouts remain unchanged; five closed limit codes identify failures.
  Debug/Release solution builds pass. Unit: 79 cases each / 7,210 Debug and
  7,122 Release assertions. ImportIsolation: 185 cases / 48,952 assertions each.
  Seven explicit large scans pass 31,776 assertions: 2–4 GiB GLB/STL and both
  PLY byte orders for 60 million triangles/points, plus a mapped 2 GiB glTF
  sidecar. Counts/bounds, progressive publication and bounded catalogs pass;
  peak worker private memory stays below 30 MiB, host growth below 129 MiB.
  Mapped address space is recorded separately and is not resident-RAM evidence.
  Both viewers complete six split fixtures; Release also opens/cancels four
  multi-GiB sources and recovers. Queued payloads stay below 64 MiB, measured
  aggregate private growth below 704 MiB Debug / 565 MiB Release. Progressive,
  texture and recovery checks pass: fourteen final viewer processes, zero
  survivors, zero Debug texture D3D12 errors. Frozen manifest, binary/harness
  hashes and reports are in [TSK-205_VERIFICATION.md](./TSK-205_VERIFICATION.md).
  No dependency/license changes or persistent cache. Large PLY mesh first
  batches take 16–17 seconds; useful representative preview, live GPU detail
  budgets/eviction and startup/frame/input qualification remain later tasks.
  **TSK-206 is next.**

- **Scope-limited MVP, Phase 2 / TSK-204 (2026-09-15): complete.**
  Added closed failure codes and validated worker phases, generation/length/enum
  checks on error notices, distinct empty/malformed/deferred-encoding handling,
  source write-time verification and typed worker timeout/crash/limit/upload paths.
  Retry now owns its failed-path copy before clearing UI state; Open another and
  Copy details use the existing card. Diagnostics report actual format/phase/code
  with no source path or basename. Picker, drop-target, About and UX documentation
  reflect local GLB/glTF sidecars, binary STL and both-endian PLY meshes/points.
  Protocol v4 adds a fixed 16-byte status payload with closed flags, saturated
  64-count warning categories and one-per-generation broker acceptance. Fixed
  host-owned warning/status strings reuse the spinner, badge and warnings menu.
  Failed and stale publications cannot dismiss the card. Cancelled partial content
  stays interactive with an incomplete label. Cancel/failure pull the renderer
  display snapshot so queued UI updates preserve correct counts/source/selection.
  Ready requires terminal catalog
  acceptance, usable geometry, verified bounds and completed copies.
  Debug/Release solution builds pass. Unit: 79 cases / 7,210 Debug and 7,122
  Release assertions. ImportIsolation: 178 cases / 48,344 assertions each.
  Recovery covers 25 failure/recovery events per configuration, actual clipboard
  privacy, Retry/Open another, corrected same-path retry and valid reopen.
  Progressive, textures and point-required lifecycle checks pass in each config:
  fourteen final viewer processes, zero survivors. Progressive first geometry is
  15/64 chunks, cancelled partial state is not Ready, and valid reopen reaches 64.
  Queue count reaches four; the 16 KiB cap holds at 12,788 bytes Debug / 12,404
  Release. Texture checks report zero D3D12 errors and about 2.8 MB peak queue.
  Commands, committed final-binary reports and qualification limits are in
  [TSK-204_VERIFICATION.md](./TSK-204_VERIFICATION.md). No dependency/license changes.
  Bounded large-source scans and representative coarse/fine catalogs remain their
  later tasks; **TSK-205 is next**.


- **Scope-limited MVP, Phase 2 / TSK-203 (2026-09-15): complete.**
  Preserved worker WIC PNG/JPEG and BMP/TIFF adapters with explicit inbox codec
  selection; direct glTF remains PNG/JPEG/KTX2. Added encoded/decoded/pixel budgets,
  source preflight, verified native JPEG scaling, cancellable tiled raster mips,
  semantic sRGB/linear filtering and validated KTX2/Basis mip/transcode targets.
  Optional texture failures preserve geometry with deterministic semantic fallback
  and a bounded host-owned warning. Protocol v3 keeps existing header layouts,
  identifies immutable image refinements and carries a fixed-width warning count.
  The broker enforces cumulative generation budgets and compatible monotonic roots.
  Small mip chains publish before full replacements; complete GPU subresources
  become visible together after their copy fence, and displaced textures/descriptors
  retire safely. Materials retain logical IDs across replacement.
  Debug/Release solution builds pass. Unit: 79 cases each / 7,210 Debug and 7,122
  Release assertions; ImportIsolation: 170 cases / 48,222 assertions each.
  Frozen PNG/JPEG/Basis pixel goldens pass through worker decode and the product
  uploader to real GPU readback, alongside hostile dimensions/pitches/mips,
  aggregate expansion, refinement catalogs and cancellation coverage.
  Texture, four progressive modes and point-required lifecycle checks pass in each
  configuration: twelve viewer processes, zero survivors. Texture smoke observes
  64-wide initial chains followed by 256-wide or capped 2,048-wide replacements,
  one logical texture, fallback warning and valid reopen. Debug has zero D3D12
  errors; peak texture queue is about 2.8 MB. Startup responsiveness and full shader
  semantics remain later tasks. Commands, committed reports and qualification
  limits are in [TSK-203_VERIFICATION.md](./TSK-203_VERIFICATION.md).
  No dependency/license changes. **TSK-204 is next**.

- **Scope-limited MVP, Phase 2 / TSK-202 (2026-09-15): complete.**
  Protocol v2 carries fixed-width double cluster origins, exact local bounds,
  source mesh/node identities and generation-tagged format/unit/axis/scene facts.
  Workers preserve double glTF transforms and both-endian PLY residuals before
  float narrowing; double STL facet arithmetic preserves tiny nondegenerate faces.
  The broker validates a private snapshot, independently verifies finite geometry
  and exact extrema, rejects stale/changed metadata and unknown versions, and
  caps generation-relative spans before unchanged float camera calculations.
  Rendering subtracts origins and camera pivots in double before shader floats.
  Compact immutable UI snapshots supply real counts, double Info dimensions,
  provisional/verified state, exact native-orientation correction and Fit/Reset
  bounds, with no retained CPU vertex/index payload. Later framing corrections
  preserve live camera input by interaction epoch while updating home bounds.
  Existing whole-document selection and Frame selected use depth-tested GPU
  coverage with one asynchronous pixel readback; source IDs remain beside GPU
  chunks. Position-only triangles and minimal one-pixel points are supported.
  Debug/Release solution builds pass. Unit: 76 cases each / 6,034 Debug and 5,947
  Release assertions; ImportIsolation: 160 cases / 3,247 assertions each,
  including NaN/Inf, fabricated bounds, stale/changed facts, protocol mismatch,
  hostile cross-batch spans and a seeded 512-input mutation corpus.
  Six precision scenes, camera refinement checks, four progressive modes and
  lifecycle smoke with point presentation required pass in each configuration:
  twelve viewer processes, zero survivors. Delayed imports show 15/64 chunks
  with provisional metadata, then 192 vertices / 64 triangles and verified bounds;
  user zoom survives and Reset uses expanded bounds. Debug reports zero D3D12
  errors. Count pressure still reaches four batches; the 16 KiB byte cap holds
  at 12,772 bytes Debug / 12,388 Release. Commands, committed reports, limits and
  remaining normalization/point-rendering work are in
  [TSK-202_VERIFICATION.md](./TSK-202_VERIFICATION.md).
  simdjson is now explicit in the manifest at its existing pinned version/features;
  no new dependency version or license. **TSK-203 is next**.

- **Scope-limited MVP, Phase 2 / TSK-201 (2026-09-15): complete.**
  Wired `ImportSessionRequest::onBatch` to a cancellation-aware upload coordinator,
  with accepted payload/vector/task capacity capped at 128 MiB and four batches
  across queued, coordinator-owned and published work. Streaming imports retain
  only a bounded generation catalog, and terminal completion carries status.
  Default-heap allocation, copy recording/submission, staging-ring waits and bounds
  scans leave the presenting render thread. Fence-complete publications append
  geometry by generation/chunk identity; immutable texture heaps and material/image
  bindings work across batches, including bounded forward references and sparse
  slots. Invalid/unresolved terminal catalogs fail closed. Acknowledgements follow
  bounded admission, and cancellation after acceptance suppresses the next ack.
  Cancel/failure/replace/close wake backpressure, discard stale work and preserve
  fence-safe resource retirement. Prior content remains visible and navigable while
  replacement copies are delayed; partial content stays Loading. Ready follows
  successful terminal catalog acceptance and all prior copies, with usable geometry.
  Debug/Release solution builds and both Catch2 binaries pass: Unit 74 cases each,
  ImportIsolation 155 cases each; targeted batch/hostile-worker coverage passes
  18 cases. Four targeted app modes and the existing lifecycle smoke pass in each
  configuration (ten viewer processes, zero survivors). Real 750 ms copy-fence
  gates plus a delayed worker final batch display 20/64 chunks while Loading, then
  all 64; full-queue cancel/replace/reopen and camera input while Loading pass.
  Count pressure reaches four batches; a 16 KiB test byte cap holds at 14,840 bytes
  Debug / 14,072 Release. Cross-batch texture binding remains intact. Exact commands,
  committed reports, capacities and qualification limits are in
  [TSK-201_VERIFICATION.md](./TSK-201_VERIFICATION.md).
  No dependency/license changes. Large-source normalization/splitting, verified
  metadata, complete coarse-proxy readiness and live GPU budgets remain their
  subsequent tasks; **TSK-202 is next**.

- **Scope-limited MVP, Phase 1 / TSK-104 (2026-09-15): complete.**
  Added a checksummed 33-input routine corpus and full A-small manifest, bounded
  deterministic small/medium/large GLB/STL/PLY mesh/point/endian recipes, source-byte
  metadata verification, and real-app open/replace/cancel/resize/recover/close smoke
  through an opt-in bounded command seam. The render thread publishes successful
  visible Present milestones and resize acknowledgements through atomics.
  Fixed the fixture-exposed PLY header splitter scanning binary payload as text.
  Debug/Release solution builds and both Catch2 binaries pass: Unit 74 cases,
  ImportIsolation 152 cases. Two routine generations and a full A-small repeat
  match their pinned hashes/metadata; twelve final real-app lifecycle runs pass
  with zero surviving viewer/child worker processes. Full A-small GLB geometry
  Present medians are 380.50 ms Debug / 125.55 ms Release. Raw reports, exact
  commands, measurement limits and future-task fixture policies are recorded in
  [TSK-104_BASELINE.md](./TSK-104_BASELINE.md) and
  [tests/fixtures/README.md](../tests/fixtures/README.md).
  Point normalization works but current point display fails its explicit
  qualification assertion; non-indexed/sparse, large catalogs/proxies, meshopt/WebP,
  shader semantics and actual pressure remain later tasks. No dependency/license
  changes. Phase 1 is complete under the updated scope; **TSK-201 is next**.

- **Scope-limited MVP, Phase 1 / TSK-103 (2026-09-15): complete.**
  All six UI binding locations already use the D3D12 model state following
  TSK-101's removal of the legacy renderer instance: `HasNavigableModel`,
  `FrameSelectedOrAll`, `ToggleShowNativeOrientation` (through `HasNavigableModel`),
  `CancelOpen`, `ID_VIEW_RESET`, and the bottom-bar snapshot in `BuildOverlayInfo`.
  `RenderThread.HasModel()` is the UI-safe atomic publication of the privately owned
  D3D12 path's model presence; the UI must not access that render-thread-owned path
  directly. No additional product-code changes were needed. Camera math and viewport
  aspect-ratio logic are unchanged.
  Verification: Debug `msbuild Preview3D.slnx /t:Preview3D,Tests_Unit` passes;
  `Tests.Unit.exe "[chrome]"` passes both cases / 175 assertions. Visible app runs
  for Empty, `tri_tight.glb`, and `tri_external.gltf` plus its external `.bin` each
  complete 40 frames with zero occluded presents at 150% DPI, remain responsive
  through Info/resize/Reset, and close with exit code 0. Client-area PrintWindow
  captures show the bottom bar only for loaded models, and the Information panel
  reserves 450 physical pixels on the right when open, shifting the scene and gizmo
  into the remaining viewport. Both remain reserved after resizing from 1522x1136
  to 878x639 client pixels. Zoom followed by Fit visibly restores the triangle's
  framing for both formats; Reset after resize keeps the model inside the viewport.
  Empty-state Info/Reset commands leave the bottom bar and panel collapsed. Local captures
  and the verification harness are under ignored `TestResults/tsk-103/`.
  No viewer/worker processes remained after the successful runs. No dependency/license
  changes. Phase 1 is complete; Phase 2 / TSK-201 is next. Information rows, picking,
  and native-orientation transforms still require CPU metadata that the D3D12 import
  path does not publish; this task verifies state bindings and reserved layout space,
  without expanding that pipeline scope.

- **Scope-limited MVP, Phase 1 / TSK-102 (2026-09-15): complete.**
  The real Direct2D chrome and vector glyph helpers have moved from `Renderer.cpp`
  into `D3D11On12Overlay.cpp`: title/caption buttons, bottom bar, scrolling Information
  panel, navigation gizmo, loading/empty/error cards, HUDs, Speed/Settings flyouts,
  and tooltips. `Renderer.cpp` remains for the camera and deprecated D3D11 scene code.
  The bridge owns one `overlayBrush`, cached DPI-dependent DirectWrite formats, and
  shared strokes; its single `ID2D1DeviceContext` targets the existing bitmap for each
  back buffer. Resize drops only the wrapped buffers/bitmaps, retaining the device
  brush. Target-loss bitmap recreation likewise retains device resources.
  Every D3D12 frame now paints chrome after scene submission and before Present/the
  frame fence. Spike primitives/options are removed; old `--overlay-spike[=N]` flags
  are accepted as no-ops. Overlay timings now measure the real chrome.
  The UI publishes immutable `OverlayFrame` snapshots with the existing chrome/gizmo
  layout and hover state, alongside frame inputs, and wakes the renderer after publication.
  The render thread snapshots the camera orientation under its existing mutex and draws
  without holding that mutex across GPU waits. Scene viewport/scissor insets now match
  the UI's existing reserved title/bottom/panel space, including fullscreen/loading rules;
  camera math and UI aspect calculations are unchanged.
  Verification: Debug `msbuild Preview3D.slnx /t:Preview3D,Tests_Unit` passes;
  `Tests.Unit.exe` passes all 73 cases / 5,733 assertions. Two new `[chrome]` tests
  read back actual D3D12 swap-chain pixels for the bars, panel, gizmo, caption-button
  hover and error card. They explicitly exercise all three target bitmaps (hidden-window
  Present can remain occluded), resize, 150% DPI, and Empty/Loading/Failed/Ready passes.
  The real-chrome test reports no new D3D12 debug-layer errors. Five hidden-window app
  smoke checks (empty, GLB, glTF plus external `.bin`, deprecated spike flag, and failed
  import) each complete 40 frames, respond after resize and Info/Settings commands, and
  close with exit code 0. These app checks establish lifecycle behavior; GPU readback tests
  establish drawing output. No viewer/worker process remained. No dependency/license changes.
  TSK-103 remains next for visible UI verification. The D3D12 import path still does not
  publish CPU model metadata for Information rows, picking, or native orientation.
  Error-card drawing is restored; import error-code/stage mapping remains TSK-204.

- **Scope-limited MVP, Phase 1 / TSK-101 (2026-09-15): complete.**
  [NEW_SCOPE_LIMITED_MVP_TASKS.md](./NEW_SCOPE_LIMITED_MVP_TASKS.md) is the active scope and
  sequence; the broader gate history below remains a record of the original plan.
  `Preview3D.cpp` no longer has a `useD3D12` switch or a legacy `Renderer` instance.
  Startup, paint/invalidation, coalesced resize, input publication, and shutdown always use
  `RenderThread`, which privately owns the exclusive `D3D12ViewerPath`. Every open uses the
  sandboxed import bridge; the in-process GLB loader and its completion handler are removed
  from application routing. `--d3d12` is accepted as a deprecated no-op for existing scripts.
  `Renderer.cpp` is retained unchanged for its camera implementation and D2D drawing reference.
  The former `RenderScene` snapshot builder is retained as `BuildOverlayInfo`, without GPU
  submission, for TSK-102. Model checks now use the render thread's atomic D3D12 model state
  because the legacy renderer instance is gone. Native-orientation re-homing remains gated on
  CPU model data so missing metadata cannot reset the camera to zero bounds.
  Verification: `msbuild Preview3D.slnx /t:Preview3D /p:Configuration=Debug` passes; hidden-window
  smoke checks for empty launch, GLB launch, GLB with the deprecated flag, and `.gltf` with an
  external `.bin` all complete 40 D3D12 frames, remain responsive after resize, and close with
  exit code 0 within the 8-second smoke deadline. No viewer or worker process remained.
  These are lifecycle checks; the hidden-window capture did not establish visual correctness.
  TSK-102 remains next: real chrome/error cards are not drawn yet. TSK-103 still needs visible
  UI verification, and the import path does not yet publish CPU model data for picking,
  Information panel stats, or native-orientation transforms. No dependency/license changes.

- **Gate 0** ("Phase 0"): done, committed (`6f52bac phase 0`). Build policy, x64-only, vcpkg+Catch2 harness scaffolding.
- **Gate 2 workstream A, part 1** — AppContainer + Job Object launch spike: done, committed (`6355698 next step`). Proves the sandbox container itself (zero-capability token, suspended launch, job assignment before resume, restricted handle inheritance, Job Object enforcement) against the real `Preview3DImportWorker.exe`.
- **Gate 2 workstream A, part 2** — wire format, synthetic in-sandbox generator, broker control protocol, copy-then-validate: done, committed (`01c51cd next phase done`). Proves the honest-worker data path end to end: a synthetic cube+point-cluster fixture is fabricated inside the real AppContainer worker, crosses a shared memory section per the versioned wire format, and is validated/copied by the host's fail-closed acceptance path.
- **Gate 2 workstream A, part 3** — synthetic hostile-worker suite + `SharedSectionValidator` TOCTOU fix: done, committed (`58b877d gate 2`). A second worker binary (`Preview3DHostileWorker.exe`, `tests/hostile-worker/`) deliberately mutates shared-section bytes after `ChunksReady`, replays a stale generation ID, and lies about a chunk's declared payload range/vertex-layout ID — all four proven rejected by the host's validator, run through the identical AppContainer + Job Object sandbox as the honest worker. Combined with part 1's already-passing Job Object overrun tests (`--overallocate`/`--hang`), this satisfies Gate 2's exit criteria in full. **Gate 3 (wiring a real parser) is no longer blocked.**
- **Gate 3 slice 1** — real `fastgltf` parsing behind the sandbox: done, committed (`36a0588`). `import-worker/src/GltfAdapter.cpp`/`GltfImportWorker.cpp` parse a real GLB file (via a new `StartGltfImport`/`ParseGltfRequest` control message, additive to the existing frozen `StartGenerationRequest` path) and emit chunks through the **unmodified** wire format and `SharedSectionValidator` — the same acceptance path the synthetic generator and the hostile-worker suite already proved. Scope: core untextured geometry only (POSITION/NORMAL/TEXCOORD_0, triangle-list primitives, node-transform baking with depth-capped cycle-detected node-tree walking). Does not touch `interactive-viewer/` except two new generated test fixtures. **Sequencing decision recorded here per explicit direction**: Gate 2 workstream B (D3D11→D3D12 renderer migration — confirmed `Renderer.cpp` is still `ID3D11Device` — plus upload ring, mapped-file/window-lease, derived cache, DXGI budget monitor, fault injection) is the **mandatory next chunk**, not an indefinite deferral. Reasoning: neither a real parser without D3D12/upload-ring, nor streaming plumbing without a real parser, alone reaches an actual "open a GLB, see it render, safely" MVP — both are required. It wasn't folded into this pass because it's a renderer-API migration comparable in size to all of Gate 2 workstream A, and bundling it with a first real-parser integration would have broken the small/buildable/independently-verifiable chunk discipline that's kept every prior increment low-risk.
- **Gate 2 workstream B, slice 1** — headless D3D12 device/queue/fence foundation: done, committed (`36a0588`). New `interactive-viewer/src/graphics/D3D12Device.{h,cpp}` (adapter selection + device + debug layer) and `D3D12CommandQueue.{h,cpp}` (one queue + its owned fence, poll + bounded event-wait) — proven via 9 `[graphics]`-tagged tests in `Tests.Unit`, real D3D12 device creation against actual hardware on this dev machine, WARP opt-in, and cross-thread fence-signal/wait mechanics, all passing on the first real run in both Debug and Release. **Deliberately not wired into `Preview3D.vcxproj`, `Renderer.cpp`, or `Preview3D.cpp`** — the files live under `interactive-viewer/src/graphics/` (the design's intended location) but are compiled only into `Tests.Unit.vcxproj` this slice, same "prove the primitive in isolation before any real content/caller touches it" discipline as the AppContainer sandbox and the fastgltf parser. The still-D3D11 `Renderer.cpp` is completely untouched.
- **Gate 2 workstream B, slice 2** — swap chain + a real (hidden, test-owned) window + one full clear-and-present cycle + resize: implemented, built, and tested (not yet committed as of this note). New `interactive-viewer/src/graphics/D3D12SwapChain.{h,cpp}` — a three-buffer flip-discard swap chain with a frame-latency waitable object, per the design doc's device/presentation step 5, taking an already-created `HWND` rather than owning window lifetime. Proven via 4 new `[graphics]` tests (18 total now, 129 assertions), all passing on the first real run in both Debug and Release: real window + swap chain creation, one clear-and-present frame, five frames in a row with fence-gated allocator reuse (the highest-value test — proves no synchronization bug in the allocator-reset/fence-retirement handshake), and resize with a follow-up frame proving the rebuilt render target views are actually usable. Still no shaders, no geometry, no Direct2D overlay (ADR-010's overlay-strategy question stays explicitly undecided), and still **not wired into `Preview3D.vcxproj`/`Renderer.cpp`/`Preview3D.cpp`** — same discipline as slice 1.
- **Gate 2 workstream B, slice 3** — upload ring + copy queue/fence integration + fence-complete publication + a synthetic streaming source to exercise it end-to-end: implemented, built, and tested (not yet committed as of this note). Two new pieces:
  - `shared/platform/include/platform/Generation.h` — the Gate 0 "cancellation/generation primitive" that had never actually been built (only a bare `generationId` field existed, in the unrelated worker IPC protocol). `GenerationSource` is a thread-safe monotonic counter; `Advance()` *is* the cancellation signal — there's no separate cancel flag. `GenerationToken` is a cheap immutable snapshot with `IsCurrent(source)`. Header-only, no `.cpp`, following `CheckedMath.h`'s existing style. Proven by 5 new `[platform]` tests including a cross-thread torn-value check in the same style as `CommandQueueTests.cpp`'s fence proofs.
  - `interactive-viewer/src/graphics/D3D12UploadRing.{h,cpp}` — composes a copy-typed `D3D12CommandQueue` (constructed exactly as slice 1's header comment anticipated: "when full, only the upload coordinator may wait on the copy-fence event") with a persistently-mapped UPLOAD-heap ring (256 MiB initial, grows in 64 MiB increments to a 512 MiB cap, reclaim-from-tail-on-fence-complete, wrap only into proven-free space, `CheckedMath.h`-guarded offset arithmetic) and a fence-complete publication path (`DrainCompletedPublications`) that promotes only still-`GenerationToken::IsCurrent()` copies into a new immutable `SceneSnapshot` (`interactive-viewer/src/graphics/SceneSnapshot.h` — the first concrete definition of this type anywhere in the codebase; previously only design-doc prose). `interactive-viewer/src/graphics/SyntheticStreamingSource.{h,cpp}` generates deterministic bounded clusters with real AABB bounds and three explicit LOD levels (proxy/mid/full, strictly increasing vertex counts) using the existing `model_core::VertexLayoutId::PositionNormalUv0_F32` layout — deliberately **not** reusing `import-worker/src/SyntheticSceneGenerator.cpp` (that one is coupled to the cross-process wire format and has no bounds/LOD concept at all). Proven via 4 new `[graphics]` tests (31 total now, 828 assertions), all passing on the first real run in both Debug and Release, run 5× in a row with no flakiness: byte-exact readback-verified roundtrip through the ring; a genuine wrap-around stress test (200×64-byte chunks through a deliberately tiny 4 KiB non-growing ring, asserting `UsedBytes() <= CapacityBytes()` after every allocation plus sampled readback content correctness — proves no unretired allocation was overwritten); ring growth triggered by a single allocation too big for the initial capacity; and the full synthetic-cluster path proving generation-staleness rejection (a deliberately pre-`Advance()` token is uploaded and never appears in the published snapshot) and epoch/handle-swap semantics (an earlier-held `SceneSnapshotPtr` stays untouched after later publishes). Still **not wired into `Preview3D.vcxproj`/`Renderer.cpp`/`Preview3D.cpp`** — same discipline as slices 1-2. DXGI budget monitor, fault injection, mapped-file/window-lease, and the derived cache remain explicitly out of scope for this slice.
- **`MappedFile`/`MappingLease` primitive** (Gate 2's own "MappedFile/window lease implementation" deliverable): implemented, built, and tested (not yet committed as of this note). New `shared/model-core/include/model_core/MappedFile.h` + `src/MappedFile.cpp`, per `.docs/design/03-file-formats-and-ingestion.md`'s "Mapped-file abstraction" section — deliberately scoped to the mapping primitive itself, not the trusted-process path-canonicalization or broker handle-duplication steps around it (both still nonexistent; see below). `MappedFile::Open` opens a local path with `CreateFileW` (an `AccessHint{Unknown,Sequential,Random}` maps to `FILE_FLAG_SEQUENTIAL_SCAN`/`FILE_FLAG_RANDOM_ACCESS`), rejects zero-length/non-regular files and captures `FileIdentity` (volume serial + `FILE_ID_128` + size) via `GetFileInformationByHandleEx`, then creates a whole-file-sized `PAGE_READONLY` mapping object. `MapWindow(offset, length)` is the actual new logic: rounds the mapping base down to the cached `SYSTEM_INFO::dwAllocationGranularity` internally and returns a `MappingLease` whose `Bytes()` transparently hides that alignment padding, returning exactly the caller's requested range — multiple leases from one `MappedFile` can be alive at once. `platform::MappedView::Map` (previously always offset-0) gained an optional `offset` parameter to make this possible, with every pre-existing call site (`GltfImportWorker.cpp`, `GltfImportTests.cpp`) left compiling unchanged via the parameter's default. Proven via 7 new `[model_core]` tests (34 total now, 4969 assertions) against **real** on-disk `.glb` fixtures from `interactive-viewer/test-assets/` plus one synthetic ~200 KiB scratch file (temp-file RAII helper, cleaned up on destruction — confirmed no leaked temp files after 5 repeat runs in both Debug and Release): byte-exact whole-file and sub-range reads against an independently-`ifstream`-read reference, a deliberately-misaligned window straddling a real allocation-granularity boundary (the one genuinely new correctness claim this primitive makes — none of the checked-in fixtures are anywhere near 64 KiB, so this needed the synthetic file), out-of-bounds rejection, and two simultaneous leases.
- **`MappedFile` wired into the real sandboxed import pipeline** (closes out the item directly above): implemented, built, and tested (not yet committed as of this note). Four additive pieces, none touching `Preview3D.vcxproj`/`Preview3D.cpp`/`Renderer.cpp`:
  - `model_core::MappedFile::FromHandle(platform::Win32Handle)` — a second constructor path alongside `Open()`, for a caller that already holds an open file handle (the sandboxed worker, receiving one the broker duplicated in) rather than a path. `Open()`'s `CreateFileW` step and the shared identity/mapping-building tail were refactored into a private `BuildFromOpenFile` helper both now call; `Open()`'s own behavior is unchanged.
  - `import_broker::SourceFileAccess` (`shared/import-broker/include/import_broker/SourceFileAccess.h` + `src/SourceFileAccess.cpp`, new, alongside `SharedSection.h`/`SandboxLauncher.h`) — `OpenAndCanonicalizeSourceFile(path)` (`CreateFileW`, not inheritable, then `GetFinalPathNameByHandleW`, per the design doc's "Input boundary" section) and `DuplicateInheritableHandle(HANDLE)` (`DuplicateHandle(..., bInheritHandle=TRUE, DUPLICATE_SAME_ACCESS)`, kept as an explicit separate step so the trusted process's own long-lived handle is never broadly inheritable, only a purpose-made duplicate is). Proven via 3 new `[import-broker]` tests in `tests/import-isolation/SourceFileAccessTests.cpp`, including directly asserting `GetHandleInformation`'s `HANDLE_FLAG_INHERIT` bit on the duplicate rather than only inferring it from a successful launch elsewhere.
  - `model_core::ControlOpcode::StartGltfImportFromFile` + `ParseGltfFileRequest` (`ControlProtocol.h`, additive — `StartGltfImport`/`ParseGltfRequest`/opcode 4 stay completely frozen, same precedent that request already set relative to `StartGeneration`) — carries a raw duplicated **file** handle (not a pre-made mapping/section handle), so the worker builds its own `MappedFile` rather than the broker pre-building a mapping and handing that across. This follows the design doc's literal architecture ("...or, for a duplicated handle received from the broker, reopen a mapping directly from that handle without a fresh `CreateFileW` call") rather than the simpler alternative a survey found (duplicating an already-built file-mapping handle into the existing opcode-4 slot, which would have needed zero protocol/worker changes but would mean `MappedFile` never actually runs inside the sandboxed worker at all).
  - `import-worker/src/GltfImportWorker.cpp` — real code in the actual shipping `Preview3DImportWorker.exe` (unlike the D3D12 graphics slices, there's no headless test-only version of the worker to defer this into). Refactored into `RunFromSection` (the existing opcode-4 path, behaviorally unchanged) and a new `RunFromFile` (opcode 5: wraps the received handle in `platform::Win32Handle`, `MappedFile::FromHandle` + `MapWhole()`, then calls the **same** `ImportGltf(...)` opcode-4 already uses), sharing a small `ReportResult` reply helper.
  - `tests/import-isolation/GltfImportTests.cpp` — a new `RunGltfImportFromRealFile` helper (deliberately parallel to `RunGltfImport`, same "small duplication over modifying a working shared path" precedent that function itself already set) reuses `LaunchGltfImportWorker` unchanged and proves the actual close-the-loop case: `tri_tight.glb`, opened and canonicalized from disk, duplicated inheritable, reaches the AppContainer-sandboxed worker and parses successfully — no `PushBytesIntoNewSection`, no in-memory byte vector standing in for the source anywhere on this path.
  
  All of `Tests.Unit`, `Tests.ImportIsolation`, and the real `Preview3DImportWorker`/`Preview3DHostileWorker` binaries build clean in Debug and Release; full suites pass repeatedly (35/35 and 29/29 test cases respectively). One real false alarm during verification, not a code defect: the very first post-rebuild run of `Tests.ImportIsolation.exe` showed 13 unrelated failures (`ReadControlMessage`/`WaitForSingleObject` timeouts spanning glTF, synthetic-generation, *and* Job Object probe tests — i.e. every real-worker-launching test, not just the new code paths). `git stash`-ing back to the baseline commit and rebuilding confirmed the baseline itself passed cleanly, and simply rebuilding-and-rerunning the changes (no code edits) also then passed cleanly and stayed clean across 5 further repeat runs in both configurations — almost certainly a one-time AV/first-launch-scan delay on the freshly-written worker `.exe` rather than a real regression, but flagged here rather than silently dismissed, per this file's own discipline.
- **Correction to the entry above**: `GltfImportWorker.cpp`'s `RunFromSection`/`RunFromFile` were renamed (not just refactored again) to `HandleGltfImportRequest`/`HandleGltfImportFileRequest` in the worker-pool slice immediately below, and moved out of the anonymous namespace so `WorkerRequestDispatch.cpp` can call them directly — same behavior, new exported names.
- **Gate 2's four remaining deliverables — worker pool + generation cancellation, DXGI budget monitor + view-priority requester + detail eviction, crash-safe bounded derived-cache prototype, and fault injection** — all implemented, built, and tested in one pass (not yet committed as of this note). This closes every item in Gate 2's own deliverable/exit-criteria list in `.docs/design/10-delivery-plan.md` *except* the 4 GiB-streaming and open/close/reopen integration exit criteria, which structurally require a real render loop (the ADR-010 D3D11-on-12-overlay decision every slice so far has deliberately deferred) — that deferral is unchanged here. None of the four touch `Preview3D.vcxproj`/`Preview3D.cpp`/`Renderer.cpp`.
  - **Worker pool + generation cancellation.** `model_core::ControlOpcode::Shutdown` (additive; every existing opcode/struct stays frozen). `GenerationWorker.cpp`/`GltfImportWorker.cpp` each now expose a per-opcode handler (`HandleStartGeneration`, `HandleGltfImportRequest`/`HandleGltfImportFileRequest`) shared between their unchanged one-shot entry points and a new `import-worker/src/WorkerRequestDispatch.{h,cpp}` (`DispatchOneRequest` + a new `--pool` main.cpp mode, `RunPoolMode`, that loops reading/dispatching/replying until `Shutdown` or a protocol error/EOF — no duplicated business logic anywhere). Host-side `import_broker::WorkerPool` (new) launches N `--pool`-mode workers up front, each keeping its own persistent control-pipe pair for its whole pool lifetime; since `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` inheritance is frozen at launch, a *second* request's section handle is duplicated directly into the already-running worker via a new cross-process `DuplicateSectionIntoWorker` (`DuplicateHandle` targeting the pooled worker's own process handle, distinct from `SourceFileAccess::DuplicateInheritableHandle`, which duplicates for *future* `CreateProcessW` inheritance, not into an already-running target). Cancellation is host-side only, per `06-application-lifecycle-and-ipc.md`'s contract: a superseded generation's eventual reply is filtered out via `platform::GenerationToken::IsCurrent()` (same pattern `D3D12UploadRing::DrainCompletedPublications` already established) and, if a worker misses a bounded `WaitForReply` grace window, `TerminateAndReplace` hard-kills it via the already-proven Job Object kill-on-close and launches a replacement so pool size never shrinks. **True mid-parse cooperative cancellation is explicitly out of scope** — fastgltf isn't cancellable mid-call today; threading a stop-token through the adapter is a separate concern for later. Proven via 4 new `[worker-pool]` tests in `tests/import-isolation/WorkerPoolTests.cpp`: the same pooled worker's PID serves two sequential generations without relaunch; `Shutdown` cleanly exits an idle worker; an artificially impossible `WaitForReply` timeout deterministically exercises terminate-and-replace (reusing, not re-deriving, `SandboxLaunchTests.cpp`'s already-proven hang-then-kill mechanism); a stale-generation reply is correctly identified via `GenerationToken` and the worker still returns to Idle cleanly regardless.
  - **DXGI budget monitor + view-priority requester + detail eviction.** New `interactive-viewer/src/graphics/DxgiBudgetMonitor.{h,cpp}` — confirmed fully greenfield beforehand (zero prior `IDXGIAdapter3`/budget-query code anywhere). Re-acquires the adapter `D3D12Device` selected (it doesn't retain one after `Initialize()`) via `GetAdapterLuid()` + `EnumAdapterByLuid()`, then QIs to `IDXGIAdapter3` for `QueryVideoMemoryInfo`/`RegisterVideoMemoryBudgetChangeNotificationEvent`. `ComputeDetailTargetBytes()` implements the design doc's exact formula (≤60% of budget, ≥512 MiB headroom, ≤format policy cap, recomputed fresh from a live query every call so it "reduces immediately when the OS budget falls" by construction, never cached) and takes an injectable `QueryFn` seam so policy math is provable deterministically alongside a real-hardware path. `interactive-viewer/src/graphics/SceneSnapshot.h`'s `ReadyResourceInfo` gained two additive trailing fields (`approximateBytes`, `lastVisibleFrame`, both defaulted) so the existing positional-aggregate-init call site in `D3D12UploadRing.cpp` kept compiling unmodified. `PlanEviction(snapshot, targetBytes)` produces an ordered least-recently-visible-first drop list only — it never calls `Release()` on a GPU resource itself, matching the design doc's ordering ("removes a chunk from a new snapshot first"). **Scoping note**: "view-priority" here is recency + size, not a camera/frustum projected-screen-error system — no renderer/camera exists in this codebase yet to feed one. Proven via 6 new `[graphics]` tests: real-hardware budget query and detail-target computation; three injected-budget tests (exact-formula match, immediate recalculation on a simulated drop, format-policy-cap enforcement); two `PlanEviction` tests against synthetic `SceneSnapshot` data. One empirical finding worth flagging: `RegisterVideoMemoryBudgetChangeNotificationEvent`'s event was already signaled the very first time it was checked on this dev machine — not documented behavior either way, so the test only confirms polling doesn't crash/hang, without asserting the boolean itself.
  - **Crash-safe bounded derived-cache prototype.** New `shared/platform/include/platform/Sha256.h`+`.cpp` (`ComputeSha256`, wrapping Windows CNG's `BCrypt` — no new vcpkg dependency; confirmed beforehand that no cryptographic hash existed anywhere in this repo, only the explicitly non-cryptographic `model_core::Fnv1a64`) and `interactive-viewer/src/cache/DerivedCache.{h,cpp}` (new subdirectory; ADR-012 frames this as viewer-owned, never shared with the worker/thumbnail provider). Binary manifest per entry (`#pragma pack(push,1)` + `static_assert`-pinned 88-byte header, same discipline as `WireFormat.h`: magic/schema version validated structurally, only the payload gets a cryptographic SHA-256, mirroring how `SectionHeader` itself is never hashed either, only what follows it). `TryGet` is fail-closed — magic, schema version, identity, length-consistency, or checksum mismatch are all a miss, never a partial-trust read. `Put` writes via the exact temp-file → `WriteFile`+`FlushFileBuffers` → `MoveFileExW`-replace-existing sequence `interactive-viewer/src/app/Settings.cpp` already established (reused, not reinvented), enforces a per-entry size cap, and evicts oldest-by-last-write-time entries (via directory enumeration, no separate index file) when a configurable total-cache-size cap would be exceeded. `Open()` deletes any stray `*.tmp` files found — "abandoned temporaries are removed on the next launch," and `Open()` *is* next launch here. **Two deliberate scoping deltas from the full design**, both explicit in `DerivedCache.h`'s own header comment: (a) identity uses only `model_core::MappedFile::Identity()` (volume serial + `FILE_ID_128` + size) — no USN/change-journal capture (confirmed beforehand that nothing in this repo touches `FSCTL_QUERY_USN_JOURNAL`/`FSCTL_READ_FILE_USN_DATA`), so a volume-serial/file-ID collision after a journal reset isn't detected; (b) an entry stores one opaque payload section a caller provides rather than real serialized `SceneSnapshot`/chunk-catalog data, since no adapter in this repo yet produces that shape outside the sandboxed worker's own wire format — this proves cache *mechanics*, not the full production staleness guarantee or real geometry serialization (a later Gate 3/4-adjacent concern). Proven via 8 new `[cache]` tests in `tests/unit/DerivedCacheTests.cpp` (using a new `ScratchCacheDir` RAII helper, extending `MappedFileTests.cpp`'s `ScratchFile` pattern to a directory of entries): byte-exact round-trip; miss on an absent key; a corrupted entry (one flipped byte) rejected; a truncated entry rejected; a wrong-schema-version entry rejected; an over-per-entry-cap `Put` rejected cleanly; eviction under a total-cache cap keeps only the newest entries and stays under budget; a stray `.tmp` is cleaned up on the next `Open()`.
  - **Fault injection**, closing the traceability gap between Gate 2's five explicitly named fault types and what's actually tested — no new production code, verification only. Cancellation/stale events: already covered by the worker-pool tests above. Budget loss: already covered by the DXGI budget monitor's injected-drop test above. Two genuine gaps filled directly in `tests/unit/UploadRingTests.cpp`: a delayed-copy-fence test (an upload deliberately left unflushed/undrained proves `CurrentSnapshot()` stays the exact same object — pointer identity, not just equivalent content — until its fence genuinely retires) and an OOM test (a single allocation larger than the ring's configured `maxCapacityBytes` returns `Failed` cleanly, and the ring remains fully usable for a normal request immediately afterward — proving a clean rejection, not a wedged ring). The cache's own over-cap-`Put` test (above) already covers the OOM fault type on that side too.

  All five test binaries touched this pass (`Tests.Unit`, `Tests.ImportIsolation`, plus the real `Preview3DImportWorker`/`Preview3DHostileWorker` — the pool-mode dispatcher is real shipping-worker code, same as the `MappedFile`-wiring slice) build clean in both Debug and Release and pass in full — 51/51 (`Tests.Unit`, 5082 assertions) and 33/33 (`Tests.ImportIsolation`, 315 assertions) — repeated multiple times in each configuration with no flakiness. One build error caught immediately, not a design flaw: a local test variable literally named `small` silently expanded via the legacy `<rpcndr.h>` MIDL-compatibility macro `#define small char` (pulled in transitively through `<windows.h>`), turning `std::vector<std::byte> small(64, ...)` into nonsense and producing a `char`-related parse error rather than a wrong-behavior bug — renamed to `smallPayload`; worth remembering `small`/`hyper` are reserved words in any file that (transitively) includes `<windows.h>` in this codebase.
- **First slice of D3D12-in-the-real-app: an opt-in `--d3d12` clear-and-present loop in the actual shipping `Preview3D.exe`** (not yet committed as of this note). Deliberately just slice 1 of the larger "replace `Renderer.cpp`'s D3D11 path" effort the ADR-010 discussion two turns ago scoped out loud — no camera, no model, no D2D/D3D11On12 chrome overlay, no device-loss recovery yet, all explicitly deferred. New `interactive-viewer/src/app/D3D12ViewerPath.{h,cpp}` (kept out of the already-2700-line `Preview3D.cpp` so this experimental path stays easy to review/remove): owns a `D3D12Device`/direct-typed `D3D12CommandQueue`/`D3D12SwapChain` plus the single command allocator/list the bare per-frame clear reuses (the exact `FrameRecorder` shape `SwapChainTests.cpp` proved in isolation, now real product code for the first time), clearing to the design doc's Gate 1-specified `#1C1C1E` idle background every frame. `Preview3D.cpp` gained: a `--d3d12` argv scan in `wWinMain` (no flag-parsing existed before — only a positional initial-file-path arg via `CommandLineToArgvW`; the scan now pulls `--d3d12` out first and still treats the first remaining non-flag argument as the path, unchanged behavior when the flag is absent) and four small branches — `WM_CREATE` (`d3d12Path.Initialize` instead of `renderer.Initialize`), `WM_SIZE` (`d3d12Path.Resize`, same `(width, height, std::wstring&)` shape as `Renderer::Resize`), the two render call sites `RenderFrame`/`WM_PAINT` (`d3d12Path.RenderClearFrame()` instead of `TickCamera`+`RenderScene`), and `WM_DESTROY` (`d3d12Path.WaitForIdle()` before cleanup — GPU work must be known-idle before `ViewerApp`'s destructor releases the D3D12 objects; RAII alone doesn't order that). The two paths are strictly mutually exclusive per window (only one swap chain can own presentation for a given `HWND`) — `renderer.Initialize`/`Resize`/`Render` are never called at all when `useD3D12` is set. `Preview3D.vcxproj` gained `src\graphics` + `..\shared\platform\include` to its include path, `d3d12.lib;dxguid.lib` to its linked libraries (`dxgi.lib` was already present), and compiles `D3D12Device.cpp`/`D3D12CommandQueue.cpp`/`D3D12SwapChain.cpp` directly (same cross-project-source-file-reuse pattern as everywhere else in this repo — `D3D12UploadRing`/`DxgiBudgetMonitor`/`SceneSnapshot` deliberately left out, since this bare slice does no upload/budget/model wiring at all). Verified by actually running the app (no unit tests for this — app-level wiring isn't meaningfully unit-testable): launched `Preview3D.exe` with no arguments first and confirmed the existing D3D11 default path is completely unaffected (screenshot matches the pre-existing chrome/empty-state UI exactly); launched `Preview3D.exe --d3d12` and confirmed a real window opens, sampling its center pixel back from a screenshot at **exactly `#1C1C1E` (28,28,30)** — the literal spec'd hex value, not just "looked dark"; resized the window (via `MoveWindow`, which the OS turns into a `WM_SIZE` the same way `WM_DPICHANGED`'s `SetWindowPos` already does for the D3D11 path) and confirmed it kept rendering the cleared color at the new size with no crash; closed both windows and confirmed via `Get-Process` that neither left a lingering process.
- **Gate 3 slice 2** — a real, product-owned binary-STL parser behind the sandbox, the second complete format wired through the pipeline: implemented, built, and tested (not yet committed as of this note). New `import-worker/src/StlAdapter.{h,cpp}` parses the fixed-size binary layout (80-byte header + little-endian `uint32_t` facet count + N×50-byte facets) directly against `.docs/design/03-file-formats-and-ingestion.md`'s spec — per facet: a non-finite (NaN/Inf) vertex or normal drops the whole facet; a degenerate (near-zero-area, `crossProduct lengthSquared <= 1e-12`) facet is dropped; a supplied normal is trusted (re-normalized) only when its length-squared falls in `(0.81, 1.21)` — roughly unit length — otherwise the generated flat (cross-product) normal is used instead, matching the design doc's "supplied normals or generated flat normals" wording. Emits exactly one `TriangleList`/`PositionNormalUv0_F32` chunk with a generated identity index buffer (STL carries no UVs — left at `(0,0)`; no indices on the wire — synthesized 0..N-1). ASCII STL is explicitly out of scope (Tier B — ADR-006 reserves product-owned Tier-A parsers for formats like this one with a trivial, fully-specified binary layout; text parsing is a different, later slice). Unlike glTF's original synthetic-section-then-real-file two-step, this slice went straight to the real-file path from the start (`import_broker::OpenAndCanonicalizeSourceFile` + `DuplicateInheritableHandle` + worker-side `model_core::MappedFile::FromHandle`), since that pattern is now already proven. Wired in additively: `ControlOpcode::StartStlImportFromFile = 7` + a new `ParseStlFileRequest` struct — field-for-field identical to `ParseGltfFileRequest` but its own named struct, continuing this repo's "small deliberate duplication over cross-format coupling" precedent rather than generalizing the two into one shared request type. `import-worker/src/StlImportWorker.{h,cpp}` mirrors `GltfImportWorker.cpp`'s structure exactly (`HandleStlImportFileRequest` + one-shot `RunStlImport` entry point); `main.cpp` gained a `--parse-stl` branch and `WorkerRequestDispatch.cpp` gained a dispatch branch, so `--pool` mode understands STL too. **`SharedSectionValidator` needed zero changes** — confirmed format-agnostic as designed. Proven via 9 new `[stl-import]` tests in `tests/import-isolation/StlImportTests.cpp`, run through the real AppContainer-sandboxed worker end to end (inline-constructed binary-STL byte buffers via a local `BuildBinaryStl` helper, written to a real temp file via a `ScratchStlFile` RAII helper, same pattern as `MappedFileTests.cpp`): a multi-triangle file round-trips with correct chunk/vertex/index counts and correct flat normals; a plausible (near-unit-length) supplied normal is trusted even when it disagrees with the flat normal; a garbage (zero) supplied normal falls back to the flat normal; a degenerate facet and a non-finite facet are each dropped without corrupting the other valid geometry in the same file; a file where every facet is dropped is rejected as `MalformedData`; a file truncated relative to its declared triangle count is rejected as `MalformedData` (trailing bytes *beyond* the declared count are tolerated, per the design doc — only truncation is rejected); a declared triangle count over the `kMaxFacets` (2,000,000) sanity cap is rejected as `ResourceLimit`; a header shorter than 84 bytes is rejected as `MalformedData`. Per Gate 3's own exit criterion, the full `Tests.ImportIsolation` suite — including the unmodified `HostileWorkerTests.cpp` — was re-run after adding STL and passes in full: 42/42 test cases, 498 assertions, in both Debug and Release, repeated multiple times with no flakiness. `git diff` scope stayed exactly within `shared/model-core/`, `import-worker/`, `tests/import-isolation/` — no `interactive-viewer/`/`Preview3D.vcxproj`/`Preview3D.cpp`/`Renderer.cpp` changes, same as glTF's slice.
- **Gate 3 slice 3** — a real, product-owned binary-PLY (Stanford Polygon) parser behind the sandbox, the third complete format wired through the pipeline: implemented, built, and tested (not yet committed as of this note). New `import-worker/src/PlyAdapter.{h,cpp}` parses binary little- and big-endian PLY (mesh or point cloud) — unlike STL/glTF, PLY's header is always ASCII text even in "binary" mode, so this adapter is genuinely two phases: a bounded line/token text-header parser (magic `"ply"` line, `format`/`comment`/`obj_info`/`element`/`property`/`property list` declarations, `end_header` terminator, all bounded by self-contained sanity constants — `kMaxHeaderBytes`=64 KiB, `kMaxHeaderLines`/`kMaxLineLength`/`kMaxElementCount`/`kMaxPropertiesPerElement` — same "no full Tier-A hard-limit table yet" simplification STL's `kMaxFacets` already made), then a streaming-cursor binary-body reader (`ReadBytes`/`ReadScalarAsDouble`, widening any of PLY's 8 scalar types with a small new product-owned big-endian byte-swap helper — confirmed beforehand no byte-swap helper existed anywhere in the repo). **This is the first real-parser producer of a `ChunkTopology::PointList`/`VertexLayoutId::PositionOnly_F32` chunk** — previously only the Gate 2 synthetic generator exercised that shape; a file declaring a `vertex` element with no (or an empty) `face` element emits a point cloud instead of a mesh. Faces are fan-triangulated from vertex 0, with a bounded running triangle-total check (`kMaxTrianglesAfterTriangulation`) in addition to the per-face index-list-length cap (`kMaxPolygonVerticesPerFace`=255) — a face-count-amplification hostile-input class STL never had, since STL's declared facet count *is* its triangle count. Elements/properties this adapter doesn't recognize (an unrecognized element entirely, or a vertex/face property like color) are read-and-discarded for cursor correctness, never stored — `VertexPositionNormalUv0F32` has no color slot, so RGB/RGBA is deliberately dropped, same precedent as STL dropping UV to `(0,0)`. A non-finite vertex *position* fails the whole file (deliberate divergence from STL's per-element-drop philosophy: PLY vertices are shared/indexed across faces, so dropping just one would require renumbering every face that referenced it); a non-finite or non-unit-length supplied *normal* falls back per-vertex to `(0,0,1)` without failing the file, and when a mesh supplies no normal properties at all, the adapter generates smooth per-vertex normals from triangle winding (accumulate-then-normalize, bounded by the already-capped vertex/triangle counts) rather than defaulting every vertex to `(0,0,1)` — a deliberate quality judgment call, flagged here in case a future reviewer prefers the cheaper default. **Confirmed zero `SharedSectionValidator`/`WireFormat.h`/`VertexLayouts.h` changes needed** — the validator was already fully generic over `PointList` vs `TriangleList` (re-read directly to confirm, not assumed). Wired in additively: `ControlOpcode::StartPlyImportFromFile = 8` + `ParsePlyFileRequest` (same per-format-struct precedent as STL/glTF); `PlyImportWorker.{h,cpp}` mirrors `StlImportWorker.{h,cpp}`'s shape exactly; `--parse-ply` in `main.cpp`, a dispatch branch in `WorkerRequestDispatch.cpp` so `--pool` mode understands PLY too. No new `ImportErrorCode` — every PLY failure mode (including a recognized-but-out-of-scope `format ascii` file) maps onto the existing 5, though an ASCII PLY reusing `MalformedData` is a slightly worse diagnostic than a dedicated "unsupported dialect" code would give — flagged, not silently absorbed. Proven via 20 new `[ply-import]` tests in `tests/import-isolation/PlyImportTests.cpp` (388 assertions), all passing on the first real run: little- and big-endian round-trip, point-cloud-vs-mesh output-shape selection (including a declared-but-empty `face` element), quad/pentagon fan-triangulation, an unrecognized vertex property and vertex color parsed-and-dropped without corrupting cursor position, an oversized face index-list rejected as `ResourceLimit` before any index bytes are read, an over-cap declared vertex count rejected before any body scan, a header exceeding the line-count cap rejected, mid-record truncation and missing-magic/`format ascii`/missing-`x`-`y`-`z` all rejected as `MalformedData`, an out-of-range-index face and a degenerate (<3-index) face each dropped without corrupting a subsequent good face, non-finite position fails the file, non-finite normal falls back per-vertex, and smooth-normal generation when none is supplied. Per Gate 3's own exit criterion, the full `Tests.ImportIsolation` suite — including the unmodified `HostileWorkerTests.cpp` — passes in full after adding PLY: 62/62 test cases, 886 assertions, in both Debug and Release, repeated multiple times with no flakiness. `git diff` scope stayed exactly within `shared/model-core/`, `import-worker/`, `tests/import-isolation/` — no `interactive-viewer/`/`Preview3D.vcxproj`/`Preview3D.cpp`/`Renderer.cpp` changes, same as glTF/STL. **Explicitly deferred, stated up front rather than discovered later**: ASCII PLY (Tier B); vertex/face color; the design doc's spike-4 "stratified proxy-quality sampling" prerequisite for a full Tier-A PLY claim (judged to be a proxy/LOD-builder concern feeding the still-unstarted meshoptimizer deliverable, not a raw-adapter parsing concern — this slice does one complete sequential scan producing full-detail geometry only); the full Tier-A hard-limit table; retaining mesh-input vertices unreferenced by any face as a second point-cloud chunk (single-chunk output only, matching STL).
- **Gate 3 slice 4** — ASCII STL, added on top of the already-shipped binary-STL adapter (slice 2): implemented, built, and tested (not yet committed as of this note). New `import-worker/src/AsciiTokenizer.{h,cpp}` — a small, bounded whitespace-delimited tokenizer (treats `\n` as ordinary whitespace, so both this and the PLY-ASCII slice below parse a flat keyword/number token stream rather than enforcing line boundaries), enforcing a 512-byte per-token length cap and using `std::from_chars` for locale-independent number parsing, per `.docs/design/03-file-formats-and-ingestion.md`'s ASCII posture ("fixed-size blocks, a token length cap, locale-independent number parsing, no recursive grammar"). `StlAdapter.cpp` now detects the dialect before parsing: a file is treated as binary whenever its 80-byte header's declared triangle count and the actual byte length are structurally consistent with the binary layout (trailing bytes still tolerated, unchanged from slice 2) — even if it also starts with the ASCII keyword `solid` (some binary STL writers put a `solid <name>`-style comment in the free-form header) — and as ASCII only when it isn't binary-shaped but does start with `solid`; anything else falls through to the unchanged binary parser, which reproduces the exact `MalformedData`/`ResourceLimit` outcomes it already returned before ASCII existed. Per-facet validation (non-finite/degenerate drop, the supplied-normal-trust window, flat-normal fallback) and the wire-format write were factored into two small shared functions (`ProcessFacet`, `WriteStlChunk`) so both dialects run through byte-identical logic — the ASCII path is purely a different token source into the same facet pipeline. ASCII's own new hostile-input wrinkle: unlike binary STL's pre-declared triangle count (bounded by `kMaxFacets` up front), ASCII has no declared count, so the same `kMaxFacets` cap is enforced incrementally as facets are scanned. Proven via 6 new `[stl-import]` tests in `tests/import-isolation/StlImportTests.cpp`: a multi-facet ASCII round trip; a binary-shaped file whose header starts with `solid` still parses as binary (proving detection priority); a non-numeric token, an over-length token, and a missing `endsolid` each rejected as `MalformedData`; a degenerate ASCII facet dropped without corrupting the rest. Per Gate 3's own exit criterion, the full `Tests.ImportIsolation` suite — including the unmodified `HostileWorkerTests.cpp` — passes after adding ASCII STL.
- **Gate 3 slice 5** — ASCII PLY, added on top of the already-shipped binary-PLY adapter (slice 3): implemented, built, and tested (not yet committed as of this note). `PlyAdapter.cpp`'s header parser already accepted `format ascii 1.0` textually; it previously rejected that format explicitly right after parsing, which this slice removes. Rather than duplicating the whole element/property/triangulation walk for a second dialect, `PlyHeader::bigEndian` became a three-way `PlyFormat` enum (`BinaryLittleEndian`/`BinaryBigEndian`/`Ascii`), and the two format-dependent primitives the walk calls through — `readScalar`/`skipRawValue` — are chosen once per file (as `std::function`s: `AsciiTokenizer`-backed for ASCII, the existing cursor/byte-swap logic for binary) and shared by every element/property loop below them, including one raw-value-skip loop that had directly called the binary-only `ReadBytes` helper and needed rerouting through `skipRawValue` to work in ASCII mode too. This is a deliberate exception to the repo's "small duplication over cross-format coupling" precedent: that precedent is about avoiding coupling *across* formats (STL/PLY/glTF each keep their own request struct); duplicating the entire validation/triangulation walk *within* one format's two dialects is exactly what sharing it here avoids. ASCII's `readScalar`/`skipRawValue` deliberately ignore the declared `PlyScalarType` (ASCII values are plain decimal text with no fixed width) — the same downstream checks that already validate binary values (list-length caps, the vertex-count bound, `isfinite` checks, index-range checks) still apply. Proven via 8 new `[ply-import]` tests in `tests/import-isolation/PlyImportTests.cpp`, replacing the now-incorrect "`format ascii` is rejected" test from slice 3 with a positive round-trip case: a minimal ASCII point cloud, a full ASCII mesh with normals, ASCII quad fan-triangulation with the expected index pattern, a non-numeric token, an over-length token, a face declaring more indices than remain in the file (all three rejected as `MalformedData`), a non-finite vertex position failing the whole file, a non-finite supplied normal falling back per-vertex, and smooth-normal generation when none is supplied — mirroring the binary suite's equivalent cases to confirm the shared validation logic really is shared. Per Gate 3's own exit criterion, the full `Tests.ImportIsolation` suite — including the unmodified `HostileWorkerTests.cpp` — passes after adding ASCII PLY: 76/76 test cases, 1184 assertions, in both Debug and Release, repeated 5× in each configuration with no flakiness. `git diff` scope for slices 4+5 combined stayed exactly within `import-worker/` and `tests/import-isolation/` (no `shared/model-core/`, `interactive-viewer/`, `Preview3D.vcxproj`/`.cpp`, or `Renderer.cpp` changes) — `ImportErrorCode`'s existing 5 values and the wire format/`SharedSectionValidator` needed no changes for either ASCII dialect. One environmental gotcha re-encountered while verifying (already documented under Gate 3 slice 2's own section below, re-flagged here since it cost real time again): a plain incremental solution build can skip vcpkg's `AppLocalFromInstalled` DLL-deployment step for `Preview3DImportWorker`, leaving `fastgltf.dll`/`simdjson.dll` missing from the shared output directory in both Debug *and* Release even after an explicit `Preview3DImportWorker:Rebuild` — Release additionally didn't reproduce the stray `import-worker\x64\<Config>\` copy-only-DLLs behavior Debug happened to leave from an earlier build, so the DLLs had to be copied in manually from `vcpkg_installed/x64-windows/x64-windows/bin/` before either configuration's full suite would launch the real worker at all (every real-worker-launching test — glTF, STL, and PLY alike — fails identically with `ReadControlMessage` returning no value when this happens, not just the new ASCII paths). Worth a more permanent fix (e.g. a documented manual DLL-copy step, or investigating why `AppLocalFromInstalled` isn't reliably firing under this repo's customized `OutDir`) before the next person hits this cold.
- **D3D12 render integration: real sandboxed geometry on screen** — the opt-in `--d3d12` path now renders an actual mesh, sourced from the sandboxed `Preview3DImportWorker.exe` pipeline rather than `Model.cpp`'s insecure in-process GLB parser: implemented, built, and tested (not yet committed as of this note). Scoped strictly additive/opt-in per this session's usual discipline — `Renderer.cpp`, `Model.cpp`, and default (`useD3D12 == false`) behavior are completely untouched.
  - New `interactive-viewer/src/app/D3D12ImportBridge.{h,cpp}` — trusted-host code that, given a real on-disk path, reproduces (generalized over glTF/STL/PLY via `ClassifyByExtension`) the exact real-file sandboxed-import sequence `tests/import-isolation/{Gltf,Stl,Ply}ImportTests.cpp` already proved: open+canonicalize, duplicate an inheritable handle, create an output shared section, launch one fresh one-shot sandboxed worker under its own throwaway `AppContainerSid` (created and deleted per import — no persistent/pooled container yet), send the matching `StartXxxImportFromFile` opcode/request, and `ValidateAndCopySection` the reply into host-owned `ImportedMesh` structs. Deliberately does **not** use `import_broker::WorkerPool` — it has no way to duplicate a *new* source-file handle into an already-running pooled worker (only `DuplicateSectionIntoWorker` exists for output sections), so every file-open launches a fresh worker; pooled reuse across opens is a later slice. `EnsureAppContainerDirectoryAccessGranted()` moves an equivalent of the test suite's `icacls`-based `GrantAppContainerAccessToWorkerDirectory()` into product code (guarded by `std::call_once`, called from `WM_CREATE` only in `--d3d12` mode) — flagged explicitly as a dev-loop placeholder; a real installed/MSIX build needs this provisioned at install time instead.
  - `interactive-viewer/Preview3D.vcxproj` gained the host-side (non-worker) `shared/platform`/`shared/import-broker`/`shared/model-core` source set `Tests.ImportIsolation.vcxproj` already compiles for this exact real-file-import path (excluding `WorkerPool.cpp`/`.h` and worker-only `MappedFile.cpp`), plus a build-order-only `ProjectReference` to `Preview3DImportWorker.vcxproj` so the worker `.exe` is always current beside `Preview3D.exe`. No `VcpkgEnableManifest` needed — none of this pulls fastgltf/simdjson into the host process.
  - `interactive-viewer/src/app/D3D12ViewerPath.{h,cpp}` grew from a bare clear-and-present loop into a real (if deliberately minimal) forward renderer: a depth-stencil buffer (rebuilt on resize), a root signature + PSO + embedded-HLSL shader pair (same runtime-`D3DCompile` convention as `Renderer.cpp`'s D3D11 shaders — no `.hlsl` files on disk) targeting `model_core::VertexPositionNormalUv0F32`'s `{position,normal,uv}` layout with a single hardcoded albedo hemisphere/Lambertian shade (no materials yet — the wire format doesn't carry any), and a plain **synchronous** per-buffer staging-upload path (`UploadModel`/`UploadOneBuffer`) rather than `D3D12UploadRing` — that primitive is for progressive/streaming multi-chunk upload, unnecessary complexity for one static mesh; it stays available for a later streaming-focused slice. `UploadModel` silently skips any chunk whose topology/layout isn't `TriangleList`/`PositionNormalUv0_F32` (PLY point clouds included) — point-cloud rendering is a later slice. `RenderFrame(camera, aspect)` draws one `DrawIndexedInstanced` per uploaded chunk, never assuming exactly one.
  - `Preview3D.cpp`: `BeginOpen` branches on `useD3D12` — extension check via `ClassifyByExtension` (accepts `.glb`/`.gltf`/`.stl`/`.ply` instead of the D3D11 path's `.glb`-only `HasGlbExtension`), and the spawned background thread calls `D3D12ImportBridge::RunImport` instead of `Model.cpp`'s `LoadGlb`, posting a new `kD3D12ImportCompleteMessage` (`WM_APP + 3`) instead of reusing `kLoadCompleteMessage` (the payload shapes differ and the two loaders never run for the same open). Its handler uploads the model, then computes camera-framing bounds with a host-side min/max scan over the uploaded vertex payloads (`ChunkDescriptor` carries no bounds field yet) and calls the same `Camera::SetBounds` the D3D11 handler already uses. **Fixed two real bugs found only by actually running the app, not by inspection**: `RenderFrame(ViewerApp&)`'s `useD3D12` branch was unconditionally calling `RenderClearFrame()` and skipping `TickCamera` entirely (so the camera never advanced in `--d3d12` mode even before this slice); and `CanNavigate`/the mouse-drag/wheel/keyboard navigation gates all checked `app.renderer.HasModel()`, which is always false under `--d3d12` since `renderer` is never touched in that mode — camera input was completely inert until `CanNavigate` was taught to check `d3d12Path.hasModel` instead when `useD3D12`. `WM_DESTROY` now also calls `d3d12Path.ClearModel()` (after the existing `WaitForIdle()`) when `useD3D12`.
  - **Two more real bugs found only by actually running the app and looking at the output** (not caught by code review or compilation): the vertex shader's cbuffer was declared with HLSL's default (column-major) packing plus a CPU-side `XMMatrixTranspose` before upload — mathematically defensible but unverified against this repo's own convention, and swapped for `Renderer.cpp`'s exact proven approach instead (`row_major float4x4` in HLSL, `view * projection` uploaded untransposed) once a first render attempt produced a blank frame; and the PSO's rasterizer state started with `D3D12_CULL_MODE_BACK` on an unverified winding-order assumption relative to the wire format's triangle winding and the camera's right-handed convention, which culled the only triangle in every test fixture — changed to `D3D12_CULL_MODE_NONE` for this slice (a real material/culling policy is still-unstarted Gate 3 scope), flagged here rather than silently fixed, since the actual winding convention is still unverified, not proven correct.
  - Verified by actually running `Preview3D.exe --d3d12 <path>` (Debug and Release) against real `.glb`/`.stl`/`.ply` fixtures — a screenshot (via `PrintWindow` with `PW_RENDERFULLCONTENT`, since plain `CopyFromScreen` captured stale/unrelated desktop content in this environment — a real screen-capture gotcha worth remembering, not a code defect) confirmed a visible triangle mesh for all three formats; a `PostMessageW`-simulated `WM_LBUTTONDOWN`/`WM_MOUSEMOVE`/`WM_LBUTTONUP` orbit-drag sequence (real hardware input via `SetCursorPos`/`mouse_event` did not reach the window in this environment — likely session/input-desktop isolation, another environment gotcha, not a code defect) produced a visibly different frame before/after, confirming `TickCamera` genuinely drives `RenderFrame`'s view matrix now; a `SetWindowPos` resize produced no crash and correctly filled the new client area; opening a garbage/corrupt file failed cleanly (title bar reflected `SetFailure`'s summary, no crash, no D2D error card yet as expected) with no lingering `Preview3DImportWorker.exe` process afterward, confirmed via `Get-Process`. The existing `Tests.ImportIsolation` suite (76/76 test cases, 1184 assertions) was re-run in both configurations afterward and is unaffected, as expected — this slice touches only `interactive-viewer/`.
  - **Explicitly deferred, matching the plan's stated scope**: point-cloud (`PositionOnly_F32`) rendering; chrome/D2D overlay (loading/progress/error cards, ground grid) on the D3D12 back buffer — needs D3D11-on-12 interop or a parallel D2D path, so failures currently fall back to `SetFailure`'s title/state bookkeeping only; `WorkerPool`-based worker reuse across file-opens; `D3D12UploadRing`/`SceneSnapshot`/`DxgiBudgetMonitor`/`DerivedCache` wiring; retiring the D3D11 path or `Model.cpp` (both untouched; `--d3d12` stays opt-in); mid-import cancellation for the D3D12 open path; open-dialog filter/drag-drop text updates for `.stl`/`.ply` (cosmetic).
- **Draco geometry decode + a texture/material wire-format extension + KTX2/Basis texture transcode + minimal D3D12 textured rendering**, done in one combined pass per explicit direction (not committed as of this note): the vcpkg-registry blocker flagged at the end of the previous session is resolved — confirmed against the live registry at the pinned baseline (`04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`): `draco` v1.5.7, `basisu` v2.50 (not `basis-universal`), `ktx` v4.4.2 (not `libktx`), all added to `vcpkg.json` and building/linking cleanly via MSBuild's real vcpkg manifest integration.
  - **Wire-format extension** (`shared/model-core/include/model_core/WireFormat.h`, new `PixelFormats.h`/`MaterialPayload.h`): `ChunkTopology` gains `Material=3`/`Image=4`, additive, `ChunkDescriptor` itself unchanged (still 92 bytes). `PixelFormatId` (RGBA8_UNORM/BC7_UNORM/BC5_UNORM/BC3_UNORM/BC1_UNORM) + `ComputeImagePixelBytes` (checked-arithmetic mip-chain byte sum, "never guess" doctrine matching `VertexStrideForLayout`); `MaterialPayload` (72 bytes, full PBR factor set + uv transform + alpha + flags, `static_assert`-pinned). The previously-unused `ChunkDescriptor::dependencyIds`/`dependencyCount` gained real semantics: a mesh's `dependencyIds[0]` can reference its Material chunk; a Material's `dependencyIds[0..3]` reference up to 4 Image chunks (only slot 0/baseColor populated this pass).
  - **`SharedSectionValidator`** (`shared/import-broker/src/SharedSectionValidator.cpp`) gained real new surface for the first time since Gate 3 began — every prior adapter (glTF/STL/PLY) needed zero validator changes; this one didn't. Restructured into two passes: Pass A builds a `chunkId -> topology` map (new checks: `chunkId == 0` and duplicate `chunkId` both rejected); Pass B extends the existing TriangleList/PointList checks with dependency validation and adds full `Material`/`Image` payload validation (finite-float checks, closed-enum checks for `alphaMode`/`pixelFormat`/`colorSpace`, `BC5_UNORM` rejected with `colorSpace=Srgb` since it has no DXGI `_SRGB` variant, three-way byte-size cross-checks via `ComputeImagePixelBytes`) plus a new aggregate-decoded-texture-pixel budget (1 gigapixel, Tier A) enforced across the whole batch, independent of whatever the worker itself enforced.
  - **A real bug found by the pre-existing test suite, not by review**: the first validator draft assumed a mesh chunk's `dependencyIds[0]`, when populated, must resolve to a `Material` chunk. Wrong — `SyntheticSceneGenerator.cpp` (Gate 2) already uses that same field for an unrelated `PointList`-depends-on-`TriangleList` LOD/derivation relationship, predating this pass entirely; the previous session's note that the field was "currently unused" was accurate only for `GltfAdapter.cpp` specifically, not the codebase as a whole. Fixed by validating only that a populated dependency id resolves to *some* existing chunk, not one fixed target topology — a consumer (`D3D12ImportBridge.cpp`) now determines a slot's meaning by inspecting the target chunk's own topology. Caught by `ImportPipelineTests.cpp`/`HostileWorkerTests.cpp` failing on the very first post-change run of the existing suite — exactly the kind of regression that suite exists to catch, and confirmation the re-run-the-old-suite-after-every-change discipline still pays for itself.
  - **Bounded Draco decode** (`import-worker/src/DracoDecodeAdapter.{h,cpp}`, decoupled from fastgltf so independently unit-testable): real `draco::Decoder`/`draco::DecoderBuffer` calls, API verified by reading the actual installed headers first (this repo's standing discipline) rather than assumed — confirmed `Decoder::DecodeMeshFromBuffer` returns `StatusOr<unique_ptr<Mesh>>`, `PointAttribute::GetValue<T,N>(AttributeValueIndex, T*)` is the bounds-checked overload (used in preference to the unchecked one, matching this worker's "never trust a library's own unchecked fast path" precedent already established for fastgltf), and `PointIndex`'s `ValueType` is `uint32_t` (not `int` — the header's own illustrative macro-usage comment was misleading here, caught by an unsigned-comparison compiler warning during writing, not left in). Enforces the design doc's literal bound — 512 MiB decoded working set or 10M triangles → `ResourceLimit` — and cross-checks decoded point/face×3 counts against the primitive's regular-accessor-declared counts *before* copying any attribute data out. `GltfAdapter.cpp`: widened `Parser`'s enabled extensions from `Extensions::None` to `KHR_draco_mesh_compression | KHR_texture_basisu | KHR_texture_transform`; `ConvertPrimitive` branches on `primitive.dracoCompression != nullptr`, both branches converging into the same existing world-transform/flat-normal-fallback/index-bounds-check tail — zero duplication. Confirmed **directly from fastgltf's own parser source** (not just its headers — the installed source tarball was extracted and grepped) that `DracoCompressedPrimitive::attributes[i].accessorIndex` is a raw, unmodified copy of the extension JSON's draco-internal unique attribute id, not a real accessor index despite the field's name — this was the one fastgltf-API assumption in the plan that hadn't been independently confirmed before writing code, and confirming it first (rather than discovering a mismatch empirically) avoided a whole debugging cycle.
  - **Material/Image emission** (`GltfAdapter.cpp`): flat PBR factors always captured off `fastgltf::Material`/`PBRData` directly (field names/types verified from installed headers: `AlphaMode`'s numeric values already match `AlphaModeId`'s 1:1, no mapping table needed). Texture handling deliberately narrow: only `pbrData.baseColorTexture` is inspected, and only when it carries a `KHR_texture_basisu` image (`Texture::basisuImageIndex`, `Image::data` must be a GLB-embedded `sources::BufferView` with `MimeType::KTX2`) — a plain PNG/JPEG/WebP base-color texture is skipped (material keeps its factors, no image dependency; not a failure). Chunk IDs assigned in fixed order (meshes, then materials, then images) with dependency ids backpatched once every chunk's id is known, extending the existing "compute everything, single write pass, header last" structure to also lay out/write `MaterialPayload`/`ImagePayloadHeader+pixelBytes`.
  - **KTX2/Basis transcode** (`import-worker/src/TextureTranscodeAdapter.{h,cpp}`): real `ktxTexture2_CreateFromMemory` → `ktxTexture2_NeedsTranscoding` → `ktxTexture2_TranscodeBasis(..., KTX_TTF_BC7_RGBA, 0)` with `KTX_TTF_RGBA32` fallback on failure, matching the design doc's "BC7... with RGBA8 fallback" verbatim; RAII-guarded `ktxTexture2_Destroy` on every exit path. Deliberately soft-failing throughout (`std::optional`, never an `ImportErrorCode`) — the asymmetric opposite of Draco's hard-fail policy, per R-19's documented mitigation (geometry required, textures optional). Mip chain narrowed to level 0 only and `colorSpace` hardcoded to `Srgb`, both flagged as adapter-scope narrowings, not wire-format limitations.
  - **D3D12 texture rendering** (`interactive-viewer/src/app/D3D12ImportBridge.{h,cpp}`, `D3D12ViewerPath.{h,cpp}`, `Preview3D.cpp`): `ImportResult` gained `materials`/`images` alongside the existing `meshes`; the chunk-conversion loop now branches on topology (ids are already validator-confirmed to cross-reference correctly — this layer is purely mechanical unpacking). `D3D12ViewerPath` gained an additive textured PSO/root-signature variant (1 SRV + 1 static sampler, `PIXEL`-visibility descriptor table) alongside the existing untextured one — untextured meshes render through the exact unchanged path. A new `UploadOneTexture` uses `ID3D12Device::GetCopyableFootprints` to build a correctly row-pitch-aligned (256-byte) UPLOAD-heap staging buffer per row before `CopyTextureRegion`, since the wire format's tightly-packed rows do not satisfy D3D12's upload-heap pitch requirement — a straight `memcpy` would have been silently wrong here. `baseColorFactor`/metallic/roughness/emissive numeric fields are carried through to `ImportedMaterial` but not sampled by either shader yet, a deliberate one-line-follow-up scope narrowing.
  - **A second real environmental bug, distinct from the already-documented Debug-config AppLocal-skip gotcha above**: the *first-ever* Release build of `Preview3DImportWorker` in this repo (draco/fastgltf/simdjson/ktx/zstd had apparently never all been linked into a Release build before) showed `AppLocalFromInstalled` correctly listing all 5 DLLs for deployment, immediately followed by MSBuild's own `IncrementalClean` target deleting all 5 in the same build invocation — reproduced deterministically across both a `:Rebuild` and a plain incremental build forced to relink (by deleting the `.exe` first); neither survived. Root cause not fully isolated (a tracking-log/target-ordering interaction between `applocal.ps1`'s write-tlog and `IncrementalClean`'s own stale-output detection, specific to a from-empty Release output directory), but the reliable workaround is to copy the 5 DLLs from `vcpkg_installed/x64-windows/x64-windows/bin/` into `x64/Release/` directly after any build that touches these dependencies — confirmed this makes the full suite pass identically to Debug (102/102, 1403 assertions) and stays stable across a subsequent `Tests_ImportIsolation` rebuild. Flagged here rather than silently working around it, since the root cause is still genuinely unconfirmed and a future dependency addition could hit it again.
  - **Test-fixture generation tooling** (`interactive-viewer/tools/gen-test-glbs-draco.cpp`+`.ps1`, `gen-test-glbs-ktx2.cpp`+`.ps1`, new, sibling to and independent of the existing zero-dependency `gen-test-glbs.cpp`): real encoder-API tools, never shipped in the sandboxed worker, same relaxed-rules precedent as `tests/hostile-worker/`. Draco side uses `draco::TriangleSoupMeshBuilder` + `SetAttributeUniqueId` to pin known attribute ids (POSITION=0/NORMAL=1/TEXCOORD_0=2) so the generated GLB's `KHR_draco_mesh_compression` JSON can reference them directly. KTX2 side uses `ktxTexture2_Create`+`ktxTexture_SetImageFromMemory`+`ktxTexture2_CompressBasis`+`ktxTexture_WriteToMemory` against a tiny synthetic 8×8 checkerboard RGBA8 buffer — confirmed the *documented minimal-usage example already inside `ktx.h`'s own doc comments* was sufficient without needing the fuller `ktxBasisParams`/`CompressBasisEx` surface, and that `ktxTexture2_CompressBasis`'s simpler quality-only overload exists and is enough for fixture generation. `VK_FORMAT_R8G8B8A8_UNORM`'s stable numeric value (37, from the Vulkan spec) is used as a literal rather than pulling in Vulkan headers this repo doesn't otherwise depend on. Produced `draco_triangle.glb`/`draco_position_only.glb`/`basisu_textured_triangle.glb`/`basisu_corrupt_ktx2.glb`/`basisu_sample.ktx2`, all working on the first real generation run.
  - Proven via 26 new tests across `SharedSectionValidatorTextureTests.cpp` (14, hand-built section buffers, no live worker needed), `DracoDecodeAdapterTests.cpp` (4, unit-level against `draco::Encoder`-produced buffers), `TextureTranscodeAdapterTests.cpp` (3, against the real generated `.ktx2` fixture), and 5 new cases in `GltfImportTests.cpp` (Draco round-trip with/without normal+uv, flat-factor-only material, full `KHR_texture_basisu` mesh→material→image chain, corrupt-KTX2 soft-fail) — all passing on the first real run after two missing-`#include` fixes, no logic iteration needed. Full `Tests.ImportIsolation` suite (including the unmodified `HostileWorkerTests.cpp`) re-run after every stage per Gate 3's own exit criterion, ending at 102/102 test cases, 1403 assertions, in both Debug and Release, repeated with no flakiness.
  - **Visually verified**, not just asserted: `Preview3D.exe --d3d12 basisu_textured_triangle.glb`, screenshotted via the same `PrintWindow`/`PW_RENDERFULLCONTENT` technique this repo already established, shows a triangle with three visually distinct checkerboard-quadrant regions and correct bilinear blending across their boundaries plus the existing hemisphere/Lambertian shading still varying brightness across the surface — genuine per-pixel texture sampling, not a flat color. A second run against the untextured `tri_tight.glb` reproduced the exact same flat-gray shaded output as before this pass (no regression to the untextured path), and no `Preview3D.exe`/`Preview3DImportWorker.exe` process was left running after either.
  - **Explicitly deferred, stated up front**: `metallicRoughnessTexture`/normal/emissive texture maps (slots 1-3 of a Material's `dependencyIds` exist in the wire format but nothing populates them yet); non-`KHR_texture_basisu` (plain PNG/JPEG/WebP) base-color textures; a native `.ktx2` sidecar-file entry path (only GLB-embedded is handled); mip levels beyond 0; semantic-based BC5/BC3/BC1 transcode-target selection (base color always attempts BC7 first); consuming `baseColorFactor`/metallic/roughness/emissive numeric fields in either shader (tint/PBR shading); `EXT_meshopt_compression`/`KHR_mesh_quantization`/`EXT_texture_webp`/`KHR_materials_unlit`'s actual shading behavior (the extension flag isn't enabled, so these still fail cleanly as unsupported); the meshoptimizer LOD/proxy builder; derived-cache production wiring for textures (ADR-012 already scopes this in, not yet built); `WorkerPool`-based reuse for this import path (still one fresh worker per file open, unchanged from the prior slice).
- **Everything above marked "not yet committed as of this note" is now committed.** Those entries were written before their commits landed and were never revised; `git log` is authoritative. As of `731121d` the tree contains all of slices 2-5, the D3D12 render integration, and the Draco/texture/KTX2 pass.
- **Sidecar file resolution + WIC texture decode for `.gltf`** (committed `731121d`, recorded here after the fact — the commit landed without a PROGRESS entry): the trusted host gained `import_broker::ResolveSidecarPath` (relative-and-local references only, canonicalized after opening, must stay inside the primary file's own directory so a reparse point cannot smuggle a reference out) and `ServiceSidecarRequest`, factored as one host-process-agnostic function so tests drive the same code the app does. Three additive opcodes: `RequestSidecarFile = 9` (worker→host, the first non-terminal worker→host message), `SidecarFileReady = 10`, `SidecarFileUnavailable = 11`. Worker side gained `SidecarFileClient`, `WicImageDecodeAdapter` (PNG/JPEG/BMP/TIFF via inbox WIC, soft-failing per `TextureTranscodeAdapter`'s convention), and `ImageFormatSniff` (container identified from magic bytes, never a declared extension or MIME type). `ImportError` gained `UnsafeReference` and `FileUnavailable`, both named verbatim from the design doc's error taxonomy. This supersedes two earlier notes in this file: the "native `.ktx2` sidecar-file entry path" deferral above, and the "no sidecar-directory containment" caveat in the notes section below.
- **External-buffer geometry for `.gltf` — the blocker that made the sidecar work incomplete**: a conventional `scene.gltf` + `scene.bin` with *uncompressed* geometry still hard-failed `MalformedData`. Sidecar resolution reached only Draco bufferViews and images; ordinary accessors were gated by an `AccessorBufferIsEmbedded` check whose comment asserted that routing them through fastgltf would require "a substantial reimplementation" of its accessor reader. **That comment was wrong**, and reading the installed headers (this repo's standing rule) showed why: `fastgltf/tools.hpp`'s `DefaultBufferDataAdapter` is a *defaulted template parameter*, and `IterableAccessor`'s constructor routes every read through it — the primary bufferView plus `sparse->indicesBufferView` and `sparse->valuesBufferView`. Supplying a `SidecarBufferDataAdapter` that delegates to the existing `ResolveBufferViewBytes` therefore keeps fastgltf's sparse-override, component-type up-conversion and interleaved-stride handling exactly as written. The feared large rewrite was a thin functor plus four call-site changes, all inside `GltfAdapter.cpp`.
  - Two safety findings came out of it, both now closed. `fastgltf::span::subspan` is **not** bounds-checked (`&data()[offset]`, `size() - offset`), so returning an empty span from a failed adapter would form an out-of-range pointer and an underflowed length and then read through it — resolvability is therefore pre-flighted in `EnsureAccessorBytesResolvable` *before* the iterate call, never signalled from inside the adapter. And `IterableAccessor` indexes `asset.bufferViews[*accessor.bufferViewIndex]` with no `has_value()` guard, so a sparse accessor that legally omits `bufferView` would dereference an empty optional; such accessors are now rejected rather than handed over. The old code reached both.
  - Failures now carry the reason the host actually gave instead of collapsing to `MalformedData`: the memoized buffer entry keeps its `ImportErrorCode`, so a missing sidecar reports `FileUnavailable` and a path-policy rejection reports `UnsafeReference`.
  - 5 new `[gltf-import][sidecar]` cases, all passing on the first run: external-`.bin` round-trip producing byte-identical geometry to the embedded `tri_tight.glb`; missing `.bin` → `FileUnavailable`; short `.bin` → `MalformedData`; `../` traversal → `UnsafeReference`; and a sparse accessor over an external buffer applying its override, which is the case that proves swapping the adapter did not bypass fastgltf's sparse handling. `RunGltfImportFromRealFile` now services `RequestSidecarFile` through the production `ServiceSidecarRequest`, so these run against the real worker process, not an in-process harness. Suite went 116 → 121 cases, 1475 → 1567 assertions, green in Debug and Release; hostile-worker suite re-run and still passing.
  - **Visually verified** with the established `PrintWindow`/`PW_RENDERFULLCONTENT` technique: `tri_external.gltf` renders 10,935 non-background pixels, *pixel-identical* to the embedded `tri_tight.glb` of the same triangle, while a negative control whose `.bin` is absent renders 0 — the fix is confirmed at the pixel level, not just at the validator.
  - New fixtures `tri_external.gltf` + `tri_external.bin` from `gen-test-glbs.cpp`; the four pre-existing `.glb` fixtures regenerate byte-identical (496/636/580/580).
- **`meshoptimizer`, `directxtex` and `libwebp` pinned in `vcpkg.json`** ahead of the slices that consume them, so the dependency and licence surface is settled before feature work. Nothing links them yet. Note `directxtex` pulls in `directxmath` transitively. Per `10-delivery-plan.md` L283 each still owes its limits, error semantics, parser threat review, fuzz corpus, thumbnail decision, licence entry and performance classification in `11-decisions-and-risks.md` as it is actually wired in.
- **The product's sandboxed import path now uses the sandbox Gate 2 actually proved** (implemented, built and tested; not committed as of this note). Not a Gate 3 deliverable — a set of six defects in already-shipped Gate 3 code, found by measuring `interactive-viewer/src/app/D3D12ImportBridge.cpp` against what `tests/import-isolation/` already enforces. They were worth doing before the `--d3d12` default flip, which turns this path on for every user.
  - **A crash, not a leak, was the worst of them.** `platform::AppContainerSid::CreateOrOpen` *throws* `std::runtime_error` (`AppContainerSid.cpp:47-49`); the bridge called it from a **detached** `std::thread` with no `try`/`catch`, so an exception escaping the thread entry called `std::terminate` and took the whole app down instead of showing an error. The `if (!sid)` guard after the call was dead code — `CreateOrOpen` never returns a null SID. The D3D11 path had wrapped its loader in `catch(bad_alloc)` + `catch(...)` all along (`Preview3D.cpp:1280-1293`); the D3D12 path had nothing. Fixed at both levels: a typed `CreateSandboxProfile` failure at the call site, and the same catch pair the D3D11 branch uses as the backstop.
  - **New `import_broker::ImportSession`** (`shared/import-broker/`) holds the launch → converse → validate sequence lifted out of the bridge. This is the enabling move, not a tidy-up: the bridge compiles only into `Preview3D.exe`, so *none* of that sequence was reachable from any test, which is exactly how it drifted. The bridge keeps only app-layer concerns (extension classification, stage → user-facing text, chunk unpacking). A new `ImportStage` enum preserves each host-side plumbing failure's distinct message, which `ImportErrorCode` alone could not — that enum is the worker's wire taxonomy, not a record of how far the host got.
  - **No Job Object commit limit was set on the shipping path at all.** `SandboxLimits{}` means `processMemoryLimitBytes = 0` = uncapped, and the only caller that ever set one was a test (`SandboxLaunchTests.cpp:277`). `09-...:76` required this "measured and fixed in Gate 2". Now `kImportWorkerCommitLimitBytes` = 4 GiB, **derived** from the documented Tier A budgets (1 GiB scratch + 512 MiB Draco + texture/overhead allowance) and labelled provisional in its own comment — the A-* corpus that would actually measure it does not exist, and this should not be cited as measured. Made injectable per request (the same seam `DxgiBudgetMonitor`'s `QueryFn` established) so a test proves enforcement differentially: the same file imports at the real ceiling and fails under a 1 MiB one.
  - **The reply read was unbounded.** `model_core::ReadControlMessage` is a blocking `ReadFile` with no timeout, so a worker that *hangs* (as opposed to crashing, which breaks the pipe) blocked the import thread forever. New `import_broker::ControlChannelWait` shares one bounded read with `WorkerPool::WaitForReply`, whose open-coded loop it replaces — **fixing two flaws rather than copying them**: the deadline now covers the whole message instead of committing to an unbounded read once any byte arrives (a worker writing a header then stalling mid-payload used to hang past its timeout), and time comes from `steady_clock` instead of `elapsed += kPollIntervalMs`, which drifted long by up to 15× because `Sleep(1)` is really 1-15 ms.
  - **Cancellation on the D3D12 open path**, which had none — its thread captured neither `alive` nor a token, so `WM_DESTROY` could not reach it. Now supersedes the previous generation on a new open and observes the same `app.cancellation` `WM_DESTROY` already trips, polled between control-channel waits. Scoped to the D3D12 branch so the D3D11 default path's behavior is untouched. **This is host-side abandonment only** — `06-...:119` wants a cooperative in-parse cancel *first*, and those stop-token checkpoints belong with the adapter loops the splitting/LOD work is going to rewrite anyway.
  - **One stable AppContainer profile** (`Binbuf.Preview3D.ImportWorker`, the name `08-...:40` already specifies) replaces a uniquely-named profile created *and deleted per import*. That churn sat inside the A-small ≤500 ms / A-medium ≤2 s gates and would have corrupted every future perf measurement, and it leaked a persistent per-user profile on the two paths that never reach the delete: a hung worker, and the app closing mid-import. `CreateOrOpen` already treated `ERROR_ALREADY_EXISTS` as success, so create-once-reuse needed no new API.
  - **The `icacls` shell-out is gone from both product and tests.** `EnsureAppContainerDirectoryAccessGranted` built a command string and called `_wsystem` — spawning `cmd.exe` from the shipping GUI — and granted `S-1-15-2-1`/`S-1-15-2-2`, i.e. *every AppContainer on the machine*. Replaced by `platform::GrantDirectoryReadExecute`/`RevokeDirectoryAccess` (`GetNamedSecurityInfoW`/`SetEntriesInAclW`/`SetNamedSecurityInfoW`) naming one specific SID. `SandboxTestSupport.h` had the identical shell-out duplicated verbatim and now shares the same helper.
  - **A verification hazard worth remembering**: the old generic ACEs persist on the build output directory, so the new per-SID grant would appear to work even if it did nothing. Verification therefore *stripped every `S-1-15-2-*` ACE first* and then ran the suites — they pass from a clean DACL, which is the only way this is actually proven. Doing that also exposed a real bug in the first draft: the fixture revoked through `sid.get()`, but `WorkerPoolTests.cpp` moves `fixture.sid` into `WorkerPool::Initialize`, leaving it null in the destructor — measured as exactly 4 stranded SIDs per suite run, one per moving test. Fixed by keeping a `CopySid` copy of the bytes; a full run now leaves **0** ACEs behind (plus the product's one deliberate stable-profile ACE when the new tests run).
  - Proven via 11 new `[import-session]` tests in `tests/import-isolation/ImportSessionTests.cpp` — real end-to-end import through the product's own path, format→adapter routing, profile reuse across sequential imports, cancel-before-launch, the differential commit-limit pair, and five deterministic control-channel cases driven through plain anonymous pipes with no worker at all (timeout, header-without-payload, closed write end, cancellation, two-part message still read whole). Deliberately **no `SandboxFixture`** in most of them: `RunImportSession` runs under the real long-lived profile, and exercising the shipping identity is the point. One assertion was wrong on first run and the test was right — a GLB fed to the STL adapter fails `ResourceLimit`, not `MalformedData`, because its bytes at offset 80 read as an oversized declared facet count and `kMaxFacets` rejects it before any body scan; that is a sharper proof the STL adapter really ran, so the assertion was corrected rather than loosened.
  - Full suites green in **Debug and Release**: `Tests.Unit` 51/51 (5082 assertions), `Tests.ImportIsolation` 132/132 (1614 assertions, up from 121/1567), including every pre-existing hostile-worker case unmodified — this slice changes no wire-format or control-protocol struct, which is what keeps it inside the Gate 2 freeze at `10-...:126`. Visually confirmed via the established `PrintWindow`/`PW_RENDERFULLCONTENT` technique across `tri_tight.glb`, `tri_external.gltf` (pixel-identical to the embedded one, as before), `basisu_textured_triangle.glb` (checkerboard quadrants still sampling), and `draco_position_only.glb`; `unsupported_extension.glb` fails cleanly at 0 non-background pixels; no leftover `Preview3DImportWorker.exe` after any run.
  - **Deliberately still open**: cooperative in-parse cancellation (above); install-time profile/ACL provisioning (Gate 7 — what is fixed here is that the runtime grant is narrow and API-based, so Gate 7 becomes a move rather than a rewrite); `WorkerPool`-based reuse for this path, which is still one fresh worker per open.
- **Frame pacing on the D3D12 path** (implemented, built and tested; not committed as of this note). First chunk of the "make the D3D12 path the only path" batch, and a prerequisite for the ADR-010 spike: the spike has to measure D3D11On12 overlay cost, and on a pipeline that stalls to idle every frame it would measure the stall.
  - **What was serialized.** `RenderFrame` and `RenderClearFrame` both opened with `WaitForIdle()`, and there was one command allocator, one `lastFrameFence`, and one constant buffer. The swap chain's frame-latency waitable object — created with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` and `SetMaximumFrameLatency(2)` since Gate 2, and exposed through `FrameLatencyWaitableHandle()` — had **never been waited on by anything**; the only consumer anywhere was a null-check in a test.
  - Now `kFrameCount` (= `D3D12SwapChain::kBufferCount` = 3) slots, each with its own allocator and the fence value of its last submission. `BeginFrame` waits on the frame-latency object, then on **that slot's own** fence before resetting its allocator — the invariant `04-rendering-and-streaming.md:32` states ("An ID3D12CommandAllocator is reset only after the fence value of its last submitted command list has completed"). The command list stays single: a list can be `Reset` onto any allocator, which is the shape `SwapChainTests.cpp`'s `FrameRecorder` already used.
  - **The constant buffer was the sharp hazard, and it is not obvious from reading the pacing code.** It was a single 256-byte persistently-mapped allocation `memcpy`'d every frame and read by the GPU through a root CBV. `WaitForIdle()` was silently making that safe. Removing the stall without slotting it would have let the CPU overwrite frame N-1's view-projection while the GPU was still reading it — an intermittently wrong camera, not a crash, and so exactly the kind of thing that would have been blamed on something else later. Now `kFrameCount * 256` bytes, bound at `+ frameIndex * 256`.
  - **Uploads no longer share the render allocator.** `UploadOneBuffer`/`UploadOneTexture` reset `commandAllocator` directly and clobbered `lastFrameFence`; with per-frame slots that would mean resetting an allocator whose frame is still in flight. They now have their own allocator, list and fence and wait only on their own lane. They keep their synchronous semantics deliberately — their staging buffers are locals released at return, which is only safe because they block. Moving them to the copy queue properly is later work (`04-...:16` wants one direct and one copy queue); this only stops the collision. `D3D12UploadRing` was the in-repo precedent, already owning its own queue/allocator/fence.
  - `WaitForIdle()` survives as a real shutdown/resize drain (`Resize`, `ClearModel`, `WM_DESTROY` all need it) but now waits on the highest outstanding slot value rather than one scalar, and `EndFrame` no longer discards `SignalNext()`'s failure — it returns 0 without incrementing on failure, which previously degraded `WaitForIdle()` into a silent no-op.
  - New `interactive-viewer/src/graphics/FrameStats.{h,cpp}` — a CPU-side stopwatch (mean, nearest-rank p95 over a 240-sample ring, presented/occluded counts). Put under `src/graphics` rather than beside `D3D12ViewerPath` specifically because that directory **is** on `Tests.Unit`'s include path, so the arithmetic is unit-testable without dragging the viewer path into the test project. Exposed behind a `--frame-stats` flag that appends the numbers to the window title — the one surface already readable from outside the process, and temporary until the D2D overlay exists. Deliberately not the ETW schema `04-...:194-203` specifies; that is verification-infrastructure work for a much later batch.
  - **Correction to the batch plan, before it was built on.** That plan had this chunk also switching the swap chain to `B8G8R8A8_UNORM` on the claim that "D2D requires BGRA". The claim was unverified and contradicts `04-rendering-and-streaming.md:17`, which specifies `DXGI_FORMAT_R8G8B8A8_UNORM` explicitly — and `D3D11_CREATE_DEVICE_BGRA_SUPPORT` is about the device supporting BGRA resources, not about the swap-chain format. Deferred to the ADR-010 spike, which creates a real 11on12 device and wraps a real back buffer and so can answer it in one line. If BGRA does turn out to be required it is a design-doc change through change control (`10-...:283`), not a silent edit. For the record the switch is exactly 3 sites — `D3D12SwapChain.cpp:17`, `D3D12ViewerPath.cpp:359`, `:462` — and no others: `ResizeBuffers` passes `DXGI_FORMAT_UNKNOWN` and RTV creation passes a null desc, and `DxgiFormatFor`'s `R8G8B8A8_UNORM` at `:160` is the **texture** lane mapping the wire format's own `PixelFormatId::RGBA8_UNORM` and must not move.
  - Proven by 8 new `[graphics]` tests. 7 cover `FrameStats` (empty state, first-present-seeds-only, mean/p95 against 1..100 ms, ring eviction, occluded counting, reset, and one real `steady_clock` interval). The eighth is the pacing proof in `SwapChainTests.cpp`: a per-frame-allocator ring over 12 frames that waits on `FrameLatencyWaitableHandle()` and asserts the fence a frame waits on is **exactly `kBufferCount - 1` submissions behind the newest**, where the pre-existing single-allocator loop in the case above it is 1 behind by construction. That assertion is deterministic — fence values increment by one per frame and a slot is revisited every `kBufferCount` frames — rather than depending on how fast this GPU happens to be. How far the CPU actually got ahead *is* hardware-dependent, so it is reported via `INFO`, never asserted.
  - **Honest limitation on the app-level measurement.** The plan wanted a before/after mean/p95 comparison in the real app. That number is not available and the burst figures should not be read as one: the viewer deliberately blocks in `MsgWaitForMultipleObjectsEx` when nothing is moving (`Preview3D.cpp:2883-2886`, "a still viewport costs no CPU or GPU"), and neither a `PostMessageW`-simulated orbit drag nor a held flight key produced sustained rendering — both runs reported ~25 frames, which is the startup/load burst, not steady state. **This is a real finding for the ADR-010 spike**, which is required to measure "at 144 Hz": there is currently no way to make this app render continuously, so the spike needs a benchmark mode that renders N frames back to back independently of the on-demand loop. The pacing claim therefore rests on the deterministic test above plus pixel-identity, not on a wall-clock number.
  - Verified pixel-identical across `tri_tight.glb`, `tri_external.gltf`, `basisu_textured_triangle.glb` and `draco_position_only.glb` — 0 differing pixels of 1,728,992 each, byte-compared against pre-change captures rather than eyeballed, which is the bar a pure pacing change should meet. `Tests.Unit` 59/59 (5200 assertions, up from 51/5082) and `Tests.ImportIsolation` 132/132 (1614 assertions, unchanged) in both Debug and Release. That the import suite did not move by a single assertion is the intended signal: this chunk touches no wire format, control protocol or import code at all.
- **ADR-010 spike: D3D11On12 + Direct2D over the D3D12 swap chain** (implemented, built and measured; not committed as of this note). Required validation spike 1 (`11-decisions-and-risks.md:243`, "Before Gate 1 completion, measure D3D11On12 overlay ordering and cost at 144 Hz, including resize and GPU validation") had never been run, which is why ADR-010 is still "Accepted **provisionally** through Gate 1". Measured on this machine's actual reference display: an RTX 4080 at 143 Hz, so a 6.99 ms frame budget.
  - **The format question is settled, and the batch plan's assumption was wrong.** That plan asserted "D2D requires BGRA" and scheduled a swap-chain format switch. **Direct2D attaches to the design's `DXGI_FORMAT_R8G8B8A8_UNORM` swap chain through D3D11On12 without complaint** — `D3D11_CREATE_DEVICE_BGRA_SUPPORT` is a device capability flag, not a constraint on the swap-chain format. So `04-rendering-and-streaming.md:17` stands, no format change is needed anywhere, and no design-doc change control is required. Deferring that switch out of the pacing chunk rather than making it on the assumption was the right call; had it gone in, three files and a design doc would have been changed for nothing.
  - New `interactive-viewer/src/graphics/D3D11On12Overlay.{h,cpp}`: the 11on12 device over the existing direct queue, a wrapped resource per back buffer, and a D2D target per buffer via the same `CreateDxgiSurfaceRenderTarget` call `Renderer.cpp:1024-1032` already uses. Declared `RENDER_TARGET` in / `PRESENT` out, so the scene pass leaves the buffer as a render target and the bridge's `ReleaseWrappedResources` performs the transition — `04-...:46` specifies exactly this ("If an overlay pass follows, it leaves the wrapped back buffer in the bridge's declared input state; otherwise it transitions the buffer to PRESENT itself"), and `D3D12ViewerPath` now skips its own `toPresent` barrier when the overlay is on.
  - **Cost, 2000 frames per configuration with the first 120 discarded as warm-up, real scene loaded:**

    | Configuration | mean | p95 | overlay CPU/frame |
    | --- | ---: | ---: | ---: |
    | overlay off | 6.94 ms | 12.7 ms | — |
    | overlay on, **interop only** (0 primitives) | 7.20 ms | 19.3 ms | **2.02 ms** |
    | overlay on, chrome-scale (250 primitives + 40 text runs) | 7.21 ms | 17.8 ms | **2.96 ms** |

    The decomposition is the interesting part: **~2.0 ms of the 2.96 ms is the `Acquire`/`Release`/`Flush` handshake itself, not the drawing.** Drawing a full chrome's worth of D2D content on top costs only ~0.9 ms more. At 143 Hz the handshake alone is ~29% of the frame budget. Measured with the brush and text format cached exactly as `Renderer.cpp` caches them — creating an `IDWriteTextFormat` per frame first showed 2.87 ms and would have been a strawman.
  - **Recommendation: do not promote or reject ADR-010 on these numbers yet.** The mean stays vsync-locked in every configuration, so frames still present every refresh. But the p95 cannot decide anything, because **the baseline p95 is already 12.7 ms with no overlay at all** — well over NFR-04's ≤8.3 ms — since rendering still happens on the UI thread interleaved with the message pump. That is precisely what the render thread (the next chunk, NFR-01/NFR-02) fixes, and it is the architecture ADR-010's own consequence clause assumes ("one render thread must own all interop contexts"). Re-measure after it; deciding now would be deciding on the wrong architecture. The 2.0 ms handshake figure is the durable result and does transfer.
  - **Two things for whoever builds the real overlay.** `CreateDxgiSurfaceRenderTarget` yields three *independent* `ID2D1RenderTarget`s, and a brush belongs to the target that created it — so device resources cannot be shared across back buffers (this file keeps a brush per buffer; `Renderer.cpp` gets away with one because it has a single target). That is an argument for moving to `ID2D1Device`/`ID2D1DeviceContext` plus a bitmap per buffer, which shares device-level resources and leaves the ported drawing code untouched, since `ID2D1DeviceContext` *is* an `ID2D1RenderTarget`. And before accepting 2.0 ms as the price, it is worth testing whether the per-frame `ID3D11DeviceContext::Flush()` is avoidable and whether redrawing the overlay only when it is dirty (into a cached bitmap) removes most of it — the real chrome does not change every frame.
  - **The benchmark mode chunk 0 said was missing now exists**: `--frame-bench=N` renders N frames back to back instead of waiting for something to move, and `--overlay-spike[=N]` turns the bridge on with a settable primitive count so interop overhead can be isolated from drawing. Without these there is no way to measure a sustained frame rate at all — the viewer deliberately blocks when the scene is still.
  - Proven via 6 new `[graphics]` tests in `tests/unit/D3D11On12OverlayTests.cpp`: the format question above; a scene pass and overlay pass sharing one back buffer and presenting (the resource-state handshake); frames repeating across the whole back-buffer ring; surviving a swap-chain resize (the wrapped resources must be dropped **and flushed** before `ResizeBuffers`, then rebuilt — the part most likely to break); ordered, idempotent shutdown per `04-...:190`; and a **debug-layer assertion**. That last one exists because passing tests are not evidence on their own: debug-layer messages go to `OutputDebugString` and fail nothing unless the info queue is inspected, so the test counts `ID3D12InfoQueue` messages at ERROR/CORRUPTION severity across six overlay frames plus a resize and asserts the count does not move. It `WARN`s rather than silently passing when the info queue is unavailable (Release, or no Graphics Tools feature). Confirmed `D3D12SDKLayers.dll` is present on this machine, so the Debug run genuinely exercised it.
  - `Tests.Unit` 65/65 (5433 assertions Debug, 5360 Release — the difference is the debug-layer case `WARN`ing out where the info queue does not exist) and `Tests.ImportIsolation` 132/132, both configurations. The default (overlay-off) render path is byte-identical to before: 0 differing pixels of 1,728,992 on `tri_tight.glb` and `basisu_textured_triangle.glb`.
- **Dedicated render thread, and a correction to the ADR-010 spike's numbers** (implemented, built and measured; not committed as of this note).
  - **The correction first, because the previous commit's message is wrong.** `2cbd8da` claimed "~2.0 ms of the 2.96 ms is the Acquire/Release/Flush handshake itself, not the drawing", and recommended investigating whether the per-frame `Flush()` could be avoided. **Both are wrong.** Those figures were every-run-was-the-first-run-after-a-build: 120 warm-up frames is under a second at 143 Hz, nowhere near enough to shake off cold start. Rebuilding the *pre-thread* commit and measuring it again in a later session produced 21.3 ms p95 on one run and 7.07 ms on another **from the same binary** — the harness variance dwarfed everything I concluded from it. With the first run after each build discarded and four runs averaged, the numbers are stable to ±0.1 ms and read completely differently:

    | Configuration | mean | p95 (range over 4 runs) | overlay CPU/frame |
    | --- | ---: | ---: | ---: |
    | overlay off | 6.96 ms | 7.08 ms (7.1-7.1) | — |
    | interop only (0 primitives) | 6.95 ms | 7.06 ms (7.1-7.1) | **0.09 ms** |
    | chrome-scale (250 prims + 40 text) | 6.94 ms | 7.24 ms (7.2-7.3) | **0.93 ms** |

    So the interop handshake is ~0.09 ms — effectively free — and the D2D *drawing* is what costs anything, the exact inverse of what the spike commit claims. Every configuration sits locked to the 143 Hz refresh (6.99 ms) with almost no jitter.
  - **That resolves ADR-010, and it passes comfortably.** p95 moves 7.08 → 7.24 ms with a full chrome's worth of overlay, against NFR-04's ≤8.3 ms gate at 144 Hz. Its escape hatch ("a product glyph atlas only if interop demonstrably violates frame/lifetime gates") is not triggered. `11-decisions-and-risks.md` updated from "Accepted provisionally through Gate 1" to Accepted, with the evidence recorded — `10-delivery-plan.md:283` requires a spike's outcome to update its ADR, and leaving it provisional after the spike passed would leave the process half-done.
  - Also corrected while here: the D3D11On12 overlay spike is **required validation spike 1** at `11-decisions-and-risks.md:243`, not "spike 3" at `:233`. The previous commit and its comments had it wrong throughout; the code comments and docs are fixed, though the commit message itself cannot be.
  - **Methodology worth keeping**: discard the first run after any build, repeat at least three times, and never compare numbers across sessions. Cross-session comparison is what produced the phantom "p95 regressed from 12.7 to 22 ms" that cost several rounds to chase down. Chasing it was still the right call — the alternative was shipping a wrong story either way.
  - **The thread itself.** New `interactive-viewer/src/app/RenderThread.{h,cpp}` owns `D3D12ViewerPath` **privately** and replaces `ViewerApp::d3d12Path`, so the ownership split is structural rather than a convention nobody can enforce in a 2900-line file. It satisfies NFR-02 ("a dedicated render thread MUST own direct-queue submission, swap-chain resize, and Present") and removes the per-frame GPU fence wait from the UI thread, which NFR-01 forbids and `09-...:289` lists among the things that cannot be waived. Command inbox (coalesced resize, model upload, clear, shutdown) plus a wake event; the loop renders when invalidated / animating / benching and otherwise waits bounded, per `04-...:38`. `AssertOnRenderThread` implements `04-...:194`'s "developer builds assert queue ownership".
  - **The camera could not simply move.** The UI thread needs synchronous reads (gizmo hit-testing, picking) and the render thread must call `Camera::Update` itself, or inertia stalls whenever no messages arrive. So it is shared under a lock, handed out by a `LockedCamera` RAII accessor. To make sure not one of the ~40 existing accesses was missed — each one a data race — `ViewerApp::camera` was **renamed to `sharedCamera`**, turning every one into a compile error rather than something to find by inspection. `04-...:172`'s compact-input-event queue remains the deferred follow-up.
  - **Two thread-affinity traps, both real.** `BuildFlightInput` calls `GetKeyState(VK_SHIFT)` and `GetClientRect`; on the render thread `GetKeyState` returns *that thread's* input state, so Shift-boost would have broken silently with no error. It stays on the UI thread and its result is published. And `WM_PAINT` — reachable from any nested modal loop (`TrackPopupMenu`, `MessageBoxW`, `IFileOpenDialog::Show`, the DWM move/size loop) — no longer renders at all on this path; it validates and signals, or it would present from the UI thread while the render thread was also presenting.
  - `D3D12ViewerPath::RenderFrame` now takes a precomputed view-projection matrix instead of a `Camera&`. Holding the camera lock across a whole frame, including `BeginFrame`'s fence and frame-latency waits, would block input for the length of a GPU frame. As a side benefit the path no longer includes `Renderer.h` at all, which is one less thing tangled up in the later Camera extraction.
  - **A real defect found by the measurement, not by review**: `RenderOneFrame` initially published frame statistics every frame, and `FrameStats::P95Ms` allocates a vector and sorts its whole 240-sample window. An allocation and a sort inside the frame path is exactly the kind of thing that shows up as an occasional missed vsync rather than a slower mean. Now throttled to every 30 frames, forced once at bench completion.
  - Upload and the O(vertices) bounds scan moved off the UI thread into the render thread, which owns the payloads by then; it posts `kRenderUploadCompleteMessage` back so the UI thread reaches Ready or Failed without ever having touched the GPU. The bounds scan also now counts `PositionOnly_F32` chunks — previously a point cloud contributed no bounds at all and framed as if empty.
  - **Verified**: pixel-identical across `tri_tight.glb`, `tri_external.gltf`, `basisu_textured_triangle.glb`, `draco_position_only.glb` — 0 differing pixels of 1,728,992 each, byte-compared. A 60-step resize storm, a nested modal loop held open for 3 s while rendering, and closing mid-import all exit cleanly with no leftover `Preview3D.exe` or `Preview3DImportWorker.exe`. Structurally confirmed that `BuildFlightInput` is called only from UI-thread code, that `RenderThread.cpp` contains no `GetKeyState`/`GetClientRect`, and that no bare `app.camera` access remains. `Tests.Unit` 65/65 and `Tests.ImportIsolation` 132/132 in both configurations — the import suite unchanged to the assertion, as intended for a chunk that touches no import code.
  - **Deliberately not fixed here**, though the new `HasModel()` facade makes each a one-liner: six call sites ask `app.renderer.HasModel()` unconditionally, so on `--d3d12` they are always false (`HasNavigableModel`, `FrameSelectedOrAll`, `ToggleShowNativeOrientation`, `CancelOpen`, `ID_VIEW_RESET`). Consequences today: the bottom bar and info panel never reserve space, `F`/double-click framing no-ops, Home/R Reset no-ops. Fixing them changes `ViewportAspect` and therefore every pixel, which would destroy this chunk's own verification — they belong with the overlay chunk, where the reserved space becomes visible and the aspect change is the intended outcome.
- **Overlay bridge moved from D2D 1.0 targets to an `ID2D1Device`/`ID2D1DeviceContext` plus a bitmap per back buffer** (implemented, built and measured; not committed as of this note). This is the thing the spike flagged as necessary before the real chrome is ported, and it had to land before the extraction rather than after.
  - **Why it matters, concretely.** `CreateDxgiSurfaceRenderTarget` gives three *independent* `ID2D1RenderTarget`s, and a D2D resource belongs to whichever target created it. `D3D12ViewerPath` was therefore carrying `overlayBrushes[kBufferCount]` — one brush per back buffer for a single colour. The ~750 lines of chrome being ported out of `Renderer.cpp` keep one brush and recolour it per primitive, plus five cached `IDWriteTextFormat`s; under the old shape every one of those would have needed triplicating. One device context retargeted per buffer makes device resources shared, so the ported code needs no change at all — `ID2D1DeviceContext` *is* an `ID2D1RenderTarget`, which is what that code is written against.
  - `BeginDraw` now returns the *same* context every frame and `SetTarget`s the right bitmap; `D3D12ViewerPath` drops to a single `overlayBrush`. Verified by a new test that draws six frames across the whole back-buffer ring reusing one brush created on the first — impossible under the previous shape.
  - **The `D2DERR_RECREATE_TARGET` bug is deliberately not reproduced.** `Renderer.cpp:1545-1549,1664-1668` resets its target on that HRESULT but nothing ever recreates it except a later `Resize`, so the entire overlay silently vanishes until the user happens to resize the window. Here `EndDraw` drops the bitmaps, the next `BeginDraw` rebuilds them, and `ConsumeTargetsWereRecreated()` tells the caller its own cached device resources went with them.
  - **Cost is unchanged, which is the expected result** — the upgrade buys shared resources, not speed. Four runs each, first-after-build discarded: overlay off 6.94 ms mean / 7.13 ms p95; interop only 7.02 / 7.15 with **0.11 ms** overlay CPU; chrome-scale 7.00 / 7.81 with **1.03 ms**. Compare 0.09 / 0.93 before the upgrade.
  - **One honest caveat on the ADR-010 margin.** The chrome-scale p95 ranged 7.3–9.0 ms across four runs, so one run crossed NFR-04's 8.3 ms gate. The average is inside it and the conclusion stands, but the margin is thinner than the single 7.24 ms figure first recorded in the ADR suggests, and the ADR's evidence note has been amended to say so. The synthetic stand-in redraws all 250 primitives with a changing brush colour every frame, which the real chrome will not; this needs re-measuring against the ported chrome, and if the margin does not improve then dirty-tracking the overlay instead of redrawing it per frame is the first thing to try.
  - Verified: the overlay composites correctly over the D3D12 scene through the new path (crisp DirectWrite text, correct alpha, geometry visible underneath); the default overlay-off path is pixel-identical (0 differing pixels of 1,728,992); `Tests.Unit` 66/66 and `Tests.ImportIsolation` 132/132 in both configurations.
- **The output shared section is sized from real Tier A budgets instead of the synthetic fixture's** (implemented, built and tested; not committed as of this note). The first of the two structural blockers named in the Gate 3 scorecard below, and deliberately the cheapest: every perf fixture is measured through this window, so nothing else in Gate 3 is claimable until it is right.
  - **What was wrong.** `SharedSection.h`'s `kSyntheticSectionBytes` is 1 MiB, chosen for the ~1.5 KiB synthetic cube+point-cluster payload and carrying its own "revisit against real budgets in Gate 3+" note. The product path used it verbatim (`D3D12ImportBridge.cpp:193`) alongside a 64-chunk cap. A-small is ~100k triangles / ~4.4 MiB normalized, so **the section, not the parser, is what every perf run would have measured** — and it would have failed at the *smallest* fixture in the corpus, not the largest.
  - **Derived, not picked.** `03-file-formats-and-ingestion.md:140` caps one normalized detail chunk at 16 MiB of GPU payload. A window that cannot hold one maximum-size chunk can never make forward progress at all, so the new `kImportSectionBytes` is 4 × that = 64 MiB — far below both the 1 GiB Tier A parser/normalizer live-scratch budget (`03-...:171`) and the 4 GiB Job Object commit ceiling, so the window is never the binding constraint on either side. `kImportMaxChunkCount` moved 64 → 1024: at 92 bytes per descriptor that table is 92 KiB, 0.14% of the window, which puts the byte budget back in front of the count. The old 64 would have bound long before the bytes did — A-medium's 2,000 nodes alone can exceed it once primitives are split "while preserving material and instance identity".
  - **Not a protocol break**, so the Gate 2 freeze at `10-...:126` holds: `sectionByteCapacity`/`maxChunkCount` were already per-request wire fields, and `kSyntheticSectionBytes` stays exactly what its name says for the ~20 synthetic-generator and hostile-worker call sites that mean it.
  - **This does not make A-medium work.** It is still one terminal write per generation; A-medium and larger exceed 64 MiB and need the non-terminal progressive delivery this window is *sized for* but does not implement. Until that lands, an oversize model fails cleanly with `ResourceLimit` through the `sectionLength > destination.size()` guard every adapter already has (`GltfAdapter.cpp:1036`, `StlAdapter.cpp:154`). Stated here so nobody reads "the section blocker is closed" as "the perf corpus runs now."
  - `ImportSessionTests.cpp` now builds its requests from the product constants rather than a convenient small pair — that file exists to exercise the shipping configuration, and the section window is part of it.
  - Proven via 3 new `[import-session]` tests: the window holds at least one maximum detail chunk plus header and descriptor while staying under both budgets (`STATIC_REQUIRE`, so a future edit that breaks the relationship fails to compile rather than fails at runtime); a differential pair where the same file imports at the real window and is rejected as `ResourceLimit` under a deliberately tiny one, **with a third import afterwards proving the path is still usable** per `03-...:176`'s "the process remains usable"; and the chunk-count cap re-proven at its new value. **One assertion was wrong on the first write and the code was right**: the count test expected `ImportStage::ValidateSection`, but the adapters enforce `maxChunkCount` themselves (`GltfAdapter.cpp:617`, `StlAdapter.cpp:340`, `PlyAdapter.cpp:437`), so an *honest* worker stops before it writes and the failure is `WorkerReportedError`. The host-side validator cap (`SharedSectionValidator.cpp:102`) is the one that matters against a *lying* worker, and a cooperating adapter can never exercise it — that stays `HostileWorkerTests.cpp`'s job. Corrected the assertion rather than loosening it.
  - **Verified**: `Tests.Unit` 66/66 and `Tests.ImportIsolation` 135/135 (1629 assertions, up from 132/1614) in both Debug and Release, first-after-build run discarded, 3 repeats each, no flakiness. Pixel-identical across `tri_tight.glb`, `tri_external.gltf`, `basisu_textured_triangle.glb` and `draco_position_only.glb` — 0 differing pixels of 1,728,992 each, byte-compared against pre-change captures taken from a `git stash`ed baseline build, with non-background pixel counts (98,456 / 73,041) recorded alongside so a blank frame could not pass trivially. No leftover `Preview3D.exe`/`Preview3DImportWorker.exe` after any capture.
  - **A new environmental gotcha, distinct from the two AppLocal ones already documented above.** The `git stash` → build baseline → `git stash pop` → rebuild cycle used to take those before/after captures left MSVC's incremental link-time-codegen state inconsistent, and the next Release build died with `fatal error C1001: Internal compiler error` in `ControlChannelIo.cpp` plus `LNK1000: Internal error during IMAGE::BuildImage`. The baseline build one step earlier had already warned `LNK4209: debugging information corrupt; recompile module` on an unrelated `.obj`, which is the tell. A `/t:Clean` followed by a full Release rebuild succeeded immediately ("Previous IPDB not found, fall back to full compilation"), confirming corrupted incremental state rather than a code defect — verified by rebuilding, not assumed. If you use the stash-and-rebuild technique for a baseline comparison, clean the configuration afterwards before trusting an incremental build of it.
- **The product now uploads on a copy queue through `D3D12UploadRing` and publishes through `SceneSnapshot`** (implemented, built and tested; not committed as of this note). Gate 2 work, not Gate 3: it closes an exit criterion the shipping path was **contradicting**, not merely failing to demonstrate.
  - **What was actually wrong.** `D3D12UploadRing`, `DxgiBudgetMonitor`, `SceneSnapshot` and `DerivedCache` were built and unit-tested in Gate 2 workstream B and then **never added to `Preview3D.vcxproj`** — the linker proved it the moment the first call was written. The product instead used `UploadOneBuffer`/`UploadOneTexture`, which submitted staging copies to the **direct queue** and blocked on them. `10-...:121` requires "direct queue records no ordinary load-time wait on the copy fence"; there was no copy queue in the product at all, and load-time GPU work serialized the direct queue. This is worth stating plainly because the gate was previously described here as merely undemonstrated.
  - **`D3D12UploadRing` grew a texture path**, so geometry and textures share one lane, one fence and one publication rather than racing on two. Needed two additions: ring-offset alignment (`D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`, 512 — `CopyTextureRegion`'s placed footprint requires it, and the old allocator only ever handed out packed offsets), and the row repitching to `RowPitch` that used to live in the viewer path. With `alignment == 1` the allocator reduces exactly to its previous tail-room arithmetic, so buffer uploads are unchanged.
  - **The unverified question is now settled.** This file's own upload-ring risk table recorded implicit `COMMON`-state promotion as confirmed for buffers on the copy queue but "not yet exercised for textures". It works: a DEFAULT-heap Texture2D created in `COMMON` promotes to `COPY_DEST` for the copy and decays back, so the direct queue can promote it to `PIXEL_SHADER_RESOURCE` with no barrier — which matters because **a copy queue cannot record one**, so the old `COPY_DEST` → `PIXEL_SHADER_RESOURCE` barrier had no equivalent on the new lane. Textures are therefore created in `COMMON` now, not `COPY_DEST`. Proven, not assumed: a test counts `ID3D12InfoQueue` ERROR/CORRUPTION messages across a mixed batch of 4 buffers and 4 textures and asserts the count does not move, with the info queue confirmed available on the Debug run rather than `WARN`ing out.
  - **Uploads are asynchronous now, and the previously loaded model keeps drawing while a new one lands.** `UploadModel` became `BeginUploadModel` (create destinations, queue, flush, return) plus `PollUploads` (drain publications; when every resource of the current generation is fence-complete, swap the staged set in). That is what `04-...`'s "the direct queue ... keeps drawing the previous proxy/LOD" describes and what Gate 2's forced-copy-delay criterion is written against — the old code called `ClearModel()` first, so a slow copy would have blanked the viewport. `RenderThread` holds its `kRenderUploadCompleteMessage` until the swap instead of posting at queue time; posting earlier would tell the UI a model was displayed while its vertex buffers were still being written.
  - **A use-after-free found by reasoning about the ownership model, not by a failing test** — the same class of bug this file already records for `D3D12UploadRing::Grow()`, and it did not reproduce in any run. Superseding an in-flight upload replaced `pendingModel`, releasing destination resources the copy queue still had recorded, not-yet-executed `CopyBufferRegion`/`CopyTextureRegion` calls into; D3D12 command lists do not keep referenced resources alive. Every early return in `BeginUploadModel` had the same hole. Fixed by retiring resources on **two** fence timelines rather than one: `directFenceValue` for a displaced drawable model (frames may still reference it) and `copyFenceValue` for a superseded in-flight one (copies may still be writing to it), released only when both have completed. `RetireStagedResources` flushes before capturing the fence value, because a value captured before submission does not cover the batch — exactly the trap `Grow()` documents.
  - **Verified**: `Tests.Unit` 71/71 (up from 66; 5559 assertions Debug / 5473 Release, the difference being the debug-layer cases that `WARN` out where the info queue does not exist) and `Tests.ImportIsolation` 135/135 (1629 assertions, **unchanged to the assertion** — the intended signal for a chunk that touches no import code), both configurations, first-after-build run discarded, 3 repeats, no flakiness. Five new `[graphics]` cases: byte-exact texture round-trip through the ring; a 3x4-byte row pitch against the 256-byte staging pitch (the case a bulk `memcpy` gets silently wrong); a texture upload following a deliberately 37-byte buffer upload, proving the alignment logic; the debug-layer count above; and **the one this chunk exists for** — a full model's worth of uploads while a direct queue's completed fence value is asserted unmoved. That last is deterministic by construction: a D3D12 fence only advances on an explicit `Signal`, so an unmoved direct queue provably had nothing submitted and waited on nothing. It also asserts the ring's queue is genuinely `D3D12_COMMAND_LIST_TYPE_COPY`, or "off the direct queue" would hold only by accident.
  - Pixel-identical across `tri_tight.glb`, `tri_external.gltf`, `basisu_textured_triangle.glb` and `draco_position_only.glb` — 0 differing pixels of 1,728,992 each, byte-compared, non-background counts (98,456 / 73,041) recorded so a blank frame could not pass. The textured fixture matching is the real evidence that repitching and the copy-queue promotion produce identical pixels. A 60-step resize storm, `WM_CLOSE` at three points during import, and normal close all exit cleanly with 0 leftover `Preview3D.exe`/`Preview3DImportWorker.exe`.
  - **Honest gaps.** (a) The **displaced-model retire path is not exercised end-to-end in the app**: a single-file launch has no previous model at the first swap, and there is no automatable second-open path (drag-drop needs a real `HDROP`, the dialog needs a person, and the singleton/pipe activation of `06-...` is Gate 5 and unbuilt). It is covered by construction and by the snapshot semantics in `UploadRingTests.cpp`, not by a real reopen — worth closing when a second-open path exists. (b) **The load-time win is unmeasured**: removing a blocking wait should shorten time-to-first-frame, but there is no load-time harness yet (that is the perf-harness chunk), so no number is claimed. (c) Steady-state frame cost is unchanged as expected — two full bench runs (~1,800 frames) gave 6.945 ms mean / 7.03-7.05 ms p95 against the 143 Hz refresh, matching ADR-010's recorded overlay-off baseline of 6.96/7.08; two further runs terminated at 30 frames because the polling harness mistook the stats publish interval for completion, and are not measurements.
  - Still not wired, and deliberately out of scope here: `DxgiBudgetMonitor` and detail eviction (needs the Pressure fixture to demonstrate anything), `DerivedCache`, non-terminal progressive delivery, and point-cloud rendering.
- **Non-terminal progressive delivery: the output window may now be handed over more than once per generation** (implemented, built and tested; not committed as of this note). This is the second of the Gate 3 scorecard's two structural blockers, and the one that had not moved. Before it, `ChunksReady` was a single terminal reply, so a model larger than the 64 MiB window could not cross at all — every A-medium-and-larger fixture failed with `ResourceLimit` before a parser's quality mattered even slightly.
  - **Protocol, strictly additive.** `ChunkBatchReady = 12` (worker→host, non-terminal, carries `{generationId, batchIndex, chunkCount, sectionBytesWritten}`) and `ChunkBatchConsumed = 13` (host→worker). Every existing opcode and struct is byte-for-byte untouched, and **`ChunksReady` keeps its exact meaning** — "the section holds a batch, validate it" — so it is the *terminal* batch rather than a separate end-of-stream message. A model that fits one window therefore sends no `ChunkBatchReady` at all and its traffic is identical to before this existed, which is what let all 135 pre-existing cases pass unmodified. `RequestSidecarFile`/`SidecarFileReady` was the precedent for a mid-generation round trip; this follows its shape and its strictly-synchronous one-reply-per-request discipline.
  - **The ack is flow control, not politeness.** The window is *reused*, so without `ChunkBatchConsumed` a worker would overwrite bytes the host is still copying out. `ChunkBatchSink` (`import-worker/src/ChunkBatchSink.{h,cpp}`, deliberately shaped like `SidecarFileClient`) blocks on it before touching the window again.
  - **A design fork resolved against the plan, on evidence found while implementing it.** The plan called for *self-contained* batches — each batch re-sending the Material/Image chunks its meshes reference — because that leaves `ValidateAndCopySection` untouched. Materials are a few hundred bytes and free to duplicate; **images are not**. `03-...:140` caps a chunk at 16 MiB and a 2048² RGBA8 texture hits exactly that, so a texture shared by meshes spread over ten batches would cross ten times and become ten GPU copies — on A-medium (350 MiB, 2k nodes, *textures*), which is the case this work exists to unblock. Chosen instead: a **per-generation catalog**. `ValidateAndCopySection` gained an optional `KnownChunkCatalog*` of `chunkId -> topology` for chunks accepted in *earlier* batches; a dependency id now resolves against this section's own table **or** that catalog. This widens what an id may point at without weakening a single check — every catalog entry already passed this same validator in full and was copied into host-owned memory — and the id-uniqueness rule got *stronger*, now spanning the whole generation rather than one section. Cycles stay impossible for the same structural reason as before: a chunk can only name an id that already exists, so cross-batch references point strictly backwards in batch order. `nullptr` (a single-window import) reproduces the old behavior exactly.
  - **Two new caps, because two different things needed bounding.** `maxChunkBatchesPerGeneration` (default **1**, i.e. single-window imports only — exactly the old behavior for any caller that changes nothing) stops a worker emitting batches forever; `maxChunksPerGeneration` bounds the catalog, which `maxChunkCount` could not because that caps one *section*. The product sets 256 batches (`D3D12ImportBridge.cpp`), derived rather than picked: `03-...:162` caps a Tier A source at 8 GiB and normalized output is smaller than its source for every format here, so 8 GiB / 64 MiB = 128 windows already covers the largest admissible file; doubled for headroom.
  - **`GltfAdapter` emits batches**, via a `PlannedChunk` list (descriptor plus spans into the parse's own buffers — planning a large model's batches costs descriptors, never a second copy of its geometry) that `CountChunksThatFit` greedily packs into windows. Chunk ids stay exactly what they always were, so a mesh in a later batch keeps pointing at a material and texture that already crossed. **Emission order is decided by whether batching is actually needed, not by whether a sink was offered** — one window keeps the historical meshes→materials→images order so the product's own output stays byte-identical, and only a genuinely multi-batch model reverses to images→materials→meshes, which is what makes cross-batch references resolvable at all (a reference may only name a chunk that has *already* crossed) and is also the order the design wants for display.
  - **Seven new hostile-worker attack modes**, because Gate 3's "re-run the Gate 2 hostile-worker suite" criterion could not possibly cover an attack surface that did not exist when those four were written. `--batches-{honest,replay-index,skip-index,reuse-chunk-id,unbounded,write-before-ack,after-terminal}`, driven through `RunImportSession` itself rather than a hand-rolled channel, since the rules under attack live in that function's own reply loop. This needed one test seam, `ImportSessionRequest::workerArgumentsOverride` (same justification as the existing injectable `commitLimitBytes`): the loop is only reachable through `RunImportSession`, so proving it rejects a misbehaving worker means launching one *through* it.
  - **A real finding, from a test that failed and was right to.** `--batches-write-before-ack` returned the *later* content, not the announced one. The host copies the window when it reaches the batch, not at the instant the notice arrives, so a worker that rewrites before its ack can have its own later section accepted. Nothing invalid gets through — bounds, checksums, generation and ids all still hold, and a mutation landing mid-copy fails the checksum and is cleanly rejected — but **"what the host accepted" is not provably "what the worker announced"**. The test assertion was wrong, not the code, and it now pins the property that does hold. Binding the two would take a content checksum in the notice; that would catch an honest worker's bug and not a hostile one, which controls both halves anyway. Recorded rather than silently reframed, and the same race explains `--batches-after-terminal`.
  - **Verified**: `Tests.ImportIsolation` **150/150** (1697 assertions, up from 135/1629) and `Tests.Unit` **71/71** (5559 Debug / 5473 Release, **unchanged to the assertion** — the intended signal for a chunk that touches no graphics code), both configurations, first-after-build run discarded, 3 repeats each, no flakiness. The load-bearing case is a differential pair through the *real* sandboxed worker: `basisu_textured_triangle.glb` imported at the shipping 64 MiB window (1 batch) and again at a window sized from the fixture to hold one chunk and little else, asserting more than one batch, the same chunk count, and **byte-identical payloads compared by chunk id** — plus the same file at the same tiny window with batching not permitted, which still fails cleanly with `ResourceLimit`. Shrinking the window rather than growing the model reaches the same code path a 350 MiB file would without committing one, the same differential shape `ImportSessionTests.cpp` already used for the section-size regression. App-level: all four fixtures render under `--d3d12` with non-background pixel counts of **98,456 / 73,041**, matching the baselines recorded two entries above exactly, and no leftover `Preview3D.exe`/`Preview3DImportWorker.exe`.
  - **Honest gaps, stated up front.** (a) **No progressive *display* yet** — `ImportSessionRequest::onBatch` exists and is proven by a test, but `D3D12ImportBridge` does not supply one, so a batched import still accumulates host-side and uploads once. Getting the first geometry on screen before the last batch crosses is the next chunk; this one built the protocol and acceptance rules it needs. (b) **STL and PLY gain nothing yet**: both emit exactly *one* chunk for an entire file, so there is nothing for a batcher to split and A-large-stl/ply are still unreachable. That needs the cluster splitting in Gate 3 deliverable 5 (`03-...:140`'s 4–16 MiB / 262,144-triangle targets), which is also what a single glTF primitive bigger than the window needs. (c) **A-large is still not reachable for glTF either**: the worker normalizes the whole model in memory before emitting any batch, so the 1 GiB parser/normalizer scratch budget still bounds it — true streaming parse-then-emit-then-free is separate work. What this chunk closes is the *window*, not the *scratch budget*.
- **Not yet started**: swapping `Renderer.cpp` off D3D11 entirely, the 4 GiB-streaming and open/close/reopen Gate 2 integration exit criteria, porting the real chrome onto the (now built) D2D overlay bridge, device-loss recovery, `WorkerPool`-based reuse, point-cloud rendering, and the rest of Gate 3's list (the meshoptimizer LOD/proxy builder, verified bounds/double-origin normalization, stratified proxy sampling, the inbox WIC/DirectXTex/libwebp mip pipeline for non-basisu images, derived-cache production wiring, draw sorting/instancing, error/warning mapping). **Gate 2 is otherwise done** — see Status above. *(Correction: this bullet previously listed "the Direct2D-overlay-on-D3D12 decision (ADR-010)" as not started. That was already stale when written — ADR-010 is a Gate 1 decision that was taken, and `2cbd8da`/`47db471`/`e86ebc5` built the bridge. What remains is porting the real chrome onto it; the overlay currently draws only `--overlay-spike` stand-in primitives.)*

## Gate 3 scorecard

Measured against `.docs/design/10-delivery-plan.md` L134-144 directly, because the
"Gate 3 slice 1..5" numbering used throughout this file **was invented during
implementation and does not appear in the design docs** — Gate 3 there is a flat list of
11 deliverables, and only Gate 4 has real numbered slices. Reading the slice numbering as
progress toward the gate badly overstates where we are.

| # | Deliverable | State |
| --- | --- | --- |
| 1 | normalized scene/chunk/material/texture contracts | Done |
| 2 | fastgltf GLB/glTF 2.0 adapter + constrained sidecar resolver | Done for GLB and `.gltf`+sidecars; `EXT_meshopt_compression`/`KHR_mesh_quantization` still absent |
| 3 | binary/ASCII STL and PLY mesh/point-cloud adapters | Import done; PLY point clouds are never rendered |
| 4 | bounded Draco decode + KTX2/Basis transcode | Done |
| 5 | verified bounds, double-origin normalization, normal/tangent policy | Not started |
| 6 | meshoptimizer cluster LOD/proxy builder | Not started (dependency now pinned) |
| 7 | WIC/DirectXTex/libwebp codecs + mip pipeline | ~25% — WIC PNG/JPEG/BMP/TIFF only, no mips, no TGA/DDS/HDR/WebP |
| 8 | source-order-independent stratified first-proxy sampling | Not started |
| 9 | persistent derived-cache production path | Prototype exists but is compiled only into `Tests.Unit`, not the app |
| 10 | draw sorting/instancing + measured ExecuteIndirect threshold | Not started |
| 11 | error/warning mapping and format diagnostics | Not started — no warning channel exists at all |

None of the six exit criteria (L146-153) is currently demonstrable. Two structural blockers
sat in front of most of the rest, and neither is a format-adapter problem. **Both are now
closed** — which removes what was in the way, and leaves the deliverables themselves as the
work:

- ~~**The 1 MiB output section fails at A-*small*.**~~ **Closed** — see the Status entry
  above. The product path now uses `kImportSectionBytes` (64 MiB, derived as 4 × the 16 MiB
  maximum detail chunk in `03-...:140`) and `kImportMaxChunkCount` (1024). It was a call-site
  and contract change, not a protocol break, as predicted. **This unblocks A-small only**: the
  section is still written once per generation, so A-medium and larger remain blocked behind
  the second bullet below, and an oversize model fails cleanly with `ResourceLimit` until it
  lands.
- ~~**`ChunksReady` is a single terminal reply per generation.**~~ **Closed** — see the Status
  entry above. `ChunkBatchReady`/`ChunkBatchConsumed` let one generation fill and hand over the
  window repeatedly; the host resolves cross-batch dependencies through a per-generation
  `KnownChunkCatalog`, and the hostile-worker suite was *extended* with seven modes for the
  attack surface a re-written section creates, exactly as this bullet said it would have to be.
  **What that does and does not buy**: a glTF model with more chunks than fit one window now
  imports, proven byte-identical to its single-window self. It does **not** yet reach A-large.
  Three things still stand in the way, and none is the window: STL and PLY emit one chunk per
  *file*, so there is nothing to split until the cluster splitting in deliverable 5 lands; the
  worker normalizes the whole model in memory before emitting, so the 1 GiB scratch budget
  binds; and "first useful partial proxy ≤2 s" needs deliverables 6 and 8 to have a proxy to
  send first. Progressive *delivery* is a prerequisite for those, not a substitute.

There is also **no perf or memory harness anywhere** — no `BENCHMARK`, no A-small/medium/large
fixtures, no private-committed-bytes assertions — so exit criteria 1 and 4 have no
infrastructure at all, and the A-large corpus is far too large to commit (generate it).

## Flagged risks and how they resolved

Each launch-spike and data-path plan flagged genuine Win32/MSBuild unknowns rather than asserting them confidently. Recorded here so the pattern (flag → verify empirically → note the outcome) stays visible, and so nobody re-derives an already-answered question.

### Launch spike (part 1)

| Risk flagged before implementation | Outcome |
| --- | --- |
| AppContainer processes need an explicit ACE (`S-1-15-2-1`/`S-1-15-2-2`) on their own build output directory to even load their `.exe`; a normal dev/CI output folder doesn't have one by default | **Confirmed necessary.** Without it, every launch fails at the loader level with `ERROR_ACCESS_DENIED`. Handled via an `icacls` grant in test fixture setup (`GrantAppContainerAccessToWorkerDirectory` in `tests/import-isolation/SandboxTestSupport.h`), not a manual machine-setup step. |
| Whether `CreateAppContainerProfile` needs elevation on a normal dev machine | **Confirmed: no.** Works unelevated. |
| The correct deallocator for the `PSID` returned by `CreateAppContainerProfile` (documented as `FreeSid`, but several similar Win32 SID APIs use different deallocators) | **Confirmed: `FreeSid` is correct.** |
| Whether `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` requires every `STARTUPINFO` standard handle (e.g. `hStdOutput`) to also appear in the handle list, or `CreateProcessW` fails | **Confirmed: yes, `ERROR_INVALID_PARAMETER` otherwise.** Turned into a deliberate test (`SandboxLaunchTests.cpp`, "Omitting the report handle from the restricted handle list fails process creation") rather than just a note. |
| Exact error code when the Job Object commit limit trips | **Confirmed: `ERROR_COMMITMENT_LIMIT`.** Asserted directly in the `--overallocate` test. |
| Exact error code when the Job Object active-process limit blocks a spawn | Left as informational/logged only, not hard-asserted — the test only asserts the spawn was denied, not the specific `GetLastError()` value. |
| Whether `Preview3D.slnx` honors `.vcxproj`-level `ProjectReference` build ordering the same way classic `.sln` does | **Confirmed: yes**, `Tests.ImportIsolation`'s `ProjectReference` to `Preview3DImportWorker.vcxproj` (with `LinkLibraryDependencies=false`, exe-to-exe, nothing to link) correctly orders the build. |

Two **unflagged** assumptions turned out wrong and were caught by the build itself, not foreseen in either plan:

- **`$(SolutionDir)` only resolves correctly when building via `Preview3D.slnx` itself** — a standalone `msbuild Tests.ImportIsolation.vcxproj` invocation falls back to the project's own directory, producing a nonsensical path (`tests\import-isolation\import-worker\x64\Debug\...`). Always build via the solution; remember the target-name-underscore gotcha too (`/t:Tests_ImportIsolation`, not the literal project-file name with a dot).
- **Every project in this solution shares one centralized output directory**, `$(SolutionDir)$(Platform)\$(Configuration)\` — there is no per-project `import-worker\x64\Debug\` subdirectory, contrary to the original plan's assumption (based on a standalone-build observation that turned out to be an artifact of not building via the solution, per the point above). The `PREVIEW3D_IMPORT_WORKER_EXE` macro in `Tests.ImportIsolation.vcxproj` reflects the corrected path.
- **winsock1/winsock2 header-order conflict**: `SandboxLauncher.h` pulls in `<windows.h>` without `WIN32_LEAN_AND_MEAN`, which drags in the legacy `winsock.h`. Any test file that also needs `<winsock2.h>` must include `<winsock2.h>`/`<ws2tcpip.h>` (with `WIN32_LEAN_AND_MEAN` defined) *before* any project header that transitively pulls `<windows.h>`.

### Data path (part 2)

| Risk flagged before implementation | Outcome |
| --- | --- |
| Whether a shared memory section's security descriptor needs an explicit ACE for the AppContainer SID, or whether inheriting an already-open handle bypasses that need (access checks happen at handle open/duplicate time, not per-use) | **Confirmed: no explicit ACE needed.** Same mechanism as the already-proven pipe-inheritance case generalizes cleanly to a file-mapping handle — `SharedSection.h`'s `CreateSharedSection` uses a null security descriptor and it works against the real AppContainer token on the first real run. |
| Whether a `HANDLE`'s numeric value survives `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`-restricted inheritance for a *non-standard* handle role (the section handle, delivered inside a control message payload, not via `hStdOutput`/`hStdInput`) | **Confirmed: yes**, worked on the first real launch (`ImportPipelineTests.cpp` test 6). |

One arithmetic mistake, caught immediately by the compiler rather than needing empirical verification: `StartGenerationRequest`'s `static_assert` claimed 32 bytes; the packed fields actually sum to 40. The `#pragma pack(1)` + `static_assert` pattern used throughout the wire format did exactly its job — a layout mistake became a compile error instead of a silent wire-format bug.

### Binary STL parser (Gate 3 slice 2)

One genuinely new, non-obvious MSBuild/vcpkg finding, discovered while getting the (pre-existing, unrelated-to-STL) `Tests.ImportIsolation` suite running again after a normal rebuild:

- **Building `Preview3DImportWorker` incrementally via the solution can silently skip vcpkg's AppLocalDeps DLL-deployment step, leaving `fastgltf.dll`/`simdjson.dll` missing from the shared output directory even though the `.exe` itself builds and links "successfully."** Root cause: vcpkg's `AppLocalFromInstalled` MSBuild target (`vcpkg.targets`) is conditioned on `'@(Link)' != ''` — it only runs when the project's own `Link` step actually executed *this invocation*. If `Preview3DImportWorker.vcxproj`'s incremental-build check decides nothing changed (e.g. only a downstream project like `Tests.ImportIsolation` was targeted, or only a `/p:` property changed with no source/link-input delta), `Link` is skipped, and the DLL copy silently never runs — even into a completely empty output directory. Every real-worker-launching test then fails at `ReadControlMessage`/`WaitForSingleObject` with no obvious error (the worker process exits near-instantly with `STATUS_DLL_NOT_FOUND`, invisible unless the `.exe` is also run standalone: `Preview3DImportWorker.exe --child-noop` surfaced the actual `error while loading shared libraries: fastgltf.dll`). **Fix that reliably works**: explicitly force-rebuild the worker project itself via the solution — `msbuild Preview3D.slnx /t:Preview3DImportWorker:Rebuild /p:VcpkgEnableManifest=true ...` — not just the test project that references it; this guarantees `Link` actually re-executes and the AppLocal copy target fires. A plain `msbuild Preview3D.slnx /t:Tests_ImportIsolation ...` (no `Rebuild`, or `Rebuild` only on the top-level target) is **not sufficient** even when nothing looks stale — worth remembering for whoever next does a fresh rebuild-and-test pass after time away from this repo, same spirit as the `$(SolutionDir)`/target-name gotchas already recorded above. Not a code defect in this repo's own projects; not specific to STL (would affect *any* rebuild-and-test pass touching the glTF path too) — flagged here because this is where it was actually hit and diagnosed.
- **Risk flagged before implementation, not yet independently confirmed**: whether the degenerate/non-finite-facet-dropping epsilon thresholds (`flatLengthSquared <= 1e-12f` for degenerate-area rejection; supplied-normal-trust window `(0.81, 1.21)` length-squared, i.e. roughly ±10% of unit length) behave sensibly against real generated geometry (e.g. STL export from actual CAD/mesh tools, which may emit normals that are exactly zero, deliberately non-unit, or very-near-but-not-exactly the flat normal due to smoothing) rather than only the hand-picked synthetic facets in `StlImportTests.cpp`. All 9 new tests pass against deliberately-constructed inputs; no real-world STL fixture has been run through this adapter yet.

### Hostile-worker suite (part 3)

**A real TOCTOU (time-of-check-to-time-of-use) defect was found and fixed in `SharedSectionValidator::ValidateAndCopySection`, found by threat-modeling for this chunk rather than by a failing test.** The validator was reading the live, adversary-writable shared section multiple separate times — once for the section checksum, once per chunk for the descriptor, once per chunk for the chunk checksum, once per chunk for the final payload copy — instead of once. Fixed by bulk-copying `[0, header.sectionLength)` into a private local buffer in one read, immediately after header validation succeeds, and rebinding every subsequent read to that private copy. This structurally eliminates the race rather than relying on timing to avoid it; there's no meaningful *internal* race left to reproduce via a test after the fix. No behavior change for well-formed input, confirmed by the full pre-existing suite passing unmodified both before and after.

| Risk flagged before implementation | Outcome |
| --- | --- |
| Whether `IsHostileTestBinary=true` genuinely suppresses `TreatWarningAsError` for only the hostile-worker project, given `Directory.Build.props`'s condition is evaluated at the `Microsoft.Cpp.Default.props` import position | **Confirmed: yes.** Inspecting the actual `cl.exe` command lines with `/v:normal` showed `/WX-` for `Preview3DHostileWorker.vcxproj` and `/WX` for every other project, as long as `IsHostileTestBinary` is set in the same `PropertyGroup Label="Globals"` block that appears before that import (matching every existing `.vcxproj`'s structure). |

The "mutate shared-section bytes after the host's first read" test deliberately avoids a literal race: the hostile worker corrupts the section on the same thread, after a fixed 300ms delay following `ChunksReady` (2+ orders of magnitude more than the low-single-digit-millisecond cost of validating the ~1KB fixture), and the test relies on `WaitForSingleObject` returning `WAIT_OBJECT_0` as a genuine Win32 happens-before guarantee that the corrupting write (same thread, before `main()` returns) has already occurred — not on timing luck. A detached background thread was deliberately avoided: its completion wouldn't be ordered against process exit, which would reintroduce a real race and require trusting an unverified assumption about thread creation under a zero-capability AppContainer token.

The Job Object overrun fault mode (the 4th of the 4 named in `.docs/design/10-delivery-plan.md`'s Gate 2 deliverables) was **not** reimplemented in the hostile-worker binary — it's already proven by part 1's `--overallocate`/`--hang` tests in `SandboxLaunchTests.cpp` against the identical sandboxed launch mechanism. Gate 2's exit criteria is satisfied by the union of part 1 + part 3, not duplicated.

### Real fastgltf integration (Gate 3 slice 1)

This is the first real, previously-unvetted third-party parser wired into the sandboxed worker. Before writing any adapter code, the actual installed fastgltf 0.9.0 headers (`vcpkg_installed/x64-windows/x64-windows/include/fastgltf/*.hpp`) were read directly rather than trusting fetched docs or training knowledge — this caught real discrepancies:

| Assumption/risk before reading the source | Outcome |
| --- | --- |
| vcpkg baseline resolves fastgltf to some recent version | **Confirmed exactly 0.9.0**, by fetching the pinned baseline commit's port manifest directly. Manifest-mode auto-wires fastgltf + transitive `simdjson` with zero manual `AdditionalIncludeDirectories`/`AdditionalDependencies` — same as `catch2` already worked. |
| `fastgltf::Options::LoadGLBBuffers` should be passed alongside `GenerateMeshIndices` | **Wrong — it's deprecated in 0.9.0** ("now default behaviour"). Passing it would have turned a deprecation warning into a build failure under this repo's `TreatWarningAsError`. Caught by reading `core.hpp` before writing the call, not by a failed build. |
| `iterateAccessor`/`copyFromAccessor`'s runtime assert covers enough type safety | **Confirmed real but narrower than assumed**: it asserts only `accessor.type` (the vector shape), never `accessor.componentType`, and compiles out entirely under `NDEBUG` (Release). The adapter manually validates both fields before every accessor read — this is load-bearing, not defensive-for-its-own-sake, since Release is exactly the build where the library's own check disappears. Confirmed the full test suite passes in Release too. |
| `fquat::asMatrix()`'s return type/element access, and `translate()/rotate()/scale()`'s composition order, were unverified going in | **Both resolved by reading `math.hpp` directly, not by trial and error**: `asMatrix()` returns `mat<T,3,3>`; `translate(m,t)`/`rotate(m,rot)`/`scale(m,s)` all post-multiply the new transform onto `m` (e.g. `rotate` returns `m * asMatrix(rot)`), so calling them in T, R, S order starting from identity yields exactly `T*R*S` — the standard composition, used directly rather than hand-rolled. |
| `world = parentWorld * local` matrix order for node-transform baking | **Confirmed correct on the first real run** against `tri_transformed_node.glb` (a translated+rotated parent node with a mesh on its identity-transform child) — hand-computed expected positions matched to float precision (~1.2e-7 residual), including the Z-rotation-invariant generated normal staying `(0,0,1)`, which would have visibly broken under a translation-leaking or wrong-matrix bug. This was deliberately built as a same-chunk acceptance test specifically because fastgltf's column-major/column-vector convention (`v'=M·v`) is the mirror image of `interactive-viewer/src/render/Model.cpp`'s row-major/row-vector `XMMATRIX` (`local * parent`) — a natural copy-paste-from-Model.cpp mistake would have gotten the multiplication order backwards. |

All of the above were resolved by reading the actual installed headers before writing code, not by writing code and iterating on build/test failures — the adapter (`import-worker/src/GltfAdapter.cpp`) built and passed all 6 new tests, including the transform-baking one, on the first real attempt.

The control protocol gained a new opcode (`ControlOpcode::StartGltfImport`) and request struct (`ParseGltfRequest`, 48 bytes) additive to the existing `StartGenerationRequest`/`ChunksReadyNotice`/`GenerationErrorNotice` — the synthetic generator's path is untouched. `ParseGltfRequest` carries handles to *two* shared sections (input GLB bytes, output chunks); since `GenerationLaunchSupport.h`'s `LaunchWorkerWithControlChannel` only takes one section handle, `GltfImportTests.cpp` calls `import_broker::LaunchSuspendedSandboxed` directly rather than modifying that shared helper — a deliberate, small amount of duplicated pipe-setup code traded for zero risk to the other test suites.

The two new GLB fixtures (`tri_transformed_node.glb`, `unsupported_extension.glb`) are generated the same way as the original two — `interactive-viewer/tools/gen-test-glbs.cpp` via `build-gen-glbs.ps1` — which needed a one-line include-path fix (`framework.h` wasn't found; the script's `$repoRoot` variable is actually `interactive-viewer/`, not the git repo root, despite the name) to run at all. Confirmed the two pre-existing fixtures regenerate byte-identical (same 496/636-byte sizes) before trusting the fix.

### Building a `.vcxproj` directly makes 85 of 135 import-isolation tests fail

A third environmental trap in the same family as the two vcpkg/AppLocal ones, hit while adding
progressive delivery and worth an entry because the failure looks exactly like a real
regression and is not one.

`msbuild tests\import-isolation\Tests.ImportIsolation.vcxproj` builds and links fine, and then
**85 of 135 tests fail** — every test that launches a real worker, including
`SandboxLaunchTests` and `SourceFileAccessTests`, which touch no import code at all. The
symptom is `LaunchSuspendedSandboxed` returning `nullopt`, so it reads as a broken sandbox.

It is a path problem. `Tests.ImportIsolation.vcxproj:51` defines
`PREVIEW3D_IMPORT_WORKER_EXE=LR"($(SolutionDir)$(Platform)\$(Configuration)\Preview3DImportWorker.exe)"`,
and **`$(SolutionDir)` is only defined when MSBuild is invoked on the solution**. Building the
project directly leaves it empty (MSBuild falls back to the project directory), so the test
binary is compiled with a worker path that does not exist, and the worker directory the
AppContainer ACE is granted on is the wrong one too. Both output trees exist side by side on
disk — `x64\Debug\` from a solution build and `tests\import-isolation\x64\Debug\` from a
project build — which makes it easy to check the wrong one and conclude the DLLs are fine.

**Always build `Preview3D.slnx`, not an individual `.vcxproj`**, and run the binaries out of
`x64\<Config>\`, not the per-project output directory. A baseline comparison built the same
wrong way will "confirm" the failure is pre-existing, which is exactly what happened here
before the paths were checked.

### Release vcpkg DLLs in `x64\Debug` — the "fastgltf is broken" false alarm

Symptom, and it is worth recording verbatim because it cost a full session and pointed squarely at the wrong component: every `[gltf-import]` test failed, with `fastgltf::Parser::loadGltfBinary` *reporting success* but returning an `Asset` with `scenes`/`nodes`/`meshes` all `0` (should have been 1/1/1) and a garbage vector size in one field, then a `0xC0000005` moments later on teardown. A minimal in-process repro — no sandbox, no pipes, no worker code — reproduced it on two different tiny `.glb` fixtures. `DracoDecodeAdapterTests` produced a second heap-corruption crash, and wide test-tag runs were being silently truncated.

None of that was a fastgltf, simdjson, toolset or ABI defect. **`x64\Debug` contained the *release* `fastgltf.dll`, `simdjson.dll`, `draco.dll`, `ktx.dll` and `zstd.dll`.** `x64-windows` is a dynamic triplet, so those are `/MD` (`_ITERATOR_DEBUG_LEVEL=0`) while every Debug `.exe` is `/MDd` (`_ITERATOR_DEBUG_LEVEL=2`). MSVC's `std::vector` carries an extra `_Container_proxy` pointer under `IDL != 0` — 32 bytes in the `.exe`'s view, 24 in the DLL's. `fastgltf::Asset` has ~16 `std::vector` members, so the `.exe` read every one of them at the wrong offset and then destroyed them through garbage pointers. Confirmed with `dumpbin /dependents`: the worker imported `MSVCP140D.dll`/`ucrtbased.dll`, the DLLs next to it imported `MSVCP140.dll`.

Two things made this vastly more expensive to find than it should have been, both worth knowing:

- **It is invisible to source-level bisection.** Reverting the adapters to the last commit, reverting the `.vcxproj`, removing `CoInitializeEx`, disabling AppContainer sandboxing entirely, and disabling Segment Heap all changed nothing, because none of them touch the deployed DLL. That correctly rules out our code and *incorrectly* implicates the library.
- **It is sticky.** vcpkg's `applocal.ps1` only overwrites a deployed DLL when the source is *newer* than what is already in `$(OutDir)`:

  ```powershell
  if ($sourceModTime -gt $destModTime) { Copy-Item ... }
  ```

  So one wrong-configuration DLL with a recent timestamp — a manual copy while debugging, a tool script pulling from `<triplet>\bin` instead of `<triplet>\debug\bin`, an interrupted build — is never corrected, and every subsequent build silently keeps it. This is aggravated by all nine projects sharing one output directory (`$(SolutionDir)x64\$(Configuration)`), so any one of them can poison the rest.

Fix: `Preview3DResyncVcpkgAppLocalDlls` in `Directory.Build.targets` re-syncs the app-local DLLs from the bin directory matching the configuration being built, after vcpkg's own `AppLocalFromInstalled`. It only refreshes names already present in `$(OutDir)` (it never drags the whole vcpkg bin directory in), uses `SkipUnchangedFiles` so a healthy tree costs one size/timestamp comparison and no I/O, and is gated on the same `_ZVcpkgClassicOrManifest`/`VcpkgApplocalDeps` conditions as vcpkg's step — necessary because projects that leave `VcpkgEnableManifest` unset still get a `_ZVcpkgCurrentInstalledDir`, pointing at the classic install root inside the Visual Studio installation. Verified by poisoning `x64\Debug` with release DLLs and confirming the next build restores them, that a healthy tree logs nothing, and that Release still deploys release DLLs. The target is deliberately silent: MSBuild's `Copy` reports skipped files in `CopiedFiles` alongside real ones, so a "replaced a stale DLL" message driven off that output fires on every build.

Post-fix, with no source changes at all: `[gltf-import]` 12/12 cases (286 assertions), full `Tests.ImportIsolation` 116/116 (1475 assertions, no truncation, no Draco crash), `Tests.Unit` 51/51 (5082 assertions). The glTF import path had been correct the whole time.

Worth carrying forward: **when a third-party library appears to return structurally impossible data on Windows, check `dumpbin /dependents` for a `MSVCP140.dll` vs `MSVCP140D.dll` split before suspecting the library.** The tool scripts under `interactive-viewer/tools/` still copy from `<triplet>\bin` (release) by design, since they build standalone fixture generators with `cl.exe` outside MSBuild — they are not covered by the new target, so they must never be pointed at `$(OutDir)`.

Cleaned up in the same pass, because build-log noise is what let the above hide: five projects (`Preview3D`, `Preview3DThumbnailProvider`, `Preview3DImportHost`, `ModelCore`, `Preview3DHostileWorker`) left `VcpkgEnableManifest` unset, so every build printed "The vcpkg manifest was disabled, but we found a manifest file in ..." once per project. None of them consumes a vcpkg dependency — `interactive-viewer`'s only vcpkg-using sources are `tools/gen-test-glbs-{draco,ktx2}.cpp`, which the `.ps1` scripts build standalone with `cl.exe` and which are not in the `.vcxproj`. `Directory.Build.props` now turns vcpkg off explicitly for anything that has not opted in via `VcpkgEnableManifest`. This changes no build inputs (those projects already resolved `_ZVcpkgClassicOrManifest` to false, so they already got no include directories, no autolink and no app-local deployment); enabling manifests instead would *not* have been equivalent, since vcpkg autolink appends the whole triplet's `lib\*.lib` — `basisu_encoder.lib` alone is 52 MB — to the link line. Verified by full `Rebuild` of both configurations: zero warnings, all eight projects link, and both suites pass in Debug and Release.

### Headless D3D12 device/queue/fence foundation (Gate 2 workstream B, slice 1)

First graphics-domain code in the repo, after five sessions of sandboxing/parsing work. Both risks flagged before implementation resolved cleanly, and everything — device creation against real hardware, WARP opt-in, fence signal/poll/event-wait, cross-thread unblock — passed on the first real run in both Debug and Release, no iteration needed.

| Risk flagged before implementation | Outcome |
| --- | --- |
| Whether the D3D12/DXGI debug layer (`D3D12GetDebugInterface`/`DXGI_CREATE_FACTORY_DEBUG`) would be available on this dev machine (requires the "Graphics Tools" optional Windows feature; documented to fail with `DXGI_ERROR_SDK_COMPONENT_MISSING` if absent) | Designed as non-fatal either way (`TryEnableDebugLayerIfDeveloperBuild` falls through cleanly on failure, `debugLayerEnabled` is informational-only in tests, never hard-asserted) — this is the correct posture regardless of the answer on any one machine, so it wasn't worth pinning down further. |
| Whether `D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)` — the documented null-`ppDevice` support-probe pattern used to test each candidate adapter without constructing a throwaway device — actually behaves as documented | **Confirmed: yes.** Real hardware adapter selection succeeded and `Initialize()` returned a working, non-WARP device with a non-empty description on the very first run. |

Design note worth restating for whoever picks up the next slice: the new `D3D12Device`/`D3D12CommandQueue` files live under `interactive-viewer/src/graphics/` (the design's intended location) but are **compiled only into `Tests.Unit.vcxproj`** this slice — `Preview3D.vcxproj` was not touched (no new include paths, no new lib deps, no uncalled object files in the shipping exe). Add them to `Preview3D.vcxproj` only in whichever future slice actually adds a caller, not preemptively.

`D3D12CommandQueue` deliberately pairs one queue with its own owned fence in a single class, rather than two separate `Queue`/`Fence` types the caller must remember to keep matched — the design's ownership table already pairs them per-lane ("direct queue... render fence"; "copy queue... copy fence"), so a queue without its fence isn't a meaningful thing to test or use on its own here.

### Swap chain + window + presented frame (Gate 2 workstream B, slice 2)

First real window and swap chain in the repo. All four flagged risks resolved cleanly on the first real run — no iteration, no surprises requiring a design change.

| Risk flagged before implementation | Outcome |
| --- | --- |
| `GetFrameLatencyWaitableObject()`'s returned `HANDLE` ownership (swap-chain-owned vs. caller-owned) | Treated as swap-chain-owned (never `CloseHandle`d) per best-confidence reading of Microsoft's own D3D12 samples. No leak/crash observed across the test run; not exercised in a long-running repeated-construction loop specifically, so this is "no evidence of a problem" rather than an exhaustive proof — revisit if a future slice does heavy churn of `D3D12SwapChain` instances. |
| Presenting against a never-shown, never-pumped window | **Confirmed: `Present()` returned `S_OK` (0), not `DXGI_STATUS_OCCLUDED`**, for every present in every test, including the 5-consecutive-frames case. So a hidden `WS_OVERLAPPEDWINDOW` that's never `ShowWindow`'d apparently doesn't register as "occluded" to DWM the way a genuinely covered visible window would — worth knowing precisely because the test code was written to *tolerate* `DXGI_STATUS_OCCLUDED` as a non-error outcome, but in practice never needed to. |
| Whether a minimal message pump is needed between frames | **Confirmed: no.** Zero `PeekMessageW`/`DispatchMessageW` pumping anywhere in the test file; 5 consecutive present-and-fence-wait cycles completed without a hang or timeout. |
| Non-interactive-session environment (service account/headless CI) might fail window/present calls for reasons unrelated to the code | Not applicable on this dev machine (interactive session) — noted as a standing caveat for whoever eventually runs this suite somewhere else, not something resolved one way or the other here. |

`D3D12SwapChain` deliberately owns no command allocator/list and no frame fence — per the design's ownership table, "render allocators/lists" and the render fence belong to the render thread, not the presentation surface. The tests' local `FrameRecorder` helper (allocator + command list) and reuse of `D3D12CommandQueue`'s existing fence for frame-retirement bookkeeping are the pattern any real render-thread integration should follow later, not something to relitigate.

`Resize()`'s precondition — every command list referencing the current back buffers must have already retired via the frame fence before calling it — is enforced by the *caller*, not `D3D12SwapChain` itself (it owns no fence to self-check against). This is documented in the header, not just implied; don't add self-checking fence logic to this class later without also giving it fence ownership, which would contradict the ownership-table split above.

### Upload ring + fence-complete publication (Gate 2 workstream B, slice 3)

| Risk flagged before implementation | Outcome |
| --- | --- |
| `ID3D12Resource::Map()` persistent-mapping semantics with a `nullptr`-equivalent read range on an UPLOAD heap | **Confirmed the documented pattern works as expected on the first run**: `Map(0, &{0,0}, &ptr)` (an explicit zero-length `D3D12_RANGE`, not a literal `nullptr`, since the CPU never reads from this resource) once at resource creation, write through the returned pointer for the resource's whole lifetime, no interim `Unmap`/`Map` cycling. No corruption across any of the wrap/growth/backpressure test paths. |
| Whether buffer resources created in `COMMON` genuinely need zero explicit barriers across the copy-queue `CopyBufferRegion` → `ExecuteCommandLists` boundary | **Confirmed: yes, zero barriers needed** for the DEFAULT-heap destination buffers in every test (readback verification round-tripped correctly with no transition ever recorded) — matches the design doc's "implicit promotion" claim for buffers on the copy queue. Not yet exercised for textures (out of scope this slice). |
| Growth-resource retire-behind-fence timing under concurrent in-flight allocations from the *old* ring resource | **A real bug, caught during implementation before any test ran, not by a failing test.** The original `Grow()` retired the old ring resource behind whatever `lastSubmittedFenceValue_` happened to be at that moment — but if `Grow()` fires in the middle of an already-open, not-yet-flushed batch (some `CopyBufferRegion` calls already recorded against the old resource in the still-open command list), that stale fence value predates the work actually referencing the resource being retired. `ReclaimCompleted()` could then `Unmap`/release the old resource — a plain `ComPtr`, since D3D12 command lists do **not** keep referenced resources alive on the application's behalf — while a not-yet-submitted command list still pointed at it. Fixed by having `Grow()` flush the currently-open batch first (if it has recorded work) so `lastSubmittedFenceValue_` genuinely covers everything read from the resource before it's tagged for retirement. None of the four written tests happened to exercise the "grow mid-open-batch" ordering, so this was reasoned out from the ownership-model discipline stated in `04-rendering-and-streaming.md` ("Destruction enters a deferred-release queue tagged with the last direct and copy fence values that could reference the object"), not caught empirically — worth a dedicated regression test if a future slice touches `Grow()`. |
| `std::min`/`std::max` usable directly in a file that transitively includes `<windows.h>` (via `<d3d12.h>`) | **Confirmed: no, not directly** — this repo doesn't set `NOMINMAX` globally (`Directory.Build.props` doesn't define it; only a few unrelated files set it locally), so `<windows.h>`'s `min`/`max` function-like macros silently mangle `std::min(...)` into `std::(((a)<(b))?(a):(b))`, a hard compile error (`C2589`) rather than a silent logic bug — caught immediately by the build. Worked around locally with a manual ternary in `D3D12UploadRing::Grow()` rather than adding `NOMINMAX` repo-wide (out of scope for this slice; worth considering if more graphics code starts hitting this). |

`D3D12UploadRing` deliberately reuses a single command allocator/list across batches, waiting (bounded) for the previous batch's fence before each `Reset()` — the same fence-wait-before-reset discipline `SwapChainTests.cpp`'s `FrameRecorder` already established, traded here for simplicity over a rotating multi-allocator scheme that would avoid the stall. Revisit only if measured batch cadence throughput becomes a real bottleneck; it isn't needed to prove the ring/publication semantics this slice targets.

`SceneSnapshot`/`SceneSnapshotPublisher` (`interactive-viewer/src/graphics/SceneSnapshot.h`) are the first concrete definitions of this type anywhere in the codebase — previously only design-doc prose (`04-rendering-and-streaming.md`, `11-decisions-and-risks.md` ADR-004). Deliberately minimal: an immutable vector of `ReadyResourceInfo` (resource pointer, generation, cluster/LOD ids), no materials/textures/culling. A real renderer slice will need to grow this type, not replace it.

### `MappedFile`/`MappingLease` primitive

| Risk flagged before implementation | Outcome |
| --- | --- |
| A nested `struct OpenResult { std::optional<MappedFile> file; ... };` declared inside `class MappedFile` itself | **Not flagged in advance — caught immediately by the build, not reasoned out first.** MSVC rejected it with `C2139`/`C2079` ("undefined class is not allowed as an argument to compiler intrinsic type trait"): a nested class is parsed as part of its enclosing class's member-specification, so `MappedFile` is still incomplete for `std::optional<MappedFile>`'s special-member instantiation while `OpenResult` is being defined inside it — genuine self-referential incompleteness, not a fluke of this compiler. Fixed by moving the result type out to a free `MappedFileOpenResult` struct defined *after* `class MappedFile { ... };` closes (with only a forward declaration visible at the point `MappedFile::Open`'s return type is named inside the class body, which is sufficient for a mere declaration). Worth remembering for any future "factory method returns a wrapper containing `optional<Self>`" pattern in this codebase. |
| `GetFileInformationByHandleEx(..., FileIdInfo, ...)`'s `FILE_ID_128` behavior on this dev machine's actual NTFS volume | **Confirmed non-degenerate** — every test that opens a real fixture succeeds and reports a real, non-zero `sizeBytes`/identity; the call never failed across five repeat runs in both Debug and Release. Not independently asserted byte-for-byte non-zero (no test reads `FileIdentity::fileId128` directly), so this is "the call always succeeds," not "the returned bytes were inspected" — revisit if a future caller actually keys off file identity for real (e.g. the derived cache). |
| Whether a single whole-file-sized `CreateFileMappingW` mapping object genuinely supports many independent windowed `MapViewOfFile` calls at different (aligned) offsets, including two live at once | **Confirmed: yes**, on the first real run — the "two simultaneous leases" test maps `[0,8)` and `[8,16)` from the same mapping object concurrently and both read back correctly; no need to create a new mapping object per window. |
| Whether `MapViewOfFile`'s offset parameter genuinely must be allocation-granularity-aligned (documented behavior) | **Not independently tested as a failure case** — `MapWindow`'s own contract always aligns internally before calling `MapViewOfFile`, so there's no way to drive a misaligned offset through the public API by design. The granularity-boundary test proves the alignment math is *correct*, not that skipping it would fail; treated as a documented Win32 contract taken on faith rather than empirically reproduced, consistent with how slice 3 treated "implicit COMMON-state buffer promotion" for the cases it didn't have a failure-mode test for either. |

`platform::MappedView::Map` gained its `offset` parameter as a fourth, defaulted argument (`SIZE_T sizeBytes, uint64_t offset = 0`) rather than reordering existing parameters — every pre-existing positional 3-argument call site keeps compiling and behaving identically. Consistent with this repo's established "additive, not breaking" posture for shared primitives (see the control-protocol note below).

`MappedFile`/`MappingLease` live in `shared/model-core` per the design doc's own framing ("Model Core exposes a read-only MappedFile and MappingLease abstraction"), compiled directly into `Tests.Unit.vcxproj` via relative-path `ClCompile`/`ClInclude` entries — the same cross-project-source-file-reuse pattern as `shared/model-core/src/ControlChannelIo.cpp`, not routed through the mostly-unused `ModelCore.vcxproj` static-lib scaffold.

### Wiring `MappedFile` into the real sandboxed pipeline

| Risk flagged before implementation | Outcome |
| --- | --- |
| Whether `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` inheritance — already proven for pipes and a pagefile-section handle — behaves identically for a real disk-file `HANDLE` (a different kernel object type) under a zero-capability AppContainer token | **Confirmed: yes**, on the first real run — `RunGltfImportFromRealFile`'s duplicated file handle crosses into the sandboxed worker and `MappedFile::FromHandle` successfully builds a mapping from it, with no code-path difference from the already-proven handle types (`SandboxLauncher::LaunchSuspendedSandboxed`'s handle-list mechanism is generically typed, per its own header — see the D3D12/MappedFile-primitive slices' surveys). Matches the launch spike's original finding that the mechanism is type-agnostic in principle, now also confirmed for this specific type. |
| A verification false alarm, not a risk flagged in advance: the first post-rebuild run of the full `Tests.ImportIsolation` suite showed 13 failures spanning glTF, synthetic-generation, *and* unrelated Job Object probe tests | **Confirmed environmental, not a regression** — `git stash`-ing to the last commit (`ce2e628`) and rebuilding that baseline passed cleanly on the first try; popping the stash back and simply rebuilding-and-rerunning the *identical* changed code (no edits) also then passed cleanly and stayed clean across 5 further repeat runs in Debug and Release. Most likely a one-time AV/first-launch scan delay on the freshly-written `Preview3DImportWorker.exe`. Recorded here as a reminder: if a fresh rebuild's first test run shows broad, unrelated failures, rebuild-and-rerun once before concluding there's a real regression — but always verify empirically (the stash/rebuild-baseline check here), never just assume "probably fine" and move on. |

The new `StartGltfImportFromFile`/`ParseGltfFileRequest` path deliberately does **not** reuse `platform::MappedView::Map` the way the existing opcode-4 path does — it goes through `model_core::MappedFile::FromHandle` + `MapWhole()` instead, specifically so the primitive built in the previous chunk actually executes inside the sandboxed worker. A survey before implementation found a strictly smaller change was possible (duplicate an already-built file-*mapping* handle into the existing `ParseGltfRequest.sourceHandleValue` slot — `MappedView::Map` doesn't care whether its backing object is a pagefile section or a real file mapping, so this would have needed zero protocol/worker changes at all). That path was deliberately rejected: it would satisfy "a real file's bytes reach the worker" without `MappedFile` itself ever running past `Open()`/`FromHandle()` inside the actual sandbox, which is the point of having built it. Worth remembering if a future slice is tempted to take the cheaper-looking shortcut for a similar reason.

### Worker pool + generation cancellation

| Risk flagged before implementation | Outcome |
| --- | --- |
| Whether a section handle can be duplicated (via `DuplicateHandle`, third-party-style — broker calling with `hSourceProcessHandle=GetCurrentProcess()` and a *different*, non-current `hTargetProcessHandle`) directly into an **already-running** AppContainer child under a zero-capability token, as opposed to the already-proven launch-time `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` inheritance path | **Confirmed: yes**, on the first real run — `DuplicateSectionIntoWorker` succeeds and the pooled worker maps the duplicated handle correctly via the existing `platform::MappedView::Map` path, no different from a launch-time-inherited section handle. The zero-capability token evidently doesn't block accepting an explicitly-duplicated-in handle any differently than an inherited one. |
| Whether `PeekNamedPipe`-based bounded polling (rather than an overlapped/async read) is an adequate wait mechanism for a per-worker notice pipe, given `ControlChannelIo` itself has no timeout concept | **Confirmed adequate for a prototype's synchronous, one-worker-at-a-time test usage** — no hangs, no false timeouts against a genuinely fast reply, across 5+ repeat runs in both configurations. Not stress-tested with many workers replying concurrently under real contention; revisit with proper overlapped I/O if a future slice needs true concurrent multi-worker polling rather than sequential per-index waits. |
| Whether dropping a `SandboxProcess`'s Job Object handle (`platform::Win32Handle::reset()`) genuinely terminates a *pooled* worker the same way the original launch-spike's `--hang`/`--overallocate` kill-on-close tests already proved for a one-shot worker | **Confirmed: yes** — `TerminateAndReplace`'s reused mechanism (no new Job Object logic written) terminates the targeted pooled worker and a freshly-launched replacement is immediately usable for a subsequent request, proven by the timeout test. |

`--pool` mode is purely additive to `main.cpp`'s dispatch table — none of `--generate`/`--parse-gltf`/`--hang`/`--overallocate`/`--probes`' existing strict single-opcode-checking behavior changed; `DispatchOneRequest`/`RunPoolMode` are new code paths reusing the same per-opcode handlers, not a rewrite of the one-shot modes. `import_broker::WorkerPool` is deliberately synchronous/index-based rather than a general async task queue — a prototype's job is proving reuse-across-generations and stale-reply/timeout semantics, not being the final scheduler; a real production scheduler is a distinctly later concern.

### DXGI budget monitor / eviction planner

| Risk flagged before implementation | Outcome |
| --- | --- |
| Whether `D3D12Device`'s adapter can be cleanly re-acquired after the fact (it doesn't retain an `IDXGIAdapter1`/`3` reference past `Initialize()`) via `GetAdapterLuid()` + `IDXGIFactory6::EnumAdapterByLuid()` | **Confirmed: yes**, first real run — re-acquires the exact same adapter and QIs to `IDXGIAdapter3` successfully on this dev machine's hardware. |
| Whether `RegisterVideoMemoryBudgetChangeNotificationEvent`'s event only signals on a genuine subsequent budget change, or also fires once at/near registration time | **Confirmed: the event was already signaled the very first time it was checked** on this dev machine — undocumented either way, so treated as informational only (see the Status entry above), not asserted in either direction. Revisit if a future caller needs to distinguish "just registered" from "a real change happened." |

### Crash-safe bounded derived-cache prototype

| Risk flagged before implementation | Outcome |
| --- | --- |
| Whether Windows CNG's `BCrypt` SHA-256 path (`BCryptOpenAlgorithmProvider`/`BCryptCreateHash`/`BCryptHashData`/`BCryptFinishHash`) works via the explicit provider-handle pattern without needing SDK-version-dependent pseudo-handle constants (`BCRYPT_SHA256_ALG_HANDLE`, whose availability wasn't verified before implementation) | **Confirmed: the explicit provider-handle pattern works correctly on the first run** — deliberately used instead of the pseudo-handle shortcut specifically to sidestep this uncertainty; every cache test that depends on hashing (round-trip, corruption-rejection) passed immediately. |
| A build error, not a design risk, caught immediately: a local test variable named `small` | See the Status entry above — `<rpcndr.h>`'s legacy MIDL-compatibility macro (`#define small char`), pulled in transitively via `<windows.h>`. Renamed to `smallPayload`. Worth a repo-wide mental note: `small`/`hyper` are effectively reserved identifiers in any translation unit that (transitively) includes `<windows.h>`, which in this codebase is nearly all of them. |
| Whether directory-enumeration-based LRU eviction (`FindFirstFileW`/`FindNextFileW` over `*.cache`, sorted by `WIN32_FIND_DATAW::ftLastWriteTime`, re-enumerated on every eviction iteration) is adequate for a small/test-scale cache without a maintained index file | **Confirmed adequate for a prototype** — correctly evicts oldest-first and converges to under the configured cap across every eviction test. Not measured for efficiency at anywhere near the design doc's real 10 GiB/many-thousand-entry scale; a maintained index would be the natural upgrade if that ever matters. |

### D3D12 clear-and-present loop in the real app

| Risk flagged before implementation | Outcome |
| --- | --- |
| Whether `D3D12SwapChain`/`D3D12Device`/`D3D12CommandQueue` — all previously only exercised against a hidden, never-shown, test-owned `WS_OVERLAPPEDWINDOW` in `SwapChainTests.cpp` — behave identically against the real, shown, chrome-managed `HWND` (custom-caption-stripped via `WM_NCCALCSIZE`, DWM dark-mode/rounded-corner attributes, `WS_EX_ACCEPTFILES`/`WS_EX_CONTROLPARENT`) | **Confirmed: yes**, on the first real run — device/queue/swap-chain creation, per-frame clear+present, and `Resize` all worked identically against the real window with no code changes needed beyond what the isolated tests already proved. |
| Whether `Present()` would report `DXGI_STATUS_OCCLUDED` or behave differently against a genuinely visible, on-screen, possibly-partially-covered window, unlike the never-shown test window `SwapChainTests.cpp` used | **Not specifically distinguished** — `RenderClearFrame()` doesn't branch on `Present()`'s return value at all (silently ignores it past the fence signal), so this wasn't actually observable either way this run. Worth adding an explicit check if a future slice needs to react to occlusion (e.g. skipping work while minimized/covered). |
| An unflagged consequence, found empirically rather than reasoned out first: with `Renderer::Initialize` never called on the `--d3d12` path, the custom D2D-drawn chrome (title text, minimize/maximize/close buttons) never renders at all — but `WM_NCCALCSIZE`'s native-caption removal runs unconditionally regardless of `useD3D12`, since that logic lives outside the renderer entirely | **Confirmed via screenshot**: the `--d3d12` window is a bare rounded-corner dark rectangle with no titlebar and no visible close button whatsoever — closing it via a title-bar click isn't possible in this slice (Alt+F4/`WM_CLOSE` still works; verified via `Stop-Process` in this session, not yet via an actual Alt+F4 keypress). Worth remembering for anyone trying this flag interactively, and worth deciding explicitly in a later slice whether the chrome overlay should be one of the earlier follow-ups specifically so the window is self-closable again, rather than leaving it last. |

`D3D12ViewerPath` deliberately ignores `Present()`'s `HRESULT` and silently no-ops out of `RenderClearFrame()` on any `Reset()`/`Close()` failure rather than surfacing an error — acceptable for a bare proof-of-concept clear loop with no telemetry yet, but a real Gate 1 render thread will need proper device-loss detection (`DXGI_ERROR_DEVICE_REMOVED`/`RESET`) per `.docs/design/04-rendering-and-streaming.md`'s "Device loss and recovery" section — explicitly not attempted here.

## Notes for whoever picks up the next chunk

### USD-002 OpenUSD compatibility-host findings

- OpenUSD 26.08 is pinned at commit `ee47c679abde5b467a7b6a41f3b2285564a4222e`
  through `packaging/vcpkg-ports/openusd`. The selected build is monolithic and
  disables Python, imaging/Hydra, tools, tests, validation, MaterialX, optional
  format/renderer plugins, and graphics APIs. The shared comparison produced
  33 DLLs / 21,945,856 bytes and 70 resource files; monolithic is one
  18,053,120-byte OpenUSD DLL and the reviewed subset is 13 resource files.
- A bootstrap executable cannot use `/DELAYLOAD:usd_ms.dll`: OpenUSD imports
  data symbols and MSVC rejects that with `LNK1194`. Keep the small bootstrap
  free of OpenUSD imports, lock DLL/environment discovery there, then
  `LoadLibraryExW` the core DLL by absolute path. The compatibility payload now
  has its own `x64/<Config>/OpenUsdHost/` tree; keep that separation in the
  installer and ACL only that tree to its distinct AppContainer SID.
- A custom OpenUSD resolver must be advertised by an explicitly registered
  `plugInfo.json`; a runtime-only `TfType::Define` has no associated plugin and
  OpenUSD falls back to `ArDefaultResolver`. Register the hash-pinned manifest
  first. Model assets use a URI resolver (`preview3d://`), not a replacement
  primary resolver: this gives Pcp stable absolute identities and causes
  relative arcs to dispatch through the URI anchor while installed schema
  resources continue through the default resolver.
- `ArResolvedPath` values that merely look like relative synthetic paths can be
  opened but do not form a usable Pcp layer stack. Use a registered URI scheme.
  Reject all other schemes, absolute paths, drives, backslashes, and namespace
  escape before lookup; never fall through to the default resolver for a model
  dependency.
- OpenUSD runtime resources are part of the security payload, not incidental
  data. Hash exact files, recursively reject missing/modified/unlisted files,
  explicitly register only the audited root manifest, and reject registry
  plugin/resource paths outside the private payload. Use
  `lexically_relative`, not `std::filesystem::relative`, inside AppContainer;
  the latter canonicalizes through inaccessible parent directories and can
  falsely fail a valid inventory.
- Open stages with `UsdStage::LoadNone`; `TraverseAll` is required to discover
  unloaded payload sites. Sort each breadth and enforce payload-count, byte,
  resolver-open, graph-depth, wall-clock, and Job-memory bounds. Treat
  `GetCompositionErrors()` as fatal even if root geometry is otherwise usable.
- The brokered OpenUSD `ArAsset` can have no `FILE*`; `GetBuffer`/`Read` over a
  bounded copied section is sufficient for USDA, references/sublayers,
  payloads, textures, and USDZ package reads. OpenUSD has no useful
  cooperative cancellation hook for this operation, so retain bounded Job
  termination and fresh-host replacement.
- The common `mesh.usda` facts agree between TinyUSDZ and OpenUSD (hierarchy,
  transform, one mesh/four points/one quad, units/up axis, bounds), but their
  spike hashes intentionally cover different representations. USD-003 must
  define one post-triangulation, format-neutral canonical digest rather than
  reusing either temporary hash.
- Build the solution, not an individual test project: the USD host path macro
  and vcpkg layout depend on `$(SolutionDir)`. The focused USD-002 suite is
  `[usd-002]`; it passed 201 assertions in both Debug and Release.

- The `#pragma pack(1)` + `static_assert(sizeof(...) == N, ...)` pattern on every wire-format struct (`shared/model-core/include/model_core/WireFormat.h`, `ControlProtocol.h`) is deliberate and worth keeping for any new struct added to the wire format or control protocol — it turns layout mistakes into compile errors.
- `SandboxTestSupport.h` (`tests/import-isolation/`) holds the reusable AppContainer test fixture (`SandboxFixture`, unique per-run profile name, ACL grant, profile cleanup in the destructor) plus both worker exe path accessors (`WorkerExePath()`, `HostileWorkerExePath()`). `GenerationLaunchSupport.h` holds the reusable control-channel launch helper (`LaunchWorkerWithControlChannel`, generalized over exe path/args). Both are shared across `SandboxLaunchTests.cpp`, `ImportPipelineTests.cpp`, and `HostileWorkerTests.cpp` — reuse them rather than re-deriving the pattern.
- The checksum (`model_core::Fnv1a64`) is explicitly non-cryptographic and known to be defeatable by a worker that computes its own checksum over its own lies. It only guards the honest path against incidental corruption. The real defense against a *lying* worker is the copy-then-validate bounds/arithmetic checks in `SharedSectionValidator` — now proven adversarially by the hostile-worker suite (part 3).
- `SharedSectionValidator::ValidateAndCopySection` is fail-closed at the section level: any single check failure (header or any one chunk) rejects the whole batch. If a future gate wants partial acceptance (e.g. admit the chunks that did validate), that's a deliberate policy change to make explicitly, not an oversight to "fix."
- **Cross-project source-file reuse is an established pattern in this repo**, not a one-off: `shared/platform/src/MappedView.cpp`, `shared/model-core/src/ControlChannelIo.cpp`, and now `import-worker/src/SyntheticSceneGenerator.cpp` are each compiled directly into multiple `.vcxproj`s via relative-path `ClCompile`/`ClInclude` entries (no shared static lib). When adding a new file that legitimately belongs to more than one executable, prefer this over duplicating code or introducing a new lib project.
- A synthetic hostile-worker/attack-binary project should follow `tests/hostile-worker/Preview3DHostileWorker.vcxproj`'s template: `IsHostileTestBinary=true` set inside `PropertyGroup Label="Globals"` (must be before the `Microsoft.Cpp.Default.props` import), lives under `tests/` (never shipped, so it's a sibling of `tests/unit`/`tests/import-isolation`, not of `import-worker`/`interactive-viewer`), and is wired into the consuming test project via a build-order-only `ProjectReference` (`LinkLibraryDependencies=false`) plus its own `PREVIEW3D_*_EXE` preprocessor macro.
- **When wiring in a new vcpkg-based third-party parser (Gate 3/4), read the actual installed headers under `vcpkg_installed/x64-windows/x64-windows/include/` before writing adapter code**, not just fetched docs — this caught a deprecated-option build failure and resolved two genuinely unverifiable-from-docs API questions (matrix composition order, assert coverage) before any code was written, on the `fastgltf` integration (Gate 3 slice 1). Same discipline is worth repeating for Draco/ufsx/lib3mf/TinyUSDZ/OpenUSD in later gates.
- **The control protocol is additive, not append-only-frozen**: `StartGenerationRequest`/`ChunksReadyNotice`/`GenerationErrorNotice` stayed untouched when real glTF parsing needed a request shape with two section handles instead of one — a new opcode (`StartGltfImport`) and request struct (`ParseGltfRequest`) were added alongside instead of modifying the existing ones. What's actually frozen per the design docs is the *wire format and validator's security properties* (bounds checks, checksums, generation staleness, closed enums) — not this repo's own control-message struct shapes, which may keep growing one opcode at a time as more adapters are wired in behind the sandbox.
- **Gate 2's own deliverable list is done**; the two integration exit criteria that need a real render loop are now **in progress**, not just deferred — see Status above for the full Gate 2 list (workstream A in full; workstream B slices 1-3; `MappedFile`/`MappingLease`, proven standalone and wired into the real sandboxed pipeline; worker pool + generation cancellation; DXGI budget monitor + view-priority requester + detail eviction; the crash-safe bounded derived-cache prototype; fault injection covering all five named fault types) plus the new "D3D12 clear-and-present loop in the real app" entry, slice 1 of actually closing those last two criteria. Correction to an earlier note in this file: `MappedFile` does **not** plug into `D3D12UploadRing::Upload()` directly — per `03-file-formats-and-ingestion.md`'s own "Mapped-file abstraction" section, it's a Model Core primitive used *inside the sandboxed import worker* to read source bytes for parsing, not something the trusted viewer process maps and uploads itself; `D3D12UploadRing`'s `sourceBytes` come from validated wire-format chunk payloads the host already copied out of a shared section, never a raw file mapping. **Also worth correcting explicitly**: prior entries in this file described ADR-010 (D3D11On12 + Direct2D chrome overlay) as an "open architectural question" being deferred — it isn't; that ADR already picked a default ("accepted provisionally through Gate 1," with a profiling-triggered escape hatch to a product glyph atlas). What every slice up to this one actually deferred was the *implementation work* of wiring D3D12 into the real app, not a pending decision. Don't re-litigate ADR-010 itself before revisiting it with real profiling evidence.
- **Likely next slices for the D3D12-in-the-real-app effort**, in roughly dependency order: (a) a fixed-shader camera/orbit + a neutral triangle/cube/point-cloud draw (Gate 1's own next deliverable, needs a root signature/PSO/vertex buffer this slice deliberately didn't build); (b) the D3D11On12/Direct2D chrome overlay bridge (ADR-010's already-decided path) so the `--d3d12` window is self-closable and visually on par with the D3D11 default again; (c) wiring `D3D12UploadRing`/`DxgiBudgetMonitor`/the worker pool/`MappedFile`/`DerivedCache` together behind a real "open a file" flow, replacing `Model.cpp`'s insecure in-process parser; (d) device-loss/recovery (`DXGI_ERROR_DEVICE_REMOVED`/`RESET`), explicitly not attempted in slice 1. Only after some of (a)-(c) land does it make sense to flip `--d3d12` from opt-in to the default and start planning `Renderer.cpp`'s removal.
- **The trusted-process-side file-open step (`import_broker::OpenAndCanonicalizeSourceFile`) is deliberately minimal, not the full Input-boundary policy engine** — no UNC/reparse-point rejection, no "file changed since open" re-verification. It only opens and canonicalizes. Whoever eventually wires real file-open into `Preview3D.cpp` itself needs to layer that broader policy (`03-file-formats-and-ingestion.md`'s "Input boundary" section) on top — don't mistake this function's current behavior for that policy being implemented. **Partial correction since this note was written**: sidecar-directory containment *is* now implemented, in `import_broker::ResolveSidecarPath` (`shared/import-broker/src/SidecarPathResolver.cpp`) — but only for sidecar references reached through the `RequestSidecarFile` protocol, not for the primary file itself, which is what the rest of this bullet still describes.
- **Gate 0's "cancellation/generation primitive" is no longer missing** — `shared/platform/include/platform/Generation.h` (`GenerationSource`/`GenerationToken`), built in slice 3 once the upload ring's publication path needed a real "is this still current" check. Reuse it rather than inventing a second generation/cancellation mechanism anywhere else in the codebase (e.g. a future worker-pool generation-cancellation primitive, also still a Gate 0/ADR-002 debt item, should build on this, not duplicate it).

### USD-004 TinyUSDZ static adapter findings

- TinyUSDZ 0.9.1 commit `a04ee0bcbd1a930e30cc40938fcee3526a6fa8eb`
  implements USDA `PointInstancer` reconstruction but does not register that
  callback in `USDAReader::Impl::Init` (the USDC path does reconstruct it).
  The parse therefore succeeds while silently materializing the prim as a
  `Scope`, and Tydra drops its instance arrays. The overlay-port patch
  `register-usda-point-instancer.patch` adds the missing registration and the
  overlay is revisioned as `0.9.1#1` so existing manifest installs see the
  changed package. Keep this patch, or verify the upstream equivalent before
  advancing the pin.
- TinyUSDZ `Prim::as<T>` uses role-aware, non-strict casts; it is not a safe
  concrete-schema discriminator. `UsdAdapter` compares `Prim::type_id()` with
  `TypeTraits<T>::type_id()` before every schema cast. Also do not use the
  stage's `Prim::absolute_path()` strings as render-node keys: build canonical
  `/Root/Child` paths during deterministic traversal so they match Tydra's
  `Node::abs_path` and relationship targets.
- Tydra triangulation plus `build_vertex_indices` converts supported constant,
  uniform, vertex/varying, indexed, and face-varying normals/UV/color data to
  one vertex index domain. The adapter then deliberately emits bounded
  deindexed chunks; those chunks are shared by ordinary and point-instancer
  `MeshInstance` records. Point-instancer prototype subtree transforms must be
  composed with each instance placement—using only the prototype mesh ID loses
  transforms and multi-mesh prototypes.
- TinyUSDZ exposes no allocation or cooperative-cancellation callback. Keep
  `max_memory_limit_in_mb` advisory only; the worker Job commit cap is the
  authoritative memory boundary, and cancellation is checked between parse,
  classification, conversion, mesh chunks, nodes, and instances. Instantiating
  `TypedTimeSamples::get` for point-instancer arrays produces MSVC C4702 in the
  pinned header, so warning 4702 is disabled only for `UsdAdapter.cpp` in the
  worker project.
- Composition classification must finish before `BoundedChunkWriter` exists.
  This guarantees that a valid sublayer/reference/payload/variant/instanceable
  stage returns `UnsupportedComposition` with zero candidate batches. At
  USD-004 completion valid USDZ still returned `UnsupportedEncoding`; USD-005
  superseded that handoff with archive-backed import. Malformed/unsafe archives
  remain terminal `ArchiveLimit` and never request compatibility fallback.
- Qualification: full Debug and Release solution builds passed. `[usd-004]`
  passed 56 assertions and `[usd-003]` passed 123 assertions in both
  configurations; Debug `Tests.Unit` passed 7,617 assertions and Release
  `Tests.Unit` passed 7,529 assertions. The full Debug isolation run passed
  267/271 cases (101,226 assertions); its four failures are pre-existing FBX
  fixture-mutation tests whose `FindBytes` helper cannot locate the requested
  byte pattern, before any USD route is entered. The Release isolation run
  passed 264/271 cases (101,214 assertions): the same four FBX failures plus
  three unrelated `SidecarPathResolverTests` scratch-directory name collisions
  in that randomized full-suite order. All focused USD tests passed in both
  runs.

### USD-005 USDZ, resolver, material, and packaging findings

- Product-owned archive inspection now does more than structure preflight: it
  verifies each stored entry's CRC32 with cancellation checks, rejects local
  entry overlaps, and returns offset/length views into the original brokered
  mapping. Keep this map as the sole USDZ asset authority; extracting files or
  letting parser-generated paths reach Win32 would undo the boundary.
- With `RenderSceneConverterEnv.scene_config.load_texture_assets=false`,
  TinyUSDZ 0.9.1 still uses `AssetResolutionHandler` to obtain encoded texture
  bytes and fills `RenderScene::images`/`buffers`. Register a wildcard handler
  with resolve, size, and read callbacks—otherwise the library can fall back
  to its own filesystem behavior. Product code, not TinyUSDZ, then owns
  sniffing and WIC/WebP/KTX decode.
- The wildcard resolver intentionally approves image extensions only. Under
  the USD-001 policy, local sublayers/references/payloads are composition, not
  “fast dependencies”; they must stay `UnsupportedComposition` until the
  broker-only OpenUSD path exists. USD-006/007 must not reuse the image-only
  sidecar rule as a composition resolver policy.
- For USDZ only, pinned TinyUSDZ/Tydra fails conversion of the canonical static
  cube when `RenderSceneConverterEnv.timecode` is numeric zero, while
  `TimeCode::Default()` succeeds; the same contained crate converts at zero.
  The adapter therefore uses Default only for Tydra's USDZ conversion while
  retaining the separately classified deterministic stage time. Re-test this
  workaround on every TinyUSDZ pin change.
- Tydra's material-subset indices refer to its triangulated face domain for
  the supported conversion. Validate every index and reject overlaps before
  writing; then split consecutive equal-material runs. This preserves bounded
  chunking and lets all ordinary/point instances reuse geometry with the
  correct normalized material ID.
- Treat a positively sniffed image whose extension disagrees as terminal
  `UnsafeReference`; treat bytes that cannot be sniffed/decoded as a corrupt
  optional texture and publish the deterministic warning/fallback image. This
  distinction prevents type spoofing without making ordinary texture damage a
  permissive compatibility retry.
- The minimal TinyUSDZ static archive still contains enabled vendored source.
  The overlay's combined copyright must include expected-lite, optional-lite,
  LZ4, fast_float, floaxie, jsteemann, ghc filesystem, glob, stb resize,
  tinymeshutils, Project Nayuki's sRGB routines, mapbox earcut/eternal, linalg,
  string_id, dtoa_milo, jeaiii, and the OpenUSD-derived
  crate/integer/compression/transform notices.
  `Create-PortableRelease.ps1` now stages `tinyusdz.txt`; preserve that entry
  when packaging changes. The port revision is `0.9.1#2` so existing local
  installs do not retain the prior incomplete copyright.

### USD-006 compatibility-host lifecycle findings

- Keep producer identity in the control plane even though both adapters emit
  the same normalized wire records. `StartOpenUsdImportFromFile=18` and
  `ParseOpenUsdFileRequest` deliberately match the fast request's 48-byte
  layout but have a distinct type/opcode. This prevents a child or test double
  from changing producer identity without the broker noticing and avoids a
  protocol-version bump.
- Atomic fallback is simplest and strongest when the second attempt calls the
  same single-producer session engine from scratch. It naturally gets a new
  output section, validation catalogs, handle duplications, cancellation
  event, and process lease. Do not pass a fast section/catalog into USD-007;
  the only shared state should be the generation and the trusted broker's path
  authority.
- A compatibility process must never report `UnsupportedComposition`: that is
  a fast-classifier transition, not a general importer error. Treat it as
  reverse fallback/protocol failure. Once the compatibility transition is
  accepted, every non-cancel failure is mapped to a redacted host-owned fact
  and `compatibilityFallbackRequired` is cleared.
- The host's allowed “bounded idle grace” can be zero. Exiting immediately
  after the current generation preserves the strongest isolation from the
  USD-002 spike (OpenUSD has process-global initialization) and makes restart
  behavior deterministic. A typed result receives a graceful `Shutdown`; a
  crash, hang, missed cancellation grace, or malformed reply drops the
  kill-on-close Job and is never replaced for that generation.
- `WorkerPool` is process-agnostic once its executable, arguments, SID, and
  limits are inputs. Reuse it for the compatibility host, but do not reuse the
  worker coordinator/profile: the host is lazy, size one, immediate-exit, and
  ACLed only to `OpenUsdHost/`; the general pool remains prewarmed, size two,
  and session-reused.
- Keep the bootstrap free of OpenUSD imports. The production `--pool` mode can
  use the shared framed control protocol without loading OpenUSD; only the
  first accepted compatibility request calls `LoadLibraryExW` on the absolute
  private core path and invokes the exported resource audit. USD-007 should
  extend that already-loaded core entry point rather than linking OpenUSD into
  the bootstrap or broker.
- On this Windows build, a Job memory kill does not always leave
  `JobObjectLimitViolationInformation.ViolationLimitFlags` observable after
  the process disappears. Check the closed NT memory-exhaustion exit statuses
  too. The explicit small compatibility limit is a test-only qualification
  seam and maps an EOF under that cap to `CompatibilityHostLimit`; production
  uses the derived `min(4 GiB, 35% physical RAM)` cap plus Job/exit evidence.
- Build the solution rather than `Tests.ImportIsolation.vcxproj` directly.
  Direct project builds redefine `$(SolutionDir)` to the project directory and
  put worker/host test dependencies under per-project `x64/`, while the
  compiled path macros intentionally target the solution-level output tree.
- Current qualification is 277 isolation cases. Debug passes 273 with the
  four pre-existing FBX fixture-pattern failures. Release passes 271 with the
  same four plus two existing randomized `SidecarPathResolverTests` directory
  collisions. All USD-002..006 filters pass; none of those six full-run
  failures enters a USD route.

### USD-008 viewer and distribution findings

- The generic viewer bridge's `nextDetail` callback is not merely a host-side
  scheduling hint: `MakeFileRequest` turns it into
  `kImportRequestDetailService`. The USD request's allowed flag mask contains
  only the expected-encoding bits, so passing the Tier-A detail callback makes
  an otherwise valid USD request fail as `ImportProtocolViolation`. Keep both
  `enableCoarseProxy` and `nextDetail` unset for USD; its Tier-B adapters still
  stream bounded normalized batches through the ordinary `onBatch` path.
- Product `RunImport` must prepare the general worker pool itself even though
  normal window startup prewarms it. Direct bridge tests exposed the stale
  assumption: acquiring an uninitialized coordinator returns `LaunchWorker`.
  `EnsureImportSandboxPrepared()` is idempotent and now runs at the bridge
  boundary, so retry/direct callers do not depend on window-startup timing.
- Product builds and packaging must build the viewer project, not only copy an
  already-present host tree. `Preview3D.vcxproj` now has a build-order-only
  reference to `Preview3DImportHost.vcxproj`; that project already references
  the OpenUSD core and materializes its 13 audited resources under
  `x64/<Config>/OpenUsdHost/`.
- The release host allowlist is 26 files: 8 host/runtime binaries, 13 audited
  OpenUSD resources, and 5 app-local CRT DLLs. Copy the tree from its private build
  output by explicit relative path, then compare the staged recursive
  inventory in both directions. Do not recursively copy the build directory:
  it also contains PDB/import-library/export artifacts. `usd_ms.dll` imports
  Windows system `dbghelp.dll` and `shlwapi.dll`; both belong in the PE system
  allowlist, not in the private payload.
- Installer ACL provisioning must derive both deterministic SIDs before
  changing either directory. On each tree, remove broad application-package
  grants and both product SIDs, add only the owning SID, and verify that the
  other SID has no remaining ACE. Cleanup symmetrically removes both SIDs from
  both trees and deletes both current-user profiles.
- The actual distribution surface is one USD family ProgID, not one per
  encoding. Keep `.usd`, `.usda`, `.usdc`, and `.usdz` mapped to
  `Binbuf.Preview3D.USD.1` through capabilities, OpenWithProgids, SupportedTypes,
  reset, and uninstall. USD-008 intentionally adds no CLSID/shellex/thumbnail
  registration; Explorer thumbnails remain USD-010.
- Qualification: Debug/Release solution builds and the 98-case Unit suite
  pass (7,625 / 7,537 assertions). Focused USD-002..008 passes 26 cases / 711
  assertions in both configurations, and the real-app USD smoke passes eleven
  activation/fallback/warning/failure-retention/replacement checks in both.
  Full isolation is 279/283 Debug and 271/283 Release; the only failures are
  the four established FBX
  fixture-pattern misses plus eight Release randomized sidecar scratch-name
  collisions. Portable staging passes PE/closed-inventory validation with 63
  files and opens a composed USD stage from the staged layout; NSIS builds a
  64-file unsigned engineering payload. Release hashes are recorded in
  `.docs/usd.md` and `.docs/INSTALLER_VERIFICATION.md`.

### STEP-002 host, Part-21 admission, and closed protocol route

- `Preview3DStepHost.exe` is a fifth runtime component with its own
  zero-capability AppContainer identity (`Binbuf.Preview3D.StepHost`),
  kill-on-close Job Object, and private `StepHost` payload directory. The
  constrained OCCT closure stays isolated from the viewer, general worker, and
  thumbnail provider; `dumpbin /dependents` reports zero OCCT imports for
  `Preview3D.exe` and `Preview3DImportWorker.exe`.
- Protocol v10 gained only additive identities:
  `SourceFormatId::Step` (13), `ImportFormat::Step`,
  `StartStepImportFromFile` (20), the 48-byte `ParseStepFileRequest`, and the
  `StepHostFailure`/`StepHostLimit` error codes. No existing layout, fixture,
  or hostile-worker case changed meaning.
- `StepPart21Preflight` is a bounded streaming lexical scanner over the
  inherited handle. It verifies the physical envelope, counts entities/
  references/sections/nesting/lexed bytes with checked arithmetic, rejects
  duplicate/zero identifiers, binary/XML/compressed/UTF-16 encodings,
  unterminated strings/comments, and flags `FILE_POPULATION`/`DOCUMENT_FILE`
  external declarations as `UnsupportedRequiredFeature` (STEP-005 is a no-go).
  The OCCT reader is unreachable until admission succeeds; STEP-003 replaces
  the bounded synthetic placeholder with the real XDE traversal.
- Two implementation traps worth keeping: a 1 MiB `std::array` on the stack
  overflowed the host (use heap scratch), and a non-default host commit limit
  must be reported as `ResourceLimit` for both compatibility and STEP hosts,
  not only the USD compatibility host.
- Evidence: `[step-002]` passes 13 cases / 155 assertions in Debug and
  Release, covering valid admission through the dedicated route, malformed/
  external rejection before transfer, cancellation, crash/hang/stale/
  wrong-format/unknown-error/over-allocation recovery, path/network/child-
  process denial, and the private viewer-bridge route. The full
  import-isolation suite has pre-existing USD failures (fixture SHA-256 review
  and the legacy `--usd-002-spike` host route) that fail identically on the
  STEP-002 baseline with these changes stashed. See
  [`STEP-002-VERIFICATION.md`](STEP-002-VERIFICATION.md).

### STEP-003 self-contained XDE assembly scene adapter

- `StepXdeAdapter` now replaces the STEP-002 synthetic placeholder behind the
  existing `StartStepImportFromFile` route. It reads accepted bytes exclusively
  through the inherited read-only handle, transfers them with
  `STEPCAFControl_Reader::ReadStream`, and walks the XDE document into protocol
  v10 nodes, reusable geometry, materials, and mesh instances.
- Enumeration is product-owned and bounded: one node per occurrence, one
  reusable geometry record per definition/color seam, recursive composition of
  nested `TopLoc_Location` transforms in double precision, cycle detection on
  the definition ancestry, hierarchy-depth/node/definition/material/triangle/
  vertex caps, and exact double world bounds recomputed exactly as
  `SharedSectionValidator` does. Orphan definitions are never drawn; an empty
  document returns `EmptyGeometry`.
- Units: on the pinned OCCT 7.8.1 build the reader normalizes transferred
  coordinates to the Cascade system unit and `GetLengthUnit` reports that
  unit's verified metre factor; `FileUnits` is name-only and is required to
  prove a length unit was authored. The STEP-001 handoff's suggestion to
  re-derive an authored factor and rescale is therefore unnecessary for
  physical correctness and would risk a factor/geometry mismatch; STEP-003
  reports the factor describing the stored geometry, keeps `UpAxisId::Unknown`,
  and fails absent/contradictory/zero/non-finite units.
- Materials use instance/shape/subshape precedence: a component instance color
  overrides the whole occurrence without duplicating geometry; otherwise the
  shape-level color wins and the neutral material (id 0) is the fallback.
  Per-face subshape colors split reusable geometry into bounded seam groups,
  and sRGB is converted to the linear `baseColorFactor` with opacity mapped to
  `AlphaModeId::Blend`.
- Eight immutable fixtures are checked in under `tests/fixtures/stp-spike/`
  (AP203/AP214/AP242 parts, a reused-definition assembly, a nested assembly,
  an instance-color assembly, a face-color part, and an inch-authored part);
  the generator gained the nested/instance/face-color cases.
- Evidence: `[step-003]` passes 9 cases / 191 assertions in Debug and Release,
  and `[step-002]` remains green. The known pre-existing USD/FBX failures in
  the full suite are unrelated and unchanged. See
  [`STEP-003-VERIFICATION.md`](STEP-003-VERIFICATION.md).

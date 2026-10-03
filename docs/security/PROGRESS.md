# Security hardening progress notes

Shared notebook for the `security` task set. Earlier sessions record findings, decisions, and
things later tasks must know here; the harness maintains the "Key facts" digest at the top.

<!-- symphony:digest:start -->
## Key facts (maintained by symphony — do not edit)

- **T01 — SEC-01 Bound glTF traversal and fix worker limit ordering**: Reusable facts for later sessions:; **glTF node walk budget.** `import-worker/src/GltfAdapter.cpp` `WalkState::totalNodeVisits` is
- **T02 — SEC-02 Preflight compressed decoders before allocation**: Reusable facts for later sessions:; **Shared preflights.** `shared/model-core/include/model_core/DracoPreflight.h` and
- **T03 — SEC-03 Sandbox limits and broker defensive checks**: Reusable facts for later sessions:; **Job limits.** `SandboxLimits::processCpuTimeLimitMs` maps to
- **T04 — SEC-04 Sidecar reference validation (NUL/control, per-format)**: Reusable facts for later sessions:; **Two-layer control check + explicit UTF-8 failure.** `import_broker/src/SidecarRequestServicer.cpp`
- **T05 — SEC-05 3MF OPC preflight/library reconciliation**: Reusable facts for later sessions:; **Preflight policy now matches USDZ.** `import-worker/src/ThreeMfOpcPreflight.cpp` rejects
- **T06 — SEC-06 Provider adapter exception containment**: Reusable facts for later sessions:; **Pattern (chosen).** Keep the frozen `IFamilyAdapter` methods `noexcept`; every adapter's
- **T06a — SEC-03b Fix user-chosen asset-root canonicalization**: Reusable facts for later sessions:; **Root cause (audit F-11).** `ResolveSidecarPath`'s user-root branch compared
- **T07 — SEC-07 Provider stream/raster/accounting robustness**: Reusable facts for later sessions:; **Stream size validation.** `thumbnail-provider/StreamSource.cpp` has
- **T08 — SEC-08 Provider containment policy (AV, stack, OCCT)**: Reusable facts for later sessions:; **Policy chosen: quarantine, not fail-fast (ADR-0037).** On the first contained structured
- **T09 — SEC-09 Child-process loader/plugin hardening**: Reusable facts for later sessions:; **Child hardening pattern.** Each import child must, before parsing, call
- **T10 — SEC-10 Build and process mitigation hardening**: Reusable facts for later sessions:; **Shared build mitigations.** `Directory.Build.props` now sets `ControlFlowGuard=Guard` and
- **T11 — SEC-11 Viewer local attack-surface reduction**: Reusable facts for later sessions:; **One reusable safety module.** `interactive-viewer/src/platform/SafeFileOps.{h,cpp}`
- **T12 - SEC-12 Active-instance IPC hardening**: Reusable facts for later sessions:; **One security builder, per-object rights.** `interactive-viewer/src/app/ActiveInstance.cpp`
- **T17 — SEC-17 Provider pipeline fuzz + surrogate soak**: Reusable facts for later sessions:; **Provider pipeline fuzz target.** `tests/fuzz/ProviderFuzz.cpp` (+ `ProviderFuzz.vcxproj`,
- **Follow-ups**: T12/SEC-14: once `Preview3D.exe` is Authenticode-signed, extend; T12: add a functional low-integrity rejection test (spawn/impersonate a low-integrity token) to
- **T13 — SEC-13 CI test gate for PRs and releases**: Reusable facts for later sessions:; **One reusable gate workflow.** `.github/workflows/ci.yml` triggers on `pull_request`, push to
- **T14 — SEC-14 Release and supply-chain hardening**: Reusable facts for later sessions:; **Permissions cannot be conditional.** GitHub Actions `permissions` (top-level or job-level) does
- **T15 — SEC-15 Fuzz targets: STL, PLY, OBJ**: Reusable facts for later sessions:; **Three targets, one convention.** `tests/fuzz/StlFuzz.cpp`, `PlyFuzz.cpp`, and `ObjFuzz.cpp`
- **T16 - SEC-16 Fuzz targets: glTF + compressed codecs**: Reusable facts for later sessions:; **One target, seven domains.** `tests/fuzz/GltfFuzz.cpp` (+ `GltfFuzz.vcxproj`, GUID
- **T16b — SEC-16b KTX2/BasisLZ ETC1S decoder finding and GltfFuzz smoke promotion**: Reusable facts for later sessions:; **Root cause (characterized).** KTX-Software 4.4.2's `ktxTexture2_transcodeLzEtc1s`
- **T18 — SEC-18 Reconcile design docs with implemented controls**: Reusable facts for later sessions:; **Docs-only task.** No product code or tests changed; the harness verify is still
- **T19 — SEC-19 License/SBOM/dependency metadata**: Reusable facts for later sessions:; **One generator, one mapping.** `packaging/ReleaseMetadata.ps1` holds the installed-closure
<!-- symphony:digest:end -->

## T01 — SEC-01 Bound glTF traversal and fix worker limit ordering

Reusable facts for later sessions:

- **glTF node walk budget.** `import-worker/src/GltfAdapter.cpp` `WalkState::totalNodeVisits` is
  capped at `model_core::kTierAObjectLimit` (100000, `shared/model-core/include/model_core/TierALimits.h`).
  A DAG diamond is still reprocessed (world transforms differ per parent); the cap, not subtree
  memoization, bounds the walk. `VisitNode` also checks `TextureDecodeOptions::Cancelled()` at
  entry. If a limit change is ever needed it is a new public limit and needs an ADR.
- **USD UV-expansion ordering.** `import-worker/src/UsdAdapter.h::PrimvarExpansionLimit(corners)`
  is the single pre-allocation admission check for face-varying primvar expansion
  (`kTierBTriangleLimit` / `kTierBVertexLimit`). `RecoverMissingTexcoords` returns
  `ImportErrorCode` and runs before the scene mesh-count cap; `ExpandPrimvarForTriangulatedMesh`
  reserves with `platform::CheckedMultiply`. `FlattenNodes` has depth cap `kUsdMaxNodeDepth = 256`
  and a `kTierBObjectLimit` node cap.
- **Building the tests without the full solution.** Build only this project but point SolutionDir
  at the repo root so the worker lands in the shared output dir:
  `MSBuild tests\import-isolation\Tests.ImportIsolation.vcxproj /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\"`.
  Building `import-worker\Preview3DImportWorker.vcxproj` directly puts the exe in
  `import-worker\x64\...`, which the tests do not use.
- **Run a subset.** `x64\Debug\Tests.ImportIsolation.exe "[dag]"` (new glTF DAG cases) or by test
  name. The 40-level doubling DAG fails fast: Debug 321 ms mesh-less / 37 ms mesh-bearing;
  Release 45 ms / 26 ms.
- **Baseline is not green.** After the `.gitattributes` CRLF fix (below), `Tests.ImportIsolation`
  has 5 Debug / 3 Release pre-existing failures: `SidecarPathResolverTests.cpp` user-asset-root
  cases (Debug 920/929/950, Release 489/504/518), `ThreeMfSpikeTests.cpp:630` (absent problem-model
  file), and `UsdSpikeTests.cpp:230` (low Job commit cap). None involve glTF or the USD adapter; do
  not attribute them to this task. Before the fix the suite showed 13 Debug / 11 Release failures;
  the extra ones were the CRLF-hashed `.usda` fixtures, now resolved.
- **USD spike worker is not the adapter.** `UsdSpikeWorker.cpp` runs its own TinyUSDZ conversion and
  never calls `ImportUsd`/`UsdAdapter.cpp`, so a regression for the adapter must go through the real
  worker (`ImportFormat::Usd` / `--parse-usd`, as `ImportSession.cpp` wires it), not `--usd-spike-pool`.
- **Verify is `npm test`, not the isolation suite.** `package.json` maps `test` to
  `x64\Release\Tests.Unit.exe`; the harness verify command is `npm test`. The prior hand-off only ran
  `Tests.ImportIsolation`, so it missed the real blocker.
- **Fixture line endings broke verify (and looked like USD SHA drift).** With no `.gitattributes`
  and `core.autocrlf=true`, Git checked out the text fixtures as CRLF while the committed blobs and
  the manifests are LF. That failed `tests/unit/FixtureManifestTests.cpp:52` (14 corpus hashes, the
  `npm test` exit-42 blocker) *and* the `.usda` hash checks in `UsdProtocolTests.cpp` /
  `tests/import-isolation/FixtureManifestTests.cpp` that had been misrecorded as "fixture SHA
  drift". The `.usda` LF bytes hash to the manifest values exactly. Fixed by `.gitattributes`
  (`interactive-viewer/test-assets/** -text`, `tests/fixtures/** -text`) plus re-checkout; see ADR
  0029. Do not remove those attributes, and never commit regenerated fixtures as CRLF.
- **How to confirm fixture bytes.** `git ls-files --eol interactive-viewer/test-assets/corpus tests/fixtures`
  should show `w/lf` after the fix (it showed `w/crlf` before). To restore after a bad checkout:
  delete the tracked files and `git checkout -- tests/fixtures interactive-viewer/test-assets`.

## T02 — SEC-02 Preflight compressed decoders before allocation

Reusable facts for later sessions:

- **Shared preflights.** `shared/model-core/include/model_core/DracoPreflight.h` and
  `.../Ktx2Preflight.h` are header-only and are the single definition of the Draco/KTX2 header
  layouts, called by both `import-worker/src/DracoDecodeAdapter.cpp` /
  `thumbnail-provider/GltfFamilyAdapter.cpp` and the worker/provider KTX2 paths. `PreflightKtx2`
  is a direct extraction of the worker's old inline checks; `ValidateDracoCounts` parses the
  declared counts and rejects accessor mismatch (`Malformed`) or over-budget (`OverLimit`).
- **Draco bitstream gotcha.** The fixed header is 11 bytes (flags is a little-endian `uint16`).
  After the optional metadata block, the edgebreaker path consumes a one-byte traversal selector
  in `MeshEdgebreakerDecoder::InitializeDecoder` *before* `DecodeConnectivity` reads the
  varint counts. Omitting that byte misreads `num_encoded_vertices`/`num_faces`. Metadata is
  variable-length; skip it with Draco's own `MetadataDecoder::DecodeGeometryMetadata` rather than
  reimplementing the format.
- **No cancellation.** Draco and KTX expose no progress/cancel callback. Decodes are bounded but
  not interruptible mid-payload; ADR 0030 records this and narrows design/02's "bounded
  cancellable decode jobs". Do not claim per-payload cancellation.
- **Evidence seam.** `model_core::DracoDecoderInvocations()` / `Ktx2DecoderInvocations()` are
  relaxed atomics incremented immediately before the library call; tests read them to prove the
  third-party decoder was never reached. Any new compressed codec behind these adapters should
  add the same preflight + counter pattern.
- **NOMINMAX.** Any TU that includes `model_core/DracoPreflight.h` pulls in draco core headers
  that use `std::numeric_limits<T>::max()`. TUs that also see `windows.h` (e.g.
  `tests/unit/ProviderGltfAdapterTests.cpp`) must `#define NOMINMAX` first.
- **Build/verify.** `MSBuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\"`
  then `npm test` (350 cases). ImportIsolation:
  `MSBuild tests\import-isolation\Tests.ImportIsolation.vcxproj /p:Configuration=Debug ...`.
  `Tests.ProviderHost.exe "[parallel]"` passes alone but is a pre-existing concurrency flake in a
  full run; none of its fixtures use Draco/KTX2.
- **Pre-existing `npm test` blockers fixed here.** `interactive-viewer/src/ui/InfoPanel.cpp`
  `FormatDimension` never streamed `value` (every dimension rendered as just the unit); and
  `tests/unit/LocalizationTests.cpp` leaked the French active locale into later cases. Both were
  repaired to reach a green gate (see the T02 hand-off); they are unrelated to SEC-02.

## T03 — SEC-03 Sandbox limits and broker defensive checks

Reusable facts for later sessions:

- **Job limits.** `SandboxLimits::processCpuTimeLimitMs` maps to
  `JOB_OBJECT_LIMIT_PROCESS_TIME` (`PerProcessUserTimeLimit`, 100 ns units) in
  `SandboxLauncher.cpp::CreateConfiguredJob`. `ImportSession.cpp` maps a Job violation in that flag
  (and `JOB_OBJECT_LIMIT_PROCESS_MEMORY`) to `ResourceLimit`. **Never set the CPU cap on a process
  reused across generations** — it is cumulative per process; the general worker pool is bounded by
  the generation deadline instead. The compatibility/STEP hosts and the one-shot path get
  `kImportProcessCpuTimeLimitMs` (15 min, `ImportSession.h`).
- **Generation wall-clock deadline.** `ImportSessionRequest::generationWallClockBudgetMs`
  (default `kImportGenerationWallClockBudgetMs`, 300 s, `ImportSession.h`). Every mid-generation
  read uses `boundedReplyWait()` = `min(replyTimeout, ceil(remaining))`; expiry returns the new
  `ImportStage::GenerationDeadline` with `ResourceLimit`. The `ceil` matters: truncation lets a
  read time out a hair before the deadline and be misreported as `ReplyTimedOut` (this was hit in
  T03 testing).
- **CPU-time probe.** `import-worker --cpu-spin` (`RunCpuSpinProbe`) blocks on no handle and burns
  user CPU, so only a Job CPU-time limit can stop it. `SandboxLaunchTests` uses it with a 200 ms cap.
- **Progress-spam probe.** The hostile worker's `--batches-unbounded` keeps sending valid batches;
  drive it through `RunImportSession` with `maxChunkBatchesPerGeneration` / `maxChunksPerGeneration`
  raised and a tiny `generationWallClockBudgetMs` to prove the deadline, not the batch cap, stops
  it. `--sidecar-stale-generation` sends one `RequestSidecarFile` with `generationId + 1`.
- **Primary-path ADS.** `OpenAndCanonicalizeSourceFile` rejects any `:` that is not the volume
  separator (ADS, drive-relative, URI). `\\?\C:\` has its volume colon at index 5, normal `C:\` at
  index 1.
- **Texture accounting.** `BatchAcceptance::Record` now returns `bool` and re-derives RGBA8 bytes
  with a checked `ComputeImagePixelBytes`; a null result fails `ValidateSection`/`MalformedData`.
- **Build/run.** `MSBuild tests\import-isolation\Tests.ImportIsolation.vcxproj /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\"`
  (or `Release`), then `x64\<Config>\Tests.ImportIsolation.exe`. `npm test` is still the harness
  verify (Tests.Unit, 350 cases, green).
- **Baseline still not green.** Debug full run: 386 cases, 6 failed — all pre-existing
  (`SidecarPathResolverTests` user-asset-root ×3, `UsdSpikeTests:230`, `ThreeMfSpikeTests` 20 MB
  Job memory-pressure recovery, `OpenUsdHostSpikeTests` startup-timing, which varies 1–3 assertions
  run to run). Release: 3 failed, the `SidecarPathResolverTests` trio only. Do not attribute these
  to new work; confirm with `git stash` + rebuild if unsure.

## T04 — SEC-04 Sidecar reference validation (NUL/control, per-format)

Reusable facts for later sessions:

- **Two-layer control check + explicit UTF-8 failure.** `import_broker/src/SidecarRequestServicer.cpp`
  rejects any byte `< 0x20` or `0x7F` before constructing a reference; `SidecarPathResolver.cpp`
  repeats the raw-byte check, then decodes with `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
  ...)` returning `std::optional<std::wstring>` and re-checks the decoded text. Without
  `MB_ERR_INVALID_CHARS` invalid sequences became U+FFFD and never failed; an embedded NUL moved
  through untouched. Both are now hard `UnsafeReference`.
- **Per-format allowlist.** `IsAllowedSidecarExtension(ImportFormat, path)` in
  `SidecarPathResolver.cpp` is the single policy point: Gltf `.bin`+images, Obj `.mtl`+images,
  Fbx images, Usd `.usd/.usda/.usdc`+images, everything else (Stl/Ply/ThreeMf/Step) none. The
  format is the trusted host's `ImportSessionRequest::format`, threaded through
  `ServiceSidecarRequest`/`ResolveSidecarPath`; the worker never supplies it, so **no
  control-protocol change**. The task text said "USD: layers" but USD also brokers direct image
  assets (`materials.usda` -> brokered `albedo.png`) and FBX brokers textures too; the USDZ
  in-archive path is unaffected. If a format gains a new dependency type, extend this switch.
- **`SidecarPathResolver.h` now includes `ImportSession.h`** for `ImportFormat` (no cycle:
  `ImportSession.h` does not include either sidecar header). `ResolveSidecarPath`/`ServiceSidecarRequest`
  gained a trailing `ImportFormat format = ImportFormat::Gltf` default so existing callers/tests
  compile; production always passes the real format.
- **Corpus seeds.** `tests/fixtures/generate.py` sidecar loop now adds `sidecar-nul-extension.gltf`
  (`approved.bin\0.png`) and `sidecar-nul-trailing.gltf` (`approved.bin\0`); they are hostile
  entries in `tests/import-isolation/FixtureManifestTests.cpp`. Regenerating the committed
  `interactive-viewer/test-assets/corpus/{manifest.json,Expectations.h}` and
  `tests/fixtures/manifests/qualification-small.json` needs the Release meshoptimizer DLL at
  `vcpkg_installed/x64-windows/x64-windows/bin/meshoptimizer.dll`; **that DLL is not present in this
  checkout**, so the manifest was produced by importing `generate.py` and stubbing `meshopt()` to
  copy the committed `meshopt.glb`. Existing entries were verified byte-identical; a real-DLL
  regeneration should reproduce it.
- **Commands.** Build/run Debug:
  `MSBuild tests\import-isolation\Tests.ImportIsolation.vcxproj /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\"`
  then `x64\Debug\Tests.ImportIsolation.exe`. Harness verify is `npm test` (Tests.Unit Release,
  350 cases, green). New resolver cases are tagged `[sidecar-resolver][security]`.
- **Baseline unchanged.** Debug full run 392 cases / 6 failed, Release 392 / 3 failed — the same
  pre-existing set recorded under T03 (Debug: SidecarPathResolver user-asset-root x3, ThreeMfSpike,
  OpenUsdHostSpike, UsdSpike; Release: the resolver trio only). Nothing new.

## T05 — SEC-05 3MF OPC preflight/library reconciliation

Reusable facts for later sessions:

- **Preflight policy now matches USDZ.** `import-worker/src/ThreeMfOpcPreflight.cpp` rejects
  general-purpose bit 3 (`flags & 8`) with `InvalidDirectory`, reconciles the local header's CRC,
  compressed/uncompressed size and raw name bytes against the central directory, and bounds the
  local offset (`off + 30 <= coff`) and the ZIP64 locator offset (`zoff + 56` checked, `<= e - 20`)
  before any read. The ZIP64 locator total-disk must now be 1 (was 0); other values return
  `MultiDisk`. A shared `zip64()` helper parses sentinel-selected extra fields for both central and
  local entries. `InvalidDirectory` maps to `ArchiveLimit` at the worker/provider boundary.
- **Deliberate divergence.** Streaming/data-descriptor (`bit 3`) 3MF packages are rejected rather
  than parsed, unlike the original 3MF-002 plan text. Recorded in ADR 0033 and reflected in
  `docs/design/03-file-formats-and-ingestion.md` and `adapters/3mf-thumbnail.md`.
- **Test seams.** `ThreeMfSpikeTests.cpp` has in-process ZIP mutators (`FindEocd`, `WithFirstCentralFlags`,
  `WithLocalSizeMismatch`, `WithLocalNameMismatch`, `AsZip64`, `WithZip64LocatorOffset`). `AsZip64`
  wraps a normal stored package in a spec-shaped ZIP64 record/locator without touching the 32-bit
  central records. `Put16`/`Read16` were added next to the existing `Put32`/`Read32`.
- **Fixture corpus.** `tests/fixtures/3mf/verify.py` now derives `bit3-entry`,
  `local-central-size-mismatch`, `local-name-mismatch`, `zip64-valid`, `zip64-locator-wrap` from
  `core-box`; `manifest.json` carries the frozen bytes/sha256. `python tests/fixtures/3mf/verify.py`
  prints "verified 7 sources and 12 derived cases". `prepare_3mf_seeds.py` then materializes 19
  seeds (7 + 12).
- **Fuzz smoke.** `MSBuild tests\fuzz\ThreeMfFuzz.vcxproj /p:Configuration=Release /p:Platform=x64
  "/p:SolutionDir=D:\repos\binbuf\preview-3d\\"` puts the exe at
  `tests\fuzz\x64\Release\ThreeMfFuzz.exe`. Run against a fresh seed dir (preparer refuses a
  non-empty one); 45 s smoke executed 872k units with no crash artifact.
- **Baseline.** Debug ImportIsolation is now 393 cases / 6 failed, the same pre-existing set
  (SidecarPathResolver ×3, ThreeMfSpike Job memory-pressure recovery, UsdSpike:230,
  OpenUsdHostSpike:244). Release `npm test` (Tests.Unit) is 350/350. The `ThreeMfSpikeTests` Job
  pressure failure is unrelated to preflight: it runs `--3mf-spike-pool`, which never calls
  `InspectThreeMfOpc`.

## T06 — SEC-06 Provider adapter exception containment

Reusable facts for later sessions:

- **Pattern (chosen).** Keep the frozen `IFamilyAdapter` methods `noexcept`; every adapter's
  `Initialize`/`Parse`/`EnumerateMaterials`/`EnumerateGeometry` is now a thin wrapper around a
  non-`noexcept` `...Impl()` body routed through
  `preview3d::provider::RunContainedStage` (`thumbnail-provider/ContainmentStage.h`, implemented in
  `Containment.cpp`, compiled `/EHsc`). It returns the body's own `ErrorCode` unchanged,
  `ErrorCode::OutOfMemory` for `std::bad_alloc`, else `InternalImporterFailure`. `Reset` is
  untouched. Rationale and the rejected "drop `noexcept` everywhere" alternative: ADR-0034.
- **Windows-free split.** `ContainmentStage.h` includes only `ProviderTypes.h` and a new
  `DiagnosticStage.h` (the stage enum moved out of `Diagnostics.h`), so an adapter TU with a vendored
  parser header does not get `windows.h`/`min`/`max` macros. Do not add `windows.h` to it.
- **Adapters touched.** STL, PLY, OBJ, glTF, FBX, 3MF, USD all use the wrapper. STEP is different by
  design (its opaque OCCT calls already go through `RunContained`); only its `Parse`/
  `EnumerateGeometry` catches were reordered to add `catch (const std::bad_alloc&) ->
  ErrorCode::OutOfMemory` before `catch (const std::exception&)` (which previously swallowed
  `bad_alloc` as `InternalImporterFailure`). Any internal helper that allocates had `noexcept`
  removed so its throw reaches the boundary.
- **`RunContainedStage` passes `deadline = nullptr`**, so it does not rewrite a successful-but-
  overran result to `Deadline`; the stage's own `Checkpoint()` and the pipeline's between-stage
  check keep their exact semantics. Diagnostics still record the stage
  (`AdapterInitialize`/`Parse`/`Materials`/`Geometry`).
- **Fault injection.** `tests/provider-host/FaultInjectingAllocator.{h,cpp}` replaces the
  provider-host exe's global `operator new`/`delete` with `malloc`/`free` (`_malloc_dbg`/`_free_dbg`
  in Debug) and fails on demand via `preview3d::test::ScopedAllocationFailure`. It only reaches
  product-owned (C++) allocations compiled into the exe; 3MF/USD/STEP library allocations are C
  allocators and need the SEC-17 soak/fuzz lane.
- **Debug gotcha.** Under `_ITERATOR_DEBUG_LEVEL=2`, a default-constructed STL container allocates
  its `_Container_proxy` inside a `noexcept` constructor; a `bad_alloc` there terminates before the
  adapter boundary. The injector therefore never fails allocations `<= sizeof(std::_Container_proxy)`
  in Debug (Release fails everything). If this test ever hangs, a modal CRT assertion is the cause:
  `ProviderHostMain.cpp` redirects CRT reports to stderr in Debug to keep CI from blocking.
- **Coverage.** `Tests.ProviderHost.exe "[host][containment]"` in `ProviderHostTests.cpp` drives a
  valid fixture per family (STL/PLY/OBJ/glTF/FBX) directly through the adapter with allocations
  armed and asserts `ErrorCode::OutOfMemory`.
- **Verify commands.** Build the host through the solution dir:
  `MSBuild tests\provider-host\Tests.ProviderHost.vcxproj /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\" /m`
  (same for Release), then run `x64\Debug\Tests.ProviderHost.exe` and `x64\Release\...`.
  Provider unit coverage: `x64\Release\Tests.Unit.exe "[provider]"`.
- **Baseline after T06.** `Tests.ProviderHost` Debug and Release: 128 assertions / 6 test cases,
  all pass (was 103/5). `Tests.Unit.exe "[provider]"` Release: 60901 assertions / 214 cases, all
  pass.

## T06a — SEC-03b Fix user-chosen asset-root canonicalization

Reusable facts for later sessions:

- **Root cause (audit F-11).** `ResolveSidecarPath`'s user-root branch compared
  `AcceptCandidate`'s handle-canonicalized candidate path (`\\?\C:\...`, from
  `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED)`) against `DirectoryPrefix(rawRoot)` (`C:\...`),
  so the prefix never matched and the fallback always missed. Fix:
  `SidecarPathResolver.cpp::CanonicalDirectoryPrefix` opens the root with
  `CreateFileW(..., FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE, OPEN_EXISTING,
  FILE_FLAG_BACKUP_SEMANTICS)`, requires `FILE_ATTRIBUTE_DIRECTORY`, and derives the lowercase
  trailing-separator prefix from `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED)`. This mirrors
  `AcceptCandidate`, so both sides are in the same canonical form. `std::nullopt` (root unopenable /
  not a directory / canonicalization failed) makes the loop skip the root: fail closed, never compare
  raw text.
- **Only the prefix changed.** `FindAssetInUserRoot` still searches the raw user path; the raw path
  itself is fine for lookup and was never the bug. Do not "fix" the lookup side.
- **Contract is in `SidecarPathResolver.h`** (the `additionalSearchRoots` comment now states the
  canonicalize-or-skip rule). ADR 0035 records the decision; rejected a string normalization because
  it must reimplement reparse/short-name/volume normalization and would disagree with the candidate.
- **Tests.** `tests/import-isolation/SidecarPathResolverTests.cpp` `[sidecar-resolver][asset-root]`
  now has six cases: the original normal-path (574), texture subfolder (598), non-image exact-name
  (612), "still bounded to its own directory tree" security case (626), plus new equivalent-spellings
  (raw / trailing `\` / `\\?\` prefix / upper-case) and uncanonicalizable-root-skipped. Debug and
  Release both green (`[asset-root]`: 77 assertions / 6 cases).
- **Baseline after T06a.** Debug `Tests.ImportIsolation` 395 cases / 2 failed — only
  `ThreeMfSpikeTests.cpp:345` (Job commit-pressure recovery) and `UsdSpikeTests.cpp:230` (low Job
  commit cap), both pre-existing and unrelated. Release `Tests.ImportIsolation` is fully green
  (293018 assertions / 395 cases), which is the task's verify command. `npm test`
  (`x64\Release\Tests.Unit.exe`) green: 134972 assertions / 350 cases.
- **Build gotcha.** `MSBuild` is not on PATH in this shell; use
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`.

## T07 — SEC-07 Provider stream/raster/accounting robustness

Reusable facts for later sessions:

- **Stream size validation.** `thumbnail-provider/StreamSource.cpp` has
  `TryValidateStreamSize(raw, out)` in its anonymous namespace; it rejects the
  high bit (signed `-1`/unknown sentinel) and `> kStreamMaxBytes` before anything
  is stored. Both `STATSTG.cbSize` and `Seek(STREAM_SEEK_END)` go through it; the
  seek-end path is only reachable when `Stat` is unavailable or fails, and the
  non-seekable materialization cap is unchanged. `Tests.Unit` needs
  `MemoryStream::SetSeekEndReportOnly(true)` to
  produce a raw high-bit `STREAM_SEEK_END` report (the default double rejects it
  via signed position arithmetic, so it cannot otherwise exercise the guard).
- **Rasterizer extent clamp.** `ClampToRasterExtent(value, lo, hi)` in
  `thumbnail-provider/CpuRasterizer.cpp` replaces the three
  `static_cast<int>(floor/ceil(...))` screen-extent sites. NaN/`-inf` map to the
  low bound, `+inf`/over-high to the high bound. The floor-shadow pass
  (`RenderFloorShadow`) now takes `WorkGuard&`, returns `bool`, and checkpoints
  per pixel; a `false` return becomes `ErrorCode::Cancelled` with an empty
  image. Do not add a bare `static_cast<int>` on a model-derived double here.
- **STEP scratch reservation is now derived, not flat.** Constants in
  `StepFamilyAdapter.cpp`: `kStepGeometryBytesPerTriangle` = 152
  (2*(9+9)*4 + 2*4, growth headroom), `kStepMaxTrianglesPerDefinition` =
  500,000 (was 1,000,000), `kStepMaxGeometryCacheTriangles` = 750,000,
  `kStepScratchReservationBytes` = (500,000+750,000)*152 = 190,000,000 bytes
  (181.2 MiB), guarded by a `static_assert` against
  `ProviderLimits::kAccountedScratchMaxBytes` (192 MiB). A single definition
  above 500k triangles now fails `TessellationFailed`; the 2,000,000 overall
  emission cap is unchanged. ADR-0036 records the decision. Verify with the
  `[provider][step]` case "the STEP scratch reservation covers the worst-case
  cache plus build" (100 MiB ledger refused, default admitted).
- **Budget documentation.** `docs/design/05-thumbnail-provider.md` and
  `docs/design/03-file-formats-and-ingestion.md` list charged-vs-library-owned
  items; the T51 measured-target link is still a placeholder because T51 has not
  landed.
- **Verify commands (same as T06a).** `MSBuild` is not on PATH; use
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`.
  Build `tests\unit\Tests.Unit.vcxproj` and `tests\provider-host\Tests.ProviderHost.vcxproj`
  with `/p:Configuration=Debug|Release /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\" /m`.
- **Baseline after T07.** `Tests.Unit` Debug and Release: 355 cases, all pass
  (was 350). `Tests.ProviderHost` Debug and Release: 128 assertions / 6 cases,
  all pass. New cases: 2 stream, 2 rasterizer, 1 STEP.

## T08 — SEC-08 Provider containment policy (AV, stack, OCCT)

Reusable facts for later sessions:

- **Policy chosen: quarantine, not fail-fast (ADR-0037).** On the first contained structured
  exception the T16 boundary records the code and sets a process-global quarantine
  (`thumbnail-provider/Containment.cpp`: `MarkContainmentQuarantined`,
  `ContainmentQuarantined`, `ContainmentQuarantineCode`). `RunThumbnailPipeline` refuses every
  later request with `ProviderOutcome::DecoderFailure` (`E_FAIL`, no image) before any adapter or
  parser runs, while in-flight requests drain. The transition and each refusal emit a
  `DiagnosticStage::Containment` event (`DiagnosticEvent.quarantined=true`; new enum value `8`, new
  `Diagnostics.cpp` field `quarantined=%u`). Chosen over fail-fast because it keeps the safety
  property (no later request on suspect state) without discarding unrelated in-flight work and is
  positively testable in-process; the Shell reclaims the surrogate. Stack
  overflow/breakpoint/single-step are not contained and never quarantine.
- **Test-only reset.** `ResetContainmentQuarantineForTest()` must bracket any case that injects an
  AV through `RunContained` (the state is process-global; `Tests.Unit` shares one process). The AV
  cases in `ProviderThreadingTests.cpp` use a `QuarantineReset` RAII guard; the pipeline/host cases
  call the reset directly.
- **OCCT singleton serialized.** `StepFamilyAdapter.cpp` holds one process-global
  `SRWLOCK g_occtApplicationLock` (guard type `OcctApplicationGuard`) across `Impl::Reset`
  (`GetApplication()->Close`), `LoadDocument` (lazy `GetApplication` + `NewDocument` + reader +
  transfer) and `BuildGeometry` (meshing). `SRWLOCK` not `std::mutex` because the acquisition sites
  are `noexcept`; the guard lives in the caller of `RunContained`, *above* the SEH handler, so a
  contained AV cannot leave it locked. Do not acquire it recursively (no nesting today).
- **Stack-overflow/`__fastfail` are explicit allowed failures.** design/05 and design/09 now record
  that a re-raised stack overflow or an uncatchable `__fastfail`/stack-cookie fault kills the
  surrogate and must be classified by the SEC-17 soak as an allowed failure (code + input recorded,
  process restarts), never as a pass. T08 did not add the soak; SEC-17 owns it (see Follow-ups).
- **Tests.** `Tests.Unit` `[provider][threading][quarantine]` (boundary transition + first-fault-wins
  + uncatchable code) and `[provider][pipeline][quarantine]` (quarantined request refused before any
  adapter runs); `Tests.ProviderHost` `[host][containment][quarantine]` (real injected AV through the
  shipped boundary then a refused fixture) and `[host][step][concurrency]` (two STEP requests in one
  surrogate, both succeed). Unit: Debug/Release 357 cases; host: Debug/Release 142 assertions /
  8 cases.
- **Build gotcha (LTCG).** After changing a provider inline header (Diagnostics/Containment), an
  incremental `/GL` build can crash at link time (`LNK1000: Internal error during IMAGE::BuildImage`,
  sometimes reported as `C1001` on an unrelated TU such as `gltfspikedecodetests.cpp`) because stale
  LTCG objects are mixed with new ones. Fix: `MSBuild ... /t:Rebuild`. A normal incremental build is
  fine once every object matches the header.
- **Verify commands.** `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`
  (MSBuild is not on PATH). Build `thumbnail-provider\Preview3DThumbnailProvider.vcxproj`,
  `tests\unit\Tests.Unit.vcxproj` and `tests\provider-host\Tests.ProviderHost.vcxproj` with
  `/p:Configuration=Debug|Release /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\"`.
  Run `x64\<Config>\Tests.Unit.exe` and `x64\<Config>\Tests.ProviderHost.exe`. Do **not** pipe the
  exe through `Select-Object -Last N` to read its tail: closing the pipe early can return a bogus
  nonzero exit code; redirect to a file and read the tail instead.

## T09 — SEC-09 Child-process loader/plugin hardening

Reusable facts for later sessions:

- **Child hardening pattern.** Each import child must, before parsing, call
  `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS)`, add
  exactly one `AddDllDirectory` for its own payload directory, `SetCurrentDirectoryW` to that
  directory, and scrub the OpenUSD/plugin + OCCT `CSF_*` variables. `import-worker/src/main.cpp`
  `HardenProcessDiscovery` is the third copy (after both hosts); keep the three lists in sync.
- **Broker passes a clean environment.** `SandboxLauncher.cpp::LaunchSuspendedSandboxedWithSid`
  copies the caller's environment, drops the same scrub list, and passes it with
  `CREATE_UNICODE_ENVIRONMENT` plus `lpCurrentDirectory = payload dir`. **If the Unicode flag is
  omitted, `CreateProcessW` interprets the wide block as ANSI and every launch returns `nullopt`
  with no diagnostic** (cost: a full bisect). Do not drop that flag.
- **Fault harness is Debug-only.** `PREVIEW3D_ENABLE_FAULT_HARNESS` is defined in
  `Directory.Build.props` for `Configuration == Debug`. It guards the fault switches in
  `import-worker`, `compatibility-host`, and `compatibility-host-step` mains.
  `sandbox_test_support::FaultHarnessEnabled()` (SandboxTestSupport.h) returns the same value in the
  test exe; fault cases `SKIP` in Release. The `[sandbox][security]` "Release binaries reject
  compiled-out fault flags" case drives each child directly with every removed flag and asserts a
  nonzero exit (Release-only).
- **Kept in Release on purpose:** `--probes`, `--pool`, `--generate`, `--*-spike`/`--*-spike-pool`,
  and the normal `--parse-*` routes. design/09 requires the AppContainer/Job restriction suite
  against every child, so removing these would remove the Release containment proof. Only fault
  injection is compiled out (ADR-0038).
- **STEP lexical admission is strict everywhere.** `StepPart21Preflight.cpp` applies the
  control-byte reject at the top of `Consume` (so NUL/control inside strings and `/* */` comments
  fails) and buffers up to four leading bytes across `Feed` (`DecideLeadingSignature`, flushed in
  `Finish`) so split UTF-8/UTF-16 BOM/ZIP/gzip/XML signatures are still classified. New tests:
  `[step-008][preflight-control][security]`, `[step-008][preflight-split][security]`.
- **Verify (same MSBuild path as T06a).** Build
  `tests\import-isolation\Tests.ImportIsolation.vcxproj` with
  `/p:Configuration=Debug|Release /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\"`.
  Release `x64\Release\Tests.ImportIsolation.exe`: 398 cases / 0 failed / 5 fault skips. Debug:
  398 / 2 pre-existing failures (ThreeMfSpike:630, UsdSpike:296) / 1 skip. `npm test`
  (`Tests.Unit` Release) 357 cases green.

## T10 — SEC-10 Build and process mitigation hardening

Reusable facts for later sessions:

- **Shared build mitigations.** `Directory.Build.props` now sets `ControlFlowGuard=Guard` and
  `CETCompat=true` for every project; `Microsoft.CppCommon.targets` derives linker `/GUARD:CF` from
  the ClCompile metadata, so no explicit linker property is needed. Debug pins
  `DebugInformationFormat=ProgramDatabase` to keep `/guard:cf` and `/ZI` from colliding. Verified
  with `dumpbin /headers` + `/loadconfig` on all five product images (Guard CF, CET, DYNAMICBASE,
  NXCOMPAT, high-entropy VA). See ADR-0039.
- **`/guard:ehcont` is opt-in and off.** Every product image statically imports vcpkg libraries
  (draco/ktx/lib3mf/libwebp/meshoptimizer/simdjson/tbb/tinyusdz/usd_m/ufbx/zip/zstd) built without EH
  continuation metadata; linking with `GuardEHContMetadata` fails LNK1386/LNK2047 (measured on
  worker, viewer, provider). The switch is `Preview3DEnableGuardEhCont` (default false). This
  supersedes design/09's unconditional EHCont claim until the ports are rebuilt.
- **`/Qspectre` is conditional.** `Directory.Build.targets` enables `SpectreMitigation=Spectre` only
  when `$(VC_LibraryPath_VC_Desktop_CurrentPlatform_spectre)` exists; otherwise MSB8040 fails the
  build. The detection must live in `Directory.Build.targets`, not props, because the VC library
  paths are not computed during `Directory.Build.props` evaluation. `-getProperty:
  Preview3DSpectreMitigationEnabled` reports the measured outcome (`false` on this image).
- **Broker creation mitigation policy.** `SandboxLauncher.cpp` adds
  `PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY` (CFG always-on + extension-point disable + ACG) as the
  third attribute; `UpdateProcThreadAttribute` failure returns `nullopt` (fail closed). No
  MicrosoftSignedOnly — the payload is unsigned until SEC-14. The attribute is read back while the
  child is still suspended by the new `[sandbox][security]` case in `SandboxLaunchTests.cpp`.
- **Startup mitigations.** `shared/platform/ProcessMitigations.h` is header-only:
  `platform::ApplyProcessMitigations(bool prohibitDynamicCode)` enables
  `HeapEnableTerminationOnCorruption` and `ProcessExtensionPointDisablePolicy`, and ACG
  (`ProcessDynamicCodePolicy`) when requested. Called from the viewer (`false`), worker, import host,
  and STEP host (`true`). Failure is fatal (nonzero exit). Do **not** call
  `SetProcessMitigationPolicy(ProcessControlFlowGuardPolicy)`: it returns ERROR_ACCESS_DENIED
  (measured err=5) on an already-CFG image; CFG comes from the PE header, linker, and broker.
- **Application manifest.** `shared/platform/app.manifest` is added globally through
  `<Manifest><AdditionalManifestFiles>` in `Directory.Build.props`; `mt.exe` merges it with the
  linker manifest. The result keeps `trustInfo` + `heapType SegmentHeap` and adds `longPathAware`,
  `activeCodePage UTF-8`, and the Win10/11 `supportedOS` GUID `{8e0f7a12-...}`.
- **Gotcha — host startup got slower.** CFG-always-on + ACG + the manifest measurably lengthen child
  startup; `OpenUsdHostSpikeTests.cpp` replaced its fixed `Sleep(150)` "started" check with a bounded
  `WaitForState` poll. Any test that assumes a fixed child-startup latency must poll instead.
- **Build/verify.** MSBuild is not on PATH; use
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`. Build with
  `/p:Configuration=Debug|Release /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\" /m`.
  `Tests.ImportIsolation` Release 399 cases / 0 failed / 5 skipped, Debug 399 / 0 / 1; `npm test`
  (Tests.Unit Release) 357 green; all seven product projects build in both configs.

## T11 — SEC-11 Viewer local attack-surface reduction

Reusable facts for later sessions:

- **One reusable safety module.** `interactive-viewer/src/platform/SafeFileOps.{h,cpp}`
  (`preview3d::safeio`) holds `SanitizeDisplayText`, `AppDataDirectory`,
  `IsSafeOutputPath`, and `WriteFileAtomically`. Compiled into both the viewer and `Tests.Unit`;
  add new local-file/UI-hardening primitives here so the exact shipped code stays unit-testable.
- **Reparse-safe write pattern.** `WriteFileAtomically` creates a GUID-named sibling with
  `CREATE_NEW | FILE_FLAG_OPEN_REPARSE_POINT`, checks `GetFileInformationByHandle` for
  `FILE_ATTRIBUTE_REPARSE_POINT`, writes+flushes with the handle still open, then
  `MoveFileExW(MOVEFILE_WRITE_THROUGH [, MOVEFILE_REPLACE_EXISTING])`. `replaceExisting=false`
  fails closed. Settings, the Open With cache, and DerivedCache use it; the old fixed
  `<file>.tmp` name is gone everywhere. Tests plant a symlink at `<final>.tmp` and at `<final>`
  itself and assert the victim is untouched (SKIP when `CreateSymbolicLinkW` is denied).
- **Control-surface macro.** `PREVIEW3D_ENABLE_TEST_CONTROL` is defined **only for Debug**
  in `Directory.Build.props`. In `Preview3D.cpp` it gates parsing of `--app-smoke`,
  `--activation-smoke`, every `--*-smoke`, and `--benchmark-worker-budget-failure`, and
  short-circuits the `WM_APP+104` and `WM_COPYDATA` cases to `return 0`. Release therefore hits
  the unknown-option usage exit (2) for those flags. Note `--coarse-proxy-smoke` sets
  `app.appSmoke`, so it is also Debug-only.
- **Benchmark output confinement.** `--benchmark*` stays in Release. A non-empty
  `--benchmark-result` must pass `IsSafeOutputPath(path, L".json", AppDataDirectory())`:
  absolute drive path, no `\\`/`\\?\`/`\\.\`, no extra `:`/ADS, no `..` component, no controls,
  `.json`, and under `%LOCALAPPDATA%\Binbuf\Preview 3D`. It is written via
  `WriteFileAtomically`. `tests/performance/qualify.py` now writes `app-N.json` to
  `%LOCALAPPDATA%\Binbuf\Preview 3D\benchmarks` and copies it into `run_dir` for evidence;
  `--copy-delay`/`--worker-budget-failure` are refused on Release.
- **Parser unlinked from the trusted EXE.** `src/render/Model.cpp` is out of
  `Preview3D.vcxproj` and `.filters`, and `ws2_32.lib` is off the viewer link line.
  `TransformBounds` is now header-inline in `Model.h` (the only `Model.cpp` symbol the viewer
  used); `LoadGlb`/`PickMesh` were already dead in the viewer. `Model.cpp` remains on disk and is
  still compiled by the manual `interactive-viewer/tools/build-test-loader.ps1` tool. Do not
  re-add it to the viewer project.
- **Missing-asset sanitization.** `BuildModelWarningText` runs each `app.missingAssets` entry
  through `SanitizeDisplayText(asset, 256)` before it reaches the dialog. The worker can still
  send hostile references; only display-side filtering was added.
- **Build gotcha (XML).** `Directory.Build.props` is XML: a comment cannot contain `--`. Do not
  write literal CLI flags such as `--app-smoke` inside its comments (this failed MSB4024).
- **Verify commands.** MSBuild is not on PATH; use
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe` with
  `/p:Configuration=Debug|Release /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\"`.
  `npm test` (`x64\Release\Tests.Unit.exe`) is 366 cases / 135098 assertions, green.
  `x64\Release\Tests.Unit.exe "[security]"` is 112 assertions / 13 cases.
- **Release flag verification.** Search the exe bytes as both ASCII and UTF-16LE for the flag
  literals. Release contains none of the smoke/fault flags; Debug contains all;
  `--benchmark-result=` is in both (validated, not a developer control).

## T12 - SEC-12 Active-instance IPC hardening

Reusable facts for later sessions:

- **One security builder, per-object rights.** `interactive-viewer/src/app/ActiveInstance.cpp`
  `SecurityForObject(SecuredObject, userSid, …)` is the single place the singleton DACL is built.
  Rights: mutex `SYNCHRONIZE|MUTEX_MODIFY_STATE|READ_CONTROL` (`0x120001`), event
  `SYNCHRONIZE|EVENT_MODIFY_STATE|READ_CONTROL` (`0x120002`), pipe `FRFW`; each `O:<user>D:P(SY,user)`
  plus a medium mandatory label `S:(ML;;NW;;;LW)`. No `GENERIC_ALL`/`WRITE_DAC`/`WRITE_OWNER`. Extend
  here, not with a new SDDL literal elsewhere.
- **`CreateEventEx` is auto-reset by default.** `CreateEventW(..., TRUE, ...)` is manual-reset, but
  `CreateEventExW(..., dwFlags, ...)` needs `CREATE_EVENT_MANUAL_RESET`; flags `0` makes the Ready
  event auto-reset and the *second* secondary silently times out (`WAIT_TIMEOUT`). This was the T12
  bug that only showed up on the second activation.
- **Reusing one named-pipe instance across `DisconnectNamedPipe`→`ConnectNamedPipe` is unsafe here.**
  Create a fresh instance per connection. `FILE_FLAG_FIRST_PIPE_INSTANCE` is applied only to the first
  instance; each next instance is created before the previous handle is closed (`nMaxInstances=2`) so
  the name never lapses and a pre-created pipe still loses.
- **Peer policy is a pure function.** `active_instance::ClientIdentityAccepted(integrityRid,
  appContainer, imageMatches, ownChild)` is the testable decision; `AuthenticateClient` feeds it the
  impersonated token integrity (`TokenIntegrityLevel` last RID), `TokenIsAppContainer`, the client
  image (`QueryFullProcessImageNameW` from a handle opened *before* impersonation), and parent PID
  (`CreateToolhelp32Snapshot`). Path equality against `GetModuleFileNameW` handles the portable layout;
  signer checks are deferred to SEC-14/T10.
- **Squatter rejection.** On `ERROR_ALREADY_EXISTS`, `ObjectSecurityMatches` compares the existing
  owner+DACL (via `GetKernelObjectSecurity` → `ConvertSecurityDescriptorToStringSecurityDescriptorW`,
  `OWNER|DACL` only) to the expected descriptor; mismatch → `Initialize` fails. The abandoned-mutex
  recovery branch still works because a legitimate predecessor wrote the same descriptor.
- **Mandatory labels cannot be read in the test process.** Reading a SACL needs `SeSecurityPrivilege`;
  `GetKernelObjectSecurity(..., SACL_SECURITY_INFORMATION, …)` fails for the unprivileged harness, so
  `Tests.Unit` asserts the minimum-rights DACL (parsing the SDDL) rather than the `ML` ACE. The label
  is still passed at creation.
- **Verify commands.** `MSBuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Release /p:Platform=x64
  "/p:SolutionDir=D:\repos\binbuf\preview-3d\\"` then `npm test` (369 cases / 135121 assertions) and
  `x64\Release\Tests.Unit.exe "[activation]"` (7 cases / 66 assertions). The smoke is Debug-only:
  build `interactive-viewer\Preview3D.vcxproj /p:Configuration=Debug …` then
  `python tests/app-smoke/activation.py --configuration Debug` (8 checks).
- **Residual squat risk.** Names stay deterministic (`session + SID hash`); a medium same-user process
  can still race to pre-create but must reproduce the exact owner+DACL and viewer image, and the pipe
reservation blocks capture. A per-logon secret does not help against a medium same-user reader; see
  ADR-0041.

## T17 — SEC-17 Provider pipeline fuzz + surrogate soak

Reusable facts for later sessions:

- **Provider pipeline fuzz target.** `tests/fuzz/ProviderFuzz.cpp` (+ `ProviderFuzz.vcxproj`,
  `prepare_provider_seeds.py`) is a libFuzzer + ASan target over the real provider sources. A 24-byte
  envelope (`<IBBBBIQI`: magic `PRVZ`, domain, family, flags, reserved, cx, reported size,
  payload length) selects `Pipeline` (registry -> adapter -> sampler -> rasterizer over a
  memory-backed `BoundedStreamSource`), `Stream`, `Sampler`, or `Raster`. `Prepare_provider_seeds.py`
  materializes 36 seeds (one real fixture per family from `.../corpus`, `tests/fixtures/*-spike`, plus
  hostile stream/sampler/raster seeds) and refuses a non-empty directory.
- **Build needs two vcpkg manifests.** The target links all eight adapters including STEP/OCCT, so a
  cold build needs both the root `vcpkg_installed` and `thumbnail-provider\step-occt\vcpkg_installed`
  restored. It sets `_DISABLE_STL_ANNOTATION` to link the prebuilt non-ASan static libraries (same
  trick as `GltfFuzz`); a distinct `IntDir` avoids the MSB8028 shared-intermediate hazard with the
  other `tests\fuzz` projects. MSBuild is not on PATH; use
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`.
- **Measured smoke.** `tests\fuzz\x64\Release\ProviderFuzz.exe <seeds> -max_total_time=60 -timeout=10
  -rss_limit_mb=2048 -max_len=2100000 -print_final_stats=1 -verbosity=0` ran 788 executions / 447 MiB
  peak RSS in 60 s (30 s: 590 / 330 MiB) with no crash/ASan report. The STEP adapter prints OCCT
  `StepFile`/`StepReaderData` errors to stderr on malformed input; that is expected and harmless.
- **Surrogate soak.** `packaging/smoke/ProviderSoak.{h,cpp}` is compiled into the existing
  `ProviderSmokeHost.exe` and reached with `--soak --dll <staged> --<family> <fixture>...
  --apartments 4 --iterations 100 --cx 256 --out <dir>`. It drives concurrent STA apartments through
  `IThumbnailCache::GetThumbnail` (`WTS_EXTRACT|WTS_FORCEEXTRACTION`) so the provider is loaded,
  rendered and torn down in the real `dllhost.exe` surrogate with cache churn, then requires every
  call to return a bitmap, the surrogate to disappear within 30 s, and bounded growth (GDI/User +64,
  handles +256, threads +8, private +32 MiB). `soak-report.txt` records the raw baseline/final numbers.
- **Measured soak.** Stage/register with `packaging\smoke\Stage-ProviderSmoke.ps1` +
  `Register-ProviderSmoke.ps1`; the 7-family Release run (4 apartments x 100 + 4 warm-up = 404
  thumbnails) had 0 failures, GDI 0->0, User 6->8, handles 189->207, threads 10->11, private bytes
  4,665,344 -> 9,818,112 (+5.15 MiB), one `dllhost.exe` surrogate, teardown in 5.1 s, then
  `Unregister-ProviderSmoke.ps1`.
- **No finding.** The committed seeds produced no crash, hang, OOM or leak in the fuzz target or the
  soak, so no new regression fixture was needed. The SEC-08 quarantine and OCCT serialization are
  unchanged.
- **Restriction suite re-run.** `x64\Release\Tests.ImportIsolation.exe "[sandbox]"`: 17 cases
  (15 passed, 2 skipped for the compiled-out Release fault harness), 764 assertions. Full Release
  `Tests.ImportIsolation.exe`: 403 cases / 398 passed / 5 skipped / 0 failed. Harness verify
  `x64\Release\Tests.Unit.exe "~[graphics]"`: 312 cases / 131557 assertions, all pass.
- **ADR.** [ADR-0047](../design/adr/0047-provider-pipeline-fuzz-and-surrogate-soak.md) records the
  target domains, the `_DISABLE_STL_ANNOTATION`/two-manifest constraint, and the soak classification.

## Follow-ups

- T12/SEC-14: once `Preview3D.exe` is Authenticode-signed, extend
  `active_instance::ClientIdentityAccepted` (or `AuthenticateClient`) to require a signer/`
  WinVerifyTrust` check on the client image, and re-run the Debug activation smoke.
- T12: add a functional low-integrity rejection test (spawn/impersonate a low-integrity token) to
  exercise `AuthenticateClient` end to end; today the rejection is covered by the pure policy test.
- T12 (viewer attack surface): `tests/app-smoke/activation.py` still accepts `--configuration Release`
  although `--activation-smoke` is Debug-only (same T11 follow-up as the other lanes).

- SEC-14: once the payload is Authenticode-signed, enable Microsoft-signed-image enforcement
  (`PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_ALWAYS_ON` in `SandboxLauncher`
  and a `ProcessSignaturePolicy` in `ApplyProcessMitigations`), then re-run the isolation suite.
- SEC-14/T10: rebuild the vcpkg ports (and OCCT closures) with `/guard:ehcont` and install the MSVC
  Spectre-mitigated libraries component, then flip `Preview3DEnableGuardEhCont=true` and re-verify
  that `/Qspectre` activates; no code change beyond the switch is required.
- T11/GPU: validate whether the viewer can also take ACG (`prohibitDynamicCode=true`) and whether CFG
  strict mode/`ProcessControlFlowGuardPolicy` are usable once the D3D runtime is exercised on real
  hardware.

- T11 (viewer attack surface): **resolved** — `--benchmark-worker-budget-failure` and the WM
  fault-injection message are now Debug-only (`PREVIEW3D_ENABLE_TEST_CONTROL`). Remaining: the other
  `tests/app-smoke/*.py` lanes still accept `--configuration Release` but drive the now-compiled-out
  control surface; only `run.py` was given an explicit Release rejection. Add the same guard (or
  force Debug) to `activation.py`, `bounded.py`, `budget.py`, `coarse.py`, `ground_axis.py`,
  `metadata.py`, `progressive.py`, `recovery.py`, `step.py`, `textures.py`, `three_mf.py`, `usd.py`
  when next touched; all already default to Debug.
- T09/SEC-10: process-mitigation attributes are untouched (out of T09 scope).

- T18/SEC-19: **resolved by T19** — `THIRD-PARTY-LICENSES.md` now carries versions and
  links the pinned baseline, and the portable/installer SBOM, notices, and `licenses/`
  files are generated from the installed vcpkg closure with a fail-closed SPDX mapping
  (`packaging/portable/dependency-licenses.json`). The dedicated baseline check in
  `packaging/Test-ReleaseMetadata.ps1` keeps the doc from drifting again.

- SEC-17: the real-dllhost soak now exists (`ProviderSmokeHost.exe --soak`); it counts a surrogate
  crash/hang as a hard failure and a persistent surrogate/growth as failures. Remaining: implement
  the SEC-08 allowed-failure *classification* - a stack-overflow or `__fastfail`/stack-cookie process
  death should be recorded (fault code + input) and the process restarted rather than reported as a
  generic crash, and a contained AV observed as a quarantine with later requests failing closed (see
  `docs/design/09-quality-performance-and-security.md`, "End-to-end soak", and ADR-0037). The
  committed valid-fixture soak cannot trigger an AV, so this needs a hostile-input soak lane.
- SEC-17/CI: `ProviderFuzz` is documented and runnable but not in the scheduled `fuzz-smoke` matrix
  (`.github/workflows/ci.yml`) because that job restores only the root vcpkg manifest and the target
  links the isolated `thumbnail-provider\step-occt` manifest. Restore that manifest in the job, add
  the matrix entry, then promote the lane to a required gate (ADR-0047).
- SEC-08/T51: STEP's `occurrences_` vector (16-byte transform + definition/index
  per instance, bounded only by the 20M reference preflight) is still not charged
  to the ledger; T07 reconciled only the geometry cache/build reservation. If a
  hostile STEP file can create millions of visible occurrences, this is the next
  unaccounted product-owned growth. Also still owed: T51's measured
  process-private-commit peak and the T07 doc placeholder link.
- SEC-17: the provider-host allocator seam only fails C++ allocations; add a soak/fuzz lane that
  exercises the library (C-allocator) paths of 3MF/USD/STEP and a genuinely over-committed ledger.
- `ThreeMfFamilyAdapter.h` still declares `ErrorCode ScanRequiredExtensions();` with no definition
  anywhere in the provider (pre-existing dead declaration, carried through the T06 header edit).
  Remove it or implement the intended XML scan.
- SEC-04 and later: the primary-path ADS check is a coarse `:` rule shared in spirit with the
  sidecar resolver; if `ResolveSidecarPath` ever adopts per-format rules, keep the primary rule at
  least as strict. (T04 did add per-format extension rules; the `:`/control rules stay at least as
  strict in both.)
- The generation wall-clock deadline currently wraps the main reply loop but not the post-terminal
  `nextDetail` replay loop (`ImportSession.cpp`, the `request.nextDetail` branch), which still uses
  the per-reply timeout. If a hostile detail loop becomes a concern, apply `boundedReplyWait()`
  there too.
- Consider a measured multi-gigabyte import budget to re-derive `kImportGenerationWallClockBudgetMs`
  and `kImportProcessCpuTimeLimitMs`; both are currently backstops anchored to the STEP Ready ≤180 s
  figure.
- Consider a large, opt-in (`[.]`) qualification fixture that drives the real `--parse-usd` worker
  with a >20M-triangle UV-bearing USDA and asserts `ResourceLimit` before the expansion reserve.
  Deliberately not added now: the fixture is ~150-200 MB and risks the 120 s worker reply timeout.
- PLY `meshIndices.reserve(vertexElement->count * 3)` is safe only because `vertexElement->count` is
  already bounded by the Tier B scratch budget; switching it to `platform::CheckedMultiply` would be
  a defensive cleanup, not a fix.
- SEC-16: add fuzz seeds/bounds for `model_core::PreflightKtx2` and
  `model_core::ValidateDracoCounts` (both are pure and never allocate from a file-declared size).
- SEC-18: reconcile ADR-0030's "no cancel hook" wording with design/02/design/03 if a future codec
  version adds a real progress/cancel callback.
- Provider-host `[parallel]` counts a transient failure in some run orderings; make the fixture loop
  or COM apartment setup deterministic so the leak/parallel gates are not flaky.
- T04 follow-up: `ImportFormat::Usd` cannot distinguish `.usd`/`.usda`/`.usdc` from `.usdz`, so a
  `.usdz` primary would also be allowed to request local layer sidecars even though its content is
  in-archive. Harmless today (the adapter resolves entries in-archive and never asks), but if the
  wire/`requestFlags` already carries the encoding, tighten the resolver per value.
- T04 follow-up: the committed corpus manifest cannot be regenerated without the Release
  `meshoptimizer.dll`; consider committing that frozen seed path or a generator mode that copies the
  existing `meshopt.glb`, so the documented `--compare` workflow works on a clean checkout.
- T13/SEC-16/17: finish extending the `fuzz-smoke` job in `.github/workflows/ci.yml` with the
  glTF+codec and provider-soak targets, then promote it from the nightly/dispatch-only matrix into
  the required pull-request gate. SEC-15 added the STL/PLY/OBJ entries (one target per runner). It is
  deliberately not a `needs:` of the release job yet.
- SEC-15: the `fuzz-smoke` job does not restore the root vcpkg manifest before building `ObjFuzz`
  (header-only `ufbx`) or `ThreeMfFuzz` (zlib); the job relies on the runner's vcpkg integration and
  warm binary cache. If a cold runner fails the manifest restore at build time, add an explicit
  restore/tools step like the `test` job has.
- T13 (run-time evidence): the first real GitHub Actions run must confirm `windows-2025` accepts
  the AppContainer/Job child processes the import-isolation suite launches, and that the Debug and
  Release matrices fit the 360-minute timeout from a warm cache. If a suite is environment-sensitive
  on the runner, scope it explicitly and record why; do not silently skip it.
- T13 (branch protection): a maintainer must require the `CI` checks `Build and test (Debug)` and
  `Build and test (Release)` on `main`; GitHub repository settings cannot be changed from the repo.
- T14/SEC-14: the first maintainer tag run must configure the `release` environment (required
  reviewers, tags-only deployment), the `WINDOWS_CERTIFICATE_BASE64`/`WINDOWS_CERTIFICATE_PASSWORD`
  secrets, and a `v*` tag protection rule, then confirm `gh attestation verify` and
  `signtool verify /p7` succeed. The `/p7` checksum-signing step and attestation are unverified
  offline.
- T14/SEC-14: `ci.yml` still declares workflow-level `packages: write`; its `test` job is a single
  reusable gate and cannot be split by event without duplicating the Debug/Release matrix. It is not
  a write vector today (`feed-access: read` off `push`), but a future refactor could split it the
  same way `dependencies.yml` was.
- **SEC-16 finding (KTX-Software 4.4.2 ETC1S/basisu) — resolved by SEC-16b.** `TranscodeKtx2BasisImage`
  (and therefore `ImportGltf` with an embedded `KHR_texture_basisu` image) reached a deterministic
  null-deref in `basist::basisu_lowlevel_etc1s_transcoder::transcode_slice` for a two-byte mutation of
  the frozen `interactive-viewer/test-assets/basisu_sample.ktx2` ETC1S supercompression global data.
  `model_core::ValidateEtc1sGlobalData` (`shared/model-core/include/model_core/Etc1sTablePreflight.h`),
  called from `PreflightKtx2`, now replays basisu's bounded Huffman-table read and rejects the
  container before the library. See `docs/design/adr/0046-etc1s-global-data-preflight.md`.
- **SEC-16 finding (fastgltf 0.9.0 base64) — fixed.** `ImportGltf`'s
  `fastgltf::Parser::loadGltf` reached a heap-buffer-overflow in
  `fastgltf::base64::fallback_decode_inplace` (`base64.cpp:413`) for a `.gltf` data URI whose base64
  payload length is not a multiple of four. `model_core::ValidateGltfDataUri`
  (`shared/model-core/include/model_core/GltfDataUriPreflight.h`) now rejects malformed/oversized
  base64 `data:` URIs in the `GltfAdapter.cpp` simdjson preflight before `loadGltf`; the minimized seed
  `tests/fuzz/corpus/gltf/fastgltf-base64-overflow.env` is now a safe regression.
- **SEC-16/17 CI promotion — glTF done, lane still nightly.** SEC-16b added the glTF+codec entry to the
  `fuzz-smoke` matrix (one target per runner). Remaining: SEC-17 provider-soak, then promote the whole
  lane to a required pull-request gate.
- **SEC-16b (new fuzz finding, fixed): out-of-range `primitive.materialIndex`.** The promoted glTF
  corpus found `GltfAdapter.cpp` `ConvertPrimitive` indexing `asset.materials[*primitive.materialIndex]`
  for `normalTexture` without a bounds check; fastgltf keeps a material index even when the `materials`
  array is absent (a corrupted JSON key suffices), so an empty array null-derefs. Fixed by bounding the
  index against `asset.materials.size()` before the read. Real-worker regression:
  `GltfImportTests.cpp` `[gltf-import][security]`. The next fuzz drift here is to keep the Adapter
  domain's other direct `asset.*[index]` reads reviewed (they are currently guarded by fastgltf
  validation or explicit checks).
- **SEC-16b follow-up (required on a KTX/basisu bump):** re-validate the `Etc1sTablePreflight.h` port
  against the new `external/basisu` `read_huffman_table`/`decode_tables`, and rerun the promoted
  `GltfFuzz` corpus. The port is version-pinned to BASISD_LIB_VERSION 116.

## T13 — SEC-13 CI test gate for PRs and releases

Reusable facts for later sessions:

- **One reusable gate workflow.** `.github/workflows/ci.yml` triggers on `pull_request`, push to
  `main`, `workflow_dispatch`, and `workflow_call`. It is intentionally *not* path-filtered, so a
  required branch-protection check always reports. Job `test` is a `Debug`/`Release` matrix that
  builds `Preview3D.slnx` through the solution and then runs three separate steps:
  `Tests.Unit.exe "~[graphics]"`, `Tests.ImportIsolation.exe`, `Tests.ProviderHost.exe`, each failing
  the job on a nonzero exit. `[graphics]` is excluded because the hosted runner has no D3D12 device
  (the same filter as the harness verify command).
- **Release fails closed.** `release.yml` now has a `gate` job (`uses: ./.github/workflows/ci.yml`)
  and `release` declares `needs: gate`, so packaging cannot start until the tag's gate is green. The
  reusable workflow is loaded from the pushed tag's ref.
- **vcpkg restore mirrors release.yml.** The `test` job runs the same three-manifest restore (root,
  `compatibility-host-step`, `thumbnail-provider/step-occt`) with the same Actions cache key and
  GitHub Packages NuGet feed as `dependencies.yml`/`release.yml`, routes scratch to the workspace
  volume, then builds with `/p:VcpkgManifestInstall=false /p:VcpkgRoot=<root>`. The NuGet feed is
  `readwrite` on `push` and `read` everywhere else, so a fork PR restores warm packages without
  attempting a push. Keep the cache key in sync with the other two workflows.
- **Fuzz smoke is opt-in and non-blocking.** Job `fuzz-smoke` runs only on `schedule` or
  `workflow_dispatch` (never on the PR path, never a `needs:` of release). It is a two-entry matrix
  (`STEP`, `3MF`) so each target gets its own runner — the fuzz projects share
  `tests\fuzz\x64\Release` as an intermediate directory (MSB8028). Commands:
  `python tests/fuzz/prepare_step_seeds.py TestResults/step-008/fuzz-seeds` then
  `tests\fuzz\x64\Release\StepFuzz.exe <seeds> -max_total_time=60 ...`; same shape for 3MF with
  `prepare_3mf_seeds.py TestResults/3mf-007/fuzz-seeds`. SEC-15/16/17 add the remaining targets.
- **Local baseline (this checkout, prebuilt binaries).** `x64\Release\Tests.Unit.exe "~[graphics]"`:
  307 cases / 131544 assertions green; `x64\Debug\Tests.Unit.exe "~[graphics]"`: 304 / 131511 green.
  `x64\Release\Tests.ImportIsolation.exe`: 399 cases (394 passed, 5 skipped) / 293037 assertions
  green; Debug: 399 (398 passed, 1 skipped) / 293034 green. `Tests.ProviderHost.exe` Debug and
  Release: 8 cases / 142 assertions green. Fuzz smoke: `StepFuzz` built and 15 s run ended exit 0
  (18876 units, peak RSS 425 MB); `ThreeMfFuzz` builds the same way.
- **MSBuild is not on PATH locally.** Use
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`. Fuzz
  projects build standalone with `/p:SolutionDir=<repo root>\` and land in
  `tests\fuzz\x64\Release\` (Step/3MF/Usd set `OutDir`; Fbx relies on `SolutionDir`).
- **Manifest install gotcha.** Passing `/p:VcpkgManifestInstall=false` for the solution build is
  correct *only* because the three manifests are restored up front; the isolated-manifest MSBuild
  target is gated on the OCCT `Standard_Failure.hxx` header being absent, so the pre-restore makes it
  a no-op. Do not drop the restore step.

## T14 — SEC-14 Release and supply-chain hardening

Reusable facts for later sessions:

- **Permissions cannot be conditional.** GitHub Actions `permissions` (top-level or job-level) does
  not accept `${{ }}` expressions, so "read on PR, write on push" needs separate jobs. The vcpkg
  restore moved to `.github/workflows/dependencies-restore.yml` (`on: workflow_call`,
  `inputs.feed-access`); `dependencies.yml` calls it from `restore-pr`
  (`if: github.event_name == 'pull_request'`, `packages: read`, `feed-access: read`) and `warm`
  (`else`, `packages: write`, `feed-access: readwrite`). The reusable workflow must **not** declare
  `permissions:` — an unspecified permission evaluates to `none` and would strip the caller's
  `packages: write`.
- **Ephemeral NuGet credentials.** Do not `nuget sources add` without `-ConfigFile`: that persists
  the token in the user-profile `NuGet.Config`. Instead write a throwaway config under
  `$env:RUNNER_TEMP` with a `defaultPushSource` entry, add the source/API key with
  `-ConfigFile`, and consume it through vcpkg's **`nugetconfig`** source (only that provider takes
  `-Config`, per the vcpkg binary-caching reference). Set
  `VCPKG_BINARY_SOURCES=clear;nugetconfig,<cfg>,<access>;files,<dir>,readwrite` and delete the file in
  an `if: always()` step. Applied in `dependencies-restore.yml`, `ci.yml`, and `release.yml`.
  Gotcha: a PowerShell here-string (`@" ... "@`) inside a YAML `run: |` block breaks the block scalar
  because the closing `"@` must be column-0; use an array of lines instead.
  Verified locally with the vcpkg-provided NuGet 7.6.0 (`nuget-7.6.0-windows` from
  `...\vcpkg\vcpkg.exe fetch nuget`): the temp file ends up with the source,
  `packageSourceCredentials`, `defaultPushSource`, and `apikeys`, and the user-profile config is
  untouched.
- **Pinned actions (Dependabot comment form).** `actions/checkout`
  `d23441a48e516b6c34aea4fa41551a30e30af803 # v6.1.0`, `actions/cache`
  `0057852bfaa89a56745cba8c7296529d2fc39830 # v4.3.0`, `actions/attest-build-provenance`
  `e8998f949152b193b063cb0ec769d69d929409be # v2.4.0`. `.github/dependabot.yml` (github-actions,
  weekly) keeps them current. `git ls-remote --tags <url> 'refs/tags/vN*'` gives the peeled SHAs.
- **Signing is mandatory for a tag.** `release.yml`'s "Import signing certificate" throws when
  `WINDOWS_CERTIFICATE_BASE64`/`WINDOWS_CERTIFICATE_PASSWORD` are absent. The unsigned path is the
  local packaging scripts only (`Create-PortableRelease.ps1`/`Create-Installer.ps1` warn and
  continue without a thumbprint). The `release` job also has `environment: release` (required
  reviewers, tags-only deployment) and needs `id-token: write` + `attestations: write`.
- **Checksum signing is detached PKCS#7.** Authenticode cannot embed a signature in a text file, so
  `Sign release metadata` runs
  `signtool sign /sha1 <tp> /fd SHA256 /tr <ts> /td SHA256 /p7 <dir> /p7co 1.3.6.1.4.1.311.2.1.4 <checksums>`
  and verifies each output with `signtool verify /p7`. Outputs are `<file>.p7`. Untested offline (no
  cert/signtool here).
- **Draft-then-publish, no clobber.** "Publish GitHub release" refuses an already **published** tag,
  deletes a leftover draft, `gh release create --draft --verify-tag`, uploads (no `--clobber`), then
  `gh release edit <tag> --draft=false`. Assets = the four binaries/checksums plus everything in
  `artifacts/release/` (renamed SBOM/manifest copies + `Preview3D-<v>-SHA256SUMS` + `.p7` sidecars).
  The two distributions both emit `SBOM.cdx.json`/`MANIFEST.json`, so the copies are renamed.
- **Attestation.** `actions/attest-build-provenance` runs over the zip, installer, both SBOMs, both
  manifests, and `SHA256SUMS`. Verify with `gh attestation verify <file> --repo binbuf/preview-3d`.
- **Check command.** `actionlint` is on PATH
  (`C:\Users\dan\go\bin\actionlint.exe`); PowerShell does not glob for native commands, so pass
  files explicitly: `$files = Get-ChildItem .github/workflows -Filter *.yml | % FullName; actionlint @files`.
  It resolves the local `uses: ./.github/workflows/dependencies-restore.yml` reference.
- **Policy doc.** `docs/WINDOWS-SECURITY.md` "What we are doing"/"Verifying a release" is now the
  user-visible tag-protection/signing policy; `docs/design/adr/0043-signed-attested-release-supply-chain.md`
  records the decision; design/09 "CI and release evidence" links them. The README no longer says
  releases are unsigned.

## T15 — SEC-15 Fuzz targets: STL, PLY, OBJ

Reusable facts for later sessions:

- **Three targets, one convention.** `tests/fuzz/StlFuzz.cpp`, `PlyFuzz.cpp`, and `ObjFuzz.cpp`
  follow the existing STEP/3MF/USD/FBX pattern: `extern "C" LLVMFuzzerTestOneInput`, `/fsanitize=fuzzer`
  + `EnableASAN=true`, `OutDir tests\fuzz\x64\$(Configuration)\`, and a `CopyAddressSanitizerRuntime`
  post-build target. Build one at a time: the projects share `tests\fuzz\x64\Release` (MSB8028).
- **STL/PLY link the real adapters.** `StlFuzz.vcxproj`/`PlyFuzz.vcxproj` compile
  `import-worker/src/{Stl,Ply}Adapter.cpp` and `shared/parser-core/src/*` with a null `MappedFile`
  and a 4 MiB output window. `BoundedChunkWriter::AddRaw` references
  `ChunkBatchSink::PublishBatch` even when the sink is null, so the projects also compile
  `import-worker/src/ChunkBatchSink.cpp` and `shared/model-core/src/ControlChannelIo.cpp` plus
  `MappedFile.cpp`/`MappedView.cpp`. No vcpkg dependency.
- **OBJ uses ufbx directly.** `ObjFuzz.vcxproj` has `VcpkgEnableManifest=true` (header-only `ufbx`,
  root manifest) and links only `ObjFuzz.cpp`. Build with `/p:VcpkgRoot=<VS>\VC\vcpkg` locally; CI
  relies on the runner's vcpkg integration. Envelope = FbxFuzz's `(<IBBHI)` shape, plus a flag to
  parse the primary as MTL. The open-file callback serves exactly one in-memory sidecar and denies
  every other path, so hostile/missing references never touch the filesystem.
- **Generated seeds, not committed.** `prepare_{stl,ply,obj}_seeds.py` write into a fresh directory
  and refuse a non-empty one (exit 1), matching `prepare_step_seeds.py`. 12 / 12 / 9 seeds.
- **Build command** (MSBuild is not on PATH):
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe tests\fuzz\<T>Fuzz.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\" /m:1`.
  Run: `tests\fuzz\x64\Release\<T>Fuzz.exe <seeds> -max_total_time=60 -timeout=5 -rss_limit_mb=1024 -max_len=2097160 -print_final_stats=1 -verbosity=0`.
- **Smoke baseline (30 s each, exit 0, no ASan finding).** StlFuzz 41,338 units / peak RSS 335 MB;
  PlyFuzz 28,875 units / 339 MB; ObjFuzz 212,803 units / 403 MB. `Tests.Unit.exe "~[graphics]"` stays
  307 cases / 131,544 assertions green (no production code changed).
- **Not instrumented.** The OBJ adapter's texture/image decode stages (SEC-16 codecs), the
  `ChunkBatchSink` batch handshake, mapped-window streaming, and the real AppContainer/Job/product
  size caps; those stay in the provider and import-isolation lanes. See ADR-0044.
## T16 - SEC-16 Fuzz targets: glTF + compressed codecs

Reusable facts for later sessions:

- **One target, seven domains.** `tests/fuzz/GltfFuzz.cpp` (+ `GltfFuzz.vcxproj`, GUID
  `{A1B2C3D4-0004-...}`) links the real product code: `import-worker/src/GltfAdapter.cpp` and the
  Draco/meshopt/KTX2/WebP/WIC adapters, plus `ChunkBatchSink.cpp`, `SidecarFileClient.cpp`,
  `model-core/{ControlChannelIo,MappedFile}.cpp`, `platform/MappedView.cpp`. Envelope is
  `struct { uint32_t magic ("GLTZ"=0x5A544C47); uint8_t domain; uint8_t flags; uint16_t reserved; }`
  (8 bytes) + payload. Domains: `Adapter=0`, `Draco=1`, `Meshopt=2`, `Ktx2=3`, `WebP=4`,
  `WicRaster=5`, `Sniff=6`. Adapter drives `ImportGltf(payload, dest, 1, 1+(flags&0x1f), nullptr,
  nullptr, TextureDecodeOptions)` with a 4 MiB output window.
- **Linking against non-ASan vcpkg static libs.** The target must define
  `_DISABLE_STL_ANNOTATION` (in the vcxproj `PreprocessorDefinitions`) or `ktx.lib`/`simdjson.lib`
  fail `LNK2038` (`annotate_string/vector/optional` 0 vs 1). ASan still instruments product code and
  its `memcpy`/store interceptors catch some third-party over-writes. Needs the root vcpkg manifest
  (`fastgltf`, `simdjson`, `draco`, `basisu`, `ktx`, `meshoptimizer`, `libwebp`) and
  `FASTGLTF_ENABLE_DEPRECATED_EXT=1`.
- **Build/run** (MSBuild not on PATH):
  `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" tests\fuzz\GltfFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\" /p:VcpkgRoot=C:\vcpkg /p:VcpkgManifestInstall=false /m:1`
  then `python tests/fuzz/prepare_gltf_seeds.py TestResults/security-t16/gltf-seeds` and
  `tests\fuzz\x64\Release\GltfFuzz.exe TestResults/security-t16/gltf-seeds -max_total_time=60 -timeout=5 -rss_limit_mb=1024 -max_len=2097160 -print_final_stats=1 -verbosity=0`.
- **One finding fixed, one deferred (SEC-16b).** The first run crashed on the pinned decoders within
  ~20 s. `tests/fuzz/corpus/gltf/` holds the minimized seeds + README:
  `fastgltf-base64-overflow.env` (fastgltf 0.9.0 `fallback_decode_inplace` heap overflow on a
  non-multiple-of-four data-URI base64) and `basislz-etc1s-crash.env`/`.ktx2` (KTX-Software 4.4.2
  ETC1S `transcode_slice` null-deref). The fastgltf class is fixed by
  `model_core::ValidateGltfDataUri` (`shared/model-core/include/model_core/GltfDataUriPreflight.h`),
  called from the `GltfAdapter.cpp` simdjson preflight before `loadGltf`; the seed now exits 0 under
  `GltfFuzz ... -runs=1` and is a safe regression. The ETC1S class is deferred to
  `docs/tasks/security/16b-fuzz-ktx-etc1s-finding.md` (guard/decision + smoke promotion). `GltfFuzz`
  drives `PreflightKtx2` only for BasisLZ/ETC1S (no third-party ETC1S transcode), excludes the valid
  BasisLZ GLB from its generated adapter seeds, and is NOT yet in the `fuzz-smoke` matrix (blocked on
  ETC1S). Codec domain sizes: Draco header 8 bytes (u32 expectedVertices, u32 expectedIndices),
  Meshopt header 16 bytes (u32 count, u32 stride, u64 decodedByteLength), flags select
  mode/filter/semantics.
- **Do not edit the corpus manifest for these.** `interactive-viewer/test-assets/corpus/manifest.json`
  is SHA-pinned by `FixtureManifestTests`; the findings live under `tests/fuzz/corpus/gltf/` and
  `.gitattributes` marks `tests/fuzz/corpus/** -text`.
- **Verify.** The base64 guard changed product code: `GltfDataUriPreflightTests.cpp`
  (`[gltf][data-uri]`, 5 cases) is in `Tests.Unit`, and a real-worker `[gltf-import][security]` case is
  in `GltfImportTests.cpp`. `npm test` (Tests.Unit Release) is 374 cases green; the ETC1S seed still
  crashes and is owned by SEC-16b. See ADR-0045.

## T16b — SEC-16b KTX2/BasisLZ ETC1S decoder finding and GltfFuzz smoke promotion

Reusable facts for later sessions:

- **Root cause (characterized).** KTX-Software 4.4.2's `ktxTexture2_transcodeLzEtc1s`
  (`lib/basis_transcode.cpp`) calls basisu's low-level `decode_palettes`/`decode_tables` and **ignores
  their bool return**, then enters `transcode_slice` unconditionally. The minimized two-byte mutation
  (`tests/fuzz/corpus/gltf/basislz-etc1s-crash.ktx2`, offsets 109/110 of the 208-byte SGD in
  `interactive-viewer/test-assets/basisu_sample.ktx2`) corrupts the Huffman table blob so the third
  `read_huffman_table` reads `num_codelength_codes == 0`; `decode_tables` returns false, `m_selector_model`
  stays empty, and `transcode_slice` indexes its empty lookup at `basisu_transcoder.cpp:8013`. Every KTX2
  structural field is valid, so a bounds-only guard cannot catch it. A second class (found while
  validating the promoted corpus): a one-byte selector-codebook mutation (file offset 296) makes
  `decode_palettes` fail an unsupported selector-codebook variant; the same ignored-return bug leaves the
  selector objects uninitialized and `convert_etc1s_to_bc7_m5_color` indexes a `[4][4]` table out of
  bounds. Both must be rejected at preflight.
- **Pinned decoder is KTX-vendored, not the vcpkg `basisu` package.** KTX 4.4.2 vendors basisu under
  `external/basisu` (BASISD_LIB_VERSION 116); its symbols are inside `ktx.lib`. The standalone `basisu`
  vcpkg headers are a different (newer) ABI, so product code must **not** instantiate the transcoder
  class. The fix therefore re-implements the bounded table read instead of calling the library.
- **Fix.** `model_core::ValidateEtc1sGlobalData`
  (`shared/model-core/include/model_core/Etc1sTablePreflight.h`), a bounded allocation-free port of
  `bitwise_decoder` + `huffman_decoding_table::init` + `read_huffman_table`, is called from
  `PreflightKtx2` for `vkFormat==0 && supercompression==1` (KTX_SS_BASIS_LZ) before
  `ktxTexture2_CreateFromMemory`. It replays the deciding reads of both `decode_palettes` (four
  endpoint tables + selector-codebook variant flags) and `decode_tables` (four tables + 13-bit
  selector-history-buffer size), and validates the endpoint/selector counts and codebook byte-length
  sums vs `sgdLength`. Policy #1; no capability regression. See ADR-0046.
- **Second promoted-corpus finding fixed.** The real `Ktx2` domain and the promoted seeds exposed
  `GltfAdapter.cpp` `ConvertPrimitive` indexing `asset.materials[*primitive.materialIndex]` without a
  bounds check; fastgltf keeps `material` even when the `materials` array is absent (a corrupted JSON
  key suffices) so an empty array null-derefs. Fixed with a `size()` check.
- **How to build/run the target** (MSBuild not on PATH):
  `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" tests\fuzz\GltfFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\" /p:VcpkgRoot=C:\vcpkg /p:VcpkgManifestInstall=false /m:1`
  then `python tests/fuzz/prepare_gltf_seeds.py TestResults/security-t16b/gltf-seeds` (38 seeds) and
  `tests\fuzz\x64\Release\GltfFuzz.exe TestResults/security-t16b/gltf-seeds -max_total_time=60 -timeout=5 -rss_limit_mb=1024 -max_len=1048576 -print_final_stats=1 -verbosity=0`.
- **Seeds promoted.** `prepare_gltf_seeds.py` now reads `tests/fuzz/corpus/gltf/` (`fastgltf-base64-overflow.env`,
  `basislz-etc1s-crash.env`, `basislz-etc1s-crash.ktx2`) and re-adds the valid `basisu_textured_triangle.glb`;
  the `Ktx2` domain drives the real ETC1S transcode for every accepted container. `GltfFuzz` is added to
  the `fuzz-smoke` matrix (`ci.yml`, target `glTF`, one target per runner). `Tests.ImportIsolation.vcxproj`
  defines `PREVIEW3D_FUZZ_CORPUS_DIR` so the regression tests read the minimized seeds.
- **Build caveat.** The fuzz projects share `tests\fuzz\x64\Release`; build one at a time (`/m:1`) or on a
  fresh runner (MSB8028). The ASan runtime DLL is copied next to the exe by the project.
- **Verify (all green).** `Tests.Unit.exe "~[graphics]"` → 312 cases / 131,557 assertions;
  `Tests.ImportIsolation.exe "[texture-transcode]"` → 7 cases; `"[gltf-import][security]"` → 3 cases;
  bounded `GltfFuzz` smoke over 38 seeds, 60 s, 55,945 units, exit 0, no ASan finding (a separate 180 s
  run did 187,491 units clean). The worker, thumbnail provider, fuzz target, unit and import-isolation
  projects all rebuilt Release.

## T18 — SEC-18 Reconcile design docs with implemented controls

Reusable facts for later sessions:

- **Docs-only task.** No product code or tests changed; the harness verify is still
  `x64\Release\Tests.Unit.exe "~[graphics]"` → 312 cases / 131,557 assertions, all pass.
- **The audit's divergences partly self-resolved.** Four of the nine Context bullets were already
  fixed by the code tasks before T18: safe DLL search for every child (SEC-09/T09,
  `import-worker/src/main.cpp::HardenProcessDiscovery`), compiled-out developer/fault switches
  (SEC-09 + SEC-11), the fuzz targets (SEC-15/16/16b/17), and CPU-time limits (SEC-03 gave ADR-0031).
  T18 only reconciled the prose that was still stale.
- **The broker objects really are handle-only.** `shared/import-broker/src/{WorkerPool,ImportSession,
  SharedSection}.cpp` use empty `SECURITY_ATTRIBUTES`; no per-object DACL exists. Design/08 no longer
  claims per-generation ACLs. See ADR-0048.
- **The worker pool is session-scoped, not per-generation.** `ImportSession.cpp` `PrepareAsync` sets
  `limits.processCpuTimeLimitMs = 0` with a comment that a cumulative cap would kill a reused worker;
  `PrepareNow` (compatibility/STEP hosts) sets `kImportProcessCpuTimeLimitMs`. Design/02 wording now
  matches (one profile/Job per session; per-generation wall-clock deadline instead).
- **DirectXTex is not in the tree.** `vcpkg.json` has no DirectXTex dependency and there is no
  DirectXTex source; only `import-worker/src/WicImageDecodeAdapter.cpp` (inbox WIC) and libwebp
  exist. T18 dropped the claim from design/01,02,03,08,09,10,11 and recorded TGA/HDR/DDS as
  unsupported. Recorded in ADR-0048; matches `docs/FORMAT-SUPPORT.md`.
- **New ADR-0048** (`docs/design/adr/0048-handle-only-broker-objects-worker-reuse-and-directxtex-drop.md`)
  is the binding record for the handle-only model, session-scoped pool, and no-DirectXTex. A future
  task that wants a per-generation ACL, a CPU cap on the reused worker, or DirectXTex must supersede
  it and re-justify against ADR-0031/ADR-0005.
- **Security invariants are now in design/09's controls**: NUL/control + invalid-UTF-8 reference
  rejection (ADR-0032), validate-before-decode preflights (ADR-0030/ADR-0046), provider AV
  quarantine (ADR-0037), and an explicitly mandatory CI merge gate (ADR-0042). `SECURITY.md` scope
  now names NUL/control reference handling and provider-surrogate escapes.
- **Broken link fixed:** `SECURITY.md` pointed at `.docs/FORMAT-SUPPORT.md`; the real file is
  `docs/FORMAT-SUPPORT.md`.
- **Legacy docs are deliberately untouched.** `docs/legacy/**` still mentions DirectXTex; the task
  out-of-scope says not to rewrite the legacy baseline. Grep hits there are expected, not drift.

## T19 — SEC-19 License/SBOM/dependency metadata

Reusable facts for later sessions:

- **One generator, one mapping.** `packaging/ReleaseMetadata.ps1` holds the installed-closure
  parser (`Get-InstalledDependencyClosure`), the fail-closed SPDX gate
  (`Assert-LicenseMappingCoversClosure`), the manifest-root check
  (`Assert-ManifestRootsPresent`), the version gate (`Assert-ReleaseVersionConsistency`), and the
  doc check (`Assert-LicensesDocCoversMapping`). The reviewed SPDX table is
  `packaging/portable/dependency-licenses.json` (22 target packages; host build tools
  `vcpkg-cmake`/`vcpkg-cmake-config` are excluded by architecture). Adding a port fails packaging
  until its SPDX expression is added here.
- **Three vcpkg trees, one closure.** `Get-VcpkgInstallTree` reads the root tree
  (`vcpkg_installed\x64-windows-static-md\{vcpkg\status, x64-windows-static-md\share}`), the
  dedicated STEP host (`compatibility-host-step\vcpkg_installed\...`), and the provider STEP tree
  (`thumbnail-provider\step-occt\vcpkg_installed\...`). Only packages whose status `Architecture`
  is `x64-windows-static-md` enter the closure; `copyright` is read from the matching tree's
  `share\<pkg>\copyright`.
- **SBOM is generated, not a list.** `Create-PortableRelease.ps1` emits one CycloneDX component per
  closure package with `licenses[].expression` from the mapping plus
  `preview3d:{vcpkg-baseline,architecture,license-file,vcpkg-abi}`; OCCT now comes from its own tree
  (`7.8.1#1`), not a special case. `THIRD-PARTY-NOTICES.txt` is a template whose `@LICENSE_LIST@`
  is filled from the same closure, so the index and `licenses\` cannot disagree.
- **Version cannot drift.** `vcpkg.json` (`0.5.0`, was `0.1.0`), `Directory.Solution.targets`, and
  `Preview3D.nsi` `PRODUCT_VERSION` must match, and the packaging `-Version` argument is checked
  against them in both packaging scripts.
- **The packaging self-test is a Test.Unit case.** `tests/unit/ReleaseMetadataTests.cpp`
  (`[security][release-metadata]`) shells out to
  `pwsh packaging/Test-ReleaseMetadata.ps1 -RepositoryRoot <repo> -SelfTest`; the script's
  `-SelfTest` proves an unmapped installed dependency and a version mismatch both fail, then runs
  the real checks. It needs `pwsh` 7 and at least the root vcpkg tree present (CI restores all
  three). `PREVIEW3D_REPO_ROOT` is baked into `Tests.Unit.vcxproj` from `$(SolutionDir)`.
- **OpenUSD warning scope.** `compatibility-host\Preview3DOpenUsdCore.vcxproj` no longer suppresses
  `4244;4305` project-wide (only `4100;4201` remain). `OpenUsdHost.cpp` wraps its `pxr` includes
  and its whole body in `#pragma warning(push)` + `disable: 4244 4305` ... `pop`; every emitted
  site is upstream `pxr/base/gf/*` / `pxr/base/arch/timing.h`, audited. Rebuild it Release: exit 0.
- **Verified.** `packaging\Test-ReleaseMetadata.ps1 -SelfTest` exit 0 (22-package closure fully
  mapped). A full `Create-PortableRelease.ps1 -Distribution Portable -Version 0.5.0` run (unsigned)
  staged 106 files, generated 23 SBOM components (22 closure + CRT), and 22 `licenses\*.txt`
  including `fast-float.txt`. Harness verify `x64\Release\Tests.Unit.exe "~[graphics]"`: 313 cases
  / 131559 assertions, all pass.
- **ADR.** [ADR-0049](../design/adr/0049-generated-sbom-and-license-metadata.md). Design/08
  "Signing and supply chain" now describes the generated inventory.

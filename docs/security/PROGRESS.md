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
- **Follow-ups**: SEC-17: the provider-host allocator seam only fails C++ allocations; add a soak/fuzz lane that; `ThreeMfFamilyAdapter.h` still declares `ErrorCode ScanRequiredExtensions();` with no definition
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

## Follow-ups

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
# Security hardening progress notes

Shared notebook for the `security` task set. Earlier sessions record findings, decisions, and
things later tasks must know here; the harness maintains the "Key facts" digest at the top.

<!-- symphony:digest:start -->
## Key facts (maintained by symphony — do not edit)

- **T01 — SEC-01 Bound glTF traversal and fix worker limit ordering**: Reusable facts for later sessions:; **glTF node walk budget.** `import-worker/src/GltfAdapter.cpp` `WalkState::totalNodeVisits` is
- **T02 — SEC-02 Preflight compressed decoders before allocation**: Reusable facts for later sessions:; **Shared preflights.** `shared/model-core/include/model_core/DracoPreflight.h` and
- **Follow-ups**: Consider a large, opt-in (`[.]`) qualification fixture that drives the real `--parse-usd` worker; PLY `meshIndices.reserve(vertexElement->count * 3)` is safe only because `vertexElement->count` is
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

## Follow-ups

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
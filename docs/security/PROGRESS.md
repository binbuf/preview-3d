# Security hardening progress notes

Shared notebook for the `security` task set. Earlier sessions record findings, decisions, and
things later tasks must know here; the harness maintains the "Key facts" digest at the top.

<!-- symphony:digest:start -->
## Key facts (maintained by symphony — do not edit)

- **T01 — SEC-01 Bound glTF traversal and fix worker limit ordering**: Reusable facts for later sessions:; **glTF node walk budget.** `import-worker/src/GltfAdapter.cpp` `WalkState::totalNodeVisits` is
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

## Follow-ups

- Consider a large, opt-in (`[.]`) qualification fixture that drives the real `--parse-usd` worker
  with a >20M-triangle UV-bearing USDA and asserts `ResourceLimit` before the expansion reserve.
  Deliberately not added now: the fixture is ~150-200 MB and risks the 120 s worker reply timeout.
- PLY `meshIndices.reserve(vertexElement->count * 3)` is safe only because `vertexElement->count` is
  already bounded by the Tier B scratch budget; switching it to `platform::CheckedMultiply` would be
  a defensive cleanup, not a fix.
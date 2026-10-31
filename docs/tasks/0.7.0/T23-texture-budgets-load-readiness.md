---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T23 --configuration Debug --output TestResults/render-quality/T23
---
# T23 — Texture-budget accounting and complete-load readiness

## Goal

Open legitimate textured assets within the existing safety envelope, with honest final load status and predictable lower-detail behavior when full texture quality cannot fit.

## Context (read first)

- Depends on **T20, T21, T22**; execute in roadmap order.
- `TestResults/render-quality-review/preview-microscope-resource-limit.png` — fresh late ResourceLimit after visible geometry; this is a failed load, not a qualified Clay/Directional image.
- `GltfAdapter.cpp` texture decode/dedup, aggregate counters and progressive mip packaging; texture budget constants, broker/session status and shared-section emission.
- `tests/app-smoke/studio.py::wait_for_render`, T20 settled-capture contract and existing memory/security ADRs.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Reproduce the microscope late ResourceLimit with an instrumented Debug import/capture. Identify the exact producer/validator/broker limit and peak allocation; distinguish decoded pixel/byte totals, mip chains, image reuse across semantics, progressive duplicate copies, catalog/section limits, worker commit and GPU residency. Do not infer the cause from the generic overlay or compressed file size.
- [ ] Add bounded typed diagnostics for the limiting resource and requested/allowed quantities, with paths/model data excluded from Release logs. Make the user-facing degradation/failure state accurately reflect the stage and surviving geometry; a previously visible model cannot be treated as successful qualification after a terminal error.
- [ ] Fix duplicate charging/copying or decode-order dependence where present. Reuse immutable data safely, preserve distinct sRGB/data-map semantics and account for all simultaneously resident coarse/full mip allocations. Test shared images through multiple texture slots/materials.
- [ ] When optional texture quality exceeds budget, prefer a bounded lower-resolution mip decode or explicit missing-texture degradation if allowed by the established policy; geometry and safety limits remain hard. Plan sizes before allocations, use overflow-safe accounting, and preserve the worker Job/AppContainer/broker limits.
- [ ] Do not simply raise aggregate limits or bypass the sandbox. A necessary public-policy change requires measured legitimate/adversarial evidence and an ADR, with unchanged finite bounds and hostile cases. Avoid a full-resolution decode whose peak defeats the proposed lower-resolution fallback.
- [ ] Make capture readiness require terminal successful/degraded import, expected scene generation and required geometry/texture residency plus settled frames. Detect late errors after initial geometry; replace brightness-only visibility as a completion test.
- [ ] Add generated texture-heavy/reused-image/late-failure fixtures and boundary cases just below/above each touched limit; run malformed/oversized/decompression-bomb/cancellation tests. Review microscope, Camera and Car after the change and record final image/compatibility status.

## Verification

The front-matter command is a planned interface introduced by T20. Extend
its profile in this task; unimplemented profiles must fail. The wrapper must
run the task's meaningful unit/import checks and actual GPU image lane,
propagate failures and save exact commands/results. Build changed binaries
first. Keep import-worker suites serial. Image review and explicit reference
promotion are separate from ordinary verification; source-string checks and
self-created goldens do not prove rendering quality.

### Verification economy

The front-matter gate launches a real GPU viewer and takes minutes. Spend runs
deliberately:

- Run it in the foreground with an explicit long timeout (900000 ms or more).
  Do not background it and poll with short `Start-Sleep` loops; if the tool
  moves it to the background, wait once for completion instead of polling.
- Iterate with `--fast` (add `--no-build` once the binary is current). `--fast`
  keeps the build, generated fixtures and this task's focused lane, and skips
  the contract self-tests, external corpus, F3D references and review gate. It
  is iteration evidence, not acceptance evidence.
- Keep agent-run full gates to at most two: one early proof of the lane and
  one final after the last edit. The harness repeats the front-matter command
  once after you finish. Corpus or baseline re-promotion is one run, not a
  per-fix loop.
- Use focused unit/import filters between runs. Do not re-run a passing gate to
  "confirm" it, and do not repeat a failing configuration without a change.

## Done when

- [ ] T23 verification includes exact-limit reproduction, budget-accounting/hostile import tests and success/degradation/late-error capture-contract tests.
- [ ] The microscope has a measured, truthful disposition and complete settled captures if it can open safely; an unexplained refusal or raised limit without evidence is not completion.
- [ ] Update texture-budget/readiness/compatibility documentation and hand-off; retain first-failure evidence.

## Out of scope

Weakening isolation, unbounded texture decodes, accepting partial geometry as a completed load, or promising every arbitrarily large model will open.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

### Measured disposition (first failure)

Instrumented Debug probes opened `vintage_microscope_4k.gltf` and polled the
Debug test-control fields. The first failure was **`ImportStage::GenerationDeadline` (17) with
`ImportErrorCode::ResourceLimit` (2) at ~301 s**, while the model was already
visible and complete. At failure: textures `2048 px / 4 / 12 mips`,
`gpuBytes 134,742,016`, `targetBytes 1,023,410,176`, 20,631 triangles. The
limiter was therefore the broker's absolute 300 s generation wall clock applied
to an **idle background detail replay**; it was not a decoded byte/pixel, mip,
catalog, worker-commit or GPU-residency cap. The plan-time screenshot remains
`TestResults/render-quality-review/preview-microscope-resource-limit.png`;
instrumented evidence is under `TestResults/render-quality/T23-dev/` and
`T23-review-dev/` (ignored local evidence).

### Changes

- `shared/model-core/include/model_core/ControlProtocol.h`: `EndDetail = 24`
  (host → worker, empty payload).
- `import-worker/src/ChunkBatchSink.h`, `GltfImportWorker.cpp` behavior: a
  clean `EndDetail` leaves the replay loop and returns to the pool; the
  generation was already reported.
- `shared/import-broker/.../ImportSession.h` / `ImportSession.cpp`: bounded
  replay lifecycle (`initialDetailGraceMs` 60 s, `detailIdleGraceMs` 1.5 s,
  `maxDetailRequestsPerGeneration` 4096). Idle/cap end sends `EndDetail` and
  returns `Completed`; an outstanding reply still fails at the absolute
  deadline. New result evidence: `detailRequestCount`, `detailEndedByIdle`.
- `import-worker/src/GltfAdapter.cpp`: identical image sources (URI,
  bufferView or content hash) with the same semantic/color space reuse one
  pending decode (`ImageSourceIdentity`); detail replay skips image decode;
  typed `TextureBudgetDiagnostic` (resource/requested/allowed/decoded totals/
  scratch) emitted value-only in Debug and silent in Release; overflow-safe
  byte/pixel checks.
- `interactive-viewer/src/app/Preview3D.cpp`: a later **successful** completion
  after the coarse handoff is a no-op; a later **failure** is still surfaced.
- `tests/render-quality/viewer.py`: readiness now requires expected generation,
  terminal complete/degraded, geometry/texture residency, no pending detail,
  settled pixels/quiet counters; raises on a terminal error before or after the
  first visible geometry. New `wait_complete_hold()`.
- `tests/render-quality/texture_budget.py` + `profiles/T23.json` +
  `review/T23-review.json` + `fixtures.py::texture_reuse` + `verify.py` T23
  branch: serial import lanes, reused-image fixture, microscope hold,
  Camera/Car survey, and an explicit import-test build.
- Docs: `docs/design/adr/0081-texture-budget-accounting-and-complete-load-readiness.md`,
  `docs/design/03-file-formats-and-ingestion.md`,
  `docs/design/04-rendering-and-streaming.md`,
  `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`, `docs/0.7.0/progress/T23.md`.

No aggregate limit was raised and no isolation was weakened. Divergence from a
pure texture-budget fix: the measured limiter was the detail-replay lifetime, so
the fix is there; texture dedup/accounting and the readiness contract are the
other halves of the scope.

### Checks (exact)

- Build: `MSBuild.exe Preview3D.slnx /t:Preview3D /p:Configuration=Debug /p:Platform=x64 /m` — pass.
- Build: `MSBuild.exe tests/import-isolation/Tests.ImportIsolation.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=<repo>\ /m` — pass.
- `x64\Debug\Tests.ImportIsolation.exe "[texture-budget],[detail-budget]"` — pass, 259 assertions / 6 cases.
- `x64\Debug\Tests.ImportIsolation.exe "[detail-budget],[coarse-proxy]"` — pass, 2796 assertions / 9 cases.
- `x64\Debug\Tests.ImportIsolation.exe "[compressed]"` — pass, 43 assertions / 3 cases (decoded/compressed boundary).
- `x64\Debug\Tests.ImportIsolation.exe "[shared-section-validator]"` — pass, 97 assertions / 26 cases, including the T23 exact-cap boundary (at `kMaxAggregateTextureBytes`/`Pixels` accepted, one unit above rejected).
- Focused reused-image case: `[texture-budget]` — pass, 36 assertions / 2 cases.
- Capture-contract negative self-tests: 13/13 scenarios pass, including
  `late-terminal-error-after-geometry`, `complete-with-terminal-error`,
  `complete-with-pending-detail`, `bright-but-incomplete-capture` and the
  positive `terminal-resident-load-accepted`.
- Acceptance: `python tests/render-quality/verify.py --task T23 --configuration Debug --output TestResults/render-quality/T23` — **pass**, 8 captures, external qualification available, reference available, 0 failures, review recorded. Contract self-tests pass; import-test build pass.
- Measured in the acceptance run: reused-image fixture reports **2** resident textures (128 px, 8 mips); microscope **3 × 2048 px / 12 mips**, GPU **112,263,168** vs target **1,023,410,176**, load ~5.5 s, **held complete with errorCode 0**; Camera and Car sampled complete/error-free.
- Before/after on the microscope: 4 resident textures / 134,742,016 GPU bytes → 3 / 112,263,168.

### Images / review

`tests/render-quality/review/T23-review.json` records the inspected acceptance
and review-run images: complete textured microscope (no overlay), reused-image
sphere showing both the color and data maps, Camera dark lens with reflection/
depth, and Car complete with the known T26/T27 glass speckle/ordering. No
reference promotion is made.

### Remaining work / next task notes

- After the bounded replay ends, newly visible regions use the resident coarse
  proxy until the model is reopened; re-opening the detail service on a later
  significant orbit is a follow-up (recorded in progress/T23.md).
- T34/T35 should repeat the hold on a low-GPU-budget/UMA device: a smaller
  detail target may shed texture detail (bounded degradation) but must still
  never surface as a terminal ResourceLimit.
- Car windshield/side-glass speckle and ordering remain T26/T27.


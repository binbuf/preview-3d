---
timeoutMin: 240
verify: python tests/render-quality/verify.py --task T34 --configuration Debug --output TestResults/render-quality/T34
---
# T34 — Complete corpus, cross-format and hostile-input render qualification

## Goal

Demonstrate that the improved renderer works across the actual model corpus and retains the project's safety and lifecycle guarantees.

## Context (read first)

- Depends on **T20–T33**; execute in roadmap order.
- T20 manifest/capture runner and the entire external test-model directory.
- `tests/compat/manifest.json`, T19 qualification gaps, import isolation/fuzz/hostile-worker suites.
- New wire/material/pass/cache/resource contracts from T22–T33.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [x] Complete the first-open and controlled all-model×Studio/Clay/Directional matrix from T20: GLB/glTF, STL, textured OBJ/MTL, STEP and USDZ. Include front/oblique/grazing views, targeted close-ups and orbit sequences. Record typed open/degradation failures and reference-engine unsupported cases explicitly; none become successful image skips.
- [x] Extend the checked-in portable compatibility manifest beyond T01's original families using generated/licensed fixtures for the formats added in 0.7.0. Keep external model paths/hashes in a local-corpus layer; preserve sidecar brokering and do not check in restricted models/screenshots.
- [x] Add focused adversarial import/wire/fuzz coverage for the new UV/material/texture fields, sampler/descriptor counts, NaN/Inf, oversized dimensions/mips, invalid dependencies and protocol/cache mismatches. Reject hostile data before GPU resource creation.
- [x] Exercise render lifecycle with each new pass: rapid open/cancel, mode changes, resizing/DPI, coarse-to-full textures/geometry, stale generations, low budget, allocation failure, device loss and reopen. Confirm test controls remain Debug-only and parser/codec dependencies remain outside the trusted viewer.
- [x] Review normals, topology, tessellation/creases, units/up axis, instancing and presentation per format. File or fix concrete normalized-scene/import defects found by images; do not hide import problems through exposure, material-name exceptions or blanket normal changes.
- [x] Run relevant worker suites serially; preserve first-failure logs and diagnose flakes rather than counting only a passing rerun. Add measured per-pass GPU/CPU/memory summaries with hardware/build identity and existing-budget comparisons.

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

- [x] T23 verification covers the complete external visual matrix, portable compatibility additions and the changed security/lifecycle lanes. *(T23 here is a typo for T34; the accepted lane is T34's front-matter gate, `TestResults/render-quality/T34/report.json`.)*
- [x] Every model/mode has reviewed images or a release-blocking typed disposition; Camera lens and Car glass/paint remain mandatory paired regions.
- [x] Update compatibility/corpus/security/rendering docs and hand-off with unresolved items clearly named.

## Out of scope

New format families, weakening broker/sandbox limits to improve open rate, or substituting the synthetic sphere grid for the external matrix.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Accepted 2026-11-01 through the T34b completion lane. The implementation landed
in **6ea6b5a** (22 files, 2,140 insertions, 82 deletions):

- `tests/render-quality/profiles/T34.json`, `full_corpus.py` and the `verify.py`
  T34 branch: 24-root corpus audit, portable cross-format lane, hostile inputs,
  lifecycle, Release-only controls and dependency closure, and measured cost.
- `tests/compat/manifest.json`: portable entries for STEP, FBX (binary,
  hierarchy, NURBS-required failure and layered-texture degradation), USD/USDZ
  (mesh, cube, subdivision crease and points), 3MF (core, textured materials and
  required-slice failure), textured OBJ and the existing IGES/BREP/XBF/FCStd/
  Assimp fixtures.
- Generated/committed fixtures `tests/fixtures/3mf/{core-box,materials-texture,
  production-boxes}.3mf`, `tests/fixtures/fbx/cube-binary.fbx` and
  `tests/fixtures/usd/cube.usdz`.
- Hostile oversized-image and NaN/Inf-UV GLB generation, 3MF/FBX/USD fixture
  cases, shared-validator texture coverage and the OBJ missing-MTL neutral
  default-material fix.
- `tests/render-quality/review/T34-review.json`: image review record naming the
  inspected external and portable images and the carry-forward defects.

### Deviations added by T34b

1. **Corpus-root accounting (required fix).** `corpus.is_corpus_entry()` now owns
   the single root-entry rule: leading-dot entries (`.git`, `.DS_Store`) and OS
   directory metadata (`desktop.ini`, `Thumbs.db`, `ehthumbs*.db`,
   `Thumbs.db:encryptable`, `$RECYCLE.BIN`, `System Volume Information`) are not
   corpus assets. `corpus.inventory()` and `full_corpus._external_audit()` both
   use it. No per-asset-name exceptions were added and `maxEntryBytes` is
   unchanged. Three focused checks in the T34 lane build a mixed
   metadata-plus-real-entry root, prove the real entry is still counted, prove
   zero unaccounted entries, and prove an omitted real entry is still reported.
2. **Release probe correction.** T34's first acceptance run failed only because
   `_release_probe` launched `Release/Preview3D.exe` with the Debug-only
   `--app-smoke` flag, which correctly exits 2 on a shipping binary. The probe
   now asserts that usage rejection explicitly, then launches Release normally
   and requires control fields 0/105/107/300 to return 0. No Release
   test-control authority was added.

The first-failure evidence is retained at
`TestResults/render-quality/T34-gate-first-failure.log` and
`TestResults/render-quality/T34-first-failure-report.json`; the only failure was
the Release probe above. No failure was converted into a `--fast`/capture-only
pass, a skip, a self-created golden or a relaxed threshold.

### Exact commands and results

The wrapper builds changed binaries, so the acceptance command is exactly:

```powershell
python tests/render-quality/verify.py --task T34 --configuration Debug --output TestResults/render-quality/T34
```

Result: **exit 0**, `status: pass`, `failures: []`, `fast: false`,
`captureOnly: false`, 74/74 full-corpus checks and 226 captures. Builds run by
the wrapper: Debug `Preview3D.slnx` (`/p:Configuration=Debug /p:Platform=x64`),
Debug `Tests.Unit.vcxproj` and `Tests.ImportIsolation.vcxproj` with
`/p:SolutionDir=<root>\`, and Release `Preview3D.slnx`. The serial lanes
(`[wire-format]`, `[cache]`, `[format-catalog]`, `[compat]`,
`[shared-section-validator]`, `[hostile-worker]`, `[texture-budget]`,
`[texture-progressive]`, `[fuzz]`) all exited 0.

Iteration evidence (not acceptance): `T34-dev` `--fast` run 47/47 checks and the
`T34-capture` capture-only run (221 external/generated captures). They are
supporting evidence only; acceptance is the full report above.

### Accepted matrix and evidence

- `externalQualification.status: available`, `rootEntryCount: 24`,
  `unaccountedRootEntries: []`. The 19 normal external assets are captured or
  typed-dispositioned (18 captured; mint-swimsuit FBX is a typed
  `MalformedData` open-failure with a retained failed-load image), and the five
  size-capped assets (backrooms-vr, free-honda, RAM+OBJ, space_ship, Voron STEP)
  are typed `resource-budget` dispositions.
- Portable lane: `portable-fbx-binary`, `portable-fbx-hierarchy`,
  `portable-usdz`, `portable-usd-crease`, `portable-3mf`, `portable-obj` and
  `portable-step` all captured in Studio/Clay/Directional.
- Car windshield/paint and Camera lens/lettering each have Studio, Clay and
  Directional images; all 12 mandatory-region checks pass.
- Hostile inputs: oversized image degrades to a bounded fallback before upload;
  NaN/Inf UV is rejected as a typed protocol error (code 3, stage 15) and never
  becomes a complete image.
- Lifecycle: rapid open/cancel, stale generation, three modes, 1600x1000 and
  1024x768 resize, 96/144/192 DPI, coarse-to-full, eviction, device-loss
  recovery and reopen; 32 MiB low-budget open ends in a typed `ResourceLimit`
  and a fresh normal open still succeeds.
- Release controls: `--app-smoke` exit 2; fields 0/105/107/300 all 0. Release
  dependency closure contains no denied parser/codec library.
- F3D 3.5.0 references rendered for Car and Camera in shipped-preset and
  controlled conditions (`T34/reference/`, status `available`); they are
  qualitative references, not accepted targets.

Evidence paths: `TestResults/render-quality/T34/images/` (226 captures),
`TestResults/render-quality/T34/captures.json`, `.../report.json`,
`.../full-corpus-report.md`, `.../diagnostics/full-corpus.json`,
`.../external/matrix.json`, `.../corpus-inventory.json`, and the review record
`tests/render-quality/review/T34-review.json`.

### Measured cost

The performance summary covers 31 captured assets with the environment identity
(Windows 11 10.0.26300, NVIDIA RTX 4080 driver 32.0.16.1714, Debug build). Car:
18.0 s load, 170.9 MB of a 1.02 GB detail target (0.167), mean/p95 frame
5.03/6.32 ms. Camera: 8.28 s, 239.9 MB (0.234), 6.46/8.51 ms. Portable STEP:
2.95 s, 100.3 MB (0.098), 5.46/4.64 ms. These are workstation Debug
measurements, not reference-hardware claims.

### Remaining defects (T35 must not lose)

- Mint-swimsuit FBX fails with typed `MalformedData` at section validation; it
  is a recorded failure image, not a successful model/mode capture.
- Food-collection fitted framing is very small for its extreme flat aspect
  ratio; presentation limitation, not a material defect.
- Car window/trim authored speckle remains visible in Studio/Directional.
- F3D 3.5.0 is a qualitative reference: its preset is DPI-aware and its Camera
  lens is a pale/gray disk, which is not an accepted target.
- The local corpus is restricted: models, screenshots and F3D renders stay under
  `TestResults/`; only the portable fixtures/manifest are committed.


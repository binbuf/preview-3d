---
timeoutMin: 300
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

- [ ] Complete the first-open and controlled all-model×Studio/Clay/Directional matrix from T20: GLB/glTF, STL, textured OBJ/MTL, STEP and USDZ. Include front/oblique/grazing views, targeted close-ups and orbit sequences. Record typed open/degradation failures and reference-engine unsupported cases explicitly; none become successful image skips.
- [ ] Extend the checked-in portable compatibility manifest beyond T01's original families using generated/licensed fixtures for the formats added in 0.7.0. Keep external model paths/hashes in a local-corpus layer; preserve sidecar brokering and do not check in restricted models/screenshots.
- [ ] Add focused adversarial import/wire/fuzz coverage for the new UV/material/texture fields, sampler/descriptor counts, NaN/Inf, oversized dimensions/mips, invalid dependencies and protocol/cache mismatches. Reject hostile data before GPU resource creation.
- [ ] Exercise render lifecycle with each new pass: rapid open/cancel, mode changes, resizing/DPI, coarse-to-full textures/geometry, stale generations, low budget, allocation failure, device loss and reopen. Confirm test controls remain Debug-only and parser/codec dependencies remain outside the trusted viewer.
- [ ] Review normals, topology, tessellation/creases, units/up axis, instancing and presentation per format. File or fix concrete normalized-scene/import defects found by images; do not hide import problems through exposure, material-name exceptions or blanket normal changes.
- [ ] Run relevant worker suites serially; preserve first-failure logs and diagnose flakes rather than counting only a passing rerun. Add measured per-pass GPU/CPU/memory summaries with hardware/build identity and existing-budget comparisons.

## Verification

The front-matter command is a planned interface introduced by T20. Extend
its profile in this task; unimplemented profiles must fail. The wrapper must
run the task's meaningful unit/import checks and actual GPU image lane,
propagate failures and save exact commands/results. Build changed binaries
first. Keep import-worker suites serial. Image review and explicit reference
promotion are separate from ordinary verification; source-string checks and
self-created goldens do not prove rendering quality.

## Done when

- [ ] T23 verification covers the complete external visual matrix, portable compatibility additions and the changed security/lifecycle lanes.
- [ ] Every model/mode has reviewed images or a release-blocking typed disposition; Camera lens and Car glass/paint remain mandatory paired regions.
- [ ] Update compatibility/corpus/security/rendering docs and hand-off with unresolved items clearly named.

## Out of scope

New format families, weakening broker/sandbox limits to improve open rate, or substituting the synthetic sphere grid for the external matrix.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


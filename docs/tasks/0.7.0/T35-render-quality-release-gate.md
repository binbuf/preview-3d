---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T35 --configuration Debug --output TestResults/render-quality/T35
---
# T35 — Independent visual release gate and final 0.7.0 evidence

## Goal

Close the follow-up only when every default mode has demonstrated image quality and the release checks support the final implementation.

## Context (read first)

- Depends on **T20–T34**; execute in roadmap order.
- `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`, original qualification and all new hand-offs.
- T20 immutable reference/promotion/review contract and T34 completed corpus reports.
- Build/package/dependency/installer checks used by T19.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Include T21a's appearance gate: Photos-style dark by default, stable
  chosen background across exposure/mode/preset/model changes, persisted
  custom color and independent highlight/shadow/grid behavior.
- [ ] Run the consolidated rendering gate against reviewed references without a baseline-update flag. Include numerical material invariants, local glass/highlight/normal/contact/edge regions, orbit stability and all external-model modes. Prove missing references, unreviewed candidates, blank captures and empty test selections fail.
- [ ] Perform an image-capable review of actual first-open and controlled before/after/reference images with reviewer/tool identity, asset/view/mode and concrete defect notes. No unresolved high-severity glass/order/visibility/normal/exposure/aliasing defect may remain in any default mode; a text-only summary is not visual acceptance.
- [ ] Recheck Camera lens quality explicitly while assessing Car windshield, side windows, headlights and paint. Record where we intentionally differ from F3D and why Khronos/authored semantics support it. Do not require pixel equality with an engine/version that ignores an extension.
- [ ] Build the final Debug and Release configurations as required; run unfiltered unit/import-isolation suites and changed app-smoke/streaming/recovery/picking/package tests serially. Run parser dependency closure, Release test-control rejection, installer/catalog/SBOM/license checks. Do not substitute pre-change binaries.
- [ ] Report per-mode GPU/CPU p50/p95, memory, first-open and fallback costs on identified hardware against T20 and existing product budgets. If reference hardware is unavailable, label the remaining claim unavailable and preserve the existing performance gate.
- [ ] Write a new dated render-quality qualification report with exact source/binary/reference/corpus identities, commands and results, image/review manifests, known approximations and deferred features. Keep T19's historical evidence intact and link the new report from the task-set docs.
- [ ] Only mark this extension complete after its requirements are met. If a visual inspection capability or required corpus is unavailable, report needs_visual_review/incomplete through the runner and use the harness's truthful continuation/blocked process; never bless missing evidence.

## Verification

The front-matter command is a planned interface introduced by T20. Extend
its profile in this task; unimplemented profiles must fail. The wrapper must
run the task's meaningful unit/import checks and actual GPU image lane,
propagate failures and save exact commands/results. Build changed binaries
first. Keep import-worker suites serial. Image review and explicit reference
promotion are separate from ordinary verification; source-string checks and
self-created goldens do not prove rendering quality.

## Done when

- [ ] T34 verification runs the complete gate and required suites, rejects unreviewed/self-generated baselines, and produces a coherent final evidence package.
- [ ] Studio, Clay and Directional each have a completed quality rubric; no open mandatory regression is hidden behind a passing aggregate number.
- [ ] Update task-set status, rendering/material/support docs and hand-off, preserving actual limitations and original task history.

## Out of scope

New feature scope, automatic baseline refresh to make the release pass, or changing the user's Symphony provider/notification settings.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.


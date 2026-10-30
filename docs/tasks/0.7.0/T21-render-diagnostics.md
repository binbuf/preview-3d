---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T21 --configuration Debug --output TestResults/render-quality/T21
---
# T21 — Material and light-contribution diagnostics

## Goal

Isolate why a region looks wrong before changing exposure or adding a light. Make imported values, actual GPU inputs and per-pass contributions inspectable.

## Context (read first)

- Depends on **T20**; execute in roadmap order.
- `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md` and T20 runner/report contract.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp` — PSMain, HDR resolve, material root constants and effective PSO selection.
- `interactive-viewer/src/render/Model.*`, `shared/model-core/include/model_core/MaterialPayload.h`.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Add bounded Debug inspection/capture channels for base color, geometric/shading normal, UV0/UV1, metallic, roughness, authored AO/strength, coverage alpha, transmission, IOR/F0, clearcoat and sheen. Mark unsupported/unbound channels explicitly rather than presenting neutral fallbacks as authored data.
- [ ] Add direct diffuse/specular, environment diffuse/specular, coat, emission and pre-tone-map HDR views, plus effective opaque/masked/blended/transmissive bucket and depth-write/cull state. Clamp visualization only at the display step; preserve numeric HDR readback.
- [ ] Generate a per-material diagnostic record linking source material/primitive IDs to validated wire values, bound texture/color-space/sampler/UV metadata and effective draw state. Read source metadata in the worker/test tooling; the viewer must not parse glTF.
- [ ] Make output channel selection and numeric pixel/region sampling part of T20's Debug capture route. Verify a small set of independently computed patches through actual GPU readback, not shader-token existence.
- [ ] Use Car windows/paint, Camera lens, Suit and a roughness/metal grid to record which contributions dominate. Keep the result as evidence; do not label hypotheses as proven causes without isolated comparisons.
- [ ] Keep shipping test-control rejection intact; inspection controls must not add a Release message/path authority surface.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

## Done when

- [ ] T21 verification covers diagnostic GPU outputs, material/texture identity and Release test-control rejection.
- [ ] A short cause-by-region report identifies the inputs to fix in T22–T27; images and numeric samples are included.
- [ ] Update rendering diagnostics documentation and hand-off.

## Out of scope

Material editing UI, altered default appearance, protocol expansion solely for convenient debug strings, or logging full user paths/model contents in Release.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Implemented:

- Debug inspection channels 0-20 in the textured PBR shader, selected through
  `gPresentation.w`; raw linear values, display clamp only. UV1 is explicitly
  unsupported (magenta marker + capability mask bit absent).
- Numeric HDR readback of a bounded (≤64×64) RGBA16F scene-target region through
  a READBACK resource and the frame fence; tests assert authored values, not
  shader tokens.
- Bounded per-material draw record: primitive/material/node/instance ids,
  vertex/index counts, validated wire material, bound texture chunk id/format/
  colour space, map mask, UV transform, and effective bucket/depth-write/cull.
  Viewer never parses glTF; `tests/render-quality/diagnostics.py` reads source
  metadata for comparison.
- T21 profile, `material-probe.glb` six-material fixture, cause report and
  image review. All fields are inside the existing Debug + `--app-smoke` guard;
  the wrapper builds Release and asserts every T21 message returns 0.
- Docs: `tests/render-quality/README.md`, `docs/design/04-rendering-and-streaming.md`,
  `docs/design/adr/0077-debug-material-light-diagnostics.md`,
  `docs/0.7.0/progress/T21.md`.

Verification (all run in the foreground on 2026-10-30):

```powershell
python tests/render-quality/verify.py --task T21 --configuration Debug --output TestResults/render-quality/T21
# status pass; 10 captures; 83/83 diagnostic checks; Release rejection pass
python tests/render-quality/verify.py --task T20 --configuration Debug --output TestResults/render-quality/T20-regression
# status pass; 53 captures; 15/15 regression comparisons
```

The T21 report is `TestResults/render-quality/T21/report.json`; diagnostic
numbers are `diagnostics/diagnostics.json`, channel images are
`diagnostics/channels/`, and the cause-by-region report is `cause-report.md`.
Review record: `tests/render-quality/review/T21-review.json`.

Observed dominance (local evidence, not proven causes): Car windshield and paint
are environment-specular dominated (0.698 / 0.188 mean linear), Camera lens and
body environment-specular (0.081 / 0.023), Suit torso environment-diffuse
(0.093). T22-T27 should use the isolated channels before changing exposure or
light intensity.

Deviations: the material probes need TANGENT/COLOR_0 so the importer selects the
textured shader; without them the compact shader intentionally ignores the
diagnostic channel. Region rectangles for Camera were tightened to the actual
lens/body from the captured lens view.

Remaining/blockers: none for T21. Texture-driven lobes (T25), independent AO
texture (T22), physical transmission/ordering (T26/T27) remain later tasks.



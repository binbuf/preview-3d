# ADR-0076 — Render-quality evidence is manifest-driven and separate from reference promotion

- **Status:** Accepted
- **Date:** 2026-10-30
- **Context:** T20 introduces the first GPU image gate for the 0.7.0 render-quality
  work. Earlier Studio lanes could write a missing per-machine golden and then
  compare against it, so a blank or stale frame could bless itself. The gate also
  has to run generated fixtures without the restricted external corpus and has to
  keep installed F3D 3.5.0/VTK distinct from the local 4.0.0 source tree.

## Decision

1. `tests/render-quality/verify.py` is the task wrapper. It accepts
   `--task TNN --configuration Debug --output PATH`, loads a registered profile
   from `tests/render-quality/profiles/TNN.json`, and fails on an unknown or
   unimplemented profile, an empty case/lane selection, a renderer/load error, a
   missing image, a missing accepted reference, or an unsupported required lane.
2. Captures are only accepted after the viewer reports a verified complete load,
   the generation matches, texture counters are quiet, and the viewport produces
   a stable, non-blank frame. A verified-complete hand-off alone is insufficient:
   T20 observed a background-only frame for several seconds after Car reported
   complete. That readiness gap is a T23 defect, not a passing capture.
3. First-open/default presentation and controlled comparisons are separate case
   classes. Controlled cases use the Debug-only canonical camera/viewport fields
   (yaw, pitch, distance scale, projection, immediate home reset) and a recorded
   neutral presentation (Studio PBR, dark background, PBR Neutral, exposure 1,
   studio intensity 1, grid/shadow on). UI, gizmo, reset button, warning badge,
   loading card, HUDs, tooltip and selection are excluded from the captured
   viewport; the toolbar and bottom bar are cropped. The capture-clean switch is
   compiled only under `PREVIEW3D_ENABLE_TEST_CONTROL` (Debug).
4. Ordinary verification never writes an accepted reference. Candidate images,
   observed baselines, diffs/crops and the review record are outputs.
   `tests/render-quality/compare.py` is the only promotion path: it records the
   previous image, candidate hash, camera/viewport, reason, reviewer and review
   record before replacing an accepted reference. Accepted references defend
   reviewed current behaviour; they are not a quality target.
5. F3D images are reference observations, not pixel-match targets. The shipped
   `05_all.json` preset and recorded no-config controlled settings are captured
   separately. Installed 3.5.0/VTK images are never attributed to the local 4.0.0
   source, which has no built binary on this workstation. The Khronos glass route
   is a separate incomplete lane until a sample viewer is available; F3D's gray
   Camera lens is a known defect, not an accepted target.
6. External corpus screenshots stay out of the repository until redistribution
   rights are established. Generated fixtures are the portable regression lane;
   an absent corpus or asset marks external qualification `incomplete`, never a
   passing skip.

## Consequences

- Camera or settings changes invalidate a numeric comparison: the accepted
  reference stores camera, physical viewport and region rectangles, and a
  mismatch fails before pixels are compared.
- Blank, flat, unsettled, proxy-only or generation-mismatched captures fail the
  contract; the negative self-test proves each rejection.
- Later tasks add a profile and cases; they do not branch inside `verify.py`.
- A future complete-load fix must keep the T20 capture contract: visible,
  textured and settled is narrower than "state Ready".

## References

- `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`
- `docs/tasks/0.7.0/T20-visual-comparison-harness.md`
- ADR-0040 viewer local attack surface and Debug-only test control
- ADR-0075 0.7.0 qualification evidence boundary

# ADR-0089 — Scale-aware screen-space AO over the existing depth, applied to indirect light only

**Status:** Accepted

## Context

T31 replaced the bounds-ellipse contact shadow with a fitted geometry map, but
the renderer still had no screen-space AO: cavity/contact shading came only from
an authored ORM map, which untextured/CAD and many PBR assets lack. The task
required a bounded SSAO/GTAO-class path with correct depth reconstruction,
scene-scale-aware radius, edge-aware filtering and stable sampling, applied to
indirect illumination only, with transparent/masked surfaces excluded and a
recorded low-budget fallback. The existing renderer already has one depth
target, a linear HDR scene target, camera-target-relative world positions and a
full-screen resolve; there is no spare normal G-buffer, and temporal history
without reprojection is out of scope.

## Decision

1. **Depth only.** Reconstruct camera-target-relative positions from the
   existing 32-bit depth with the inverse view-projection. Normals come from the
   reconstructed neighbours. This uses the projection conventions already in
   the material shader and works for perspective, orthographic and the blended
   projection without a model-size assumption.
2. **Indirect-only compositing.** The opaque material pass writes its
   environment/IBL/ambient share to a second HDR target. The resolve subtracts
   exactly that share, `scene - indirect * (1 - ao)`, before the OIT composite
   and the display transform. Direct light, emission, glass reflection and the
   final tone-mapped image are never multiplied by an AO mask. Authored ORM AO
   stays inside the indirect term and is not re-multiplied.
3. **Pass graph exclusion.** AO runs after opaque/Clay-overlay geometry and
   before the transmission snapshot, transparent/OIT and pick passes. BLEND and
   transmissive surfaces never write the opaque depth or the indirect target,
   so a window cannot become an opaque AO sheet; MASK holes are handled by the
   authored cutout that already discarded the fragment.
4. **Bounded sampling.** Twelve deterministic spiral samples use a world radius
   derived from the fitted scene radius and converted to a capped pixel radius
   from the reconstructed frame. A 5x5 bilateral filter rejects depth
   discontinuities and background neighbours. There is no temporal history,
   no ray tracing and no resolution-independent allocation growth beyond the
   viewport.
5. **Calibration.** Studio 0.60 strength / 0.10 radius fraction, Clay 0.85 /
   0.14, Directional 0.55 / 0.09. The `A` key and the persisted
   `ambientOcclusionEnabled` preference toggle it. Debug fields 287-298 expose
   bounded overrides, target bytes, pass time, fallback reason and raw
   reconstruction/normal/gap/radius views.
6. **Lifecycle and fallback.** The indirect HDR target and two R8 masks are
   viewport-sized, accounted, recreated on resize/device recovery and derived
   from the current drawable set each frame. Allocation failure, unsupported
   extent, Debug force, diagnostic channel or point/line-only content records
   an AO-off fallback and draws normally.
7. **Transmission boundary.** The T27 scene-color copy is taken before the
   resolve subtraction, so transmitted radiance keeps its authored glass
   response. A dedicated transmitted-radiance AO pass is deferred; the task's
   nonregression requirement for glass/lens takes priority over inventing a
   second occlusion path there.

## Consequences

- AO improves only cavity/contact areas; it cannot hide wrong normals, light
  transforms or missing shadows, and a reviewer can tell it apart from a global
  exposure change.
- Compatibility fallback stays the historical two-target opaque layout, so the
  AO-off path is not a new, untested material path.
- The omitted transmitted-radiance AO is a recorded limitation, not a silent
  claim: a cavity seen only through glass does not receive a second screen-space
  pass, and T34/T35 may extend the transmission copy if release evidence needs
  it.
- The fixed spiral can alias on very high-frequency screen-space detail; T33
  owns edge/specular AA and can revisit the filter footprint without changing
  the indirect-only contract.
- Debug inspection and the generated `ao-probe`/`ao-probe-large` fixtures own
  the evidence; no per-asset-name shading or reference is involved.

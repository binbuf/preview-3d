# ADR-0088 — Geometry-derived fitted shadow map and a labelled bounds fallback

Status: accepted (T31, 0.7.0)

## Context

ADR-0072 shipped the analytic bounds ellipse as the MVP contact shadow: the
verified bounds were projected along the key light and painted as one soft
blob. It could not preserve a strap loop, a perforated sheet, a car's four
separate tyre patches, or empty space between disconnected parts, and T31
requires the ground to be derived from the model's geometry. The renderer
already has a linear HDR scene target, camera-relative draw transforms, MASK
alpha policy, OIT/transmission lifecycle patterns and a Debug-only diagnostic
surface, so a bounded depth pass can reuse those contracts without a new
trusted-process parser or a second light.

## Decision

1. A fitted orthographic light frustum is built from the verified **world**
   bounds and the same world direction used by shading (the Directional sun
   from `viewer_lighting::DirectionalSunDirection`, or the documented Studio
   key `(0.45, -0.55, 0.70)`). Bounds only size the frustum and the ground
   quad; they never define the silhouette.
2. `viewer_ground_shadow::FitShadowFrustum` bounds the frustum, clamps a
   grazing light, snap-centres it to whole light texels, and derives a
   scale-invariant normalized depth bias. `ShadowMatrixForCameraTarget`
   converts the matrix so the ground shader consumes camera-relative
   positions. The map is 2048² with a 1024² allocation fallback.
3. The caster pass is depth-only `D32_FLOAT` plus one `R8_UNORM` attenuation
   target. It draws the current drawable triangle set every frame, so streamed
   updates, generation swaps and cancellation cannot leave stale casters.
   Cull is disabled for mirrored/double-sided geometry. `MASK` keeps its
   authored base-colour alpha test. `BLEND` coverage/decal surfaces are not
   opaque occluders. Transmissive surfaces still occlude but contribute the
   bounded attenuation `clamp(1 - 0.60·transmission, 0.40, 1)`; clear glass
   therefore cannot cast an unexplained solid black silhouette. This is an
   approximation, not caustics or volume absorption.
4. The ground layer samples the map with a comparison sampler and 3×3 PCF,
   subtracts the fitted depth bias, and writes premultiplied black coverage
   into the linear HDR target. The resolve composites that coverage over the
   display backdrop, so exposure and tone-map changes cannot invert the cue.
   Pixels outside the fitted map are lit (empty space stays empty).
5. The Ground highlight remains the independent display-space backdrop layer
   from ADR-0079. `parameters[3]` still gates only the fallback shadow, and
   the grid keeps its own toggle. A hidden grid or highlight does not remove
   geometry grounding.
6. If the shadow targets cannot be allocated, or the Debug fallback switch is
   set, the renderer uses the ADR-0072 analytic bounds ellipse and labels it
   as the documented lower-quality fallback. Point/line-only content has no
   triangle casters and gets no ellipse at all. The fallback is never the
   normal path on supported hardware.
7. The depth and attenuation targets are persistent per-device resources,
   included in `AccountedAllocationBytes`, released with the device, and are
   not viewport-sized, so resize does not reallocate them. They are recreated
   by device recovery. Debug fields 279–286 expose mode, fallback reason,
   bytes, caster count, extent, pass time and a forced-fallback switch; they
   stay inside the existing Debug test-control guard.

## Consequences

Grounding now follows real silhouettes and holes for triangle content, costs
one bounded depth pass plus 20 MB at 2048² (4 MB at the fallback extent), and
keeps the display-space background/highlight contract. Very large triangle
models pay a second vertex pass; the pass is skipped when shadows are off or
when no triangle caster exists. The analytic ellipse survives only as a
recorded fallback, so later tasks must not describe it as the shipping shadow.

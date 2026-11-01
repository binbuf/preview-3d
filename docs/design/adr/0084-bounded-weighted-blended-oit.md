# ADR 0084 - Bounded weighted blended OIT and nearest-visible transparent picking
Status: accepted (T26, 0.7.0)

## Context
Sorted straight-alpha blending resolves ordinary single-layer glass, but its
answer depends on mesh-centre depth. Intersecting triangles inside one
primitive, interpenetrating decals and shuffled draw or primitive order can
change the image. T18 deferred OIT; T20 observed Car window/depth popping and
required a bounded, testable fix without making a worse fallback the normal
path. Depth peeling allocates per-layer depth/colour targets and a peel pass per
layer; weighted blended OIT needs two full-screen targets and one geometry pass.

## Decision
- Use **weighted blended OIT** (McGuire-Bavoil family) as the high-quality
  transparency path on the existing feature-level 11_0 device. Two fixed
  `RGBA16F` targets hold premultiplied weighted colour and the revealage
  product. The fragment weight is scene-relative (`gViewport.z` = eye distance)
  and clamped so every layer is finite.
- Opaque/masked geometry keeps its depth-writing prepass. The transparent pass
  tests against that depth and never writes depth. The resolve composites the
  OIT average and coverage in **linear HDR before exposure/tone mapping**.
- Keep coverage, interface reflection and transmission separate. The shader
  still consumes the authored coverage alpha, Fresnel reflection and
  transmission factor independently; diagnostic channel 22 exposes
  `(coverage, glassReflectance, transmission)`. OIT does not provide
  refraction; T27 owns the transmitted-scene sample, tint and rough refraction.
- Bound work and memory by the viewport, not by layer count: no per-pixel
  linked list and no peel-count target allocation. The validated stable overlap
  range is eight layers. Past that the resolve is still order-independent but
  is a weighted average, not an exact front-to-back stack; that limitation is
  documented rather than called exact.
- Keep the sorted path only as a **recorded fallback** when the two targets
  cannot be allocated (low memory/unsupported extent) or a Debug test forces
  it. Diagnostics fields 243/244 report the effective algorithm and reason.
- Selection is a separate nearest-visible pass. It writes only the integer pick
  target, writes depth, and reuses the material shader's cutout discard, so a
  low-coverage decal or cutout is selectable while geometry behind an opaque
  surface is not. Integer ids are never averaged. Alpha-cutout base-colour
  texture alpha is now preserved for MASK/BLEND materials (it was previously
  forced to 1 for non-layer textures).
- The fixture set includes a deliberately permuted twin: crossing triangles in
  one primitive, reversed primitive/node order and a shared-material overlap.
  OIT must be pixel-identical under the permutation; the sorted fallback must
  not be.

## Consequences
- The normal product path is order-independent within floating-point noise and
  no longer uses mesh-centre sorting as the transparent authority.
- Two `RGBA16F` targets join the accounted GPU budget and participate in
  resize, cancellation, device-loss recovery and the low-memory fallback.
- Sorted transparency remains the behavior on allocation failure; the report
  and diagnostics expose that state, so it cannot silently become the usual
  high-quality path.
- T27 starts from separate coverage/reflection/transmission inputs and a stable
  composite; it must add the transmitted-scene path rather than reusing OIT as
  a refraction approximation.

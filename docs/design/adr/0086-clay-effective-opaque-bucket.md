# ADR-0086 — Clay is an effective opaque render bucket, not an alpha override

- Status: accepted
- Date: 2026-10-31
- Task: T29

## Context

Clay previously replaced the pixel shader's base colour with gray and forced
display alpha to one, but draw classification still used the authored alpha
mode. Authored BLEND/transmission surfaces therefore stayed in the transparent
bucket: they used the Blend/OIT PSOs, did not write depth, and could show
blend-order seams or reveal interior meshes through a mode that is supposed to
be an opaque geometry review. Masked foliage and cutout holes were filled
because the gray replacement discarded the sampled base-colour alpha.

## Decision

1. **Derive an effective bucket per frame.** In Clay every triangle with
   authored BLEND or `kMaterialFlagTransmissive` is removed from the
   transparent bucket and drawn as opaque geometry. The imported material and
   wire data are never mutated. Non-Clay modes keep the authored buckets.
2. **Overlay, don't blend.** Clay-overridden triangles draw after the solid
   prepass in a dedicated opaque overlay pass. It writes depth, tests with
   `LESS_EQUAL` plus a bounded negative depth bias, and is sorted far-to-near.
   This avoids coplanar glass/trim z-fighting while keeping the nearest authored
   surface and rejecting geometry behind solid parts. The authored Blend/OIT
   PSO is not used.
3. **Cutouts are geometry, not decoration.** MASK triangles remain opaque-bucket
   draws. The Clay shader samples the base-colour texture alpha solely for the
   cutoff, independently of the gray shading, so authored holes survive and are
   not filled by the neutral material.
4. **Neutral matte BRDF.** Clay suppresses authored base colour, metal/rough,
   clearcoat, sheen, specular, emission, transmission and normal/coat-normal
   maps. It uses one diffuse albedo, zero specular F0/F80, the world-stable
   Studio irradiance and a softened key/fill/rim rig. Geometric normals, hard
   edges, winding and double-sided back-face reversal are preserved. Exposure,
   tone mapping, the environment bake and the user's camera are untouched.
5. **Lines and points are untouched.** Their pipelines are not part of the
   triangle override. A mixed line/point OBJ control proves the mode round-trip.
6. **Diagnostics are append-only.** Debug material fields 143-145 report the
   effective bucket/depth/cull for the current mode; 274-276 report the authored
   bucket/depth and whether the Clay override rewrote the draw. The records are
   refreshed once on a mode change, not per frame.

## Consequences

- Clay no longer has a transparent pass, transparent draw count or forward
  transmission draw count; the lane asserts those are zero.
- Car windows become solid gray and no longer leak the interior. Pre-existing
  authored window-edge speckles remain visible in both Studio and Clay and are
  recorded as a separate defect, not attributed to this override.
- Authored ORM occlusion stays suppressed in Clay. Geometry-derived cavity and
  contact darkening belongs to T31/T32, not to a material-decoration shortcut.
- Any future mode with a similar bucket override must extend the same effective
  state helper and keep the authored diagnostics intact.

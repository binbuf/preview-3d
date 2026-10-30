# ADR 0072 — Analytic ground contact shadow and ORM ambient occlusion

Status: accepted (T16, 0.7.0)

## Context

Studio objects floated over the grid: the clear color and grid were the only
ground cues, and the material shader sampled ORM RGB but had no documented AO
contract. The task allowed either an analytic soft blob or a full shadow map.
A shadow map needs a second 1024/2048 depth target, a depth pass per frame, a
PCF/bias policy and another failure mode on low-end feature level 11_0 devices.
The existing renderer already treats the key rig as a presentation light rather
than a physically tracked light.

## Decision

Ship the analytic soft contact footprint as the MVP shadow:

- `viewer_ground_shadow` (`GroundShadow.{h,cpp}`) is pure, Direct3D-free math:
  transform the verified source bounds with the active ground-axis matrix,
  place the plane at `min.z - 0.0015 * radius`, project the eight corners along
  the stable key direction `(0.45, -0.55, 0.70)`, then bias the result 15 %
  back toward the base footprint and add a 0.60 × radius soft rim.
- Draw the ground and its shadow as their own SV_VertexID quad before the grid
  and model, into the linear HDR target with straight alpha. It writes no
  depth, so it can neither occlude the model nor fight the grid (the grid sits
  at `min.z - 0.012 * radius`, below the ground plane).
- Defaults: opacity 0.78, softness 0.50, inner core opaque. The key elevation
  is clamped to 0.08 and the footprint to 6 × radius so a grazing light cannot
  produce an unbounded plane.
- ORM R is ambient occlusion through `OcclusionFactor(sample, strength) =
  saturate(1 + strength * (sample - 1))`. AO scales indirect diffuse,
  environment reflection and ambient terms only; every `EvaluateLight` direct
  call stays unoccluded. At T16 `MaterialPayload` had no `occlusionStrength`
  field, so the shipped strength was the glTF default 1.0 and a missing map
  sampled 1.0. T18's protocol-v12 record now carries the authored strength
  (ADR 0074); the formula and unoccluded direct path are unchanged.
- Presets: Studio CAD/PBR/Scan enable the shadow, Transparent and Point cloud
  disable it. `H` (accelerator, More menu item, and persisted
  `groundShadowEnabled`) toggles it. A manual toggle survives later preset
  applications. The grid keeps its own independent toggle.
- SSAO stays deferred: the HDR target exposes no depth/normal chain yet, and
  the contact shadow must not depend on an optional quality pass. This is
  recorded as a T16 follow-up.

## Consequences

There is no extra pass, target or GPU allocation; the cost is one blended quad.
The shadow follows bounds and the ground-axis transform exactly, and hiding the
grid cannot hide it. It is a directional contact cue, not an occluder-accurate
shadow: self-shadowing, cast shadows on other geometry and contact hardening
are out of scope. A future shadow-map task can replace the layer behind the same
`GroundShadowConstants`/toggle contract without changing presets or settings.

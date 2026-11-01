# ADR 0087 — A world-anchored Directional sun for shading, gizmo and shadow

Status: accepted (T30, 0.7.0)

## Context

Directional mode had three different ideas of where its light was. The object
shader took the app's normalized azimuth and elevation and built a direction in
the values it called camera space; the nav gizmo projected a world-bearing sun
from the same angles; the contact shadow always used the stable Studio key
`(0.45, -0.55, 0.70)`. Orbiting the camera therefore moved the lit face while
the marker stayed on its world bearing, and the shadow disagreed with both.
T24 already gave Directional the shared split-sum IBL and the same sun for the
base and coat lobes, but the transform and the shadow input were still split.

The renderer's vertex path subtracts the camera origin in double precision but
does **not** rotate by the camera; `gViewProjection` applies the camera later.
The pixel shader's normal and view vectors are therefore in the fixed Z-up
world frame. The previous "camera space" wording described the origin policy,
not the axis frame.

## Decision

- Define one documented world convention in `render/LightRig.h`. The viewer is
  right-handed Z-up after the ground-axis correction. Azimuth is normalized
  0..1 around +Z (0 = +X, 0.25 = +Y, 0.5 = -X, 0.75 = -Y); elevation is radians
  above the horizon, clamped to `kMaxSunElevation = 1.4835`. The unit vector
  towards the light is
  `(cos(az)·cos(el), sin(az)·cos(el), sin(el))`.
- `viewer_lighting::DirectionalSunDirection` is the only constructor. The frame
  constant `gSunDirection`, the nav-gizmo marker, the compact/legacy shader and
  the ground-shadow projection all consume it.
- The material shader uses that world vector directly for the base and
  clearcoat direct lobes. No extra camera quaternion is applied: the shading
  vectors are already in the world frame and the camera rotation lives in the
  view matrix. The gizmo separately uses the camera orientation for its
  screen bearing/depth; that presentation transform never changes the sun.
- Directional keeps the T24 Fresnel/BRDF-correct split-sum IBL as its ambient
  reflection floor and has no second Studio key/fill/rim or unweighted
  reflection. Sheen scales with the same environment intensity. Intensity zero
  removes direct, IBL, coat and the background-only transmission fallback;
  emission is the documented survivor.
- `GroundShadow.cpp` receives the Directional world sun for the analytic
  contact footprint and keeps `kKeyLightDirection` for Studio. T31's
  geometry-derived shadow must reuse the same interface.
- Debug test-control fields 277/278 expose the global sun azimuth/elevation
  (IEEE float bits, `lParam == -1` reads) for deterministic sun-state evidence.
  They remain inside the Debug-only test-control guard; no Release authority is
  added.
- Mode and asset changes do not create a per-asset light exception. A manual
  lighting choice and the global sun state survive opening another document.

## Consequences

With a stationary sun, camera orbit changes the view and reflections but not
the lit world face or the gizmo bearing. A sun move changes the lit face,
highlight, gizmo and shadow together. An imported-up-axis change rotates the
geometry and ground into the fixed world frame while the sun stays put; the
existing ground-axis command also refits the camera home framing, which is
view state rather than a light exception. T31 must not reintroduce a separate
shadow light. No accepted reference was promoted; the numeric T30 fixture and
the reviewed still/orbit/azimuth/elevation sequences are observed evidence.

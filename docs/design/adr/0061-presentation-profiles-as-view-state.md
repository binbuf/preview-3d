# ADR-0061: Catalog presentation profiles are per-family view state, not format data

- Status: Accepted
- Context: T06 (0.7.0 per-format presentation defaults and orientation)

## Context

The format catalog already declared each family's source up axis and unit
policy. The viewer still applied one global lighting mode and grid to every
document, OBJ reported no up axis (so the default ground transform treated it as
Z-up), and the FBX adapter hard-coded the normalized Y/1.0 metadata. We needed
one declaration path for per-format defaults without turning the catalog into a
parser or the viewer into a second format switch.

## Decision

`FormatDescriptor::presentation` is a small product-level enum (`StudioPbr`,
`StudioCad`, `PointCloud`). `ResolvePresentationProfile(descriptor, summary)` is
a pure function; content only refines PLY: a point-only/3DGS PLY selects
`PointCloud`, and any PLY with triangles keeps the declared `StudioPbr`.

The viewer applies the resolved profile once, at the first completed upload of
a new document path:

- `StudioPbr` -> Studio material shader, ground grid on, default backdrop;
- `StudioCad` -> neutral Clay material, ground grid on, slightly lighter
  backdrop;
- `PointCloud` -> existing unlit round colored splat pipeline, grid off, darker
  backdrop.

Presets change view state only. They never touch source materials and never
write a persisted setting. An explicit toolbar lighting or grid action sets a
session override; later reloads/refinements do not re-apply the preset.

OBJ reports `UpAxisId::Y` and zero (unspecified) metres per unit. The broker
validates that through the same pure `SceneUnitsAndUpAxisAllowed` predicate the
unit tests call; OBJ still accepts a legacy `Unknown` axis but never an authored
metre factor or a different axis.

FBX continues to deliver normalized Y-up/metres geometry on the wire. ufbx's
authored `settings.axes.up`/`original_unit_meters` are read but deliberately not
transmitted: `SceneMetadata` describes the geometry actually delivered, and a
future protocol revision can carry the authored pair separately. No unit is
invented, and the toolbar ground-axis control remains the user override.

## Consequences

- A new format gets its opening look by adding one catalog row, not by adding a
  viewer switch.
- OBJ now opens upright by default; the ground-axis override and "show native
  orientation" still work unchanged.
- Studio CAD is currently represented by the existing Clay review material
  because a dedicated CAD lighting lobe belongs to the Studio/IBL work (T14/T15).
  The profile value is already the stable hook for that later change.
- The info panel gains a compact Presentation section reporting the preset and
  the source up axis/units. FBX shows its delivered normalized values, not its
  authored units; this limitation is recorded in the hand-off and
  `docs/FORMAT-SUPPORT.md`.

# ADR-0090 — Depth-aware spatial edge AA and semantic texture sampling

- **Status:** Accepted
- **Date:** 2026-10-31
- **Context:** T33 — edge, specular and texture sampling quality

## Context

The viewer renders geometry into a single-sample linear HDR target, resolves
weighted-blended OIT, applies exposure/tone mapping and writes sRGB; the
`R32_UINT` pick id is a separate MRT and the D3D11On12/Direct2D chrome is drawn
after the resolve. Thin wires, grille bars, chrome rims and the glancing ground
grid aliased, and authored textures used one linear sampler for both
nearest and linear slots. Runtime mip generation reduced sRGB color in linear
light but treated all data maps as arithmetic means and did not preserve
alpha-weighted cutout color.

## Decision

1. **Default AA is a depth/coverage-aware spatial edge filter in the existing
   resolve.** It is not MSAA and not TAA. The filter evaluates four axial
   neighbours on a display-space luma/coverage probe, selects the strongest
   gradient, and blends the centre with two full composite taps. A relative
   `1 - depth` discontinuity (linear for orthographic, monotonic for
   perspective) or a coverage discontinuity receives the geometric weight
   (0.55); flat-depth texture detail receives a deliberately small 0.12 weight,
   and low-contrast texture interiors are left untouched. The bounded footprint
   is seven taps and the filter allocates no target.
2. **MSAA and integer picking are kept incompatible.** The opaque material
   pass writes scene colour, the AO indirect share and the pick id in one MRT
   set; D3D12 requires one sample count for every bound target. Resolving an
   integer id target would average ids, so MSAA would require splitting the
   pick/OIT passes and would still not filter specular shading. Bounded 2×
   window supersampling is retained only as a deterministic validation
   reference in the render-quality lane.
3. **AA is outside picking and UI.** The filter never samples the pick target,
   and the D2D overlay is recorded after the resolve. The depth SRV shares the
   resolve descriptor heap; resize and device loss recreate it, and a missing
   depth SRV reports an explicit AA-off fallback.
4. **Sampling respects authored intent.** Linear/mipmap slots use a bounded 4×
   anisotropic sampler; authored nearest slots keep the point sampler. Runtime
   mips are semantic: linear-light color with coverage-weighted alpha,
   renormalized normals, GGX-alpha RMS for metallic-roughness, and plain linear
   for AO/factor data.

## Consequences

- Edges, wires, chrome rims and glass/coverage silhouettes are filtered without
  a global blur. Engraved lettering and UI text keep their contrast because
  flat-depth texture detail is not treated as geometry.
- The pick id and its nearest-visible policy are unchanged by AA, and cutout
  holes cannot be created or closed by the filter.
- MR mips preserve highlight variance instead of raising every material's
  authored roughness; the improved data semantic is part of the cache identity
  through `TextureDecodeOptions`.
- There is no temporal component. Small orbit steps are stable but a history
  and reprojection path is required before any TAA claim.
- The whole-frame MAE against the 2× reference is recorded but not used as the
  quality verdict: the reference samples different texture LODs and pixel
  centres. Per-region edge MAE, AA on/off pairs, picking/UI equality and
  cutout/OIT invariants own acceptance.

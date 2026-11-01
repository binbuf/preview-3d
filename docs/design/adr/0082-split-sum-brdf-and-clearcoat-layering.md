# ADR 0082 - Split-sum IBL, solid-angle irradiance and Fresnel-weighted clearcoat

Status: accepted (T24, 0.7.0)

## Context
The environment response multiplied a prefiltered cubemap by Fresnel alone.
It had no BRDF integration term, so rough metals and dielectrics lost the
roughness-dependent energy and grazing response the prefilter was baked for.
The SH diffuse projection used an equal `4π/(6·size²)` texel weight even though
cubemap texels subtend different solid angles. Clearcoat removed a flat
`(1 - clearcoatFactor)` of base specular, which throws away the base F0 of a
fully coated material instead of attenuating it by the coat's Fresnel
reflectance; the direct coat lobe also carried Fresnel twice. Directional added
an unscaled second transmission reflection and used Studio directions for its
coat. These are energy and consistency defects, not art-direction choices.

## Decision
- Bake a 64² split-sum DFG LUT (`A`, `B`) on the CPU with the same documented
  GGX integration as the shader. Environment specular is
  `prefiltered * (F0·A + B)` plus the Fdez-Aguera multiple-scattering term, so
  `F0 = 1` is the white-furnace identity. The LUT and prefilter are checked
  against independent uniform-quadrature references in `Tests.Unit [brdf]`.
- Project SH9 irradiance with each cubemap texel's exact solid angle; the six
  faces sum to `4π`. Constant, directional and rotated environments are tested
  against an independent sphere reference.
- Layer clearcoat per Khronos: `base' = lerp(base, coat, c·F_schlick(coatF0,
  1, NoV))`, with base diffuse/specular/sheen and emission attenuated by
  `(1 - c·F)`. The coat lobe itself has no second Fresnel factor.
- KHR_materials_specular keeps `F0 = IOR-derived × specularColor` and
  `F90 = specularFactor` independent. Direct BRDF uses height-correlated Smith
  GGX visibility and the same F0/F90.
- Studio, Clay and Directional share one Fresnel-weighted IBL path. Directional
  uses its own sun for base and coat and adds no extra transmission reflection.
- Transmission stays on the interim alpha-carried interface Fresnel convention
  (no transmitted-scene sampling); channel 22 exposes
  `(alpha, glassReflectance, transmission)` and T27 owns the physical path.
- The material shader remains raw linear HDR. The resolve is the only
  tone-map/sRGB encoding. Debug fields 231/232 expose the effective resolve
  exposure/tone map; channel 23 is a flat 0.3 calibration patch.

## Consequences
- The environment cubemap heap grows by one 64² R32G32 descriptor; the upload is
  still one bounded startup copy and well under 4 MiB.
- Existing observed images change for metal, rough dielectric, coated and
  transmissive materials. T24 records observed evidence only; no accepted
  reference is promoted and no exposure/preset change hides an invariant.
- The low-roughness LUT/prefilter reference is finite-sampled below roughness
  ~0.4. Production importance sampling covers that range; the white-furnace and
  monotonic checks cover it independently.
- Transmission remains a documented approximation until T27; T26 owns the
  order-independent transparency path.

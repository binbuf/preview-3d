# ADR-0085 — Scene-color transmission for thin glass

**Status:** Accepted (T27)

## Context

T18/T26 attributed optical transmission to the blend alpha: a Fresnel term was
folded into coverage, and there was no transmitted-scene sample. That conflated
authored coverage with interface reflectance, made an OPAQUE transmissive
material (the Camera lens) unrepresentable in the transparent pass, and left
tint and roughness without a physical path. KHR_materials_transmission defines
transmitted radiance as `(1 - F) * baseColor * background`, optionally
attenuated by KHR_materials_volume. The importer already preserves the authored
alpha mode and marks OPAQUE/MASK transmission explicitly.

## Decision

- The opaque pass finishes into the linear HDR scene target. A bounded
  `RGBA16F` copy with an eight-level mip pyramid is taken once per product
  frame **after** the opaque pass and before any transmissive draw. Nothing
  samples a render target that is simultaneously bound for writing.
- A transmissive fragment samples that copy at its own screen position:
  `mip = round(perceptualRoughness * (mipCount - 1))`, so rough glass filters
  the transmitted radiance. There is no refraction offset; offscreen and
  occluded content is never claimed.
- When the copy has no opaque coverage at the pixel (`alpha == 0`) the studio
  environment in the transmitted direction is the documented fallback.
- Surface reflection uses the normal split-sum/Fresnel path. Transmitted
  radiance is `background * baseColor * transmissionFactor * (1 - interfaceF)`.
  `interfaceF` comes from the authored IOR only (IOR ≤ 1 has no interface).
- Coverage stays authored: BLEND keeps its alpha, OPAQUE and MASK cover the
  pixel. Interface Fresnel and optical transmission never enter coverage.
- OPAQUE transmissive materials are routed through the transparent pass (OIT or
  the recorded sorted fallback) without changing their authored alpha mode or
  coverage. MASK transmission keeps the authored cutout and is not reinterpreted
  as glass. A double-sided transmissive surface shades whichever authored face
  the camera sees, so a single plane is one thin interface and stays visible from
  both sides; a closed shell contributes its two surfaces as two bounded thin
  interfaces to OIT. No volume thickness is fabricated from bounds or names.
- With two overlapping transmissive layers each layer samples the opaque
  snapshot, not the other transmissive layer. The order-independent OIT
  composite still resolves them stably. This is the recorded bounded
  approximation; inter-layer transmission/refraction is not implemented.
- Allocation failure is a recorded degradation: transmissive materials render
  as their authored dielectric surface, coverage is unchanged, and fields
  259-263 report target bytes, mip count, the reason and OPAQUE-transmission
  draw count. KHR_materials_volume remains a specific compatibility
  degradation, never silently presented as correct.
- Debug channels 31/32 expose transmitted radiance and the raw scene sample.
  During a diagnostic frame the snapshot is not regenerated, so the channel
  reads the last product snapshot rather than the debug image itself.

## Consequences

- Transmission intermediates add ~1.33× one RGBA16F scene target; they join the
  GPU budget and resize/device-loss lifecycle.
- Rough glass is a filtered screen-space approximation, not refraction; the
  validated bound and the two-layer sampling contract are explicit and tested.
- The Camera lens now has both transmitted interior depth and Fresnel
  reflection; Car BLEND glass keeps authored coverage visibly independent of
  the interface reflectance. ADR-0084's OIT composite is unchanged.

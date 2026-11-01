# ADR-0079 — Background is display-space; the ground highlight is a backdrop layer

Status: Accepted (T21a)

## Context

The T15 resolve pass cleared `#1C1C1E` into the linear HDR scene target,
multiplied it by exposure and tone-mapped it, then sRGB-encoded the result. The
intended dark backdrop therefore appeared as a lighter gray and changed with
exposure and the tone curve. Presentation presets could also replace the
background, so Scan chose mid-grey and Transparent chose a checkerboard. The
ground quad mixed a broad bright plane and the contact shadow into the same
linear HDR draw, so the contact-shadow toggle also removed the highlight.

## Decision

1. The backdrop is display-encoded. A separate `R8G8B8A8_UNORM` target is
   cleared with the selected sRGB display color and is never exposed to
   exposure or tone mapping. `#1C1C1E`, studio gray `#808080` and a packed
   custom `0xRRGGBB` reach the screen unchanged.
2. The local illuminated ground area and the analytic contact shadow are drawn
   into that display-space target as premultiplied source-over. The highlight
   is a bounded additive tint (backdrop + 0.055) with a radial fade; it is not a
   bright whole-viewport sweep.
3. The resolve pass recovers straight-alpha scene color from the HDR target,
   tone-maps the model, then composites it over the backdrop
   (`lerp(backdrop, mapped, coverage)`). Transparent mode outputs premultiplied
   display color with preserved coverage alpha; checkerboard mode draws its
   pattern in display space. The CPU reference is
   `viewer_presentation::CompositeOverBackground`.
4. Background mode, custom color and ground highlight are global user
   preferences. Presets and model counts never rewrite them. The saved
   background enum keeps its original values 0–3; `Custom = 4` is appended.
   `groundHighlightEnabled` defaults to true and is independent of
   `groundShadowEnabled`.
5. A highlight-off backdrop is the flat selected color; an enabled contact
   shadow still darkens that flat backdrop. Disabling shadows leaves the
   highlight intact. Checkerboard and transparent modes omit the local ground
   treatment.

## Consequences

The background contract is testable with actual GPU pixels and is stable across
exposure/tone-map changes. One extra full-viewport RGBA8 target and one
premultiplied PSO are allocated; its bytes participate in the existing accounted
GPU budget and resize lifetime. Model lighting, environment intensity, exposure
and tone-map selection are unchanged. Later render-quality tasks must preserve
the global appearance precedence and the backdrop/display boundary.

# Rendering and streaming

## Goals

The renderer must keep accepting input and presenting frames while cache validation, CPU import (now brokered to the AppContainer import worker and, for USD composition, the compatibility host), PCIe transfer, shader-visible publication, LOD selection, and eviction happen concurrently. It must also render a useful approximation when the full model is larger than available video memory and exploit verified derived data on repeat opens without trusting it implicitly.

“Asynchronous” means there is no UI-thread or render-thread wait for a loading resource. It does not mean every adapter or machine has a physically independent DMA engine.

## Device and presentation

At startup Graphics:

1. Enables the D3D12 debug layer only in developer builds before device creation.
2. Creates a DXGI factory and enumerates hardware adapters in the operating system's GPU preference order.
3. Selects the first non-software adapter that supports D3D12 feature level 11_0 and presentation to the window.
4. Creates one direct command queue and one copy command queue. A developer-only command line can request WARP; WARP is not an automatic performance fallback.
5. Creates a three-buffer flip-discard swap chain using DXGI_FORMAT_R8G8B8A8_UNORM, a frame-latency waitable object, maximum frame latency two, and Present(1, 0).

The primary path uses the Windows 11 system D3D12 runtime; the MVP does not ship the Agility SDK. Shaders are built offline with a pinned DXC version and embedded as immutable assets. Runtime shader compilation is prohibited in release builds.

The window client size is converted to physical pixels using per-monitor-v2 DPI. Zero-area/minimized windows stop acquiring swap-chain buffers but do not stop import or residency management. Resize is coalesced and performed at a direct-fence-safe point; it never blocks the UI message pump.

## Ownership model

| Owner | Exclusive state |
| --- | --- |
| UI thread | HWND, message pump, input/capture state, accessibility commands, high-level AppState |
| Render thread | device, direct queue, swap chain, back buffers, render allocators/lists, render fence, pipelines, descriptor heaps, D3D11On12/D2D overlay bridge |
| Upload coordinator | copy queue, copy allocators/lists, upload-ring suballocation, copy fence |
| Residency manager | resource catalog and requested/committed/resident states; actions are executed through the owning GPU lane |

An ID3D12CommandAllocator is reset only after the fence value of its last submitted command list has completed. Direct and copy command lists are recorded by their owning lanes. No worker records into a shared list and no COM command object crosses ownership without an explicit immutable handoff.

All D3D12 objects are represented to other components by stable handles into Graphics-owned tables. Destruction enters a deferred-release queue tagged with the last direct and copy fence values that could reference the object.

## Frame loop

The render thread waits on the frame-latency handle plus a short event set for shutdown, resize, new scene data, and UI invalidation. A timer keeps animation/loading indicators alive; there is no unconstrained busy loop.

For every drawable frame it:

1. Drains a bounded number of fence-complete upload publications and retired resources.
2. Atomically acquires the latest immutable SceneSnapshot.
3. Updates camera-relative transforms, visible clusters, LOD requests, and constant buffers for the current frame context.
4. Records depth, opaque, alpha-mask, point-cloud, transparent, grid/background, and selection-independent UI composition in deterministic order.
5. Submits the D3D12 scene command list. If an overlay pass follows, it leaves the wrapped back buffer in the bridge's declared input state; otherwise it transitions the buffer to PRESENT itself.
6. When an overlay pass follows, uses the D3D11On12 bridge to acquire, draw Direct2D/DirectWrite content, release, and flush the wrapped resource into PRESENT state.
7. Signals the direct fence after both scene and overlay GPU work, tags resources and the snapshot with that fence, presents, and advances the frame context.

D3D11On12 is used only for small native UI/text overlays. Geometry, depth, materials, tonemapping, and loading proxy rendering remain D3D12. The bridge is created on the render thread over the existing direct queue and is never called from the UI thread.

## Render pipeline

The MVP uses a forward, rasterized PBR-lite pipeline:

- right-handed internal coordinates and camera-relative float matrices;
- reversed-Z depth with a floating-point depth buffer;
- one product-owned procedural studio environment baked once into a world-stable cubemap (solid-angle SH9 diffuse irradiance, a GGX-prefiltered specular mip chain and a split-sum DFG LUT) with a camera-relative key/fill light;
- base color, metallic, roughness, emissive, normal map and UV transform when present, vertex color, unlit mode, alpha mask, double-sided state;
- bounded point primitives rendered as depth-tested camera-scaled round splats with source color or neutral shading;
- opaque and masked geometry before one bounded weighted-blended-OIT
  transparent pass (sorted straight alpha only as a recorded fallback);
- back-face culling unless a material is double-sided;
- a linear `R16G16B16A16_FLOAT` HDR scene target plus the reversed-Z depth
  buffer and the R32_UINT pick target; object shaders write linear light and
  never apply a display transform;
- a full-screen resolve pass that composites the background in linear space,
  applies exposure, the selected neutral/ACES/Filmic/None tone curve and the
  sRGB transfer, then writes the UNORM swap-chain back buffer;
- single-sample MSAA-off geometry targets plus a default depth/coverage-aware
  spatial edge filter in the resolve (ADR-0090); TAA, temporal history, ray
  tracing and mesh shaders remain outside the MVP. A bounded fitted shadow map
  for ground contact and ORM ambient occlusion are in the MVP (see below).

### Edge anti-aliasing and sampling

The product default is a single-sample `R16G16B16A16_FLOAT` scene target
followed by a depth/coverage-aware spatial filter inside the existing
full-screen resolve. The filter evaluates four axial neighbours on a
display-space luma/coverage probe and two full composite taps along the
strongest gradient, with a bounded seven-tap footprint. A geometry/occlusion
discontinuity (`1 - depth`, which is linear for orthographic and monotonic for
perspective) or a coverage discontinuity (opaque alpha plus OIT revealage)
receives the meaningful 0.55 blend; flat-depth texture detail receives only a
0.12 detail-preserving blend, and low-contrast texture interiors are untouched.
This keeps engraved lettering, grille bars and UI text crisp while filtering
sub-pixel wires, chrome rims, glass silhouettes and the glancing ground grid.

MSAA is deliberately not the default. The opaque material pass writes scene
colour, the AO indirect share and the `R32_UINT` pick id in one MRT set, and
D3D12 requires all bound render targets to share a sample count. Resolving an
integer id target would average instance ids, so 4× MSAA would require
splitting the pick and OIT passes and would still not filter specular shading
aliasing. Bounded 2× supersampling is retained as a deterministic validation
reference in the render-quality lane, not as a runtime default. Picking stays a
separate nearest-visible `R32_UINT` target and the D3D11On12/Direct2D overlay is
recorded after the resolve, so neither ids nor text enter the filter. The depth
SRV shares the resolve descriptor heap; the filter adds no render target and
participates in resize/device-loss recreation with a recorded AA-off fallback.

Materials are flattened into fixed-size GPU records. Feature level 11_0-compatible descriptor tables are used instead of assuming resource binding tier 3 or shader model 6.6 bindless behavior. Missing resources point at permanent neutral descriptors.

Direct descriptor-heap indexing ("bindless") requires both shader model 6.6 and resource binding tier 3, which the MVP's feature-level-11_0 floor does not guarantee. Descriptor tables are therefore the baseline rendering path for the life of the MVP, not a placeholder pending a bindless rewrite; a bindless path may be added later strictly as an optional, separately gated capability tier behind a runtime feature check, evaluated on its own performance evidence rather than adopted as a wholesale replacement.

Per-frame camera/pass data (view-projection, camera-relative origin, lighting) is bound through a per-frame constant buffer view — a root CBV or a root-signature-referenced table entry — not packed into inline root constants. Root signatures have a hard 64-DWORD budget and materially less native fast storage on typical hardware; root constants are reserved for small, genuinely per-draw values such as a material/instance/draw ID, not for data that is naturally a buffer.

The normalized-chunk vertex layout is a small, closed, product-enumerated set of explicit layouts (at minimum: a position-only layout for point clouds, a position+normal+UV layout for untextured/simple meshes, and a full PBR layout with tangents/vertex color for textured meshes), each with a stable numeric layout ID carried on the chunk descriptor. It is not one universal fixed-size vertex record that every format's normalizer is forced into: a struct sized to fit worst-case data (as in the advisor's illustrative 32-byte sample, whose declared members total 28 bytes and only reach 32 through tail padding, and whose UV field is full float2 rather than half-precision) wastes bandwidth on the common case and still cannot represent every source faithfully. Adding a layout to the enumeration is a normal chunk-schema change reviewed like any other in [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md); it is not a reason to add a second universal struct.

CPU frustum culling operates on cluster bounds. Draws are grouped by pipeline/material and compatible instances are batched. When measured visible draw count or CPU submission time crosses the Gate-3 threshold, a feature-level-11-compatible `ExecuteIndirect` path consumes product-generated commands after CPU cluster culling; the ordinary direct-draw path remains the correctness fallback. GPU occlusion culling and mesh shaders remain deferred. The normalized cluster size bounds CPU work in either path.

## Studio environment and color pipeline

The default Studio view does not read a user file or a shipped HDRI. A
product-owned procedural environment is generated at startup
([`StudioEnvironment.h`](../../interactive-viewer/src/render/StudioEnvironment.h)):
three rectangular softboxes, a soft overhead strip and a neutral floor/ceiling
gradient, evaluated analytically in direction space. The default bake is 128
texels per cube face, six mips, with a fixed GGX importance sequence; total
GPU storage is well under 4 MiB. `BakeOptions` clamps the face size to 4–256 and
the mip count to the full chain, so the startup cost is bounded and the output
is bit-identical across runs.

The bake produces four derived views of the same radiance:

- the base RGBA32F cubemap (Direct3D face order +X, -X, +Y, -Y, +Z, -Z);
- a GGX-prefiltered specular mip chain, where roughness 0 selects mip 0 and
  roughness 1 selects the last mip (`roughness * (mipCount - 1)`);
- SH9 coefficients of irradiance/π for diffuse lighting, projected with the
  **exact per-texel cubemap solid angle** (`6 × size²` texels sum to `4π`), not
  an equal `4π/(6 × size²)` weight;
- a split-sum DFG integration LUT (64² R32G32_FLOAT, x = NoV, y = perceptual
  roughness) for `F0 * A + B`, including the Fdez-Aguera multiple-scattering
  compensation (ADR-0082).

The environment and LUT are uploaded once on the direct queue during
initialization and never re-baked for a camera move or a frame. Their SRVs
occupy two slots appended to each model's material descriptor heap, after the
thirteen neutral per-slot fallbacks: D3D12 permits only one shader-visible
CBV/SRV/UAV heap at a time, so a separate environment heap cannot be bound
alongside the material heap. The same slots are re-written when progressive
material descriptors are rebuilt.

The vertex shader keeps the camera-relative position/normal precision policy:
it subtracts the camera origin in double precision but leaves directions in
the fixed Z-up world frame, and the view/projection matrix applies the camera
rotation. The environment path applies the existing world-from-camera
quaternion before sampling the cubemap. The Studio key/fill analytic rig is a
presentation rig. Studio exposes intensity, yaw and exposure in the per-frame
constant buffer.

Directional has one documented world-space sun (`render/LightRig.h`,
ADR-0087). Azimuth is normalized 0..1 around world +Z (0 = +X, 0.25 = +Y,
0.5 = -X, 0.75 = -Y) and elevation is radians clamped to 1.4835. The object
shader, the compact legacy shader, the nav-gizmo sun marker and the contact
shadow input all consume `DirectionalSunDirection`; because the shading vectors
already share the world frame, no second camera rotation is applied to the
sun. Orbiting the camera therefore moves the view around a stationary sun, and
moving the sun changes the lit base/coat face, highlight, gizmo bearing and
shadow together. Directional keeps the same Fresnel/BRDF-correct split-sum IBL
as its ambient reflection floor and adds no Studio key or unweighted
reflection (ADR-0082, ADR-0087). Studio, Clay and Directional share that IBL
path. Clay does not merely set alpha to one: it derives an effective draw
bucket on the CPU, so ordinary triangles including authored
BLEND/transmission render as opaque depth-writing geometry (ADR-0086). Authored
MASK cutouts keep their sampled coverage, so modelled holes survive. The Clay
shader replaces authored colour, metal/roughness, clearcoat, sheen, specular,
emission, transmission and normal maps with one neutral matte and a softened
form rig; it does not change exposure, the environment bake or the
Studio/Directional rig.

The object shader writes linear HDR radiance into an
`R16G16B16A16_FLOAT` scene target; it deliberately does not apply the display
transform. A separate display-space `R8G8B8A8_UNORM` backdrop target carries
the selected backdrop and the local ground treatment. A full-screen resolve
pass reads both targets, recovers straight-alpha scene color, multiplies by
exposure, applies the selected tone curve, then composites the tone-mapped
model over the display-space backdrop before writing sRGB to the swap-chain
back buffer. Tone mapping the model before compositing keeps alpha and
transmission correct; keeping the backdrop outside the curve is what makes
`#1C1C1E` reach the screen as `#1C1C1E` (ADR-0079).

The default curve is still Khronos PBR Neutral (`startCompression` 0.76,
desaturation 0.15); ACES, Filmic and None are selectable, and non-finite scene
values are contained before the curve. Exposure is a view setting applied
before the curve. `[` / `]` change it, `\` cycles the background and `` ` ``
cycles the tone curve; exposure, background and tone-map choice are persisted
with the other view preferences. Background choices are "Windows Dark"
(`#1C1C1E`, the default), "Studio gray" (`#808080`), "Custom color…"
(native color picker, packed `0xRRGGBB`), Transparent and Checkerboard.
Transparent mode preserves coverage alpha and premultiplies the display color;
Checkerboard is drawn in display space and is an explicit transparency-review
choice. Neither preset selection nor model counts rewrite the global background
or ground-highlight preference.

Presentation presets are selected from a bounded summary of the imported
material catalog (base-color texture presence, metallic/roughness range,
alpha/transmission share, unlit flags, point-only geometry and CAD format).
Studio PBR, Studio CAD, Scan/Sculpt, Transparent and Point cloud each supply
environment intensity, direct key/fill/rim multipliers, exposure, grid and
ground-shadow defaults. Background and ground-highlight are deliberately not
preset-controlled. The selection is a pure function in
[`Presentation.h`](../../interactive-viewer/src/render/Presentation.h); the
material loop is capped independently of the file. An explicit toolbar/preset
choice pins the session and wins over later catalog recommendations.
Environment intensity scales the direct rig and IBL together, matching F3D's
paired `render.light.intensity` / `IBLIntensity` behavior. The CPU reference in
`StudioEnvironment.cpp` and `Presentation.cpp` and their HLSL copies are kept
in lock-step by the `[studio-ibl]`, `[brdf]`, `[tonemap]` and
`[studio-presets]` unit tests, including pixel-shader compile checks at the
feature-level-11_0 baseline.

T28 calibrates the shipped bake instead of adding a second lighting path.
`StudioEnvironmentVariant::ProductProcedural` (bake version 2) is the only
automatically selected variant; `NeutralReference` exists solely for the
measured comparison and is never picked by the product. The camera-relative
rig directions are `(0.45, -0.55, 0.70)`, `(-0.70, 0.25, 0.38)` and
`(0.18, 0.76, 0.28)` with base intensities 1.30/0.38/0.24. Preset and user
key/fill/rim scales are multiplied on the CPU before they reach the shader, so
the environment intensity scales the rig and IBL together and there is no
second literal intensity source. The bake exposes its version, variant,
identity hash, resident bytes and bake time through the Debug test-control
surface for evidence; those fields are read-only and do not add shipping
authority. Presentation selection uses contributed triangle shares (alpha or
transmission, unlit, textured) in addition to the bounded catalog summary, so
a minor glass detail cannot force the transparency checker and density alone
cannot turn an authored PBR mesh into a scan. There is no per-model-name
tuning.

## Ground contact shadow, highlight and ambient occlusion

Objects sit on a ground layer that is independent of the grid. T31 replaced the
T16 analytic bounds blob with a geometry-derived fitted shadow map (ADR-0088).
[`GroundShadow.cpp`](../../interactive-viewer/src/render/GroundShadow.cpp)
transforms the verified model bounds with the active ground-axis matrix and
fits an orthographic light frustum around them using the stable Studio key
direction `(0.45, -0.55, 0.70)` or, in Directional mode, the same world-space
sun the material shader and gizmo use (ADR-0087). The light-space centre is
snapped to whole texels, the grazing elevation is clamped, and a
scale-invariant normalized depth bias is derived from the texel size, so tiny
and large models do not crawl or acne. `D3D12ViewerPath` renders the current
triangle set into a 2048² `D32_FLOAT` depth target plus a small `R8_UNORM`
caster-attenuation target (1024² allocation fallback). Bounds only size the
frustum and the ground quad; they are never the shadow silhouette.

The caster pass is depth-only: `MASK` keeps its authored alpha test and its
holes, double-sided/mirrored geometry culls nothing, and `BLEND` coverage or
decal surfaces do not become opaque occluders. Transmissive surfaces still
occlude but contribute a bounded approximate attenuation
`clamp(1 - 0.60 × transmission, 0.40, 1)`, so clear glass cannot paint a solid
black silhouette; this is not caustics or volume absorption. The ground layer
samples the map with a comparison sampler and 3×3 PCF, subtracts the fitted
bias, and writes premultiplied black coverage into the linear HDR target. The
resolve composites that coverage over the display-space backdrop, so exposure
and tone-map changes cannot invert it. Pixels outside the fitted map are lit,
and disconnected parts, strap loops, cutouts and empty space stay that way.
The depth and attenuation targets are per-device, participate in
`AccountedAllocationBytes`, are not viewport-sized (resize does not reallocate
them), and are recreated by device recovery.

The ground draw combines two independent flags. The contact shadow uses the
fitted map (or its labelled fallback); the illuminated highlight is a subtle,
local additive tint around the base (backdrop + 0.055 with a radial fade) in
the display-space backdrop (ADR-0079), enabled by default. Turning the highlight
off leaves the selected flat backdrop while the contact shadow still darkens
it; turning the shadow off leaves the highlight intact. `H`, the More menu item
and the persisted `groundShadowEnabled` preference toggle the shadow. The
Settings "Ground highlight" switch toggles `groundHighlightEnabled`
independently. Studio CAD/PBR/Scan enable the shadow by default; Transparent and
Point cloud disable it. A manual toggle is independent of the grid toggle and
survives later preset applications. Checkerboard and transparent modes omit the
local ground treatment. Point/line-only content has no triangle caster and gets
no invented ellipse. If the shadow targets cannot be allocated, the T16
analytic bounds ellipse is used as the documented lower-quality fallback; it is
not the normal path on supported hardware. Screen-space AO is a separate layer
(T32) and can never gate or replace the contact shadow.

Ambient occlusion is sampled only from an authored `occlusionTexture` (protocol
v13 dependency slot 4); the metallic-roughness red channel is unused by the ORM
convention and is never treated as AO. The shader applies the glTF strength
formula `saturate(1 + strength × (sample - 1))`, with sample = 1 when the map is
absent and `strength` from the `occlusionStrength` field (default 1). The
occlusion slot has its own UV set, transform and sampler and is bound to `t5`.
AO multiplies indirect diffuse, environment reflection and ambient terms only;
direct `EvaluateLightSplit` key/fill/rim specular is never AO-scaled, so metals
do not read as dirty.

### Screen-space ambient occlusion (T32)

T32 adds a bounded, scale-aware SSAO pass over the existing scene depth
(ADR-0089). The opaque material pass writes its environment/IBL/ambient share to
a second `R16G16B16A16_FLOAT` target alongside the full lit colour and pick id
(the AO-off fallback keeps the historical two-target scene+pick layout). After
the opaque and Clay-overlay draws, the depth buffer is transitioned to a shader
resource and two viewport-sized `R8_UNORM` targets are produced: a raw mask and
an edge-aware 5×5 bilateral filtered mask. The resolve subtracts only the
indirect share it receives:

```
adjusted = max(sceneStraight - indirect * (1 - aoMask), 0)
```

before the OIT composite, exposure, tone map and sRGB transfer. Direct light,
emission, the selection outline, transparent reflection/transmission and the
display transform are never multiplied by the mask. Authored ORM AO remains
folded into the indirect term; the screen mask composes softly with it rather
than applying a second full multiply.

Depth reconstruction uses the inverse view-projection and the camera-target
relative conventions already used by the material shader, so perspective,
orthographic and the blended projection share one path. A 12-sample
interleaved-gradient spiral uses a world radius that is a fraction of the fitted
scene radius, converted to a bounded pixel radius from the reconstructed frame
(no fixed model-unit constant and no perspective-only assumption). Neighbouring
background/depth-discontinuity pixels return an unoccluded value, which keeps
silhouettes, narrow gaps, thin straps and the ring hole intact. The spiral is
fixed and there is no temporal history or reprojection.

Per-mode defaults are Studio 0.60 strength / 0.10 scene-radius fraction, Clay
0.85 / 0.14 (geometry review) and Directional 0.55 / 0.09. The `A` key, the More
menu item and the persisted `ambientOcclusionEnabled` preference toggle the
layer; Debug fields 287-298 inside the existing test-control guard expose the
toggle, bounded overrides, target bytes, pass time, fallback reason and raw
reconstruction/normal/gap/radius inspection views. Transparent and MASK
surfaces do not write the opaque depth the AO passes sample, and transparent
draws do not write the indirect target, so a window or cutout cannot become an
opaque AO sheet or a background halo. The T27 transmission snapshot is copied
before the resolve subtraction, so transmitted radiance keeps its authored
glass response; a dedicated transmitted-radiance AO pass is deferred.

The indirect target and two masks are viewport-sized, included in
`AccountedAllocationBytes`, recreated by resize and device recovery, and derived
from the current published drawable set every frame (streamed/superseded
geometry cannot leave stale AO). A two-query GPU timestamp heap, resolved into a
16-byte readback after the frame fence, records the AO pass time without a
same-frame wait (Debug field 299). Allocation failure, an unsupported extent, a
Debug force, the diagnostic channel or point/line-only content records an
AO-off fallback and draws normally; it is never a startup failure. The AO layer
is applied after the contact shadow so it cannot double as a missing shadow.

### Material lobes and transparency

The textured pixel shader reads its complete material record from a per-material
`b2` constant buffer (`MaterialConstants`): base color, the protocol-v12 lobe
block, both normal scales, the map mask and thirteen per-slot UV/sampler pairs.
Moving the material block out of root constants is what keeps the root signature
inside D3D12's 64-DWORD budget; v14's fifteen single-descriptor SRV tables,
the 25-DWORD draw block and the two CBVs total 44 of 64 DWORDs. Per-slot data
never becomes root constants. The fixed 1024-byte per-frame material stride and
lifetime rule are recorded in ADR-0080 and ADR-0083. Each material owns a
CPU-writable UPLOAD buffer with one region per swap-chain frame, so a progressive
material/texture update never races a frame that is still sampling the previous
copy. Peak shader model remains 5.1. Unbound slots point at one of thirteen
neutral descriptors; the shader's map mask skips them, so a fallback descriptor
is never reported as an authored resident map.

- **Specular / IOR.** Dielectric F0 is `((ior-1)/(ior+1))²` tinted by the
  authored `specularColor`; `specularFactor` is the F90 grazing reflectance
  (KHR_materials_specular), so normal and grazing incidence stay independent.
  The metallic path uses base color as F0 and 1 as F90. The old fixed 0.04
  survives only as the default IOR 1.5 result.
- **Environment BRDF.** `F0 * A + B` from the split-sum LUT, with the
  Fdez-Aguera multiple-scattering term. A white metal (`F0 = 1`) is the
  white-furnace identity (scale 1.0) at every roughness. Direct light uses the
  height-correlated Smith GGX visibility and the same F0/F90 Fresnel.
- **Clearcoat.** A second dielectric GGX layer. The base (diffuse, specular and
  sheen) is mixed toward the coat by `clearcoatFactor × F_schlick(coatF0, 1,
  NoV)`; emission beneath the coat is attenuated by the same `(1 - cF)`.
  There is no flat `(1 - clearcoatFactor)` removal of base F0, and the coat
  lobe does not carry a second Fresnel factor.
- **Sheen.** A grazing-only `pow(1 - N·V, 3)` velvet term, strongest for a
  rough weave, added at low weight. It never applies to the clay mode.
- **Emissive.** `emissiveStrength` multiplies emissive radiance before the
  resolve pass, so an authored 5× sign enters tone mapping as linear HDR
  rather than clipping at 1.0.
- **Occlusion strength.** Applied through the same glTF formula above.

Authored texture metadata is applied per slot, not per material: slot 0 keeps
the legacy UV transform fields and slots 1-12 carry their own
offset/scale/rotation plus the packed UV set, U/V address mode and
nearest-filter intent. Linear/mipmap slots sample through a bounded 4×
anisotropic sampler for oblique surfaces; an authored nearest slot routes to
the point sampler and keeps its exact texel semantics. A slot whose UV set is
not 0 is reported invalid and skipped; it is never sampled with UV0 coordinates. The base normal map's
tangent-space XY is multiplied by `normalScale`; the coat normal uses its own
`clearcoatNormalScale` and perturbs only the coat lobe. Both share one TBN basis
formed in camera space from the transformed normal and tangent; a zero-length
tangent falls back to screen-space derivatives of the active normal-map UV. A
mirrored (negative-determinant) instance multiplies the tangent handedness by
the instance sign. For a double-sided material the geometric normal is reversed
on a back-facing fragment *before* the TBN is formed (glTF/Khronos reference
behavior), so the normal map keeps its authored slope rather than being flipped
toward the eye.

T25's extension maps multiply their scalar factors per the Khronos channel
definitions: clearcoat R, clearcoat roughness G, specular factor A, specular
color RGB, sheen color RGB, sheen roughness A and transmission R. Each keeps
its own sampler/UV metadata and color space; a color map and a data map that
share one authored image decode into distinct sRGB/linear chunks. Diagnostic
channels 24-30 expose the post-map effective inputs (`clearcoat/roughness`,
`specularFactor`, `specularColor*F0`, `sheenColor`, `transmission`,
`coatNormal`, `sheenRoughness`) so the T25 lane can verify variation across each
probe without inferring it from lighting.

Transparency uses **bounded weighted blended OIT** (ADR-0084). Opaque and
masked geometry render first with depth writes and the integer pick target.
Transparent triangles then render in one pass into two fixed `RGBA16F` targets:
premultiplied weighted colour and a revealage product. The pass tests against
the opaque depth and never writes depth, so hidden geometry cannot contribute;
intersecting triangles inside one primitive, nested front/back glass, decals
and shuffled draw/primitive order all accumulate commutatively. The full-screen
resolve samples both targets and composites the transparent average and
coverage in **linear HDR before exposure and tone mapping**, then applies the
display-space background. Mesh-centre depth is no longer the transparency
authority (transparent draws use a stable material/chunk order; the sorted path
is retained only as a recorded fallback).

Resource work is bounded by the viewport, not by a peel count: two targets, one
extra geometry pass, no per-pixel linked list and no per-layer allocation. The
validated stable overlap range is eight layers; beyond it the result is still
order-independent but is a weighted average, not an exact front-to-back stack.
The two targets are accounted in the existing GPU budget and participate in
resize, cancellation, device-loss recovery and a low-memory fallback. If the
targets cannot be allocated the sorted straight-alpha path is used and
diagnostics fields 243/244 report the fallback; it never silently replaces the
high-quality path. Depth peeling is not used.

Clay derives a per-frame **effective draw bucket** rather than changing only the
shader. All triangles with authored BLEND or transmission move out of the
transparent bucket into an opaque overlay bucket drawn after the solid prepass;
the overlay writes depth, tests with `LESS_EQUAL` and a bounded negative depth
bias, and is sorted far-to-near. This keeps a glass surface coplanar with its
trim from z-fighting and prevents authored alpha from exposing interior meshes,
without retaining the authored Blend/OIT PSO. MASK triangles stay in the opaque
bucket and keep their sampled cutout, so modelled holes are not filled. Lines
and points keep their existing pipelines. The imported material data is never
mutated: the override is recomputed from the lighting mode each frame, and the
per-material debug record reports both the authored and effective bucket/depth
state (fields 143-145 and 274-276).

Coverage, interface reflection and transmission are independent material
inputs. Translucent glass samples the finished opaque scene in linear HDR
through a bounded transmitted-radiance pyramid (ADR-0085): after the opaque
pass a `RGBA16F` copy with eight mip levels is taken, and a transmissive
fragment samples its own screen position at a roughness-selected level. Surface
reflection uses the normal split-sum/Fresnel path; transmitted radiance is
`background * baseColor * transmissionFactor * (1 - F_ior)`. There is no
refraction offset, inter-layer transmission or invented volume thickness; where
the copy has no opaque coverage the studio environment in the transmitted
direction is the fallback.

Authored coverage stays separate: BLEND keeps its alpha, OPAQUE and MASK cover
the pixel, and interface Fresnel is never folded into alpha. OPAQUE
transmissive materials (for example the Camera lens) are routed through the
transparent pass without rewriting their authored alpha mode or coverage. MASK
transmission keeps its authored cutout and stays opaque. A double-sided
transmissive surface shades whichever authored face the camera sees: a single
plane is one thin interface and remains visible from both sides, while a closed
shell contributes two bounded thin interfaces. Two overlapping transmissive
layers each sample the opaque snapshot, not each other; T26's order-independent
composite still resolves them stably. The Camera lens keeps both its transmitted interior
depth and a Fresnel reflection; Car BLEND glass keeps authored coverage visibly
independent of its interface reflectance.

Allocation failure of the transmission copy is a recorded degradation:
transmissive materials fall back to their authored dielectric response, with
coverage unchanged, and diagnostics fields 259-263 report bytes, mip count,
reason and OPAQUE-transmission draw count. `KHR_materials_volume` absorption and
refraction remain a specific compatibility degradation rather than a fabricated
thickness. Both the OIT targets and the transmission pyramid are accounted in
the existing GPU budget and participate in resize, cancellation, device-loss
recovery and the recorded fallbacks.

Selection is a separate nearest-visible pass after the colour passes. It writes
only the `R32_UINT` pick target and depth, reuses the material shader's alpha
cutout discard, and never averages ids. A low-coverage decal or cutout is
selectable; geometry behind an opaque surface and back nested glass sheets are
not. Base-colour texture alpha is preserved for MASK/BLEND materials so
textured cutouts discard correctly. The T33 spatial edge filter runs after the
OIT/coverage composite and never samples this pick target, so mask/cutout
coverage and transparent ordering are unchanged by AA.

### Material and light diagnostics

T21 adds a bounded Debug inspection surface to the same textured PBR shader.
The channel selector travels in the otherwise-unused `gPresentation.w` frame
constant; channel 0 is the normal product render, 1-20 select raw linear views
(base colour, geometric/shading normal, UV0, metallic, roughness, effective AO,
coverage alpha, transmission, IOR/F0, clearcoat, sheen, isolated direct/
environment diffuse and specular, coat, emission, pre-tone-map HDR). T24 adds
21 (the effective environment BRDF scale `F0*A+B` with multiple scattering),
22 (`coverage, interfaceFresnel, authoredTransmission`) and 23 (a flat 0.3
linear display-calibration patch). T25 adds 24-30 for the effective
clearcoat/roughness, specular factor, IOR-tinted specular F0, sheen color,
transmission, coat normal and sheen roughness after map × factor. T27 adds 31
(the tinted/weighted transmitted radiance) and 32 (the raw untinted transmitted
scene sample). During a diagnostic frame the transmitted-radiance snapshot is
not regenerated: channel 31/32 reads the last product snapshot instead of the
debug-channel image itself. The shader writes linear values and the resolve
pass remains the only clamp.
UV1 is not on the wire; channel 5 renders a deliberate magenta marker and the
capability mask reports the channel unsupported rather than substituting
neutral data.

T24 also records the exposure/tone-map pair the last resolve actually used
(Debug fields 231/232) so the CPU tone-map reference is compared with the GPU's
effective values. The material shader contains no tone curve or sRGB transfer;
the tone-mapped/flat-patch comparison proves exactly one display encoding.

Numeric verification copies a bounded (at most 64x64) RGBA16F region of the HDR
scene target into a readback resource, waits on the frame fence, and averages it
on the CPU. This keeps numeric HDR readback independent of the display transform
and proves the values through actual GPU output.

The same Debug surface exposes a bounded per-material draw record: source
geometry/material/node/instance ids, vertex/index counts, validated wire values
(including both normal scales, the thirteen per-slot sampler/UV-set flags and
the per-slot transforms), bound texture chunk ids/formats/colour spaces for all
thirteen material slots and the effective bucket
(opaque/masked/blended/transmissive), depth-write and cull state. Fields
201-230 extend the T21 record. T25 selects one of the eight extension slots with
field 233 and reads its flags/transform/binding through the bounded 234-242
window, so the published field range does not grow per slot. The earlier fields
keep their meaning and ordering. The viewer
never parses glTF for this; source metadata is read only by test tooling and
matched against the viewer's validated snapshot. All fields remain inside the
existing `PREVIEW3D_ENABLE_TEST_CONTROL` + `--app-smoke` guard, so a shipping
Release binary answers none of them (verified by T21).

### Line and point topology

`ChunkTopology::LineList` is an indexed pair list: `indexCount` is even and each
`{indices[2i], indices[2i+1]}` pair is one independent segment. There are no
strips or loops on the wire. glTF `LINES` maps directly; `LINE_STRIP` and
`LINE_LOOP` normalize to pairs; triangle strips/fans become indexed
`TriangleList` with degenerate stitches dropped. OBJ `l`/`p` and FBX
`line_curves` use the same pair contract. The validator rejects an odd index
count or an out-of-range index as `MalformedData`, and line payloads use the
triangle vertex/byte caps.

The viewer draws lines with a position-only vertex/pixel shader, a dedicated
`D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE` pipeline, negative depth bias and
`LESS_EQUAL` with depth writes off, so authored curves stay visible over
coplanar surfaces without occluding them. Lines participate in bounds,
visibility, LOD and the pick target. `PointList` remains unindexed and renders
as bounded round/colored point splats; the point pipeline keeps its own pick
identity so a point cloud can be selected even though it has no triangles.

## Upload ring

The upload lane owns a persistently mapped ID3D12Resource in an UPLOAD heap. Its initial size is 256 MiB, allowed to grow in 64 MiB increments up to the lesser of 512 MiB and its CPU/GPU memory budget. It is a logical ring of allocations aligned to D3D12 placement and copy requirements.

Each allocation records offset, size, generation, destination resource, and the copy fence value that retires it. Allocation follows these rules:

- reclaim from the tail only when GetCompletedValue covers the allocation's fence;
- wrap only into proven-free space;
- split input into bounded chunks rather than waiting for one huge contiguous segment;
- when full, only the upload coordinator may wait on the copy-fence event, with cancellation and shutdown also in its wait set;
- apply backpressure to the bounded ReadyForUpload queue before exceeding memory budgets;
- never expose the CPU pointer or upload resource to parser workers.

The coordinator copies normalized bytes into the ring, records CopyBufferRegion or texture footprint copies into DEFAULT-heap resources, closes and executes a batch, and signals a monotonically increasing copy fence. Default buffers are created in COMMON. On the copy queue they may be implicitly promoted to COPY_DEST; buffer resources decay back to COMMON after the ExecuteCommandLists boundary. Texture transitions obey the explicit copy-queue-compatible state rules and are finalized before publication.

The fast path uses batches large enough to amortize submission but small enough to publish progressively: normally 16–64 MiB or 2 ms of recorded copy work, whichever comes first.

## Fence-complete publication

Copy submission does not make a resource renderable. The coordinator maintains a min-ordered list of PendingPublication records. When the copy fence reaches a record:

1. Graphics validates that its generation is still current.
2. The resource moves from Uploading to Ready.
3. A new immutable SceneSnapshot references the ready mesh/texture descriptor.
4. The renderer may use the resource on its next frame, causing a valid COMMON-to-read promotion/transition on the direct queue.

An incomplete resource is absent from every renderable snapshot. Consequently, the direct queue does not issue Wait against a copy fence for ordinary streaming; it keeps drawing the previous proxy/LOD. An explicit cross-queue wait is reserved for a rare atomic dependency batch that cannot be represented by prior publication, and it must be justified by a performance test.

This is an epoch/handle swap, not an unowned atomic raw pointer. Old snapshots remain alive until the last direct fence that used them completes.

## Scene lifecycle

Resource states follow:

    Parsed -> ReadyForUpload -> Uploading -> Ready -> Visible
                                              |
                                              v
                                      EvictRequested
                                              |
                                              v
                                    Retired -> Released

Cancellation can divert any pre-visible state to Retired. A new file does not remove the current scene until the new generation has a complete coarse proxy, unless the user explicitly closes the current document. This prevents a bad or slow file from blanking useful content. After a successful proxy handoff, old resources retire behind their last direct fence.

Geometry and textures publish independently, with neutral material fallbacks until dependent textures are ready. A material snapshot changes only at a frame boundary.

## Progressive LOD

The renderer always selects from resources in Ready state:

- while early loading, draw the partial coarse proxy and an explicit loading treatment;
- once the complete coarse catalog is ready, draw it as the always-resident safety representation;
- request intermediate/full chunks by projected screen error, visibility, distance, and camera motion;
- keep a parent LOD visible until every child needed for the replacement region is ready;
- cross-fade or dither a replacement for a short fixed interval to avoid obvious pops;
- reduce detail rapidly during camera motion and refine after a short idle hysteresis.

Requests include generation, cluster, desired LOD, priority, and last visible frame. The scheduler favors visible center-screen clusters, then silhouette-relevant large projected error, then near-camera prefetch. A bounded cancellation-aware priority queue prevents a camera sweep from producing unbounded obsolete work.

The detail replay is a bounded background refinement session, not part of the
forward load. The broker keeps it available for a generous bounded window before
the first visible request, then for a short bounded idle window between
requests, and ends it cleanly with `ControlOpcode::EndDetail` (or at a bounded
request count). The worker returns to the pooled process; the generation is
already reported complete and every accepted detail chunk is in host memory. An
outstanding detail reply that never arrives still terminates at the absolute
generation deadline. This prevents a fully loaded model from turning into a
late `ResourceLimit` merely because the viewer stayed idle.

Complete-load readiness is terminal and resident, not brightness-based. A
capture requires the expected generation, `Ready`/complete (or an explicit
degraded completion), resident geometry and texture counters, no pending detail
request, settled pixels and quiet counters. A terminal error observed before or
after the first visible geometry is a failed load, so a previously visible model
is never treated as qualified evidence (ADR-0081).

## Video-memory budget and residency

IDXGIAdapter3::QueryVideoMemoryInfo is the authority for current local/non-local usage and budget. The app registers for budget-change notifications and also samples at a low frequency while streaming. On UMA, “local” memory is shared system memory and the CPU working-set limit also constrains allocations.

The reserved set contains swap-chain/depth resources, pipeline assets, permanent fallback textures, frame data, and the coarse proxy. The detail target is recalculated as:

- no more than 60% of the reported local budget for all app-managed model resources;
- at least 512 MiB of reported budget headroom when the budget permits;
- no more than the format generation's CPU/GPU policy cap;
- reduced immediately when the OS budget falls.

If the reserved set itself cannot fit with headroom, the renderer lowers proxy density and texture resolution. It never treats CreateCommittedResource success as evidence that future allocations are safe.

Fine chunks use an LRU weighted by visible recency, projected error, re-import cost, and dependency sharing. Eviction removes a chunk from a new snapshot first, waits only in deferred retirement for the last direct fence, and then releases or uses explicit heap residency APIs where profiling justifies them. The MVP uses D3D12 Memory Allocator (D3D12MA) for suballocation and budget telemetry, but its policy remains product-owned.

D3D12MA is configured as one device-wide allocator instance, not one physical DEFAULT heap: it intentionally manages a pool of separate heaps and must keep buffers, render-target/depth-stencil resources, and small vs. large textures in distinct heaps on Resource Heap Tier 1 hardware, where mixing resource categories in one heap is not supported. "One allocator" describes the object the renderer talks to, not a claim that model geometry, textures, and target resources ever share underlying heap memory.

The system does not promise full-detail simultaneous residency for an 8 GiB source. Its contract is a stable proxy plus view-prioritized refinement.

## Textures

HDR and EXR material textures arrive as CPU-tonemapped RGBA8 (color/emissive
use Reinhard + sRGB; normal/data stay linear), so the GPU format contract does
not grow for T13. A future IBL source uses the import worker's separate linear
equirect float environment hook (bytes-only, budgeted) rather than a material
image chunk.

Validated KTX2/DDS mip tails and transcoded Basis mip levels are uploaded before higher-resolution mips. Compatible precompressed BC resources avoid RGBA expansion and runtime mip generation. Texture resources are created with copy-compatible layouts and immutable final content per published resource. The MVP does not overwrite a texture while it is sampled; a more detailed texture is a new handle swapped into a later material snapshot.

Sampler count is bounded by a small static sampler set (linear/anisotropic,
point/nearest and environment wrap); authored nearest intent is never upgraded
to linear. Runtime mip generation is semantic (ADR-0090): color and emissive
reduce in linear light; the base-color map (ColorCoverage) additionally uses
coverage-weighted alpha (so a cutout keeps its covered fraction and does not
gain a coloured fringe) while sheen/specular color maps keep their unused alpha
out of the reduction. Normals average the
decoded tangent vectors then renormalize, metallic-roughness averages the green
roughness channel in squared GGX-alpha space and converts back to perceptual
roughness to preserve highlight variance, and AO/factor data stays a plain
linear area average. Alpha mode, color space, normal/data usage, dimensions,
source digest, transcode target, and transcode version are part of a texture cache key. Live GPU texture-cache scope remains one active document; eligible decoded/transcoded mip data may also use the persistent derived cache below.

## Persistent derived-data cache

The viewer maintains a best-effort per-user cache under `%LOCALAPPDATA%\Binbuf\Preview 3D\DerivedCache\v1`. It is enabled by default and may be disabled or cleared from the keyboard-accessible overflow menu. The enabled flag is the only persisted viewer preference and is stored in a bounded, versioned, atomically replaced settings file adjacent to—not inside—the cache. A missing/corrupt setting restores the default. The Shell thumbnail provider never reads or writes either location.

An opaque entry key covers primary and geometry/material dependency version evidence, parser/normalizer versions, enabled feature set, and cache schema. A cheap source preflight discovers and safely opens current dependencies before a hit is accepted. The fast identity path requires a stable local volume/file ID plus an accessible change-journal ID and per-file USN, all captured from the open handles together with length; last-write time alone is never sufficient. If that evidence is unavailable or the journal was reset, the reader must verify a full streaming content digest before using the entry, or treat it as a miss when hashing would exceed the current load budget. The entry stores only opaque identity digests, not dependency names or paths. An entry contains only normalized coarse proxy data, bounds/chunk catalog, eligible reusable fine chunks, and decoded/transcoded texture levels. It contains no original encoded file bytes, source path, node/material names, or author metadata. Geometry is GPU-independent; any texture variant is keyed by DXGI format and decoder/transcoder version.

Cache data is untrusted. Before any byte reaches an upload batch, the reader validates the manifest, version tuple, every current source/dependency version token or required content digest, entry and section lengths, checked offsets/counts, per-section SHA-256, normalized-scene invariants, and generation. An absent, stale, partial, corrupt, unsupported, or over-budget entry is a miss. Only then may a cache hit publish its verified coarse proxy. Optional background source verification may continue, but it is not a substitute for the identity/content validation required by the hit. Warm-cache performance gates apply to fixtures on volumes that provide the fast identity path and report the validation mode separately.

The default global soft cap is 10 GiB and no entry may exceed 2 GiB. The cache stops admitting writes and schedules LRU eviction when the cap would be exceeded or free disk falls below the greater of 10 GiB and 10% of the volume. Cache maintenance is low-priority, cancellable, rate-limited, and never delays first geometry, Ready, close, or shutdown. Writers create current-user-only, non-executable temporary files, flush a complete manifest, and atomically rename; abandoned temporaries are removed on the next launch. Clear cached previews closes active cache leases, deletes entries through the cache coordinator, and reports completion without deleting source files or the Windows thumbnail cache.

## Input and camera concurrency

The UI thread converts pointer, wheel, keyboard, and capture changes into compact input events. The render thread drains them before camera update. Move events may coalesce; button, key, and capture transitions may not. Camera math never reads HWND state directly.

Bounds events update camera scale and target through AppState. A provisional-to-verified correction auto-frames only if the camera's userInteractionEpoch has not changed since open. Near/far behavior derives from verified scene scale with finite clamps so millimeter and planetary coordinate ranges do not destroy depth precision.

## Device loss and recovery

Every device-facing call that can fail is checked. On DXGI_ERROR_DEVICE_REMOVED or RESET:

1. Stop accepting uploads and capture GetDeviceRemovedReason plus DRED breadcrumbs/page-fault data in the local diagnostic log.
2. Publish a non-blocking recovering state to the UI.
3. Retire the device lanes and rebuild the adapter/device/swap chain once.
4. Recreate the coarse proxy and then visible detail from retained normalized cache/source ranges.
5. If recovery repeats, preserve app responsiveness and show a stable GPU failure with Copy diagnostics action.

Recovery never retries in a tight loop. Unsupported hardware fails before file parsing begins.

## Shutdown

Normal frames never flush the GPU. On document close, generation cancellation and fence-based deferred release proceed asynchronously. On process shutdown only, the render and upload coordinators stop accepting work, signal their queues, wait with finite diagnostics timeouts for their last fence values, release D2D/D3D11On12 before D3D12, then close handles. A driver hang must not leave the UI thread waiting forever; shutdown escalates to process exit after diagnostics are recorded.

## Required assertions and telemetry

Developer builds assert queue ownership, generation identity, legal resource-state use, allocator retirement, ring non-overlap, descriptor lifetime, and snapshot fence coverage. ETW events record:

- parse/decode/normalize/simplify/upload bytes and durations;
- derived-cache lookup/validation/write bytes, hit/miss reason, eviction, and warm-open milestones;
- import-worker and compatibility-host start, brokered bytes, shared-section validation, peak commit, cancellation, and exit reason;
- ring occupancy/backpressure and copy batch sizes;
- fence submission/completion latency;
- per-frame CPU time, GPU time, present result, and queue wait source;
- proxy/full-detail readiness;
- local budget, app model bytes, eviction, and allocation failure;
- cancellation latency and stale events dropped.

Release logging contains no model data or full paths by default.

Primary references: [D3D12 fence-based resource management](https://learn.microsoft.com/windows/win32/direct3d12/fence-based-resource-management), [resource barriers](https://learn.microsoft.com/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12), [multi-engine synchronization](https://learn.microsoft.com/windows/win32/direct3d12/user-mode-heap-synchronization), [DXGI video-memory reservation](https://learn.microsoft.com/windows/win32/direct3d12/residency), [D3D12 root signature limits](https://learn.microsoft.com/windows/win32/direct3d12/root-signature-limits), [DirectX shader model 6.6 dynamic resources](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html), and [D3D12 Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator).

### T22 cache identity and material application

The derived-cache key uses `model_core::kDerivedCacheParserIdentity`
(protocol, glTF importer, material-semantics and vertex-layout versions) rather
than a caller default; cache schema is 4. A different parser identity misses
even for the same source file identity. The v13 authored inputs are applied by
the shader: normal scale, per-slot UV/sampler/transform metadata, the
independent occlusion slot and the front-face/mirrored TBN rules described
above. MR red is not read as AO anywhere.

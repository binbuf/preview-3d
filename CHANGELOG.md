# Changelog

All notable changes to Preview 3D are documented here. This project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) and the format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Entries are grouped by release tag, newest first. `0.2.0` was the first tagged
release; it includes the initial development of the viewer, so its notes cover
the whole pre-release build-up as well as the changes made in that tag.

## [0.3.9] - 2026-09-25

### Added

- The bottom-right warning badge is now an interactive control, and missing
  sidecar assets are named. Opening a model whose glTF/OBJ/FBX/USD sidecars
  cannot be found (a texture, `.bin` buffer, `.mtl` file, or USD layer) shows
  the badge; clicking it lists the warning text and every unresolved reference.
  A **Locate folder…** button opens a folder picker, adds the chosen directory
  as an asset search root, and re-imports the model so the assets can resolve.
  The chosen root is matched by file name (in the folder and its
  `texture`/`textures` subfolders) and still goes through the same canonical
  containment and size checks, so it can never address anything outside itself.
  The existing "Model warnings…" menu entry opens the same dialog.

### Fixed

- glTF materials that author `KHR_materials_transmission` together with
  `KHR_materials_ior` at or below 1 no longer render as see-through ghost
  glass. `KHR_materials_ior` is now enabled in the parser, so the transmission
  guard can read the authored index of refraction instead of fastgltf's 1.5
  default -- the crew-suit export that stamps `transmission=1`, `ior=1`, and
  `specular=0` on every material stays opaque.
- Downloaded model packages now resolve their textures for both common
  layouts: `model/source/model.<ext>` with a sibling `model/textures/`, and
  `model/model.<ext>` with `model/texture(s)/` or images beside the model. For
  a local import, a missing image reference is looked up by file name in
  exactly these directories, closest first: beside the model, its `texture/`
  and `textures/` subfolders, then the same under the parent. The search is
  image-only (buffers, MTL files, and USD layers are never relocated), matches
  case, space/underscore, and jpg/jpeg/tif/tiff spelling changes so renamed
  downloads still match, treats wildcards as misses, and keeps the ordinary
  canonical containment and size checks. The closest directory wins, so
  sibling model packages cannot collide. Names also match across material-role
  abbreviations (`_BaseColor`/`_B`, `_Normal`/`_N`, ...) and a trailing
  download annotation such as `_(Personalizado)`. glTF image URIs that use
  `../textures/...` resolve the same way (the authored traversal is reduced to
  its file name and never followed). Traversal, UNC/device, ADS, URL, and
  forward-slash drive references still fail closed, and the authored path is
  never opened. FBX image references that carry an authoring machine's
  absolute path are reduced to their file name first, so
  `C:\Project\Textures\foo.png` no longer fails the whole model with
  `UnsafeReference`.
- FBX materials whose base color map was not wired into the file but whose
  `<Material>_Base_color.png`/`.jpg` sits in a package texture folder now
  preview with that map. The inference is best-effort (a small, capped number
  of requests per generation) and can never fail a model whose authored maps
  already fit.
- The FBX importer downscales remaining maps to fit the aggregate
  decoded-texture budget instead of failing the model, matching the glTF path,
  and the transient encoded-image read budget is raised to 512 MiB so 16-bit
  4K PNG texture sets no longer run out while decoding.
- FBX texture-heavy packages no longer fail to import once the aggregate
  decoded-texture budget is reached: remaining maps are downscaled (to a small
  preview resolution at minimum) so the model loads with as many maps as fit,
  matching the glTF path. A genuinely exhausted budget still reports the typed
  resource limit.
- USD metallic and roughness inputs connected to maps the decoder cannot
  consume (for example separate EXR exports) fall back to the shader's scalar
  values -- UsdPreviewSurface's metallic 0 and roughness 0.5 -- instead of the
  texture-path 1.0 that made painted surfaces look like rough bare metal.

## [0.3.8] - 2026-09-25

### Fixed

- The OpenUSD compatibility host now whole-archives the statically linked
  `usd_m.lib`. A plain archive link dropped OpenUSD's file-format, schema, and
  resolver registration objects (`TF_REGISTRY_FUNCTION` global constructors that
  no caller symbol references), so `UsdStage::Open` failed for every USDZ and
  composed USD stage with the compatibility-host error.
- FBX and USD models whose materials reference formats the decoder does not
  handle (for example EXR normal/roughness/metalness maps) now import with those
  maps skipped and a bounded warning, instead of failing the whole model with
  `UnsafeReference`. Unsafe references still fail closed through the sidecar
  resolver.
- glTF materials authored with `KHR_materials_transmission` and
  `KHR_materials_ior` at or below 1 are no longer rendered as see-through ghost
  glass. An index of refraction of 1 has no optical interface, so the authored
  transmission is unobservable and the surface stays opaque.
- glTF `ALPHA BLEND` materials with no transparency source at all (opaque base
  color factor over a JPEG base color texture) are recovered as transmissive
  glass rather than an opaque cover. This matches the intended glass of exports
  whose separate opacity map was not included.
- USD skeletal bindings are ignored so skinned meshes preview their authored
  rest pose instead of failing; the blend/rest pose is what FBX already bakes.
  The skinning primvars are removed as well, because TinyUSDZ's converter
  indexes its skeleton table from joint indices even without a bound skeleton.

## [0.3.7] - 2026-09-23

### Added

- Import glTF scenes that require the archived `KHR_materials_pbrSpecularGlossiness`
  extension and approximate them as diffuse albedo with a dielectric response
  (metallic 0, roughness `1 - glossiness`). The combined specular-glossiness
  texture has no matching slot and is dropped with a bounded warning.

### Fixed

- Textured single-sided meshes no longer render inside-out. The textured D3D12
  pipelines culled clockwise triangles as front-facing while glTF and every
  importer author front faces counter-clockwise, so a model's near surface
  vanished and its far interior showed through.
- FBX materials that author both `Opacity` and the inverted `TransparencyFactor`
  (the common 3ds Max pairing) no longer render fully transparent, which made
  entire models invisible in the textured shading modes. The explicit authored
  `Opacity` now wins over `TransparencyFactor`.
- FBX and OBJ textures now sample with the correct V orientation. Those formats
  author a bottom-left texture origin while the renderer's samplers expect a
  top-left one, so texture atlases previously landed on their vertical mirror
  and mixed UV islands across the surface.
- Large embedded PNG, BMP, and TIFF textures decode again. Sources above a flat
  pixel budget were rejected even when a downscaled result would have fit, so
  4K PNG texture sets fell back to the 2×2 checker. They now pass through a
  bounded WIC scaler at the requested output size.
- FBX scenes that reuse one bitmap across several material maps decode it once
  instead of once per slot, so texture-heavy interiors fit the aggregate texture
  budget instead of failing with a resource limit.
- Progressive glTF texture publication accounts for the low-resolution copies it
  keeps alongside each full refinement, so texture-heavy scenes no longer trip
  the broker's independent aggregate texture cap on the refinement batch.

## [0.3.6] - 2026-09-22

### Added

- Load glTF sidecar buffers and textures from anywhere inside the primary
  file's directory tree (for example a `textures/` subfolder), not just beside
  the primary file. References that escape the primary file's directory are
  still rejected.
- A hero screenshot and a screenshot shot list in the documentation.

### Changed

- Render glTF `KHR_materials_transmission` as Fresnel-weighted glass instead of
  a fixed ~70% opacity. The transmissive factor is carried through to the
  shader in a new material flag, which suppresses transmitted diffuse and drives
  the blend alpha from the dielectric Fresnel term, so clear lenses and heavily
  tinted windows both read correctly while the authored base-color alpha and
  environment reflection are preserved.
- Reworked the studio lighting preset for more even, less blown-out shading.
- Made the directional-light sun azimuth a 3D control and improved the azimuth
  user experience and its interaction with the navigation gizmo.
- Expanded and reorganized the README, and added `FORMAT-SUPPORT.md`,
  `SECURITY.md`, `CODE_OF_CONDUCT.md`, `CONTRIBUTING.md`, and GitHub issue and
  pull-request templates.

### Fixed

- Transmission materials no longer discard the authored base-color alpha or
  ignore the environment reflection, and the material validator now enforces the
  transmission factor range only when the flag is set.

## [0.3.5] - 2026-09-21

### Changed

- Statically link the entire dependency closure using the
  `x64-windows-static-md` triplet. The release payload now contains only
  Preview 3D's own executables plus the Microsoft-signed CRT, removing every
  unsigned upstream DLL (for example `worker\zstd.dll`).
- Point the OpenUSD overlay port and dependent projects at the static install
  tree, link `usd_m` and the additional system libraries a static OpenUSD needs,
  and ship only the app images and OpenUSD resources.

### Added

- Smart App Control detection in the installer and application, with guidance
  explaining why an unsigned build may be blocked.

### Fixed

- Resolved Smart App Control blocking a bundled unsigned dependency (Bad Image
  status `0xC0E90002`) and made the whole payload eligible for the SignPath
  Foundation certificate, which cannot cover upstream binaries.

## [0.3.4] - 2026-09-20

### Added

- A GitHub Packages NuGet binary cache so OpenUSD and OCCT are built once and
  restored across releases, plus a dependency-restore workflow that runs on
  pull requests, `main`, manually, and weekly.

### Changed

- Route vcpkg's `buildtrees`, `packages`, and `downloads` onto the workspace
  volume instead of the runner image's `C:` drive, build Release only through an
  overlay triplet, and prune scratch after each package.

### Fixed

- Fixed release-runner disk exhaustion (`C1085`) that occurred while building
  OCCT during a release.

## [0.3.3] - 2026-09-20

### Fixed

- Corrected the OpenCASCADE vcpkg port dependency patch.

## [0.3.2] - 2026-09-20

### Fixed

- Release workflow fix.

## [0.3.1] - 2026-09-20

### Changed

- Completed the rename to **Preview 3D** across in-product text, registration,
  packaging, and documentation.
- Reworked the navigation gizmo and the shading-mode icons.
- Improved directional lighting.
- Updated the supported-format documentation.

### Fixed

- USDZ rendering fixes.

## [0.3.0] - 2026-09-19

### Added

- **Wavefront OBJ** support: `.obj` with optional local `.mtl` and texture
  sidecars.
- **FBX** support: binary or ASCII `.fbx` with static hierarchy, instances,
  supported materials and textures, and a deterministic baked start pose
  (FBX-001 through FBX-006).
- **USD / USDZ** support: `.usd`, `.usda`, `.usdc`, and `.usdz` through a
  TinyUSDZ fast path and a separately isolated, lazily started OpenUSD
  compatibility host for supported composition. Includes static meshes,
  hierarchy and instances, common primvars, display color, bounded USD Preview
  Surface materials and textures, and bounded local composition
  (USD-001 through USD-009).
- **3MF** support for the static preview subset: Core geometry, components, and
  build items; Materials and Properties colors and textures; Production model
  parts; and bounded Beam Lattice previews (3MF-001 through 3MF-007).
- **STEP / STP** support through a dedicated zero-capability OCCT host:
  self-contained ISO 10303-21 AP203/AP214/AP242 B-rep or authored AP242
  tessellated geometry, assemblies and reused definitions, instance/shape/face
  colors, and authored length units (STEP-001 through STEP-008).
- Multiple viewing modes and improved scene lighting.

### Changed

- Expanded the viewer from the initial glTF/STL/PLY core into a multi-format
  tool with separate isolated compatibility hosts for interop-heavy formats.

### Fixed

- Large-proxy and PLY timeout blockers; various USDZ rendering fixes.

## [0.2.3] - 2026-09-16

### Fixed

- Continuous-integration fix.

## [0.2.2] - 2026-09-16

### Fixed

- Continuous-integration fix.

## [0.2.1] - 2026-09-16

### Changed

- Improved controls user experience.
- Improved the CI/CD pipeline.

## [0.2.0] - 2026-09-16

The first tagged release. It contains the initial development of Preview 3D
(originally named "3D Preview") plus the first CI/CD pipeline.

### Added

- A native Windows 11 **D3D12** viewer with Blender-style orbit, pan, and frame
  controls, right-mouse fly navigation, and a selectable model up axis.
- Import of **glTF/GLB** (including Draco/meshopt geometry and KTX2/Basis,
  PNG, JPEG, and WebP textures), **STL** (ASCII and binary), and **PLY** (ASCII
  and binary triangle meshes and point clouds), with point-cloud rendering.
- A zero-capability AppContainer import worker using a read-only inherited file
  handle; models are never uploaded or modified.
- A bounded progressive display pipeline: verified bounds and precision
  metadata, progressive textures and chunks, coarse proxies with fence-safe
  handoff, live detail budgets with recovery, and recoverable import errors with
  a loading state (TSK-201 through TSK-209).
- Dedicated render thread, D3D12 upload ring on a copy queue, and a
  D3D11On12/Direct2D overlay bridge.
- Worker reuse and recovery, a qualification harness, portable and installer
  packaging, activation and accessibility work, and release acceptance
  (TSK-301 through TSK-305).
- An NSIS installer and portable ZIP, including Windows **Open with** /
  **Default apps** registration for supported formats, SHA-256 checksums, and an
  opt-in code-signing path.
- A status-bar render timer, loading animation, improved large-file load times,
  and single-instance / command-line / drag-and-drop opening.

### Fixed

- Large-proxy and PLY timeout blockers, and stray console windows.
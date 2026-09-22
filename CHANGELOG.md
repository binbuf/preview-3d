# Changelog

All notable changes to Preview 3D are documented here. This project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) and the format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Entries are grouped by release tag, newest first. `0.2.0` was the first tagged
release; it includes the initial development of the viewer, so its notes cover
the whole pre-release build-up as well as the changes made in that tag.

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
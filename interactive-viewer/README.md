# Interactive viewer

This project is the native Windows 11 scope-limited viewer. The thumbnail provider,
persistent caching, and intermediate LOD construction remain separate work.

## Included

- GLB 2.0 files with an embedded BIN chunk
- triangle meshes using float `POSITION` and optional float `NORMAL`
- unsigned 8/16/32-bit indices, unindexed triangles, vertex colors, node transforms, and base-color factors
- asynchronous mapped-file import with cancellation and stale-load replacement
- drag/drop, system Open dialog, command-line paths, loading and recoverable error states
- a hybrid Unreal/Blender camera: right-mouse Unreal flight (WASD + Q/E + scroll speed), Blender middle-mouse orbit/pan/dolly with a wrapped cursor and inertial glide, and left-mouse click-to-select mesh picking
- an interactive Blender-style navigation gizmo in the viewport corner with axis-snap views and drag-to-orbit
- Blender-standard viewport hotkeys: Numpad 1/3/7 (+Ctrl for reverse), Numpad 5 perspective/orthographic, Numpad ./F frame, G ground grid, Home reset
- a continuous, vsync-paced render loop that only runs while the camera is in motion or a load is in
  flight, so a still viewport costs no CPU or GPU
- eased wheel zoom, Fit/Reset camera glides, quaternion orientation without gimbal lock, distance-aware
  flight speed, and transient speed/mode HUD readouts
- DPI-aware dark native UI, keyboard-reachable controls, tooltips, and a neutral studio/grid render
- a Windows 11 Photos-style unified title bar (custom `WM_NCCALCSIZE`/`WM_NCHITTEST` non-client
  handling, with `DwmDefWindowProc` passthrough so Snap Layout hover-on-maximize still works): icon
  action buttons (Grid/Ground axis/Ground direction/Snap/Speed/Fit/Reset/Share/•••) on the left,
  the centered file name, and
  Open-With plus the system minimize/maximize/close on the right — the caption buttons stay usable
  even before a model finishes loading
- a Photos-style bottom bar (shown once a model is loaded) with a centered lighting toolbar: neutral
  Studio lighting by default, texture-free Clay/Solid inspection, a rotatable sharp Directional light,
  and a fill-free Wireframe mode; the bar also includes the Information toggle, a zoom slider
  synced to camera distance, and a Fullscreen toggle (also `F11`) that expands the window edge-to-edge
  over its monitor — above the taskbar and with the title bar/bottom bar hidden so the viewport fills
  the whole screen — distinct from Maximize, which snaps to the work area and keeps the taskbar and
  our own chrome visible; `Esc` (or `F11` again) restores the window; live-updating "Stats & Shading"
  Information side panel (mesh/texture/animation/performance/scene counts scanned from the glTF JSON)
- Open With (cached Windows handlers, with installed CAD/modeling/3D-printing apps
  promoted into purpose-based groups, plus "Choose another app...")
  and Windows Share (`DataTransferManager`/C++WinRT) for the currently open file

## Rendering and imports

D3D12 is the application's exclusive rendering path. A dedicated render thread owns device
creation, presentation, resize, and shutdown. All files are parsed inside
`Preview3DImportWorker.exe` under AppContainer and reach the viewer as validated wire-format
chunks. The old `--d3d12` argument is accepted as a deprecated no-op.

The sandboxed importer accepts `.glb`, `.gltf` (including external `.bin`/image siblings fetched
through the brokered sidecar protocol), `.obj` with optional local `.mtl` and texture sidecars,
binary/ASCII `.fbx`, ASCII/binary `.stl`, ASCII/binary `.ply`, `.3mf`, and
`.usd`/`.usda`/`.usdc`/`.usdz`. STEP/STP (`.step`/`.stp`) is served by the
separate zero-capability `Preview3DStepHost.exe`, which owns the pinned OCCT
closure and returns only normalized chunks. Its static glTF path supports bounded
Draco/meshopt geometry, mesh quantization, KTX2/Basis, PNG/JPEG/WebP, material texture slots and
texture transforms. The OBJ path uses ufbx for polygon triangulation, smoothing/generated normals,
UVs, vertex colors, object/group meshes, and MTL material factors, with broker-approved base-color,
normal/bump, and emissive maps. The Open dialog, command line, and drag/drop accept
all supported direct formats; `.mtl` remains a sidecar and is never a primary open type. The FBX path preserves
static hierarchy and instances, evaluates its deterministic first-stack start/rest pose, bakes
supported skin and blend deformation, and maps supported embedded or broker-approved texture data.

3MF is a bounded Tier B static-preview path for Core geometry, components, all
root-build items, Materials and Properties colors/textures, Production model
parts, and bounded Beam Lattice previews. Every standard build occurrence is
shown together in authored coordinates. Slicer-private multi-plate grouping and
settings are ignored, and Slice, Secure Content, Volumetric, Implicit, toolpath,
repair, slicing, and export features are outside the supported subset.

USD is a bounded Tier B static-preview path. Self-contained meshes, hierarchy,
instances/point instances, transforms, common UV/color/normal primvars, display
color, material subsets, and the supported USD Preview Surface factors/textures
use TinyUSDZ in the general worker. A stage with supported local composition is
discarded before publication and restarted atomically in the separate
zero-capability `Preview3DImportHost.exe`, which loads the pinned private OpenUSD
payload only for that generation. The compatibility subset includes bounded
sublayers, references, inherits/specializes, authored default variants, and
selectively loaded payloads. All dependencies are local, relative, and supplied
through the trusted broker; remote/custom resolvers and unrestricted plugins
are rejected. The UI reports the detected USDA, USDC, or USDZ encoding rather
than inferring it from `.usd`.

STEP is a bounded Tier B static-preview path for self-contained ISO 10303-21
AP203/AP214/AP242 files: B-rep or authored AP242 tessellated geometry,
assemblies and reused definitions, instance/shape/face colors, and authored
length units. The dedicated zero-capability `Preview3DStepHost.exe` receives an
inherited read-only handle (never a path), verifies the Part-21 byte envelope
before the CAD kernel runs, and exits after the generation. External STEP
documents are out of scope and fail typed; PMI/GD&T, editing, saved views,
exact measurement, and shape healing are not supported.

Portable packaging keeps `Preview3D.exe` at the package root and the sandbox
executable plus its private DLL closure under `worker\`. The viewer prefers that
layout and grants the AppContainer read/execute only on `worker\`; same-directory
worker lookup remains as a developer/test-build fallback.
The separate OpenUSD bootstrap, core DLL, monolithic OpenUSD runtime, oneTBB,
codec dependencies, and hash-audited schema/plugin resources live only under
`OpenUsdHost\`; its distinct AppContainer is granted access only to that tree.
The constrained OCCT closure and `Preview3DStepHost.exe` live only under
`StepHost\`, with a third distinct AppContainer granted access only to that tree.

## Current limitations and deferred work

The limited MVP now includes the D3D11On12 Direct2D chrome, information/navigation/error UI,
bounded picking metadata, point splats, semantic mip chains, static PBR/unlit materials and the
documented compressed glTF subset. `Renderer.cpp` remains only as legacy camera/reference code;
its D3D11 renderer is never instantiated.

Still deferred: TGA/DDS/HDR, animation playback, advanced material lobes, meshoptimizer-built
LOD/hierarchies, persistent derived cache, and the remaining Tier B formats. FBX geometry caches,
dynamic constraints, NURBS/subdivision tessellation, cameras, and lights are outside the supported
static subset. USD skeletal data, MaterialX, procedural schemas, remote assets,
arbitrary renderer/file-format plugins, interactive variants, and animation are
also outside the supported subset. Tier B USD ceilings include 2 GiB primary,
4 GiB aggregate local source/archive expansion, 20 million triangles or points,
50,000 nodes, and a compatibility-host commit cap of min(4 GiB, 35% physical
memory). Explorer thumbnails, including 3MF, USD, FBX, and STEP thumbnails, remain a
separate deliverable.

## Controls

| Action | Input |
| --- | --- |
| Select mesh / clear | Click the mesh / click the background |
| Orbit | Left drag, gizmo ball drag, or arrow keys |
| Pan | Middle drag or Shift+arrow keys |
| Fly (Unreal) | Hold right mouse + `W`/`A`/`S`/`D`, `Q`/`E` |
| Roll | Hold right mouse + `Z`/`C` |
| Fly faster | Hold right mouse + `Shift`; wheel, Speed flyout, or `+`/`-` sets speed |
| Zoom | Wheel, `+`, `-`, Ctrl+middle drag, or the bottom-bar zoom slider |
| Front / Right / Top view | Numpad `1` / `3` / `7` (Ctrl for reverse) |
| Perspective / Orthographic | Numpad `5` |
| Gizmo view snap | Click an axis ball in the corner gizmo |
| Frame model / selection | `F`, Numpad `.`, the title bar's Fit button, or double-click |
| Ground grid | `G` / `Shift+Alt+G` / the title bar's Grid button |
| Axis-snap truck | The title bar's Snap button |
| Model information | The bottom bar's Info button (opens the Stats & Shading side panel) |
| Studio / Clay / Directional lighting | The centered bottom lighting toolbar |
| Rotate directional light | Drag the sun around the navigation gizmo's outer ring in Directional mode |
| Wireframe | The mesh icon beside the lighting modes; hides all triangle surfaces |
| Share the open file | The title bar's Share button (Windows Share) |
| Open the file in another app | The title bar's Open With dropdown |
| Reset view | `Home` or `R`, or the title bar's Reset button |
| Open | `Ctrl+O` (primarily launched via file-type registration or drag/drop instead) |
| Fullscreen | `F11` or the bottom bar's Fullscreen button |
| Cancel open / exit fullscreen | `Esc` |
| Minimize / maximize / restore / close | The title bar's own buttons (top-right) |

Camera motion is time-corrected and eased: flight ramps up and settles instead of stepping, wheel
zoom and Fit/Reset glide to their destination, drags carry exponential inertia, and view snaps
slerp the camera orientation along the shortest arc. Mouse drags wrap the cursor at the viewport
edge so long gestures are never trapped by a display border. By default, the pointer is hidden
during left-, middle-, and right-mouse camera drags; this can be disabled in Settings. Right-mouse
flight re-centers the cursor Unreal-style. Orientation is stored as a quaternion (yaw about the world
up axis, pitch about the camera right axis), so there is no Euler order and no gimbal lock; pitch
clamps just short of the up-axis pole so the horizon never flips. Rendering runs continuously only
while anything is in motion; a still viewport idles at zero CPU/GPU cost.

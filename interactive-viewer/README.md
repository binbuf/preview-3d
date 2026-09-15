# Interactive viewer vertical slice

This project is a focused native Windows 11 GLB viewer. It intentionally leaves the thumbnail provider,
file registration, caching, streaming/LOD infrastructure, and broader format support for later slices.

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
  action buttons (Grid/Snap/Speed/Fit/Reset/Share/•••) on the left, the centered file name, and
  Open-With plus the system minimize/maximize/close on the right — the caption buttons stay usable
  even before a model finishes loading
- a Photos-style bottom bar (shown once a model is loaded) with the Information toggle, a zoom slider
  synced to camera distance, and a Fullscreen toggle (also `F11`) that expands the window edge-to-edge
  over its monitor — above the taskbar and with the title bar/bottom bar hidden so the viewport fills
  the whole screen — distinct from Maximize, which snaps to the work area and keeps the taskbar and
  our own chrome visible; `Esc` (or `F11` again) restores the window; live-updating "Stats & Shading"
  Information side panel (mesh/texture/animation/performance/scene counts scanned from the glTF JSON)
- Open With (real system-recommended handlers via `SHAssocEnumHandlers`, plus "Choose another app...")
  and Windows Share (`DataTransferManager`/C++WinRT) for the currently open file

## Rendering and imports

D3D12 is the application's exclusive rendering path. A dedicated render thread owns device
creation, presentation, resize, and shutdown. All files are parsed inside
`Preview3DImportWorker.exe` under AppContainer and reach the viewer as validated wire-format
chunks. The old `--d3d12` argument is accepted as a deprecated no-op.

The sandboxed importer accepts `.glb`, `.gltf` (including external `.bin`/image siblings fetched
through the brokered sidecar protocol), `.stl`, and `.ply`. It renders base-color textures from
KTX2/Basis or WIC-decoded PNG/JPEG/BMP/TIFF. The Open dialog still filters for GLB; pass other
formats on the command line or drop them onto the window.

## Current limitations and deferred work

The real Direct2D chrome has not yet been ported onto the D3D11On12 bridge (TSK-102).
The controls above describe the existing UI being restored in Phase 1; title/bottom bars,
Information panel, navigation gizmo, and error cards are currently not drawn. `--overlay-spike`
retains the bridge's developer stand-in primitives. `Renderer.cpp` remains as the camera
implementation and reference for the chrome port, but its D3D11 renderer is never instantiated.
CPU model data for picking, statistics, and native-orientation transforms is also not yet
published by the D3D12 import path.

Also deferred: point-cloud rendering (`.ply` point clouds import but are not drawn), mip levels
beyond 0, metallic-roughness/normal/emissive texture slots, PBR shading,
`EXT_meshopt_compression`/`KHR_mesh_quantization`, WebP, TGA/DDS/HDR, LOD/proxy generation,
and derived cache. Shell registration and installer work are outside this MVP pass.

## Controls

| Action | Input |
| --- | --- |
| Select mesh / clear | Click the mesh / click the background |
| Orbit | Middle drag, gizmo ball drag, or arrow keys |
| Pan | Shift+middle drag or Shift+arrow keys |
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
| Share the open file | The title bar's Share button (Windows Share) |
| Open the file in another app | The title bar's Open With dropdown |
| Reset view | `Home` or `R`, or the title bar's Reset button |
| Open | `Ctrl+O` (primarily launched via file-type registration or drag/drop instead) |
| Fullscreen | `F11` or the bottom bar's Fullscreen button |
| Cancel open / exit fullscreen | `Esc` |
| Minimize / maximize / restore / close | The title bar's own buttons (top-right) |

Camera motion is time-corrected and eased: flight ramps up and settles instead of stepping, wheel
zoom and Fit/Reset glide to their destination, drags carry exponential inertia, and view snaps
slerp the camera orientation along the shortest arc. Middle-drag orbit and pan wrap the cursor at
the viewport edge so long gestures are never trapped by a display border; right-mouse flight hides
and re-centers the cursor Unreal-style. Orientation is stored as a quaternion (yaw about the world
up axis, pitch about the camera right axis), so there is no Euler order and no gimbal lock; pitch
clamps just short of the up-axis pole so the horizon never flips. Rendering runs continuously only
while anything is in motion; a still viewport idles at zero CPU/GPU cost.

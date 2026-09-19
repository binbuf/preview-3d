# User experience

## Scope of this document

This document describes the UI/UX of the **current implemented vertical slice** (`interactive-viewer`, scope-limited Tier A viewer), not a forward-looking aspiration. The team spent a dedicated pass on window chrome, camera feel, and information display before broad format/cache/accessibility work started, and the product's visual and interaction identity now comes from that pass. Other design documents describe the target multi-format ingestion, caching, and accessibility architecture ([01](./01-product-scope.md), [03](./03-file-formats-and-ingestion.md), [04](./04-rendering-and-streaming.md)); where this document's current behavior is narrower than those targets (format support, persistence, accessibility), it says so explicitly rather than describing unbuilt behavior. Widening format/cache/accessibility support in later gates must preserve the window, chrome, and camera behavior fixed here unless a change is deliberately proposed and recorded.

## Experience principles

The viewer should feel like a small, fast, native tool: a custom dark window with no MDI/browser chrome, one document at a time, an immediately responsive camera, and information available on demand rather than always on screen. Loading remains indeterminate but displays bounded provisional, texture-refining, and upload-pressure status. Geometry is published progressively; camera interaction with usable content continues throughout loading and failure. Large-source scanning and representative coarse proxies remain TSK-205/206.

## Window

The window opens at 1000 × 720 logical pixels, centered on the primary monitor's work area, with a 480 × 360 logical-pixel minimum enforced through `WM_GETMINMAXINFO`. Size/position is not persisted between launches. DPI awareness is per-monitor-v2; `WM_DPICHANGED` resizes to the suggested rect and rebuilds the chrome font and layout.

The window background — both the window-class brush shown before the first swap-chain frame and the D3D12 clear color — is **#1C1C1E**, not a lighter neutral. There is no visible flash between the native background and the first rendered frame.

Chrome is a real `WS_OVERLAPPEDWINDOW` with the non-client caption area removed in `WM_NCCALCSIZE`/`WM_NCHITTEST` and passed through `DwmDefWindowProc`, so resize borders, Snap Layouts, Alt+Space, and the system move/size behaviors keep working even though the entire visible title bar is custom-drawn. `DWMWA_USE_IMMERSIVE_DARK_MODE` is set, corners use `DWMWCP_ROUNDSMALL` while windowed and `DWMWCP_DONOTROUND` while fullscreen, and caption/border colors are set to #242426/#3A3A3C through the corresponding DWM attributes.

**Fullscreen** (F11, or the bottom-bar fullscreen button) is a distinct mode from Maximize: the window expands to the exact monitor bounds, becomes topmost, hides the taskbar, and both toolbars collapse their viewport insets to zero while still drawing as floating overlays. Maximize snaps to the work area normally (taskbar visible, not topmost). Esc or F11 exits fullscreen; Esc otherwise cancels an in-progress open.

## Chrome

All chrome except three error-state buttons is drawn with Direct2D/DirectWrite over the D3D12 scene, not native HWND controls.

### Title bar (52 logical px)

Left to right, present while a model is loaded: **Grid**, **Ground axis** (shows the effective X/Y/Z model-up axis and cycles Z → Y → X), **Ground direction** (flips whether the positive or negative side of that axis is up), **Snap** (axis snap for the truck/pan tool), **Speed** (opens the speed flyout), **Fit**, **Reset**, **Share**, and an overflow **"…"** button — each a 40-logical-pixel icon button (overflow 34px). Choosing a ground axis or direction maps that signed model axis to the viewer's fixed Z-up world, disables native-orientation display so the choice takes effect, immediately captures the resulting framing as Home/Reset, and persists the explicit choice. Automatic behavior before the first explicit choice remains source +Y-up for glTF and +Z-up for formats without declared axes. A draggable empty strip follows, then the centered filename, then another drag strip. On the right, **Open With** (72px, hidden with no model loaded or while fullscreen) precedes the system **Minimize / Maximize-Restore / Close** buttons (46px each), which remain visible even in fullscreen.

The overflow menu is a native popup `HMENU` (the one non-D2D menu surface) containing, conditionally, "Model warnings…" and a separator, then "Controls", a separator, and "About 3D Preview". It does **not** contain cache commands — there is no derived-data cache in this slice — or a recent-files list.

### Bottom bar (44 logical px)

Shown only with a model loaded and not fullscreen: an **Info** toggle at the far left, a **zoom slider** with a live percentage readout on the right, and a **Fullscreen** toggle at the far right.

### Speed flyout

Clicking Speed opens a floating 220 × 64 logical-pixel panel anchored beneath it, with a draggable slider mapping a log scale from 0.05× to 40× fly speed and a live numeric readout. The mouse wheel while fly-looking (RMB held) adjusts the same value.

### Navigation gizmo

A Blender-style six-axis ball gizmo sits in the viewport's top-right corner (44 logical-pixel outer radius) with colored axis stems. Dragging the ball orbits the camera using the same math as an ordinary orbit drag; clicking an axis node snaps to that canonical view.

### Information panel ("Stats & Shading")

Docked to the right edge, a fixed 300 logical px wide (clamped to 90% of viewport width when narrow), scrollable, and modeled on the layout of the discontinued Windows 11 3D Viewer app. Sections: Dimensions (verified/provisional width/height/depth, in meters for glTF and source units for unspecified STL/PLY units), Mesh Data (triangle/vertex counts, UV0/UV1 presence, vertex colors, material count), Texture Data (actual imported per-channel texture presence, with optional decode failures represented by fallbacks), Animation Data (bone/skin/take counts), Performance Data (draw calls), and Scene Data (node count). Toggled by the bottom-bar Info button or the `ID_VIEW_INFO` command.

### Transient HUDs and tooltips

A speed HUD ("Travel speed ×1.20") and a mode HUD ("Ground grid shown", "Ground axis Y", "Ground direction -Y up", "Orthographic", "Axis snap on") appear on the corresponding action and fade out: visible 1.3 s, then a 0.30 s fade. Chrome buttons use a custom hover-delay tooltip (1500 ms delay) rather than the stock Win32 tooltip control.

### Color palette

Panel/bar fill #2C2C2E / #242426; borders #3A3A3C / #48484C; primary text #F5F5F7; secondary text #A1A1A6; accent/active #0A84FF; disabled fill/text #2C2C2E / #707075; Close-button hover/press #E81123 / #C42B1C; warning #FF9F0A; error #FF453A. Chrome text uses "Segoe UI Variable Text" semibold.

## Application states

`ViewerState` has **Empty**, **Loading**, **Ready**, **Failed**, and **Partial**. Status facts use closed flags and saturated warning counts rather than worker-provided text. Ready requires broker terminal catalog acceptance, resolved dependencies, usable geometry, verified bounds, and completed uploads. For today's small fixtures the accepted complete scene serves as the complete catalog; bounded coarse/fine relationships arrive in TSK-206.

| State | Canvas | Available actions |
| --- | --- | --- |
| Empty | Dashed drop target, "Drop a 3D model here" and supported formats | Open, drop |
| Loading | Spinner and actual format/status; usable prior or partial content stays interactive | Camera, cancel (Esc), replace open, warnings |
| Ready | Complete accepted representation, bottom bar and Info available | Full interaction |
| Failed | Error card with actual format/phase over usable content or empty background | Retry, Open another, Copy details, camera on usable content |
| Partial | Cancelled incomplete geometry with a static status label; never labeled Ready | Camera, replace open, warnings |

Each open runs a broker session on a detached background thread with a zero-capability AppContainer worker. A bounded upload coordinator prepares GPU resources away from the UI and presenting render thread; fence-complete batches publish at frame boundaries. Each open increments a generation counter; a new open or window close invalidates in-flight results for the prior generation, and cancellation is a `std::atomic_bool` checked by the load thread.

## Open behavior

Open is available through:

- the initial command-line path, if one was passed to the process;
- Ctrl+O or the (currently overflow-only, chrome-driven) Open action using `IFileOpenDialog`, filtered to supported models (`*.glb;*.gltf;*.obj;*.fbx;*.stl;*.ply;*.3mf;*.usd;*.usda;*.usdc;*.usdz;*.step;*.stp`), glTF, OBJ, FBX, STL, PLY meshes/points, 3MF, USD, STEP, and All files, with `FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST`;
- drag/drop, implemented as `WM_DROPFILES` via `DragAcceptFiles` (a plain OLE drag-accept, not a custom `IDropTarget`).

`.glb`, `.gltf` with broker-approved local sidecars, `.obj` with optional broker-approved local `.mtl` and texture sidecars, binary/ASCII `.fbx`, ASCII/binary STL, ASCII/binary little/big-endian PLY meshes and points, `.3mf`, `.usd`/`.usda`/`.usdc`/`.usdz`, and self-contained `.step`/`.stp` are accepted case-insensitively. MTL is sidecar-only and is never a primary open type. OBJ, FBX, 3MF, ASCII STL/PLY, and USD use bounded Tier B paths and have lower source, geometry, and scratch limits than the binary Tier A paths; unsupported extensions fail before parsing. Standard 3MF root-build items display together in authored coordinates; no slicer-private plate selector or offset is inferred. `.usd` is identified by bytes, and successful metadata reports USDA, USDC, or USDZ rather than echoing the suffix. A `.step`/`.stp` extension selects the dedicated STEP host, but the host's Part-21 admission verifies the ISO 10303-21 bytes before any CAD-kernel work, so a renamed file cannot bypass it. Malformed data receives a separate typed error. Remote/UNC, mapped network drives, device paths, and escaping sidecar references are rejected; local regular files and Unicode paths are supported. Dropping multiple files reports "Open one model at a time." Single-instance forwarding accepts the same extension set, replaces an in-flight load, and accepts a later valid activation after failure without relaunch.


## Camera and input

Camera orientation is a double-precision quaternion with no Euler-angle singularity; render transforms are camera-relative. Pointer mode is an exclusive state machine chosen on button-down:

| Action | Mapping |
| --- | --- |
| Orbit | LMB drag (turntable, pitch-clamped near the poles); cursor wraps at viewport edges during the drag and orbit continues with exponential inertia after release |
| Click-select / clear selection | LMB press+release under 6 px movement and 0.5 s selects the document through asynchronous depth-tested GPU coverage, or clears selection if nothing is hit |
| Pan (truck) | MMB drag along the flattened ground plane, with optional axis snap (Snap button); same wrap+inertia treatment as orbit |
| Dolly (drag) | Ctrl+MMB drag, exponential log-distance mapping, no cursor wrap |
| Fly-look | RMB held: cursor hidden, raw `WM_INPUT` deltas drive look (independent of OS pointer acceleration); W/A/S/D translate, Q/E move down/up, Z/C roll, Shift doubles speed, wheel or the Speed flyout adjusts a persistent 0.05×–40× fly-speed multiplier |
| Orbit via gizmo | Dragging the navigation gizmo ball uses the same orbit math as an LMB drag |
| Orbit/pan by keyboard | Arrow keys orbit normally, or pan while the flight-speed modifier is held |
| Dolly (wheel/pinch) | Mouse wheel, `+`/`-` (or numpad Add/Subtract) for discrete steps, or pinch; wheel/pinch dolly is exponential and clamped to `sceneRadius * [0.025, 250]` |
| Touch | One-finger drag orbits; two-finger drag pans (centroid) and pinches to dolly |
| Fit | F, numpad Decimal, double-click, or the Fit button — frames the current selection if one exists, otherwise the whole model |
| Reset | Home, R, or the Reset button — eases back to the framing captured at load |
| View snaps | Numpad 1/3/7 for Front/Right/Top (Ctrl+ same key for Back/Left/Bottom); Numpad 5 cross-fades Perspective/Orthographic over 0.16 s |
| Toggle ground grid | G or Shift+Alt+G, or the Grid button; guarded against key-repeat for 0.30 s |
| Cycle model ground axis | X/Y/Z title-bar button; cycles Z → Y → X and immediately establishes a new Home/Reset framing |
| Flip model ground direction | Ground-direction title-bar button; toggles the selected axis between positive-up and negative-up and immediately establishes a new Home/Reset framing |
| Cancel open / exit fullscreen | Esc |
| Toggle fullscreen | F11 |
| Open overflow menu | Alt+M |
| Open | Ctrl+O |

## Errors

The error card is drawn with Direct2D; only its three action buttons — **Retry**, **Open another**, **Copy details** — are real, tab-stoppable HWND buttons. Failures carry a closed code, host stage, and validated worker phase. This includes malformed data, unsupported required features/encoding, empty geometry, memory/resource limits, unavailable/unsafe sidecars, changed sources, worker exit/timeout, protocol rejection, and upload failure. Cancellation is a status transition, not a document error. Specific descriptions remain in `D3D12ImportBridge`; the card exposes Retry, Open another, and Copy details.

Copy details writes the host-owned summary/details, actual format, failing phase, and numeric code. Source paths and basenames are omitted unconditionally; no worker-provided diagnostic text is accepted. Optional-feature/texture warnings use fixed host-owned text in the existing warning badge/menu, capped at 64 facts per category and one validated status payload per generation. New generations clear warnings; stale or late-after-failure publications cannot replace the card.

Controls uses the existing `MessageBoxW` control scheme. About describes GLB/glTF with local sidecars plus ASCII/binary STL and PLY meshes/points. The unused `IDD_ABOUTBOX` resource remains separate from the active About command.


## Accessibility

The D2D-drawn title/bottom-bar controls and six gizmo axis actions are exposed as UI Automation Button, CheckBox, or Slider fragments with stable names, help text, enabled/checked/focused state, bounds, Invoke/Toggle patterns where applicable, and keyboard focus. Tab/Shift+Tab traverse the visible virtual controls; Enter/Space activates them and arrow keys adjust the focused zoom or speed slider. The three error-card actions remain real tab-stop HWND buttons and also have stable Button fragments, so their native dialog behavior is preserved. Document Loading, Ready, warning, and error changes raise bounded UIA notifications.

Per-monitor-v2 DPI relayout remains active. Windows high-contrast changes remap the overlay through the current system window/text/highlight colors. When client-area animations are disabled, the loading indicator remains static, HUDs do not fade, camera fit/reset/snap, wheel zoom and projection changes settle without transitional animation, and post-drag inertia is suppressed. Default visuals and camera easing are unchanged when these preferences are off. The 3D canvas itself is intentionally a single interactive surface rather than an accessibility tree of mesh geometry; this viewer does not add scene editing or object-browser semantics.

## Settings and persistence

Native-orientation display and an explicit model ground-axis and signed-direction choice persist in `%LOCALAPPDATA%\Binbuf\3D Preview\settings.json`; a missing, corrupt, or older settings file restores automatic axis selection and normalized orientation. Grid visibility, axis-snap state, and window placement (the latter tracked only in memory to support fullscreen restore) reset on the next run. No settings are written to the registry. AppContainer provisioning is separate, and no derived-data cache — the cache-enabled preference, `%LOCALAPPDATA%` cache directory, and Clear cached previews command described in [04-rendering-and-streaming.md](./04-rendering-and-streaming.md) are forward targets, not current behavior.

## Shell integration

Two on-demand, title-bar-triggered integrations exist, both invoked by the user rather than passive Explorer registration:

- **Open With**: loads a bounded local catalog of Windows handler identities and
  displays recognized CAD, modeling, and 3D-printing applications first, grouped
  by purpose, followed by other Windows-recommended handlers. The curated set is
  FreeCAD, OpenSCAD, Autodesk Fusion, Rhino, SOLIDWORKS, Autodesk Inventor,
  Blender, SketchUp, Autodesk 3ds Max, Autodesk Maya, ZBrush, MeshLab,
  Plasticity, PrusaSlicer, OrcaSlicer, Bambu Studio, UltiMaker Cura, Lychee
  Slicer, CHITUBOX, and Simplify3D. An app is shown only when Windows
  registers it as a handler for the current extension; the viewer does not guess
  vendor command lines. Discovery uses `SHAssocEnumHandlers` off the UI thread,
  adds newly found handlers no more than once every seven days, and caches only
  extension/handler/display/catalog metadata in
  `%LOCALAPPDATA%\Binbuf\3D Preview\open-with-apps-v1.dat`—never model paths or
  launch history. Selection queues registered-handler resolution and invocation
  on the discovery thread; failure removes the stale entry, notifies the UI, and
  requests an immediate background rescan. A session keeps a
  successfully resolved handler in memory. `SHOpenWithDialog` remains available
  as "Choose another app…".
- **Share**: shares the current file as a `StorageFile` through `IDataTransferManagerInterop`/`DataTransferManager`.

The NSIS installer registers only interactive Open With/Default Apps entries for
the supported extensions. No thumbnail provider or thumbnail COM registration is
installed yet; [05-thumbnail-provider.md](./05-thumbnail-provider.md) remains a
separate delivery path.

## UX acceptance scenarios (current slice)

1. Cold launch shows the #1C1C1E background immediately with no white/flash frame, then the empty-state drop target.
2. Opening a valid .glb or .obj replaces the empty state with the loading spinner, then the model; opening a second supported file keeps prior content interactive until usable replacement geometry arrives; partial geometry remains Loading until terminal acceptance.
3. Dropping an unsupported format, a UNC path, or more than one file each produce the corresponding actionable error and leave any currently open model untouched.
4. Every camera action in the table above is reachable by mouse, keyboard, or touch as specified, including fly-look, orbit inertia, and the perspective/orthographic cross-fade.
5. Fullscreen (F11) and Maximize are visibly distinct and independently reversible; Esc exits fullscreen without closing the document.
6. Copy details on any error reports the actual format, phase, and code and omits source paths/basenames.
7. Closing the window during an in-progress load leaves no visible window and no lingering process.
8. Info shows real counts, dimensions, units, and texture presence; optional texture failures preserve geometry and expose a bounded warning. Cancelled incomplete geometry is never labeled Ready.

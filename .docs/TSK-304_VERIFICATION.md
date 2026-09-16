# TSK-304 verification — single-instance activation and accessibility

Date: 2026-09-16

## Delivered behavior

- Normal launches elect one primary per interactive user and session with the documented `Local\Binbuf.Preview3D.<session>.<SHA-256 SID prefix>` mutex. A same-user secondary waits at most one second for the protected Ready event, sends either one normalized local Tier A path or Activate, receives a bounded acknowledgement, and exits. `--new-instance` and the existing test/benchmark modes are explicit developer bypasses.
- The primary owns a local-only, message-mode, overlapped named pipe with an explicit current-user/System DACL and remote-client rejection. The listener reads at most 256 KiB, authenticates the completed write by named-pipe impersonation plus SID/session checks, validates the fixed header/version/correlation and strict UTF-8 JSON, coalesces requests in a 16-command queue, and posts only a queue notification to the HWND. It never parses or opens a model on the pipe thread and never waits on the UI thread.
- Relative command-line paths are made absolute before forwarding. Unknown options, missing `--open` values, multiple paths, UNC paths, unsupported extensions, invalid Unicode, unknown/duplicate JSON shape, malformed versions, partial-sized frames, and declared oversized frames fail closed. A new accepted Open uses the existing generation cancellation/replacement path; Activate restores a minimized window and flashes the taskbar if Windows refuses foreground transfer.
- The custom title/bottom chrome and gizmo axis actions now expose native UI Automation fragments with stable names, roles, help text, bounds, enabled/checked/focus state, runtime IDs, and Invoke/Toggle patterns. Tab/Shift+Tab, Enter/Space, and slider arrow handling make visible custom controls keyboard reachable. UIA calls marshal back to the owner UI thread; providers are disconnected and guarded before app-state destruction.
- The three error actions remain real tab-stop HWND buttons. The top-level provider supplies stable Button counterparts while native click, dialog-tab, tooltip, and focus behavior remains intact. Loading, Ready, warning, cancellation, and failure transitions raise both WinEvent and UIA notification events without source paths.
- `WM_SETTINGCHANGE`/`WM_THEMECHANGED` refresh high-contrast and client-animation preferences. High contrast maps overlay background/text/accent drawing to current system colors. Reduced motion makes the loading indicator static, removes HUD fades, suppresses post-drag inertia, and makes fit/reset/snap, zoom, and projection transitions settle immediately; default camera behavior is unchanged.
- DPI changes now recompute both title and bottom bar heights. The same relayout path is exercised at 96/144/192 DPI in a narrow 480×360 window. The local two-monitor system is physically 150% on both displays, so it cannot produce an actual differing-DPI monitor crossing; final hardware-matrix qualification should repeat that physical transition.

No third-party dependency, source-path authority, import protocol, model wire format, persistent data, registration, or packaging surface changed. New link dependencies are Windows inbox libraries only (`advapi32`, `bcrypt`, `oleacc`, and `uiautomationcore`).

## Automated evidence

Builds, from the repository root:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' Preview3D.slnx /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' Preview3D.slnx /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

Both completed with zero warnings/errors after the final source changes.

Catch2:

```powershell
.\x64\Debug\Tests.Unit.exe --reporter compact
.\x64\Release\Tests.Unit.exe --reporter compact
.\x64\Debug\Tests.ImportIsolation.exe --reporter compact
.\x64\Release\Tests.ImportIsolation.exe --reporter compact
```

| Suite | Result |
| --- | --- |
| Unit Debug | 91 cases, 7,354 assertions passed |
| Unit Release | 91 cases, 7,266 assertions passed; expected Release debug-layer skip warnings |
| ImportIsolation Debug | 200 cases, 51,943 assertions passed |
| ImportIsolation Release | 200 cases, 51,943 assertions passed |

The four new activation unit cases cover Unicode/JSON metacharacter round-trip, strict JSON/UTF-8/size validation, local Tier A path policy, authenticated primary/secondary forwarding, malformed and oversized real-pipe frames, and close/relaunch election.

Real-app automation, in both Debug and Release:

```powershell
python tests\app-smoke\activation.py --configuration Debug
python tests\app-smoke\activation.py --configuration Release
& tests\app-smoke\accessibility.ps1 -Configuration Debug
& tests\app-smoke\accessibility.ps1 -Configuration Release
python tests\app-smoke\run.py --configuration Debug --output artifacts\tsk-304\lifecycle-debug.json --runs 1 --timeout 30 --require-points
python tests\app-smoke\run.py --configuration Release --output artifacts\tsk-304\lifecycle-release.json --runs 1 --timeout 30 --require-points
```

Activation passed second-process replacement while work was active, a valid reopen after document failure, pathless activation, and close/immediate relaunch. Accessibility passed names/roles/states/focusability, UIA SetFocus/Toggle/Invoke, controls during delayed replacement, Alt+Space system-menu availability, `HTMAXBUTTON` Snap Layout routing, fullscreen/maximize distinction, native Open-dialog focus restoration, narrow 100/150/200% layouts, preference paths, and accessible error actions. Lifecycle passed with zero failures and no surviving processes in each configuration. Frozen smoke results are under `tests/fixtures/baselines/tsk-304/`.

## Retained qualification notes

- Foreground activation remains subject to Windows policy; denial deliberately flashes the taskbar rather than attempting an input-policy bypass.
- The canvas remains one interactive surface; mesh geometry is not exposed as an editable accessibility tree. This does not add selection/object-browser scope.
- Physical mixed-DPI movement between unlike monitors, Narrator/JAWS/NVDA manual speech review, touch at each DPI, and a long-running close/activation storm belong in the TSK-305 compatibility matrix. Deterministic UIA, DPI-layout, malformed-client, and close/relaunch coverage is in place and passing here.

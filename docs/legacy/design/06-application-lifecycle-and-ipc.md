# Application lifecycle and IPC

## Process model

interactive-viewer is a standalone native GUI executable. It is not a daemon, service, tray application, browser host, or child of the thumbnail provider. It links no third-party format parser or decoder itself; it brokers a zero-capability AppContainer `Preview3DImportWorker.exe` for ordinary format parsing and, for USD generations outside the TinyUSDZ subset, a second zero-capability AppContainer `Preview3DImportHost.exe` for OpenUSD composition. Neither child has UI and neither is persistent background infrastructure — the worker is reused across generations within a session but is not a daemon, and the host starts only per USD generation.

There is at most one normal viewer process per interactive Windows user session while a viewer window is open. Launching another model forwards an Open request to that visible instance and activates it. Closing the last viewer window requests cancellation, terminates any import-worker and compatibility-host jobs, and terminates the process; there is no warm background mode in the MVP.

This is a usability policy, not an architectural dependency. A developer-only switch may bypass forwarding for debugging, but it is not registered with Explorer.

## Command line

Release syntax:

    Preview3D.exe [model-path]
    Preview3D.exe --open <model-path>

Rules:

- zero paths opens an empty window with the Open command;
- exactly one path opens that document;
- options are parsed with CommandLineToArgvW and ordinal, case-insensitive option names;
- an unknown option, missing value, or multiple primary paths shows usage and returns exit code 2;
- relative paths are resolved against the launcher's current directory before forwarding;
- paths remain UTF-16 internally and are never round-tripped through the active code page.

Developer-only switches such as --warp or --diagnostics are compiled out or rejected by production registration.

## Startup sequence

The executable separates first-visible work from heavy initialization:

1. Set process DPI awareness and install fail-fast/diagnostic handlers.
2. Parse the command line and determine current user SID hash and session ID.
3. Attempt to become the per-session primary via the secured singleton protocol.
4. If secondary, authenticate to the pipe, forward the normalized path, await an acknowledgement, and exit.
5. If primary, initialize COM STA on the UI thread, register the window class, create/show the HWND, and begin the message pump.
6. Start the active-instance IPC listener, render thread, upload coordinator, cache coordinator, and bounded loader/broker pool without waiting for device/file readiness on the UI thread. Do not load OpenUSD, start the compatibility host, or start the import worker before the first Open request.
7. Send any startup path through the same UI command path as IPC and drag/drop opens.

The solid client background is painted by the window until the first swap-chain frame is ready, so device creation never presents an unresponsive black window. Startup failures become a native error surface or message box while the message pump remains operational.

## Singleton names and access

Names are scoped by both interactive session and user identity:

- mutex: Local\Binbuf.Preview3D.<session-id>.<SID-hash>
- pipe: \\.\pipe\Binbuf.Preview3D.<session-id>.<SID-hash>

The SID hash is a stable, non-secret SHA-256-derived short name component; authorization comes from ACLs, not from obscurity. The app creates an explicit security descriptor granting the current user SID and LocalSystem the minimum required access and denying anonymous/network access. It does not use a null DACL or rely on inherited defaults.

The pipe server records the client's process ID with GetNamedPipeClientProcessId, then authenticates the connection by temporarily calling ImpersonateNamedPipeClient, reading the thread token's user SID and session, and immediately calling RevertToSelf. It accepts commands only from the expected SID/session. A failed verification closes the instance without parsing its payload.

The mutex is acquired first. The primary creates the pipe before publishing a small Ready event with the same security scope. A secondary that sees the mutex waits up to one second for Ready and retries pipe connection with a bounded backoff. If the owner is alive but unresponsive, it shows “Preview 3D is not responding” and exits; it does not silently create two normal primaries. If the mutex becomes abandoned, the new process recreates all IPC objects and becomes primary.

## Pipe protocol

The pipe is local-only, message-mode, duplex, overlapped, and rejects remote clients. Each connection accepts one request and one response, then disconnects.

Frame:

| Field | Size | Meaning |
| --- | ---: | --- |
| Magic | 4 | ASCII P3DI |
| Version | 2 | Protocol version 1 |
| Type | 2 | Open=1, Activate=2 |
| PayloadBytes | 4 | Little-endian UTF-8 bytes, maximum 256 KiB |
| CorrelationId | 16 | Random GUID bytes |
| Payload | variable | Strict JSON object |

Open payload version 1:

    {"path":"D:\\models\\part.glb","activate":true}

Only path and activate are accepted; duplicate/unknown keys fail the request. JSON must be valid UTF-8, contain no embedded NUL, and decode to one absolute local path within the Win32 length supported by the app. The receiver revalidates the path by handle when loading. The path is data and is never passed to a shell or command interpreter.

Response uses the same header with Type high bit set and a bounded JSON payload containing accepted, status, and correlationId. Accepted means queued on the primary UI thread, not successfully parsed.

## IPC threading

A dedicated low-duty IPC thread owns pipe creation and overlapped connection/read/write. It has no HWND, renderer, or scene access. After validation it posts a heap-owned OpenCommand handle through a bounded thread-safe queue and signals the UI thread with a registered WM_APP message. The UI thread drains and frees handles; PostMessage never carries a pointer whose lifetime is owned solely by the sender.

At most 16 open commands may wait. Newer Open commands supersede older queued Open commands, while Activate may coalesce. Payload reads have size and time limits. Shutdown cancels overlapped I/O with CancelIoEx and closes the pipe only after completion records retire.

The active-instance protocol is distinct from the private import-broker protocol in [02-system-architecture.md](./02-system-architecture.md) — the one shared by `Preview3DImportWorker.exe` and `Preview3DImportHost.exe`. They do not share pipe names, framing, ACLs, parsers, or payload types. Each import pipe exists only while its child Job Object and generation are live and never accepts arbitrary local clients.

## Document state machine

    Empty
      |
      v
    Opening -> ProxyReady -> Refining -> Ready
       |           |            |         |
       +-----------+------------+---------+--> Failed
       |
       +--> Cancelled

An Open command creates a new generation and requests stop on the prior opening/refining generation. If an old document is visible, it stays visible with an “Opening…” overlay until the new document reaches complete ProxyReady. A malformed/cancelled new open leaves the old document intact and shows the error. Once proxy handoff succeeds, the old scene retires behind GPU fences.

Repeated selection of the current canonical file activates the window and reloads only if its handle-derived file identity/version evidence changed. MVP behavior is an automatic reload with camera preserved when bounds are compatible.

## Window activation

The primary handles an accepted request by restoring a minimized window, bringing it to the current virtual desktop when permitted, flashing the taskbar if foreground activation is denied by Windows, and starting the Open command. IPC does not call SetForegroundWindow from the server thread.

Drag/drop and the File > Open dialog normalize into the same OpenCommand and format/error pipeline. WM_COPYDATA is not used because its synchronous send could block the UI thread and has weaker framing/control.

## Cancellation

The following request generation cancellation:

- opening a different file;
- closing the document/window;
- parser/resource budget failure;
- device recovery that invalidates an upload generation;
- import-worker or compatibility-host failure/timeout;
- process shutdown.

Cancellation is normally a stop request, not a forced thread termination. Tasks check at the bounded points defined in [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md), whether they run in the trusted loader/broker pool or inside an import process. Stale CPU events and fence completions are dropped by generation. Mapped views, transient stores, shared sections, and cache-entry leases remain alive until their final lease retires. Each AppContainer import process receives a cooperative cancel first; only its Job Object may be terminated after the finite grace period.

Targets:

- queued but unstarted work disappears within 50 ms;
- active normalizer/LOD/texture tasks — now running inside the import worker rather than the trusted process — observe stop within 100 ms p95 and 500 ms maximum outside a non-interruptible third-party call;
- each import process acknowledges cancellation within 500 ms or is terminated with all of its returned sections invalidated;
- every third-party adapter uses its progress/cancellation hook where one exists;
- cancellation never waits on the UI or render thread.

## Close and shutdown

WM_CLOSE begins an asynchronous close:

1. AppState enters Closing, rejects/negatively acknowledges new IPC Open requests, and hides the window promptly.
2. UI requests stop on the document, active-instance pipe, import-worker and compatibility-host jobs, cache coordinator, workers, upload coordinator, and render thread.
3. The hidden UI thread continues pumping messages; it does not join threads or wait for GPU fences.
4. Owners drain/cancel their bounded queues. The GPU lanes perform the finite shutdown sequence in [04-rendering-and-streaming.md](./04-rendering-and-streaming.md).
5. When coordinators report Stopped, the UI destroys the window and exits the message loop.
6. Main releases RAII process services and returns zero. An abnormal finite-timeout escalation returns a diagnostic nonzero code.

Console control, system shutdown, and session-end notifications take the same stop path with the shorter deadline supplied by Windows. There is no save prompt because the app has no editable state.

## COM and apartment rules

- UI thread: CoInitializeEx(COINIT_APARTMENTTHREADED).
- Worker threads needing WIC: CoInitializeEx(COINIT_MULTITHREADED), balanced on thread exit.
- Render/upload threads: initialize COM only for the APIs they own; DirectWrite factory can be shared only according to its documented threading mode, while device contexts remain thread-affine.
- IPC thread: no COM requirement.
- import worker and compatibility host: their parser/OpenUSD libraries and broker client initialize only after process restrictions, DLL search policy, and Job Object assignment are active.

Apartment violations are caught in developer builds with owner-thread assertions.

## Crash and diagnostics behavior

The app keeps a fixed-size in-memory event ring and may write a local, path-redacted log under LocalAppData only when diagnostics are enabled or a fatal graphics/import-host error occurs. It sends nothing externally. A clean next launch may report that the prior run ended during import and removes abandoned transient cache writes, but the MVP does not implement automatic crash upload or a persistent recent-file list.

Exit codes:

| Code | Meaning |
| ---: | --- |
| 0 | Normal primary shutdown or accepted secondary forward |
| 2 | Command-line error |
| 3 | IPC primary unavailable/rejected |
| 4 | Unsupported system/device at startup |
| 5 | Fatal initialization failure |
| 6 | Forced shutdown after graphics timeout |

Document parse errors do not terminate the application and therefore do not become process exit codes.

## Tests

- simultaneous cold launches elect exactly one primary;
- different users or Terminal Services sessions do not collide;
- malformed, oversized, partial, slow, spoofed, and wrong-version IPC clients are rejected;
- paths with spaces, emoji, combining characters, long-path prefixes, and JSON metacharacters round-trip;
- open storms preserve bounded queues and newest intent;
- closing during map/parse/simplify/copy/fence/device recovery terminates without UAF or a UI wait;
- import-worker or compatibility-host crash, hang, malformed shared section, spoof attempt, and limit termination affect only the current document generation, and this is verified for every format routed through the worker, not only for OpenUSD;
- cache clear racing with lookup/write/open retires leases safely and leaves no partial committed entry;
- stale mutex/abandoned primary recovery;
- foreground-denied activation behavior;
- clean process exit leaves no listener, import worker, compatibility host, mapped source, transient store, temporary cache write, thread, or GPU handle; committed bounded derived-cache entries may remain by design.

Primary references: [Named pipe security](https://learn.microsoft.com/windows/win32/ipc/named-pipe-security-and-access-rights), [GetNamedPipeClientProcessId](https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-getnamedpipeclientprocessid), and [CancelIoEx](https://learn.microsoft.com/windows/win32/fileio/cancelioex-func).

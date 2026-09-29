# TSK-301 verification

TSK-301 replaces the product's per-import worker launch with a two-process,
asynchronously provisioned AppContainer pool and completes the cancellation,
shutdown, renderer-start, and single-attempt device-recovery contracts.

## Implemented behavior

- `WM_CREATE` no longer waits for D3D12 device/swap-chain creation or sandbox
  profile/pool provisioning. Renderer-start failure is posted to the existing
  error surface; pool startup runs on its coordinator thread. An import begun
  while the pool is starting waits only on its owned background import thread.
- Each generation opens a fresh trusted primary handle and output section and
  duplicates the primary, output, cancellation event, and approved sidecars
  into one acquired worker. Request flags carry coarse/detail/test mode per
  generation, so reusable-worker state cannot leak between documents.
- File/output/cancellation handles, mapped views, pinned sidecars, adapter
  state, and catalogs are request-scoped in the worker and close before its
  dispatch loop reads another request. A detail-serving worker owns that
  generation until cancel/failure/close; it cannot accept conflicting work.
- Parser/normalizer/image/meshopt checkpoints and progressive batch/detail
  waits observe a duplicated manual-reset cancellation event. The broker waits
  at most 500 ms for the typed `Cancelled` acknowledgement, then terminates and
  replaces an unresponsive/stale slot. Queue waits are awakened and stale CPU,
  upload, copy-complete, metadata, and UI publications retain their generation
  filters.
- Import threads are joinable application-owned threads rather than detached
  threads. Close cancels and joins them, shuts the pool down with bounded worker
  waits, and then stops the upload/render lanes.
- `Present` device removed/reset/hung results enter one recovery attempt. The
  upload lane stops, stale GPU state is released, the device/direct/copy/budget
  lanes are rebuilt, and the displayed source is reopened to reconstruct its
  coarse proxy and visible detail. A second failure becomes a stable responsive
  error. App smoke exposes a deterministic device-removal injection command.

The file-request control structs grew from 40 to 48 bytes to carry closed
request flags and the cancellation-event handle. Unknown sizes remain rejected
by dispatch. The shared-section wire layout and protocol version are unchanged.
No new path authority, persistent cache, or model-derived write was added.

## Verification evidence

Debug and Release solution builds complete with zero warnings/errors. Catch2:

- `Tests.Unit`: 85 cases / 7,318 Debug and 7,230 Release assertions.
- `Tests.ImportIsolation`: 200 cases / 51,943 assertions in each configuration.
- Pool coverage proves the same sandboxed PID imports two real `.gltf`
  generations with a brokered BIN sidecar, and proves progressive cancellation
  is acknowledged while blocked on batch consumption and that the same PID is
  reused afterward. Existing pool replacement, sandbox/job, hostile-worker,
  sidecar, progressive, compressed decode, coarse/detail replay, and bounded
  scan suites remain green.

Real-app lifecycle and recovery smoke pass in Debug and Release. Each lifecycle
run opens/replaces GLB, STL, both-endian/point PLY, sidecar, sparse, meshopt and
WebP fixtures; cancels pending and active imports; resizes; recovers after a
malformed document; closes during pressure work; and observes zero surviving
workers. Local one-run evidence:

| Configuration | UI round-trip max | Close | Viewer peak private | Worker peak private |
| --- | ---: | ---: | ---: | ---: |
| Debug | 0.39 ms | 97.6 ms | 590.7 MiB | 7.8 MiB |
| Release | 1.36 ms | 77.9 ms | 279.7 MiB | 3.6 MiB |

Recovery smoke covers worker crash, timeout, resource/protocol/upload errors,
valid reopen, warning bounds, stale cancellation, and an injected device
removal. Both configurations record exactly one graphics recovery and reach a
new Ready generation reconstructed from the source.

Commands used from the repository root:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' Preview3D.slnx /p:Configuration=Debug /p:Platform=x64 /m
& .\x64\Debug\Tests.Unit.exe --reporter compact
& .\x64\Debug\Tests.ImportIsolation.exe --reporter compact
python tests/app-smoke/run.py --configuration Debug --output tests/fixtures/baselines/tsk-301/Debug-lifecycle.json --runs 1 --timeout 30 --require-points
python tests/app-smoke/recovery.py --configuration Debug --output tests/fixtures/baselines/tsk-301/Debug-recovery.json

& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' Preview3D.slnx /p:Configuration=Release /p:Platform=x64 /m
& .\x64\Release\Tests.Unit.exe --reporter compact
& .\x64\Release\Tests.ImportIsolation.exe --reporter compact
python tests/app-smoke/run.py --configuration Release --output tests/fixtures/baselines/tsk-301/Release-lifecycle.json --runs 1 --timeout 30 --require-points
python tests/app-smoke/recovery.py --configuration Release --output tests/fixtures/baselines/tsk-301/Release-recovery.json
```

This is functional/local boundedness evidence, not TSK-302's repeated reference
system performance, ETW present/input correlation, physical device-removal, or
multi-hour soak qualification. **TSK-302 is next.**

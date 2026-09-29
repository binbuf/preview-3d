# TSK-204 verification (2026-09-15)

The existing error card now retains typed import codes, host stages and closed
worker phases. Retry owns a copy of the failed path before the new open clears
the card. Open another uses the existing picker; Copy details uses the actual
extension/phase/code and fixed host-owned descriptions, without a source path or
basename. Unsupported FBX, deferred ASCII STL/PLY, empty versus malformed data,
unsafe/missing sidecars, source changes, worker exit/timeout/limits, allocation
failure and upload/protocol failure have separate typed routes. Cancellation
never produces a document error.

Protocol v4 keeps section/descriptor/error-notice sizes unchanged and adds a
16-byte `ImportStatusPayload` topology. Flags are restricted to provisional,
refining and pressure; both warning categories saturate at 64, reserved fields
are zero, and the broker permits one status payload per generation. Invalid
sizes, fields, dependencies and duplicate status payloads fail closed. Error
notices must have the exact length, current generation, nonzero known error code
and known worker phase. Oversized control payloads are protocol violations rather
than worker-crash diagnoses. Worker phase uses the former error-notice reserved
u32; unknown/old normalized-section versions remain rejected.

Optional unknown glTF extensions produce a bounded warning with usable geometry.
Texture warnings remain supported. Only fixed host strings reach the badge/menu;
new generations clear warnings. The spinner reports actual format and current
provisional/refining/upload-pressure state. Cancelled partial geometry remains
interactive in `Partial`, with a static incomplete-preview label. It cannot
become Ready without terminal catalog acceptance, usable geometry, verified
bounds, resolved references and completed copies. Cancel/failure pull the renderer's
atomic immutable display snapshot under the generation cancellation barrier,
preserving real counts, source identity, selection generation, warnings and path
even when its UI notification is queued. Readiness comes from this snapshot's
complete-catalog bounds state, so pending UI metadata cannot label a partial scene Ready.
Today's complete small scene
is the complete catalog; representative capped coarse/fine relationships remain
TSK-206, and this task does not claim that implementation or large-source gates.

The broker rejects explicit UNC/network/device paths and mapped network drives,
validates local regular files, reports zero-byte inputs before worker launch, and
checks primary size/write-time again through its pinned handle. This does not
claim a complete primary reparse policy. Sidecar containment remains in the
existing broker resolver.

Picker, drop-target, tooltip, About and unsupported-format text now describe
GLB/glTF with local sidecars, binary STL, and both-endian binary PLY meshes/points.
[User experience](./design/07-user-experience.md) records current metadata,
texture, progressive and recovery behavior without its historical GLB-only
claims. No dependency or license change.

## Repeatable commands

Run on an interactive Windows desktop with the pinned vcpkg dependencies.
Solution targets build/deploy binaries; the following commands also execute tests.

```powershell
$taskMsbuild = 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe'
foreach ($taskConfig in @('Debug', 'Release')) {
    & $taskMsbuild Preview3D.slnx '/t:Preview3D;Tests_Unit;Tests_ImportIsolation' "/p:Configuration=$taskConfig" /m /v:minimal /nologo
    & "./x64/$taskConfig/Tests.Unit.exe"
    & "./x64/$taskConfig/Tests.ImportIsolation.exe"
    python tests/app-smoke/recovery.py --configuration $taskConfig --output "TestResults/tsk204-$taskConfig-recovery.json"
    python tests/app-smoke/progressive.py --configuration $taskConfig --output "TestResults/tsk204-$taskConfig-progressive.json"
    python tests/app-smoke/textures.py --configuration $taskConfig --output "TestResults/tsk204-$taskConfig-textures.json"
    python tests/app-smoke/run.py --configuration $taskConfig --output "TestResults/tsk204-$taskConfig-lifecycle.json" --runs 1 --require-points
}
# Fast affected slice, after the solution build:
& ./x64/Debug/Tests.ImportIsolation.exe '[recovery]'
```

`recovery.py` deterministically regenerates its tiny sources twice and compares
SHA-256 hashes. Existing corpus sidecars and the pinned triangle are reused.
The optional-feature GLB declares 100 optional extensions to exercise saturation.
Worker crash, silent hang, parser-count limit, invalid reply and upload failure
use opt-in bounded test seams. Timeout is injectable in the broker (120 seconds
in production); the test silent-hang worker avoids the older probe's text stdout.
The real Retry and Open another commands are exercised; a bounded picker-result
seam supplies the selected path for Open another without a manual system dialog.
Normal file-picker interaction itself is not automated by this seam.

Recovery also checks prior model identity and actual camera wheel input after
each failure, corrected same-path retry, optional warning clearing/truncation,
Ready bounds, cancel/invalid activation/late-reply rejection, actual Windows
clipboard text and close/worker termination. Import-isolation adds hostile error
and status fields, accepted-batch source write-time mutation, and direct warning
count checks. Legacy ASCII adapters retain sandbox regression coverage through
explicit test-only worker flags; shipping broker requests reject ASCII.
Progressive automation suppresses UI upload notifications before rendered partial
content, then verifies cancellation and failure adopt that content's metadata and
generation without marking it Ready.

## Results and qualification limits

Committed real-app reports are in
[baselines/tsk-204](../tests/fixtures/baselines/tsk-204/). Full build/test logs
remain in ignored `TestResults/tsk204-*.log`.

Both solution configurations pass. Unit: 79 cases, 7,210 Debug / 7,122 Release
assertions. ImportIsolation: 178 cases and 48,344 assertions in each configuration.
All eight affected recovery cases pass inside the full suite.

| App lane | Debug report | Release report |
| --- | --- | --- |
| 25 typed failure/recovery events, clipboard and card actions | [recovery](../tests/fixtures/baselines/tsk-204/debug-recovery.json) | [recovery](../tests/fixtures/baselines/tsk-204/release-recovery.json) |
| Four progressive/copy-pressure modes, cancelled partial state | [progressive](../tests/fixtures/baselines/tsk-204/debug-progressive.json) | [progressive](../tests/fixtures/baselines/tsk-204/release-progressive.json) |
| Low/full mips, fallback, input, cancel/reopen | [textures](../tests/fixtures/baselines/tsk-204/debug-textures.json) | [textures](../tests/fixtures/baselines/tsk-204/release-textures.json) |
| Open/replace/cancel/resize/recover/points/close | [lifecycle](../tests/fixtures/baselines/tsk-204/debug-lifecycle.json) | [lifecycle](../tests/fixtures/baselines/tsk-204/release-lifecycle.json) |

Final reports match each final viewer executable SHA-256. Fourteen viewer processes
pass, with no remaining viewer/worker processes. Both recovery runs display an
86-character warning for the 100-extension fixture; the sandbox test independently
checks the transmitted count is 64. Progressive display starts at 15/64 chunks,
remains Loading, transitions to Partial on cancellation without verified bounds,
and reaches all 64 chunks after valid reopen. Count backpressure reaches four;
the 16 KiB queue cap holds at 12,788 bytes Debug / 12,404 Release. Texture queue
peaks around 2.8 MB; real GPU readback/goldens and app texture checks pass, with
zero D3D12 errors.

These are routine recovery/progressive/GPU correctness checks on small generated
fixtures. They do not qualify multi-gigabyte scanning, spatial proxies, live GPU
budget scheduling, complete material semantics, accessibility, startup latency,
device recovery or portable release readiness; those remain subsequent tasks.
Physical host/GPU memory exhaustion is not induced. Allocation failure codes
follow actual `bad_alloc`/`E_OUTOFMEMORY` routes; the finite worker Job Object
backstop and independently injected worker limits remain covered by the suites.

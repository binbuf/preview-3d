# TSK-207 verification (2026-09-15)

The product samples its selected adapter's live DXGI local budget and budget
notifications. Admission uses at most 60% of that budget, with 512 MiB headroom
when possible. A budget at or below 512 MiB retains the 60% ceiling instead of
requiring impossible headroom. The policy counts aligned default geometry and
textures, shared packed coarse buffers once, descriptor heaps, render/depth/pick
targets, frame data, a 4 MiB permanent pipeline/overlay allowance, pending upload
reservations and retired resources. Successful allocation never bypasses policy.

The upload coordinator estimates/reserves destinations before allocation and
copy submission. Skipped fine payloads are released, not cached. A budget drop
removes fine draws and restores their coarse parents at a frame boundary. The
retired buffers remain charged until their last direct and copy fences finish.
Small immutable texture tails remain available when a larger texture replaces
them; pressure restores those tails and retires the larger resources/descriptors.
Selecting a validated mip tail on the host performs no decode or resampling.
The complete coarse set is never evicted. If the reserved set cannot fit, the
existing card reports a controlled resource error and preserves the document.
Replacement proxy admission can wait up to five seconds for old fine retirement,
with cancellation-aware coordinator waits. Larger leading textures leave the
64 MiB future complete-coarse reserve available before that catalog is ready;
unaffordable optional textures use the existing neutral fallback.

CPU admission checks worker private commit plus viewer growth against the
lesser of 1.5 GiB and 25% of physical RAM. UMA admission additionally charges
model destinations and limits their target after a 512 MiB CPU allowance for
the ring, queue and worker. Mapped source address space is not private commit.

Verified local bounds and double origins feed conservative CPU frustum culling
and projected-extent/center priority. The request queue retains at most 32 ids
and is rebuilt from the current view; the broker owns at most one request.
Fine geometry outside the view can retire to make room for visible regions.
Until a region's fine copy is complete, its coarse parent remains visible.

The document's zero-capability worker retains its pinned primary and already
approved sidecar handles after the initial validated scan/full pass. Initial
full chunks still traverse validation in source order; budget admission can
discard their normalized payloads without uploading them. Subsequent requests
decode selected source ranges, rather than retaining the normalized scene.
Ordinary glTF accessors, STL facets and PLY face/point records use bounded range
access; Draco remains an independently bounded decode unit. Variable-width PLY
vertices rebuild bounded sparse byte checkpoints. The broker rejects new
sidecar requests during replay and compares every returned descriptor, checksum,
bounds, origin, provenance and scene metadata with the original verified scan.
Cancel/replace/close abandon the generation and wake bounded downstream waits.
The final worker pool remains TSK-301.

Protocol v6 adds `RequestDetail` with a fixed 168-byte request and extends the
descriptor to 160 bytes. `sourceElementOffset` records the starting PLY polygon
fan triangle, including chunks split inside a polygon. Other formats, points
and nongeometry must leave it zero. The validator bounds the field and the
generation validator requires consistent scan/coarse/full provenance. Unknown
protocol versions fail before payload acceptance.

## Repeatable commands

Run GPU suites and visible app checks sequentially on an interactive desktop.
The shared build output's clean/rebuild ordering can remove worker-only
`fastgltf.dll` and `simdjson.dll` after their app-local deployment. After a clean
rebuild, check that both are present beside the worker and match the
configuration's manifest vcpkg bin directory; redeploy them if needed. The final
local clean builds required that redeployment before the passing test runs.
The budget harness generates missing fixtures with TSK-205's bounded recipes,
or reuses the ignored `TestResults/bounded-fixtures/` directory. Pressure uses
100,000 triangles, split mesh fixtures use two million triangles and point
fixtures use eight million points. The report hashes every source and binary.
No large generated binary is committed.

```powershell
$taskMsbuild = 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe'
foreach ($configuration in 'Debug', 'Release') {
    & $taskMsbuild Preview3D.slnx '/t:Preview3D;Tests_Unit;Tests_ImportIsolation' /p:Configuration=$configuration /m /v:minimal /nologo
    $taskDllBin = 'vcpkg_installed/x64-windows/x64-windows/' + $(if ($configuration -eq 'Debug') { 'debug/bin' } else { 'bin' })
    Copy-Item -LiteralPath "$taskDllBin/fastgltf.dll", "$taskDllBin/simdjson.dll" -Destination "x64/$configuration"
    & ./x64/$configuration/Tests.Unit.exe
    & ./x64/$configuration/Tests.ImportIsolation.exe
    python tests/app-smoke/budget.py --configuration $configuration --output TestResults/tsk207-$configuration-budget.json
    python tests/app-smoke/coarse.py --configuration $configuration --output TestResults/tsk207-$configuration-coarse.json
    python tests/app-smoke/progressive.py --configuration $configuration --output TestResults/tsk207-$configuration-progressive.json
    python tests/app-smoke/textures.py --configuration $configuration --output TestResults/tsk207-$configuration-textures.json
    python tests/app-smoke/recovery.py --configuration $configuration --output TestResults/tsk207-$configuration-recovery.json
    python tests/app-smoke/run.py --configuration $configuration --runs 1 --require-points --output TestResults/tsk207-$configuration-lifecycle.json
}
# Optional multi-GiB lane; generation requires many GiB of free disk space.
python tests/fixtures/generate.py --output TestResults/tsk205-large-fixtures --lane qualification --tier large
python tests/app-smoke/budget.py --configuration Release --large-fixture TestResults/tsk205-large-fixtures/A-large-stl.stl --output TestResults/tsk207-Release-large-budget.json
```

`[detail-budget]` isolates deterministic allocation/culling and pinned replay
tests. Replay covers both PLY byte orders, meshes and points, STL, glTF/GLB,
approved external geometry, Draco, repeat requests, unknown region ids, CPU
refusal and a PLY chunk starting inside a polygon fan. Existing upload-ring,
coarse/readback, hostile-worker, source-change and cancellation tests remain
part of the complete suites.

The budget harness uses the existing `--app-smoke` seam and the explicit
`--uma-budget-smoke` selector for a physical D3D12 UMA adapter. Fields 55–61 report
accounted destinations, target, pending reservations, evictions, actual broker
requests, admission skips and outstanding ids. Field 62 injects a model budget
cap in MiB; field 63 can separately simulate the UMA policy on the selected
adapter, while field 64 reports its effective architecture. The harness checks
the requested physical architecture, opens Pressure, split GLB/STL and
both-endian PLY meshes and points; verifies the complete proxy,
shrinks the cap, waits for fence retirement, moves the camera, restores the cap
and checks actual source re-decode recovery. It also verifies resource failure,
texture reduction from width 2048 to its retained 64-pixel tail, valid reopen,
clean shutdown and worker cleanup. These controls are unavailable
without explicit developer smoke mode.

## Qualification limits

Simulating the UMA accounting branch on a discrete adapter checks policy and
ordering; it does not qualify a physical UMA machine. Local private-commit and
budget reports do not establish resident-RAM, reference-system p95, startup,
frame/input latency or driver residency behavior. Those remain explicit
TSK-302 qualification work. No persistent cache, new dependency, license change
or additional worker path authority is introduced.

## Local results

Final Debug and Release builds pass. Complete unit suites pass 85 cases each
(7,317 Debug / 7,229 Release assertions); complete import-isolation suites pass
193 cases / 51,575 assertions each. The retirement test gates real direct and
copy queues independently and proves that shared allocations are charged once,
remain charged after eviction and direct completion, and release only after
copy completion too.

The local desktop has 67,810,054,144 bytes of physical RAM, an NVIDIA GeForce
RTX 4080 (driver 32.0.16.1664) and AMD Radeon Graphics (driver 32.0.21030.2001).
The explicit smoke selector chooses the latter through D3D12's actual UMA
architecture query. These local correctness runs do not establish the TSK-302
reference-system performance gates.

Debug's final budget run selects Pressure and the two-million-triangle STL on
both physical adapters. Both pass proxy survival, automatic eviction, camera
move, actual worker re-decode, cap restoration, 2048-to-64 texture reduction,
reserved-set error/reopen and clean shutdown with zero surviving workers.
Peak queued payloads are below the 128 MiB ceiling and outstanding ids below
32. The focused texture-only run independently passes on both adapters too.

The exact Debug subset command is:

```powershell
python tests/app-smoke/budget.py --configuration Debug --fixture Pressure.glb --fixture split.stl --output TestResults/tsk207-Debug-budget.json
```

Release checks all seven generated Pressure/split inputs and the 3,000,000,084
byte STL on both physical adapters. The large source reports 60,000,000 source
triangles and 916 coarse chunks; reducing the target to 60 MiB evicts all six
resident fine chunks while retaining the proxy, then restoring capacity issues
two bounded detail requests and recovers fine geometry. Peak queued payloads are
67,194,240 bytes and peak outstanding request ids are 24. Both architectures
finish with zero surviving workers and no reported D3D12 errors.

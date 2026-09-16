# TSK-104 fixture and app smoke lanes

Run commands from the repository root on Windows 11 x64. Python 3.11+ is required
(`hashlib.file_digest`); the scripts otherwise use the standard library. Restore/build
the existing pinned vcpkg manifest first. The fixture encoder loads the existing
Release `vcpkg_installed/x64-windows/x64-windows/bin/meshoptimizer.dll` by absolute
path. It is a developer tool dependency; no parser/encoder is added to the viewer.
Use a normal interactive desktop for app smoke. A locked desktop, minimized window,
or persistent occlusion cannot satisfy the visible-Present assertion.

## Routine lane

The committed [manifest](../../interactive-viewer/test-assets/corpus/manifest.json)
pins 33 inputs (about 246 KiB including the manifest/header). `Expectations.h` is
a generated projection for Catch2; the unit test checks every input and the JSON
manifest with Windows CNG SHA-256. Regeneration is explicit; builds never bless
new hashes. JSON expectations are the target contract, with future-task policies
called out for features the current app does not yet support.

In a Visual Studio Developer PowerShell:

```powershell
msbuild Preview3D.slnx /t:Preview3D,Tests_Unit,Tests_ImportIsolation /p:Configuration=Debug /p:Platform=x64 /m
python tests/fixtures/generate.py --output TestResults/tsk-104/repeat-1 --compare interactive-viewer/test-assets/corpus/manifest.json
python tests/fixtures/generate.py --output TestResults/tsk-104/repeat-2 --compare TestResults/tsk-104/repeat-1/manifest.json
python tests/fixtures/verify.py TestResults/tsk-104/repeat-2
Compare-Object (Get-Content TestResults/tsk-104/repeat-1/Expectations.h) (Get-Content TestResults/tsk-104/repeat-2/Expectations.h)
& ./x64/Debug/Tests.Unit.exe
& ./x64/Debug/Tests.ImportIsolation.exe
python tests/app-smoke/run.py --configuration Debug --output TestResults/tsk-104/debug-smoke.json --runs 3
```

`--compare` compares the complete canonical manifest, including hashes, byte sizes,
recipes, counts, bounds, and expectations. `verify.py` independently scans all six
small GLB/STL/PLY variants and checks binary structure, counts, geometric bounds,
spatial coverage, source colors, glTF material factors, and PNG CRC/pixels. It
checks hashes for the entire manifest and refuses metadata scans above 64 MiB.
It is deliberately a small-fixture checker, not the streaming qualification sink.
Catch2 `[fixtures]` cases additionally exercise the actual sandboxed import path,
normalized counts/bounds, four material factors/colors, decoded PNG pixels,
broker-approved/hostile sidecars, and truncated/over-limit headers.

Repeat the solution build, both Catch2 executions, and app smoke with `Release`.
Each executable/script returns nonzero on failure; a solution target only builds.
The exact MSBuild executable used for this implementation is
`C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe`.

## Qualification lane

Large files are generated under ignored `TestResults/`, never committed. Keep
generated manifests and smoke JSON as qualification artifacts. Compare a second
generation to the retained manifest before reusing the corpus. Encoder updates
require deliberate review of changed hashes/metadata; never silently replace pins.

```powershell
python tests/fixtures/generate.py --output TestResults/tsk-104/qualification-small --lane qualification --tier small --compare tests/fixtures/manifests/qualification-small.json
python tests/fixtures/verify.py TestResults/tsk-104/qualification-small
python tests/app-smoke/run.py --configuration Debug --corpus TestResults/tsk-104/qualification-small --output TestResults/tsk-104/debug-small.json --runs 3
python tests/app-smoke/run.py --configuration Release --corpus TestResults/tsk-104/qualification-small --output TestResults/tsk-104/release-small.json --runs 3

# Explicit large-file preparation; not a routine test or a readiness claim.
python tests/fixtures/generate.py --output TestResults/qualification/medium --lane qualification --tier medium
python tests/fixtures/generate.py --output TestResults/qualification/large --lane qualification --tier large
python tests/fixtures/generate.py --output TestResults/qualification/large-repeat --lane qualification --tier large --compare TestResults/qualification/large/manifest.json
```

Small needs approximately 30 MiB; medium approximately 1.5 GiB. Reserve at least
25 GiB for one large generation (50 GiB for two), plus build output. Large generation
and hashing can take minutes and saturate storage. Generation repeats at most a
1 MiB block and writes variable indices in bounded blocks; it does not allocate
a source-sized array. The adversarial externalization temporarily uses another
approximately 2 GiB GLB, copies its BIN in 1 MiB windows, then deletes that one
temporary file. No multi-gigabyte binaries are checked in. Large recipe execution,
large private-memory/queue/proxy gates, ETW, and Pressure budget injection remain
qualification work in TSK-205/206/207/302; they were not run to certify this task.

| Family | Deterministic recipe / target |
| --- | --- |
| A-small | 100,000 triangles or points; GLB padded to 8 MiB, four materials and 2×2 PNG. Routine versions explicitly scale to 240 primitives. |
| A-medium | 5,000,000 triangles or points; GLB padded to 350 MiB and 2,000 nodes (eight geometry nodes, remaining nodes empty). |
| A-large | 60,000,000 triangles or points; legal GLB near 4 GiB (below uint32 length ceiling), STL 3,000,000,084 bytes, colored PLY mesh about 3.24 GiB, padded PLY points about 2.68 GiB. |
| Spatial/proxy | Eight separated triangle/point strata at cube corners, origins 0/10, bounds `[0,0,0]`–`[11,11,11]`. First useful sample needs all eight strata and at least eight primitives; complete proxy needs all strata and verified bounds. Coarse primitive ceiling is min(2m, floor(5% source)), additionally constrained by the reserved GPU budget in TSK-206. |
| PLY variants | Binary LE and BE, RGB8 vertices; mesh faces reference source vertices in reverse order. Points cycle extrema and include bounded unknown scalar properties, 48 bytes per record. |
| Adversarial layout | Approximately 2 GiB GLTF/BIN: 35,791,392 triangles, reverse mesh traversal over ascending position/color/index ranges. Routine companions cover zero-base sparse accessors with valid increasing and invalid decreasing sparse indices. |
| Sidecar policy | Approved local BIN, missing, traversal, percent-encoded traversal, absolute drive, network URL, UNC, and alternate data stream references. No external resource is fetched. |
| Compressed/textured | Real frozen Draco position/normal and position-only seeds; KTX2/Basis texture and corrupt optional texture; pinned meshopt vertex encoding; valid frozen 1×1 lossless transparent WebP plus required `EXT_texture_webp` glTF. |
| Draw-heavy | 2,048 translated instances, 128 meshes/material groups sharing one triangle payload, bounds `[0,0,0]`–`[32,64,0]`. This deliberately exceeds the current 1,024-chunk catalog limit. |
| Pressure | Same spatial/material GLB recipe at the lane's small/medium count. Manifest names the future synthetic DXGI reduction hook; opening a file alone does not simulate pressure. |
| Malformed / limit | GLB truncation; missing STL facets; UINT32_MAX STL count; UINT64_MAX PLY count; empty PLY; missing/unsafe sidecars and invalid sparse indices. Hostile-worker/protocol fixtures remain in the existing Catch2 suite. |

Draco/Basis seeds are copied byte-for-byte from the existing tracked fixtures. Their
original encoding tools are `interactive-viewer/tools/build-gen-glbs-draco.ps1`
and `build-gen-glbs-ktx2.ps1`; re-encoding under a different library/compiler is
not assumed byte-identical. The frozen WebP seed requires no Python image package
at generation time. Material/vertex-color and texture expectations describe the
source/target semantics; current source colors are checked independently, while
their normalized/rendered preservation remains TSK-208/209.

## App smoke seam and evidence

`--app-smoke` enables a bounded, synchronous `WM_COPYDATA` open command on the real
viewer HWND. It calls the normal `BeginOpen`; operation 105 immediately calls the
normal `CancelOpen` before pumping completions to make pending cancellation
deterministic. Operation 104 opens normally. Without the flag these messages are
ignored. Payloads are UTF-16, terminated, at most 32,768 bytes, and may not contain
embedded nulls. No alternate parser, upload path, dialog, or worker privileges are used.

`WM_APP+104` returns state (Empty=1, Loading=2, Ready=3, Failed=4), generation,
first-background QPC microseconds, last geometry-generation QPC microseconds,
presented generation, and acknowledged render-thread resize extent for fields 0–5.
The render thread publishes a geometry timestamp only after that generation reaches
a successful `S_OK` Present; failed/occluded presents do not count. These are
Present-return milestones, not ETW scanout timestamps or proof of a representative
proxy. UI reads atomics; GPU ownership stays on the render thread.

The harness sets per-monitor DPI awareness before comparing physical client sizes.
It opens GLB, replaces with STL, BE PLY mesh and brokered glTF, cancels pending and
Loading generations, checks prior-model preservation, acknowledges three actual
render-thread resizes, exercises malformed failure/valid recovery, and closes while
Loading. All transitions require observed state/generation acknowledgements and
bounded deadlines. Poll delays never count as success. Successful runs assert exit
code zero and no child import workers surviving close.

JSON records source/manifest/executable/harness/generator hashes, build ID/diff hash,
Windows build, CPU/RAM, GPU/driver, storage inventory, power mode, display/refresh/DPI,
cache caveat, per-run geometry/background/close timings, sampled viewer/worker private
commit, UI ping round trips, failures, median/p95/maximum. Private-commit samples are
lower bounds at roughly 20 ms plus polling overhead, not measured Job peaks. OS and
driver caches are uncontrolled. Three runs establish a local baseline, not a robust
p95 release qualification. Do not run GPU suites/builds concurrently with timing runs.

Point-cloud presentation currently fails because the uploader requires indices;
worker point normalization is covered by Catch2. This gap remains explicit:

```powershell
python tests/app-smoke/run.py --configuration Debug --output TestResults/tsk-104/points-gap.json --runs 1 --require-points
```

That qualification assertion must pass after the point display work in TSK-202;
it currently returns nonzero. Zero-base sparse/non-indexed geometry, meshopt/WebP,
Draw-heavy catalogs, representative proxy, and actual Pressure behavior likewise
remain later tasks. This task provides fixtures and repeatable evidence, not MVP
readiness. See [the baseline record](../../.docs/TSK-104_BASELINE.md).

## TSK-202 precision and metadata qualification

`tests/app-smoke/metadata.py` regenerates precision GLB, binary STL, and both-endian
double-position PLY mesh/point fixtures twice and checks their SHA-256. It drives
the real app through Info, native orientation, GPU pixel selection, Frame selected,
and delayed provisional-to-verified framing with and without camera input. The
compact UI metadata contains no CPU vertex/index payload. Protocol v2 fixture
builders and hostile workers cover verified local bounds, double origins, stale
scene facts, non-finite geometry, unknown/old versions and excessive scene spans;
a seeded 512-input mutation corpus exercises the private-snapshot validator.

The earlier point display qualification gap now passes through the minimal
position-only point path. Round splats and color rendering remain TSK-208.
Commands, limits and final Debug/Release evidence are in
[TSK-202 verification](../../.docs/TSK-202_VERIFICATION.md), with raw reports in
[baselines/tsk-202](baselines/tsk-202/).

## TSK-203 texture verification

Frozen PNG/JPEG/Basis fixtures and pixel expectations are pinned in
[manifests/textures.json](manifests/textures.json). Catch2 verifies their hashes
and reads back every mip through worker decode and the product GPU uploader.
`tests/app-smoke/textures.py` generates deterministic PNG GLBs and drives low-mip
publication, immutable replacement, resolution capping, corrupt fallback,
warning clearing and cancellation in the real app. Decoder/transcoder composition
is test-only; shipping decode remains in the sandbox worker.

Commands, budgets and final Debug/Release results are in
[TSK-203 verification](../../.docs/TSK-203_VERIFICATION.md), with raw app reports in
[baselines/tsk-203](baselines/tsk-203/).

## TSK-205 bounded scan verification

`tests/import-isolation/BoundedScanTests.cpp` covers oversized indexed/non-indexed
glTF primitives, STL splits, both-endian PLY nonlocal faces and variable-width
unknown lists, point splits, cancellation, late truncation, source identity,
hostile provenance and source-budget admission. Routine tests generate small
inputs with bounded writes. The hidden `[large-scan]` test uses the existing
large qualification recipes, checks complete counts/bounds and catalogs, and
measures private-memory peaks separately from mapped address space:

```powershell
python tests/fixtures/generate.py --output TestResults/tsk205-large-fixtures --lane qualification --tier large
$env:PREVIEW3D_TSK205_FIXTURES = (Resolve-Path TestResults/tsk205-large-fixtures).Path
& ./x64/Release/Tests.ImportIsolation.exe '[large-scan]'
python tests/app-smoke/bounded.py --configuration Release --output TestResults/tsk205-Release-bounded-app.json --large-fixtures TestResults/tsk205-large-fixtures
```

The viewer harness checks complete medium-size imports, first partial geometry,
bounded upload queues, no UI vertex/index arrays, cancellation of multi-GiB
sources, valid reopen, clean exit and worker cleanup. It fails on any assertion
or timeout. These are scan/cancellation checks; representative proxy usefulness
and two-second first-preview qualification remain TSK-206. Generated source
binaries remain outside git; final manifest/reports are retained in
[baselines/tsk-205](baselines/tsk-205/). See
[TSK-205 verification](../../.docs/TSK-205_VERIFICATION.md) for limits and commands.

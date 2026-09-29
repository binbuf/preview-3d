# TSK-202 verification (2026-09-15)

Protocol v2 adds a 48-byte generation-tagged `SceneMetadata` to the section
header (88 bytes total), and double origins, exact local float bounds, source
mesh/node identities and source attribute flags to each geometry descriptor
(156 bytes total). Older and unknown section versions are rejected. Control
message layouts are unchanged. Producers, synthetic/hostile workers, fixture
builders, size/offset assertions and acceptance checks use the same v2 layout.

glTF node TRS and matrices are read as doubles inside the AppContainer worker:
fastgltf 0.9's public node representation narrows them to floats. The additional
metadata read uses the already installed, pinned simdjson dependency, now named
explicitly in `vcpkg.json`. No new library version or license is introduced.
JSON metadata is capped at 32 MiB before either parser allocates its document,
node transforms at one million, and the remaining source metadata counts at one
million each. Quaternion/matrix values and affine matrix rows are checked.
Linear transforms and translation stay separate before positions narrow to
floats, preserving tiny residuals at trillion-unit offsets. Rebasing does not
absorb a residual into an origin when that double addition would round it away.
PLY double positions subtract their cluster origin before narrowing in either
endianness. STL facet cross products use doubles and discard truly degenerate
facets without imposing the former small-area cutoff.

Each worker chunk reduces exact bounds from finite normalized positions.
Accessor min/max never supply verified bounds. The broker bulk-copies first,
checks that the copied header matches its initial bounded header, and checks
copied metadata, finite/ranged origins and extrema, source identities, all
normalized float attributes, and triangle indices. It independently recomputes
the exact local extrema from the private payload, including `PositionOnly_F32`.
Non-geometry chunks cannot carry geometry metadata. Scene facts must remain
identical across a generation; stale or changed metadata fails before publication.
glTF units/Y-up are checked, while STL/PLY units and up axis remain unspecified.
Origins/local extrema must be finite and within 1e30 in magnitude. Across all
batches, extrema relative to the first accepted cluster origin are limited to
1e15 in magnitude before publication; exceeding that limit returns ResourceLimit.
This keeps the unchanged float camera normalization and clip arithmetic in range
while still accepting large absolute double origins with small local residuals.

The presenting thread reduces only compact verified descriptors, not vertices.
It keeps a fixed scene origin, double relative bounds and real counts, and posts
immutable metadata snapshots to the UI. The app retains no CPU vertex/index
payload. Camera placement uses origin-subtracted float boxes with the existing
camera math, viewport aspect and layout; Info dimensions use double extents.
The Y-up/Z-up correction is an exact axis rotation. Shader draw transforms
subtract the camera pivot in double before narrowing, and their orientation
comes from the displayed generation rather than a potentially older UI snapshot.
Source mesh/node/material identities and counts stay separate from vertex bytes.
Info reports format, unspecified units, vertices, triangles, points, scene facts,
source UV/color presence and material/texture slot counts. Small dimensions use
scientific notation rather than rounding to zero. Loading bounds are provisional;
successful terminal acceptance and uploads make the complete bounds verified.

The interaction epoch advances on actual UI camera changes and held navigation
input. Later bounds expand live framing only while that epoch remains unchanged;
otherwise they update home bounds/radius without resetting the live pose or zoom.
Fit, Reset and Frame selected then use the updated bounds. The standing native
orientation setting still re-homes the camera through its existing action.

Selection uses a depth-tested R8 GPU coverage attachment and asynchronous single
pixel readback. At most one copy and one coalesced request exist, with a 256-byte
readback slot and a viewport attachment capped at 64 MiB. Publication waits for
the direct fence and filters the displayed generation. No whole-scene CPU picking
mesh or normalized payload is rebuilt. Mesh and point pixels select the existing
whole-document selection; empty pixels deselect. The existing cool Fresnel
selection feedback is carried into both shader variants. A minimal position-only
triangle/one-pixel point path supports this task's framing and metadata checks.
Round camera-scaled splats, additional layouts/colors and material completeness
remain TSK-208.

## Repeatable commands

Run from an interactive Windows desktop, with the pinned vcpkg dependencies and
Python 3.11+. Solution targets build/deploy; the executable commands run Catch2.

```powershell
$taskMsbuild = 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe'
& $taskMsbuild Preview3D.slnx /t:Preview3D,Tests_Unit,Tests_ImportIsolation /p:Configuration=Debug /m /v:minimal /nologo
& ./x64/Debug/Tests.Unit.exe
& ./x64/Debug/Tests.ImportIsolation.exe
& ./x64/Debug/Tests.ImportIsolation.exe '[precision],[metadata],[bounds],[fuzz]'
python tests/app-smoke/metadata.py --configuration Debug --output TestResults/tsk-202/debug-metadata.json
python tests/app-smoke/progressive.py --configuration Debug --output TestResults/tsk-202/debug-progressive.json
python tests/app-smoke/run.py --configuration Debug --runs 1 --require-points --output TestResults/tsk-202/debug-lifecycle.json

& $taskMsbuild Preview3D.slnx /t:Preview3D,Tests_Unit,Tests_ImportIsolation /p:Configuration=Release /m /v:minimal /nologo
& ./x64/Release/Tests.Unit.exe
& ./x64/Release/Tests.ImportIsolation.exe
& ./x64/Release/Tests.ImportIsolation.exe '[precision],[metadata],[bounds],[fuzz]'
python tests/app-smoke/metadata.py --configuration Release --output TestResults/tsk-202/release-metadata.json
python tests/app-smoke/progressive.py --configuration Release --output TestResults/tsk-202/release-progressive.json
python tests/app-smoke/run.py --configuration Release --runs 1 --require-points --output TestResults/tsk-202/release-lifecycle.json
```

`metadata.py` regenerates its six precision fixtures twice in `TestResults`,
compares SHA-256, and records those hashes with the viewer hash. It exercises
trillion-offset/tiny GLB geometry with fabricated accessor bounds, billion-offset
double PLY meshes/points in both endiannesses, and micron-scale binary STL. It
opens Info, verifies counts/dimensions, selects displayed pixels, deselects empty
pixels, uses Frame selected, toggles native orientation and restores the original
preference on close. A 64-chunk fixture with delayed copies/final IPC verifies
partial counts/provisional bounds, preserved user zoom, updated Reset framing,
and automatic corrections when the camera is untouched. The harness uses per
monitor DPI awareness so its coordinates match the viewer. Debug diagnostics
count corruption/error messages, excluding ordinary runtime warnings.

Hostile-worker cases inject origin NaN/Inf, fabricated/NaN/reversed bounds,
unknown metadata enums, stale/changed generation metadata, protocol v1, and
position NaN/Inf, plus a later batch with an excessive generation-relative span.
Direct cases add unknown versions, unverified bounds, excessive
counts and malformed unit/flag/origin fields. A seeded 512-input mutation corpus
rechecksums bounded ranges to exercise structural/geometric validation instead
of merely failing at checksums. Precision unit/import cases also cover binary64
layout, double-origin residuals, source double transforms, point geometry and
false accessor extrema.

## Evidence and limits

Both Debug and Release solution builds pass with warnings treated as errors.
Catch2 Unit passes 76 cases / 6,034 assertions in Debug and 76 cases / 5,947
assertions in Release (debug-layer assertions are conditional). ImportIsolation
passes 160 cases / 3,247 assertions in each configuration, including the seeded
mutation corpus and all sandboxed metadata/span attacks. Python harness compilation
and `git diff --check` also pass. Catch2/build logs remain under ignored
`TestResults/tsk-202-*.log`.

Raw app reports are committed in `tests/fixtures/baselines/tsk-202/`:

All three harnesses pass per configuration: six precision scenes and two delayed
framing checks in one viewer, four progressive modes in four viewers, and one
lifecycle viewer with point presentation required. All twelve viewer processes
close successfully with no surviving child workers; Debug metadata diagnostics
report an available debug layer and zero D3D12 errors. Delayed refinement displays
15/64 chunks while Loading, then 192 vertices / 64 triangles with verified bounds.
User zoom is preserved while home framing expands; untouched cameras correct live
framing. Count pressure reaches four batches; the 16,384-byte cap holds at 12,772
bytes Debug / 12,388 Release. Cross-batch textured geometry remains bound.

| Real app check | Debug report | Release report |
| --- | --- | --- |
| Precision, metadata, selection, epoch framing | [metadata](../tests/fixtures/baselines/tsk-202/debug-metadata.json) | [metadata](../tests/fixtures/baselines/tsk-202/release-metadata.json) |
| Four bounded progressive/cancel/texture modes | [progressive](../tests/fixtures/baselines/tsk-202/debug-progressive.json) | [progressive](../tests/fixtures/baselines/tsk-202/release-progressive.json) |
| Lifecycle with point presentation required | [lifecycle](../tests/fixtures/baselines/tsk-202/debug-lifecycle.json) | [lifecycle](../tests/fixtures/baselines/tsk-202/release-lifecycle.json) |

Reports pin fixture and executable SHA-256. Lifecycle `buildId` names the parent
commit because evidence is captured before this task's commit; it also records
the uncommitted diff hash at smoke time. Final completion is recorded in
[PROGRESS.md](./PROGRESS.md).

These are local correctness and lifecycle checks, not release performance or
display scanout qualification. Large-source mapped scanning/chunk splitting,
complete representative coarse catalogs, budget-driven culling/eviction, full
point/material rendering and device/worker lifecycle recovery remain their later
tasks. This task does not retain or introduce a UI/picking whole-scene CPU copy;
the adapters' pre-existing bounded but whole-import normalization is TSK-205.
Binary STL positions cannot recover precision absent from their source floats.
PLY double residuals reflect the precision already present in the source doubles.
No network authority, model-derived persistent writes or worker path capabilities
are added. No retained invariant is relaxed.

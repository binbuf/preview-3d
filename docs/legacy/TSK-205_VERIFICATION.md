# TSK-205 verification (2026-09-15)

Tier A geometry now passes through bounded normalize/publish/reuse loops. Ordinary
glTF primitives use cluster-local remapping and indexed/sparse accessor reads;
binary STL maps facet ranges; binary PLY uses two cached mapped windows and reads
nonlocal face vertices by source index. PLY supports both byte orders and bounded
unknown scalar/list skipping. Variable-width vertex records use one offset
checkpoint per 256 records rather than a source-sized position/index array.
Triangles split at 65,536 per cluster (at most 6.75 MiB for independent vertices);
point clusters contain at most 524,288 positions (6 MiB). Smaller IPC windows
reduce cluster sizes. The broker independently rejects geometry over 16 MiB,
262,144 triangles or 1,048,576 vertices.

The worker writes each cluster directly into its reusable shared section and
waits for broker acknowledgement before reuse. Only descriptors survive a
publication; completed normalized geometry is released. The existing viewer
queue holds at most four batches / 128 MiB and releases CPU upload payloads.
Nonstreaming diagnostic/test callers have an independent 128 MiB accumulation
ceiling; developer-only ASCII adapters remain outside the binary product path.

Primary and per-sidecar source files are capped at 8 GiB, combined source at
12 GiB, triangles/points at 100 million each, vertices at 300 million, nodes and
primitive occurrences at 100,000, and materials at 65,536. Shared constants derive
the catalog/batch ceiling from normalized cluster expansion and per-occurrence
tails, including initial/refined texture identities, rather than source length.
Scratch admission uses min(1 GiB, 25% physical RAM), with conservative glTF parser,
cluster, encoded/decoded texture and catalog reserves. JSON metadata and embedded
data-URI allocations have 32 MiB limits. Draco is independently capped at
512 MiB declared decoded expansion / 10 million triangles before decoding.
Existing texture admission limits remain enforced.

A separate bounded simdjson metadata preflight checks array counts before
fastgltf reserves its asset vectors. It accounts for every nested array entry
using at least 1 KiB or the largest relevant asset structure, adds that storage
to the parser reserve, and releases the preflight DOM before asset construction.
Excessive node/material arrays fail their count limits; nested metadata whose
estimated storage exceeds scratch fails `ScratchLimit`. Nesting over 256 levels
is deferred with `UnsupportedEncoding`. Tests distinguish those failures from
ordinary malformed/empty imports and require zero published batches.

New closed errors distinguish primary source, aggregate source, scratch, catalog
and Draco primitive limits. Sidecar aggregate admission precedes handle
duplication. The broker validates source ranges, cumulative geometry counts,
and primary file identity/size/write-time through its pinned handle before
publication and terminal acceptance. Worker mapped reads also check source
changes. These additions preserve protocol v4 and all wire structure sizes.

The retained catalog contains generation IDs and fixed-width source descriptors,
never normalized payloads. STL/PLY provenance is a primary-file byte interval.
glTF provenance encodes primitive ordinal in the high 32 bits and first index
element in the low 32 bits; length is the source index-element count. Mesh/node
IDs preserve occurrence identity. The immutable model snapshot retains full
file identity and GPU geometry retains its source descriptor for TSK-207.
Fine-detail requests, source reopening policy and eviction are still TSK-207;
no persistent cache was added.

Binary PLY with nonempty faces preceding vertices returns `UnsupportedEncoding`.
Oversized polygons/lists and malformed sparse accessors fail with named limits
or malformed data before large allocations. Ordinary omitted-base sparse and
non-indexed glTF accessors are supported on the progressive product path.
Legacy whole-section diagnostics reject omitted-base sparse accessors safely.
Cooperative checkpoints observe closed broker channels between bounded units;
existing Job termination handles cancellation backstops. Draco library calls
remain individually bounded but do not gain allocator/cancellation hooks here.
Explicit cancel acknowledgements and the final worker pool remain TSK-301.

## Repeatable commands

Run from the repository root on an interactive Windows desktop with the pinned
vcpkg dependencies. Run GPU suites and visible viewer harnesses sequentially.

```powershell
$taskMsbuild = 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe'
foreach ($configuration in 'Debug', 'Release') {
    & $taskMsbuild Preview3D.slnx '/t:Preview3D;Tests_Unit;Tests_ImportIsolation' /p:Configuration=$configuration /m /v:minimal /nologo
    & ./x64/$configuration/Tests.Unit.exe
    & ./x64/$configuration/Tests.ImportIsolation.exe
    python tests/app-smoke/bounded.py --configuration $configuration --output TestResults/tsk205-$configuration-bounded-app.json
    python tests/app-smoke/progressive.py --configuration $configuration --output TestResults/tsk205-$configuration-progressive.json
    python tests/app-smoke/textures.py --configuration $configuration --output TestResults/tsk205-$configuration-textures.json
    python tests/app-smoke/recovery.py --configuration $configuration --output TestResults/tsk205-$configuration-recovery.json
}
python tests/fixtures/generate.py --output TestResults/tsk205-large-fixtures --lane qualification --tier large
$env:PREVIEW3D_TSK205_FIXTURES = (Resolve-Path TestResults/tsk205-large-fixtures).Path
& ./x64/Release/Tests.ImportIsolation.exe '[large-scan]'
python tests/app-smoke/bounded.py --configuration Release --output TestResults/tsk205-Release-bounded-app.json --large-fixtures TestResults/tsk205-large-fixtures
```

The hidden large scan requires the explicit tag and fixture environment variable;
routine suites do not silently allocate multi-gigabyte inputs. Generated binaries
remain outside git. The committed baseline directory contains the deterministic
manifest, text test results and viewer JSON with executable/fixture hashes.

## Final results and limits

Debug/Release solution builds pass. Unit suites pass 79 cases each with 7,210
Debug / 7,122 Release assertions. ImportIsolation passes 185 cases / 48,952
assertions in each configuration, including parser metadata reservation,
source-limit and hostile provenance coverage.

The explicit Release large scan passes 31,776 assertions. Each binary large
fixture contains 60 million triangles or points, with verified bounds
`[0,0,0]` through `[11,11,11]`; the sidecar case contains 35,791,392 triangles.

| Source | Bytes | Geometry chunks / batches | First batch ms | Complete ms | Worker peak private MiB | Host peak growth MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| GLB | 4,294,901,760 | 920 / 102 | 700.0 | 63,406.5 | 30.0 | 128.3 |
| STL | 3,000,000,084 | 916 / 102 | 368.6 | 34,449.0 | 8.6 | 128.0 |
| PLY mesh LE | 3,480,000,245 | 916 / 102 | 15,723.9 | 100,746.0 | 23.3 | 128.0 |
| PLY mesh BE | 3,480,000,242 | 916 / 102 | 17,281.5 | 103,941.0 | 23.3 | 128.1 |
| PLY points LE | 2,880,000,398 | 115 / 12 | 1,396.3 | 15,887.8 | 15.1 | 128.1 |
| PLY points BE | 2,880,000,395 | 115 / 12 | 1,414.9 | 16,504.0 | 15.1 | 128.0 |
| glTF + mapped sidecar | 9,719 + 2,147,483,596 | 552 / 61 | 735.2 | 39,924.1 | 25.8 | 128.0 |

Mapped worker address space peaks at 4,366,319,616 bytes for GLB and
2,218,913,792 for glTF plus sidecar; it remains below 89 MB for STL/PLY.
The host's mapped address space remains about 71 MB throughout.

The Debug viewer completes oversized single-primitive GLB/STL and both-endian
PLY meshes (2 million triangles each) and points (8 million each). Initial
geometry is explicitly Loading, terminal content has correct counts and verified
bounds, UI geometry arrays remain empty, valid reopen succeeds, exit code is zero
and no workers survive. Peak queued payloads are 66,847,570 bytes; measured
viewer growth plus worker private commit is about 704 MiB, below the 1.5 GiB
aggregate acceptance ceiling. The final JSON retains each executable/fixture
hash and raw first-geometry/completion measurements.

The Release viewer repeats those complete imports and additionally opens/cancels
the large GLB, STL, PLY mesh LE and PLY points BE sources after their first
geometry. Cancelled content remains Partial with unverified scene bounds;
valid reopen reaches Ready. Peak queued payloads are 66,847,418 bytes and measured
viewer growth plus worker private commit is about 565 MiB. Exit code is zero and
no workers survive. Progressive (four modes), textures and recovery also pass in
both configurations: fourteen final viewer processes, zero survivors. Debug
texture smoke reports zero D3D12 errors. The retained inventory pins viewer,
worker, hostile-worker, C++ test, harness and changed source hashes, plus local
Windows/CPU/RAM/GPU-driver metadata.

These scans verify first publication before full normalization, terminal counts
and bounds, split/catalog integrity, peak private commit, cancellation and
late malformed input. Memory sampling uses PSAPI peak pagefile/private counters
at callbacks; mapped bytes are VirtualQueryEx committed MEM_MAPPED address space,
including IPC views, and are **not resident RAM**. glTF/GLB keeps source buffers
virtually mapped for the parser's accessor adapter; STL/PLY use bounded windows.
Viewer measurements sample process private commit and queue bytes. Caches are
uncontrolled and concurrent CPU checks may affect elapsed times.

PLY mesh vertex validation precedes its first face publication, so the large
mesh fixtures do not qualify the MVP's two-second useful representative preview.
TSK-206 must add representative sampling and coarse/fine relationships; neither
an arbitrary first chunk nor these scan timings establishes that gate. Live GPU
budget admission/eviction, UMA/discrete pressure, frame/input/startup targets,
complete material/point rendering and dependency threat qualification remain
their later tasks. No dependency or license changes were introduced.

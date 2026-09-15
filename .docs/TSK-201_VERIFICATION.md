# TSK-201 verification (2026-09-15)

The app supplies the broker batch callback and transfers private validated payloads
into the upload coordinator. Accepted capacity includes queued tasks, the active
coordinator task, and fence-complete publications awaiting render acceptance:
128 MiB and four batches, including payload/vector capacities and task metadata.
A single validated batch waiting for admission remains on the broker thread;
the 64 MiB shared section is separate. These are bounded windows, not a retained
whole-scene normalized payload. The finite generation catalog is capped at 65,536
chunks; the existing 256-round-trip ceiling remains provisional for TSK-205.

Default-heap allocation, geometry bounds scans, staging-ring waits, and copy
recording/submission run on the upload coordinator. It owns an upload-only path
sharing the render device, with its own ring/copy queue. The presenting path owns
no upload ring. Publications cross to the render thread only after their copy
fence completes; bindings/catalog additions happen between frames. Geometry keeps
its generation and chunk identity, and later batches append to the drawable set.
Each texture retains its immutable descriptor heap across batches. Missing
material/image dependencies use the existing neutral shader until resolved.

The broker permits bounded forward material/image references, checks their target
roles when delivered, and rejects unresolved terminal catalogs. Standalone section
validation remains strict by default. Existing generic geometry/LOD references
must resolve in the current/prior catalog; only a mesh's first missing dependency
may be a forward material reference. Sparse texture slots retain their semantics.
The wire layout/version is unchanged. The callback completes bounded admission
before nonterminal acknowledgement; cancellation after acceptance prevents the
next acknowledgement. Cancel/failure/replace/close invalidate the generation,
wake admission waiters, and discard stale publications. Copy destinations retire
on the copy timeline; displaced drawable resources retire on the direct timeline,
with both constraints checked wherever both can apply.

The previous model stays visible and navigable until replacement geometry is
usable. Loading does not collapse its bottom bar/Info layout or disable camera
input. Partial publications remain Loading. Ready requires successful terminal
catalog acceptance and completion of all preceding uploads, with displayable
geometry; first-copy completion alone cannot mark Ready. Verified metadata and the
complete coarse-proxy readiness contract are the subsequent TSK-202/206 work.

## Repeatable commands

Run from an interactive Windows desktop, using the existing restored vcpkg tree
and Python 3.11+. No new dependencies or licenses are introduced.

```powershell
$taskMsbuild = 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe'
& $taskMsbuild Preview3D.slnx /t:Preview3D,Tests_Unit,Tests_ImportIsolation /p:Configuration=Debug /m /v:minimal /nologo
& ./x64/Debug/Tests.Unit.exe
& ./x64/Debug/Tests.ImportIsolation.exe
python tests/app-smoke/progressive.py --configuration Debug --output TestResults/tsk-201/debug-progressive.json
python tests/app-smoke/run.py --configuration Debug --runs 1 --output TestResults/tsk-201/debug-lifecycle.json

& $taskMsbuild Preview3D.slnx /t:Preview3D,Tests_Unit,Tests_ImportIsolation /p:Configuration=Release /m /v:minimal /nologo
& ./x64/Release/Tests.Unit.exe
& ./x64/Release/Tests.ImportIsolation.exe
python tests/app-smoke/progressive.py --configuration Release --output TestResults/tsk-201/release-progressive.json
python tests/app-smoke/run.py --configuration Release --runs 1 --output TestResults/tsk-201/release-lifecycle.json

# Targeted batch/catalog and hostile-worker coverage:
& ./x64/Debug/Tests.ImportIsolation.exe '[chunk-batch]'
```

`progressive.py` generates 64/256 independent triangle meshes from the pinned
`tri_tight.glb` binary accessors, regenerates each twice, and compares SHA-256.
It adds no checked-in binary fixture. The 64-mesh hash is
`7c2429ca86b870637155b61813d0104540f64f0caacd59649a797793dfc6ae13`;
the 256-mesh hash is
`e6f74ab98fc33f9dd3ea91b64e1556280f5a55ea5ee9c9ae8134d648b6975c99`.

All four developer modes gate real copy-queue execution for 750 ms and shrink the
staging ring to 4 KiB initially / 8 KiB maximum, with 4 KiB batch flushes. The gate
timer releases on cancel/close. The progressive mode also shrinks the shared
section to 4 KiB and explicitly launches the worker's delayed-batch mode: it waits
1.5 seconds after acknowledging a nonterminal batch before producing the next
batch/terminal IPC. This establishes a real early-display interval before final
IPC, rather than merely delaying the UI's completion message. Queue modes use an
8 KiB section, with no worker delay, to exhaust downstream capacity; the byte mode
reduces the same production admission cap to 16 KiB. The texture mode uses a
420-byte section and the existing Basis textured triangle, splitting its image
from later material/geometry. All test settings require explicit command-line
flags and leave ordinary activations at the production capacities.

The smoke observes a successful non-occluded geometry Present, Loading/Ready,
additive chunk counts, the cross-batch texture binding, camera distance changing
from actual mouse-wheel input while Loading, continued prior-scene/chrome Presents,
queue caps, cancellation while full, rapid replace/cancel, valid reopen with no
stale chunks, close while work is pending, and termination of child workers.
Texture binding evidence is not a pixel-golden claim; texture/shader qualification
remains TSK-203/208. Queue pressure uses a reduced byte cap and small generated
sources; it is not multi-gigabyte or live DXGI budget qualification.

## Results

Both solution builds pass. Debug Unit: 74 cases / 6,006 assertions; Release Unit:
74 cases / 5,919 assertions (debug-layer assertions are conditional).
ImportIsolation: 155 cases / 1,835 assertions in each configuration.
The targeted batch suite passes 18 cases / 87 assertions, including forward sparse
image references, wrong image/material roles, unresolved terminal references,
and cancellation before the next acknowledgement.

| Targeted app evidence | Debug | Release |
| --- | --- | --- |
| First / final displayed chunks | 20 / 64 | 20 / 64 |
| Peak count-pressure batches | 4 | 4 |
| Peak count-pressure bytes | 29,680 | 28,144 |
| Peak bytes with 16,384-byte cap | 14,840 | 14,072 |
| Cross-batch textured mesh bindings | 1 | 1 |
| Camera input during Loading | all four modes pass | all four modes pass |
| Maximum observed UI query round trip | 0.81 ms | 8.37 ms |
| Maximum targeted close time | 118.79 ms | 83.25 ms |

Four targeted app runs and the existing lifecycle run pass per configuration:
ten real viewer processes in total, with zero surviving viewers/child workers.
The existing lifecycle smoke still covers GLB/STL/big-endian PLY/glTF sidecars,
replace/cancel/resize/malformed recovery/close. Its observed UI query maxima are
5.11 ms Debug / 0.45 ms Release. These are observed samples from this desktop,
not ETW input-to-scanout, p95 performance gates, or an MVP release qualification.

The committed targeted reports are
[Debug](../tests/fixtures/baselines/2026-09-15/tsk-201-debug-progressive.json) and
[Release](../tests/fixtures/baselines/2026-09-15/tsk-201-release-progressive.json).
Full lifecycle reports and Catch2 logs are under ignored `TestResults/tsk-201/`
and `TestResults/tsk-201-*.log`. The first app smoke caught an accidentally omitted
pipeline/depth/constant-buffer initialization during the refactor; restored before
all final verification. No source/texture decode moved into the trusted viewer.
Point display, source-sized worker normalization, verified global bounds/metadata,
coarse proxies, and live GPU pressure handling remain their named later tasks.

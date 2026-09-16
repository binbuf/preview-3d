# TSK-302 performance and memory qualification

`Preview3D.exe --benchmark=<fixture>` is the bounded, machine-readable product
mode. It accepts `--benchmark-duration-ms=100..600000`,
`--benchmark-frames=1..100000`, `--benchmark-repeat=1..100`,
`--benchmark-reference=performance|compatibility`, and an optional
`--benchmark-result=<json>` destination. Without a destination the GUI process
attaches to its parent console, or allocates one, and writes JSON there. Exit is
0 only when every gate applicable to the selected reference passes, 2 for gate
failure, 3 for output failure, and 64 for invalid arguments.

The render thread remains the only owner of `Present`; benchmark mode keeps it
awake until the first duration/frame bound is reached. Normal idle behavior is
unchanged. Results retain raw Present-return intervals and mean/median/p95/max,
failed/occluded counts, automated gate booleans, first background/loading UI/
geometry/complete-coarse/verified-bounds/refinement milestones, synthetic
input-to-next-present latency, UI heartbeat gaps, viewer/worker private commit,
mapped committed views separately, upload queue peaks, GPU accounted/pending
bytes, and the actual configured general-worker Job limit. Until the worker has
allocator-category counters, its entire private commit is asserted as a
conservative upper bound on scratch and is labeled as such. Missing cancellation
timing is emitted as `null`, not silently claimed as a pass; the lifecycle/
cancellation lane remains its source of truth.

`tests/performance/qualify.py` performs fresh-process repeats, verifies fixture
and executable hashes, adds the reference-system/run metadata required by design
doc 09, summarizes mean/median/p95/max/failures, and preserves each raw app
result. `--presentmon <PresentMon.exe>` is a local opt-in ETW lane. The wrapper
starts PresentMon 2.x against `Preview3D.exe`, retains every DXGI interval, and
labels dropped, missing, invalid, and unclassified events mechanically. Routine
tests never start ETW, require elevation, or delete intervals manually.

Example commands from a Developer PowerShell:

```powershell
msbuild Preview3D.slnx /t:Preview3D,Tests_Unit,Tests_ImportIsolation /p:Configuration=Release /p:Platform=x64 /m
& ./x64/Release/Tests.Unit.exe
& ./x64/Release/Tests.ImportIsolation.exe

python tests/performance/qualify.py interactive-viewer/test-assets/corpus/A-small-glb.glb --configuration Release --reference compatibility --runs 5 --duration-ms 10000 --frames 100000 --output TestResults/tsk-302/A-small.json
python tests/performance/qualify.py TestResults/bounded-fixtures/Pressure.glb --configuration Release --reference compatibility --runs 5 --copy-delay --duration-ms 10000 --frames 100000 --output TestResults/tsk-302/Pressure-copy-delay.json
python tests/performance/qualify.py TestResults/tsk205-large-fixtures/A-large-stl.stl --configuration Release --reference performance --runs 5 --duration-ms 30000 --frames 100000 --output TestResults/tsk-302/A-large-stl.json
python tests/performance/qualify.py interactive-viewer/test-assets/corpus/A-small-glb.glb --configuration Release --reference performance --runs 5 --presentmon C:/tools/PresentMon.exe --duration-ms 10000 --frames 100000 --output TestResults/tsk-302/A-small-etw.json

# Negative controls: each command must produce a failing app status/nonzero exit.
python tests/performance/qualify.py interactive-viewer/test-assets/corpus/A-small-glb.glb --configuration Release --runs 1 --worker-budget-failure --output TestResults/tsk-302/worker-limit-detected.json
python tests/performance/qualify.py interactive-viewer/test-assets/corpus/A-small-glb.glb --configuration Release --runs 1 --occluded --output TestResults/tsk-302/occlusion-classification-detected.json
```

The last negative control overrides the benchmark recorder's classification for
otherwise real Presents; it is deterministic classification coverage, not a claim
that the desktop compositor naturally occluded the window. The optional PresentMon
lane supplies real compositor/scan-out classifications.

Use the TSK-104 generator for A-medium/A-large GLB, STL, both-endian PLY mesh
and points, Draw-heavy, and Pressure. Run cold launches serially on the documented
reference system; do not build or run another GPU suite concurrently. Performance
reference results require its 144 Hz display, high-performance power mode and
hardware class. Compatibility runs do not claim the performance-only startup or
multi-gigabyte timing gates. Warm-cache, Tier B, thumbnail, and MSI metrics are
intentionally absent from the schema.

The final local Release compatibility run used the committed A-small GLB on
Windows 11 build 26200, Ryzen 9 7900X3D, RTX 4080, 64 GiB RAM, NVMe,
3840x2160 at 60 Hz, Balanced power. It exposed and fixed a command-line startup
race where the asynchronous render path supplied a zero-byte CPU policy to an
immediately-started import. Three post-fix cold processes all passed applicable
gates: geometry/verified coarse p95 430.259 ms, frame p95 4.406 ms, maximum
14.549 ms, input-to-present p95 3.656 ms, viewer peak private commit 290,263,040
bytes and aggregate worker peak 3,612,672 bytes. Draw-heavy and the 750 ms delayed
Pressure control also passed; Pressure moved complete-coarse to 1,237.185 ms while
retaining a 4.391 ms frame p95 and 15.084 ms maximum. The 1 MiB worker-Job and
occlusion negative controls emitted failing status/nonzero exit as required.

The same non-reference desktop recorded honest blocking results for the existing
generated multi-GiB GLB, STL and little-endian PLY-points fixtures. In 30-second
runs GLB/STL remained Loading without a presented proxy (their bounded upload
queues peaked below 64 MiB); PLY reached a `ValidateSection/ResourceLimit` failure.
Frame p95 remained 4.379–4.495 ms and sampled private commit remained under the
aggregate cap, but missing useful geometry correctly failed the timing gate. These
are retained regression artifacts, not waived results. A-medium was not regenerated
and PresentMon was not installed, so medium cold-launch and real ETW correlation
remain required on the formal reference lane. This machine/run is compatibility
development evidence, not performance-reference release qualification.

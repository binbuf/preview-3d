# TSK-104 baseline — 2026-09-15

TSK-104 establishes immutable fixtures, generation recipes, and repeatable app
lifecycle smoke for the scope-limited MVP. Exact commands and requirements are in
[tests/fixtures/README.md](../tests/fixtures/README.md). Full performance
qualification remains TSK-302; the multi-gigabyte proxy and resource gates were
not exercised here.

## Reproducibility and checks

The 33-input routine manifest SHA-256 is
`5c0f18b1e19ce6ca5c1f3c3d563ce117ede4b713539132a47ea1b4c445d3b976`.
Two independent generations match that manifest byte-for-byte, including expected
metadata; their generated Catch2 projection is identical. The full A-small manifest
is pinned separately at `tests/fixtures/manifests/qualification-small.json`, hash
`ba10c7589b79e569a9b6183a12257d1140c9de648e7443aabddee0c3791f52d6`;
its repeat generation also matches. Independent source-byte scans verify all six
routine and full A-small GLB/STL/PLY mesh/point/endian variants.

Solution builds of Preview3D, Tests_Unit, and Tests_ImportIsolation pass in Debug
and Release. Both actual Catch2 binaries pass in both configurations:

| Configuration | Tests.Unit | Tests.ImportIsolation |
| --- | --- | --- |
| Debug | 74 cases / 6,006 assertions | 152 cases / 1,816 assertions |
| Release | 74 cases / 5,919 assertions | 152 cases / 1,816 assertions |

The fixture work found and fixed an existing PLY defect: `SplitHeaderLines` scanned
binary payload after `end_header` and rejected valid files for incidental text-line
limits. It now stops at the terminator while preserving limits on actual header
text. Both endian/mesh/point cases exercise binary bodies longer than 4 KiB without
requiring incidental newline bytes.

## Local app evidence

Raw reports are retained under
[tests/fixtures/baselines/2026-09-15](../tests/fixtures/baselines/2026-09-15/).
Each includes executable/harness/generator and fixture hashes, HEAD/build identity,
working-diff hash, hardware, driver, power, display, clock, all individual timings,
memory samples, and failure counts. Local working diagnostics also remain under
ignored `TestResults/tsk-104/`.

Hardware: Windows 11 Pro build 26200, Ryzen 9 7900X3D, approximately 64 GiB RAM,
RTX 4080 driver 32.0.16.1664; desktop 3840×2160 at 240 Hz, viewer DPI 144 (150%),
Balanced power plan. The report contains the storage inventory. OS/driver caches
are uncontrolled; every run starts a fresh viewer process. No GPU tests or builds
ran concurrently with these final timing runs.

Each lane below has three runs and zero lifecycle failures. Median / p95 / maximum
are shown for timing values; with three samples nearest-rank p95 equals maximum.
The scene is opened after an observed empty background Present. These are successful
non-occluded **Present-return** milestones, not ETW scanout or complete-proxy gates.

| Lane/build | Background ms (median / p95 / max) | Initial GLB geometry ms (median / p95 / max) | Max sampled viewer / worker private MiB | Max close ms |
| --- | --- | --- | --- | --- |
| Routine Debug, 240 triangles | 406.47 / 407.09 / 407.09 | 97.01 / 97.53 / 97.53 | 759.92 / 2.61 | 90.31 |
| Routine Release, 240 triangles | 350.97 / 368.05 / 368.05 | 70.61 / 71.87 / 71.87 | 462.38 / 1.23 | 71.96 |
| Full A-small Debug, 8 MiB / 100k triangles | 402.53 / 425.15 / 425.15 | 380.50 / 382.94 / 382.94 | 801.63 / 30.32 | 87.18 |
| Full A-small Release, 8 MiB / 100k triangles | 342.52 / 347.02 / 347.02 | 125.55 / 126.17 / 126.17 | 508.90 / 28.94 | 72.08 |

The viewer numbers include its existing upload ring/device baseline and are not
large-scene incremental memory. Worker peaks are sampled lower bounds at roughly
20 ms plus polling overhead; short-lived workers can be missed. Maximum observed
UI query round trip is 1.72 / 2.11 / 1.15 / 4.64 ms in the four lanes respectively.
Those values do not establish input-to-affected-Present, cancellation observation,
or heartbeat-gap gates. Background timings exceed the original 150 ms target;
this is baseline evidence, not startup-gate success on the specified reference system.

Every successful run exercises GLB open, STL/BE-PLY-mesh/brokered-glTF replacement,
deterministic pending cancellation and asynchronous Loading cancellation, prior-model
preservation, three physical client-size changes acknowledged by the render thread,
malformed failure and valid recovery, and close during Loading. All twelve finish
with exit code zero and no surviving child import workers.

## Explicit qualification gaps

- Worker PLY point normalization passes, but the D3D12 uploader currently skips
  non-indexed payloads. `--require-points` is an explicit qualification assertion
  that fails on the current app; point display remains TSK-202.
- The valid zero-base sparse/non-indexed fixture targets TSK-205/209. The current
  glTF adapter deliberately rejects that combination; current tests do not claim
  its target acceptance is implemented.
- Meshopt/WebP target acceptance is TSK-209. Draco/KTX2/Basis frozen seeds retain
  their existing real decoder tests. Color/texture source expectations and worker
  material/PNG results do not establish full shader semantics or visual fidelity.
- Draw-heavy has 2,048 instances and 128 material groups, above the current
  1,024-chunk cap. Its target acceptance remains TSK-205. Pressure is a medium/small
  scene recipe; actual DXGI reduction and proxy survival remain TSK-207/302.
- Medium/large recipes and runtime SHA-256 manifests exist, but large generation,
  representative/complete proxy thresholds, bounded streaming/upload behavior,
  WARP/reference-system measurements, soak, and full ETW evidence remain later work.

No dependencies or license obligations were added. The generator uses the already
pinned meshoptimizer encoder, standard-library PNG construction, and existing
owned compressed seeds. No product network or model-derived persistent writes
were introduced. TSK-201 is next in the active numeric sequence.

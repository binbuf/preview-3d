# TSK-206 verification (2026-09-15)

The product worker now emits a bounded provisional preview, validates every
source region, publishes a complete coarse catalog, and re-decodes full regions.
There is no intermediate LOD builder, cross-fade or persistent derived cache.
All parsing, sampling and normalization remain inside the zero-capability
AppContainer. The host copies and revalidates every payload before upload.

Preview selects eight source strata and eight glTF occurrence strata, with up
to three adjacent PLY points per stratum. It is capped at 4,096 primitives,
1 MiB normalized bytes and 8 MiB aligned GPU allocation. Only the preview may
display before the complete scan, and only when no earlier document exists.
Its counts and bounds are provisional. Binary PLY meshes skip fixed-width
vertex bodies and walk bounded face-list record lengths to selected source
strata; variable-width vertices retain the existing sparse offset checkpoints.

The complete sampler walks one normalized cluster at a time, chooses geometry
hash representatives from 64 spatial cells, protects extrema within occupied
cells and fills remaining slots from source strata. It preserves selected
vertex attributes, winding, material dependencies and mesh/node provenance.
Only bounded samples and fixed-width descriptors survive the scan. Scan
geometry crosses validation but receives no GPU allocation. Source counts and
verified bounds derive from these scans, independently of displayed density.

[ADR-015](./design/11-decisions-and-risks.md#adr-015-bounded-coarsefull-sampling-and-the-mandatory-coverage-floor)
explicitly revises the density acceptance: below 20 valid primitives retain all;
otherwise use `min(2,000,000, max(floor(valid / 20), nonemptyRegions))`.
The coverage floor is necessary for Draw-heavy's 2,048 separate single-triangle
instances. Dense eight-component fixtures still obey their original 5% ceiling.
The independent 64 MiB coarse GPU ceiling includes 64 KiB buffer allocation
alignment. Samples share packed vertex/index buffers per publication. Both
worker and broker reject an oversized reserved set with a controlled limit.
Live geometry/texture budget admission and adaptive density remain TSK-207.

Protocol v5 preserves the 88-byte header and 156-byte descriptor sizes. Closed
preview/scan/coarse/full roles use stable logical-region identities. A completion
record requires a nonempty sample for every scanned region, exact totals,
accepted dependencies and the density/byte limits. Fine records must match scan
checksums, counts, bounds, origin and source provenance. Terminal acceptance
requires every full replacement. Primary and broker-approved sidecar handles
remain pinned across passes; changed identities fail without new path authority.
Catalog ceilings include all three region roles, bounded preview identities,
dependency records and completion/status records.

The render thread hands off a replacement only after the complete coarse set's
copies finish. Cancel/failure before that preserves the prior model, metadata
and source identity. A first document's preview remains Loading and unverified;
the complete proxy remains Loading/refining with verified source bounds until
terminal full uploads succeed. Fine regions suppress their coarse parents only
after their copy fences complete. Eviction restores parents at a frame boundary;
displaced/evicted resources survive their last direct/copy use fences.

## Repeatable commands

Run GPU tests and visible-app harnesses sequentially on an interactive Windows
desktop. Generated binary fixtures stay in ignored `TestResults/`.

```powershell
$taskMsbuild = 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe'
python tests/fixtures/coarse.py --output TestResults/tsk206-fixtures
python tests/fixtures/coarse.py --output TestResults/tsk206-fixtures-repeat
$env:PREVIEW3D_TSK206_FIXTURES = (Resolve-Path TestResults/tsk206-fixtures).Path
foreach ($configuration in 'Debug', 'Release') {
    & $taskMsbuild Preview3D.slnx '/t:Preview3D;Tests_Unit;Tests_ImportIsolation' /p:Configuration=$configuration /m /v:minimal /nologo
    & ./x64/$configuration/Tests.Unit.exe
    & ./x64/$configuration/Tests.ImportIsolation.exe
    & ./x64/$configuration/Tests.ImportIsolation.exe '[coarse-reordered]'
    python tests/app-smoke/coarse.py --configuration $configuration --output TestResults/tsk206-$configuration-coarse-app.json
    python tests/app-smoke/bounded.py --configuration $configuration --output TestResults/tsk206-$configuration-bounded-app.json
    python tests/app-smoke/progressive.py --configuration $configuration --output TestResults/tsk206-$configuration-progressive.json
    python tests/app-smoke/textures.py --configuration $configuration --output TestResults/tsk206-$configuration-textures.json
    python tests/app-smoke/recovery.py --configuration $configuration --output TestResults/tsk206-$configuration-recovery.json
    python tests/app-smoke/run.py --configuration $configuration --runs 1 --require-points --output TestResults/tsk206-$configuration-lifecycle.json
}
# Reuse TSK-205's deterministic qualification fixtures, or generate explicitly:
python tests/fixtures/generate.py --output TestResults/tsk205-large-fixtures --lane qualification --tier large
$env:PREVIEW3D_TSK205_FIXTURES = (Resolve-Path TestResults/tsk205-large-fixtures).Path
& ./x64/Release/Tests.ImportIsolation.exe '[coarse-large-preview]'
python tests/app-smoke/bounded.py --configuration Release --output TestResults/tsk206-Release-bounded-app.json --large-fixtures TestResults/tsk205-large-fixtures
```

The hidden reordered test explicitly loads twelve 100,000-primitive original/
reordered GLB/STL/PLY mesh/point fixtures. Both little and big endian PLY are
included. First preview and first coarse publication must each cover all eight
components; complete coarse coverage, density, bytes, source catalog and fine
checksum correspondence are checked. Two generator runs must produce identical
manifests/hashes. Routine C++ tests also cover spatial boundary preservation,
tiny-document caps, Draw-heavy's mandatory region floor, cancellation and ten
hostile role/order/completion/provenance attacks. A real GPU readback checks
packed region offsets/bytes and fence-incomplete publication. The eviction test
checks exact parent visibility and retirement behind the last direct fence.

## Results and qualification limits

Final reports, fixture manifests and executable/source/harness hashes are frozen
in `tests/fixtures/baselines/tsk-206/`. The inventory records the local machine;
the reports retain raw timings, private commit and queued bytes.

Debug/Release solution builds pass. Unit suites pass 82 cases each, with 7,291
Debug / 7,203 Release assertions. ImportIsolation passes 189 cases / 51,433
assertions in each configuration. The explicit reordered qualification adds
862 assertions / one case per configuration. The corrected mixed-phase hostile
fixture also passes its targeted Debug rerun: 20 assertions / one case.

Both coarse app runs preserve the previous document on cancellation and a
truncated final PLY list. They hand off all six mesh/point formats plus
Draw-heavy only when the coarse set is usable, verify full source counts/bounds,
complete fine suppression without duplicate draws, and restore every coarse
parent after real-app fine eviction. Packed coarse allocation is 64–512 KiB
for these fixtures, independently of the much larger full set. Peak queued
payloads are 15,039,728 bytes Debug / 15,039,512 Release; peak viewer private
commit is about 587 / 258 MiB and worker private commit about 27 / 14 MiB.
Each exits normally with zero surviving workers and zero D3D12 errors.
Progressive (four modes), textures, recovery and one point-required lifecycle
run also pass in each configuration. Debug texture validation is available and
reports zero errors. A transient clipboard-open failure required rerunning the
unchanged Debug recovery harness; the rerun passes all cases.

Both bounded app runs complete two-million-triangle GLB/STL and both-endian
PLY meshes, plus eight-million-point PLY files. Initial preview/handoff remains
Loading, final source counts/bounds are correct and UI geometry arrays stay
empty. Release also cancels the four multi-GiB GLB/STL/PLY mesh LE/PLY points BE
replacements after validated scans while preserving the earlier Ready model.
Peak queued bytes are 66,847,570 Debug / 66,847,418 Release. Measured viewer
growth plus peak worker private commit is about 681 / 520 MiB, below 1.5 GiB.
Both exit with code zero and no surviving workers. These sampled private-commit
values are independent of virtually mapped source address space, and do not
establish resident-RAM or live/pending/retired GPU-budget qualification.

The six explicit multi-GiB preview checks cover 60 million source triangles or
points each and pass 141 assertions. They cancel immediately after the first
representative publication, before full normalization:

| Source | Preview primitives | Normalized bytes | Local first publication ms |
| --- | ---: | ---: | ---: |
| GLB | 64 | 6,912 | 31 |
| STL | 8 | 864 | 16 |
| PLY mesh LE | 8 | 864 | 1,250 |
| PLY mesh BE | 8 | 864 | 1,250 |
| PLY points LE | 24 | 288 | 16 |
| PLY points BE | 24 | 288 | 15 |

These are single-run broker publication measurements on this workstation with
uncontrolled caches, not useful on-screen presentation or reference-system p95.
The app's coarse smoke intentionally delays real copy execution by 750 ms per
publication. Those timings establish handoff ordering and fence behavior, not
the 500 ms / 2 s / 5 s performance gates. Full multi-GiB coarse completion,
reference/UMA performance, frame/input/startup targets, budget pressure and
budget-restored detail requests remain unqualified. TSK-207 adds live detail
admission, automatic eviction and worker re-requests; TSK-208 retains complete
material and point-splat rendering. No dependency/license changes were added.

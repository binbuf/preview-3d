# STEP-004 verification: bounded tessellation, LOD shape, and progressive CAD delivery

Status: **complete for the single-pass display slice; two-pass coarse catalog
remains delegated to STEP-005**
Completed: 2026-09-19
Design authority: [stp2.md](stp2.md) (supersedes [stp.md](stp.md) from STEP-004
onward); [design/README.md](design/README.md)
Spike basis: [STEP-001-SPIKE-RESULTS.md](STEP-001-SPIKE-RESULTS.md)
Host basis: [STEP-002-VERIFICATION.md](STEP-002-VERIFICATION.md)
Scene basis: [STEP-003-VERIFICATION.md](STEP-003-VERIFICATION.md)

## Result

`StepXdeAdapter` now turns accepted B-rep/tessellated definitions into bounded,
deterministic rendering chunks through a versioned quality profile, writes them
with cluster-local float positions and double per-chunk origins, and delivers
them progressively through the existing `ChunkBatchReady`/`ChunkBatchConsumed`
window-reuse protocol instead of building the whole normalized scene in one
shared section. A new typed `ImportErrorCode::TessellationFailed` (28) carries
CAD-specific meshing/budget failures to the viewer with fixed host-owned text.

The route remains private to the broker with `ImportFormat::Step`. No public
picker, drag/drop, Open With, installer, or thumbnail path recognizes
`.step`/`.stp` yet.

## Versioned profile and importer identity

`compatibility-host-step/src/StepTessellationProfile.h` (v2) is the single
source of truth for the release-quality policy. It deliberately links no OCCT
so the arithmetic is unit-testable without the CAD kernel:

- relative coarse/display linear deflection, clamped into an absolute window;
- display/coarse angular deflection;
- relative minimum edge length with an absolute floor;
- per-definition face, edge, triangle, and elapsed-time budgets;
- output cluster cap (`chunkTriangles`);
- an explicit, truthful thread-policy member.

`StepImporterVersion.h` folds the profile version into
`kStepImporterVersion = 1 + kStepTessellationProfileVersion` so a cache entry
produced under a different policy can never be reused.

**Thread-policy correction (stp2.md STEP-004 work item 1).** The pinned
constrained OCCT port builds with `USE_TBB=OFF`; `BRepMesh_IncrementalMesh`
therefore has no parallel `OSD_Parallel` backend and meshing runs serially. The
profile now records `parallel = false` and says so, rather than implying a
parallel path that does not exist. Product-level per-definition parallelism (or
a documented serial-throughput limit) is STEP-005 work.

## Adapter changes

Two phases, so no normalized bytes are produced until the accepted graph and
generation-wide metadata are known:

- **Phase A (`ScenePlanner`)** walks the XDE document without meshing. It
  builds the node tree, reusable `PlannedDefinition`s, resolved materials, and
  per-definition occurrence lists (node id, double world transform, optional
  instance-color override). It enforces cycles, hierarchy depth, finite
  transforms/locations, and the node/definition/material/subshape caps exactly
  as STEP-003 did.
- **Phase B (`SceneEmitter` + `DefinitionMesher`)** tessellates one reusable
  definition at a time with `BRepMesh_IncrementalMesh` under the profile,
  extracts per-face triangles respecting face location and `TopAbs_REVERSED`
  orientation, splits at material seams and the per-chunk triangle cap, and
  writes each chunk immediately.

Key properties:

- **Cluster-local positions / double origins.** Each geometry chunk computes the
  componentwise minimum of its positions as a double `origin`, stores positions
  as cluster-local floats, and reports exact float `localMin`/`localMax`. The
  broker's independent `TransformGeometryBounds` recomputation and the
  adapter's own instance world bounds both use `origin + local`, so acceptance
  still never trusts worker arithmetic.
- **Progressive multi-window delivery.** `SceneEmitter::Add` accumulates fixed
  descriptors and owned payloads. When the next chunk would not fit
  `sectionByteCapacity`, it validates the window in place and hands it to a
  `StepBatchPublisher`; the terminal window is finalized and reported through
  the normal `ChunksReady` reply. `main.cpp` wires the publisher to the proven
  `import_worker::ChunkBatchSink` (`ChunkBatchReady` then a matching
  `ChunkBatchConsumed`), so the existing broker batch-identity / chunk-catalog /
  ordering / reservation rules apply unchanged. A scene whose single largest
  chunk exceeds the window fails as `ResourceLimit`; a scene whose total fits
  one window emits byte-identical traffic to STEP-003.
- **No whole-scene retention.** Geometry vertices are moved into the output
  window and dropped; only chunk identity and bounds are retained for occurrence
  references, so a repeated definition is never duplicated and the host never
  holds every definition's triangles at once.
- **Authored triangulation preferred.** The mesher runs with
  `AllowQualityDecrease = false`, so a transferred AP242 tessellated
  representation or any already-meshed face keeps its authored triangulation
  rather than being regenerated at lower quality. Accepted faceted data still
  passes the same finite/credible-triangle and bounds checks.
- **Budgets and typed failure.** Face/edge counts are bounded before extraction,
  per-definition triangle and elapsed-time budgets are enforced during
  extraction, cancellation is polled between bounded face blocks, and any
  breach returns `TessellationFailed` rather than publishing a partial
  assembly.

`StepHostImport` and `main.cpp` now forward the publisher; when it is empty the
pre-STEP-004 single-window behavior is preserved.

## New typed error

| Identity | Value | Meaning |
| --- | ---: | --- |
| `model_core::ImportErrorCode::TessellationFailed` | 28 | A definition could not be turned into bounded, finite rendering geometry: meshing failure/timeout, a CAD face/edge/triangle budget, or no credible triangles. |

`IsKnownImportErrorCode` now ends at 28. `D3D12ImportBridge` maps it to fixed
viewer text ("could not be tessellated ... exceeded the supported CAD
tessellation budget"). The broker's `MapStepFailure` preserves it (it is not
part of the resource family) so generic kernel text still never crosses.

## Verification

`x64/<Config>/Tests.ImportIsolation.exe "[step-002],[step-003],[step-004]"`
passes **26 cases / 423 assertions in both Debug and Release**, including the
unchanged STEP-003 golden counts. The new `[step-004]` slice adds 4 cases:

```text
[step-004][profile]      version ties importer identity to the profile; relative
                         deflection scales then clamps; min-edge floor; time budget
[step-004][progressive]  a 24 KiB window forces >=2 windows over the nested
                         assembly while every chunk id stays unique across the
                         generation and the golden geometry/node/instance counts hold
[step-004][origin]       every geometry chunk has finite origin, localMin == 0 on
                         every axis, non-negative localMax, and Verified bounds
[step-004][determinism]  repeated progressive imports match batch count, chunk
                         ids, byte sizes, checksums, origins, and local bounds
```

`[step-002]` (13 cases) and `[step-003]` (9 cases) remain green in both
configurations. The full `Tests.ImportIsolation` suite retains the pre-existing
USD/FBX failures documented for earlier tasks; none are STEP-related.

## Known limits and STEP-005 handoff

- **Two-pass coarse catalog is not implemented.** stp2.md STEP-004 work item 3
  asks for a complete coarse catalog published first and replaced by display
  detail. The current Tier-B broker deliberately computes
  `coarseProtocol = enableCoarseProxy && !tierBFormat`, and `D3D12ImportBridge`
  leaves `enableCoarseProxy` and `nextDetail` unset for STEP, so a coarse
  `CoarseComplete` record is rejected and `kCoarseLod` chunks would render
  alongside (not replace) fine geometry. The clean region-based replacement
  needs the scan/detail protocol enabled for STEP, which is a cross-cutting
  broker/bridge change. stp2.md STEP-005 work item 5 explicitly owns the
  two-pass-versus-single-pass decision; STEP-004 delivers the single-pass
  progressive display shape it will compare against.
- **Tessellation parallelism is serial by fact, not by measurement.** The
  pinned `USE_TBB=OFF` build has no parallel meshing backend; STEP-005 must
  prove product-level parallelism or record serial throughput.
- **Per-definition mesh cost is accumulated** (`StepXdeResult::meshMilliseconds`)
  but not yet surfaced to the UI or a report; STEP-005 makes it evidence.
- **Golden images and large/adversarial fixtures are not added.** The STEP-004
  golden-image baseline, trimmed-NURBS/fillet/hole classes, extreme tolerances,
  and the large-file corpus belong to STEP-005/STEP-008.
- **`FILE_POPULATION`/`DOCUMENT_FILE` remains `UnsupportedRequiredFeature`**
  per the STEP-001 external-reference no-go.
# STEP-005 verification: render-time performance and first-frame latency

Status: **implemented slice; large-file corpus, published Tier-B budgets, and
genuine 100 MB+ throughput evidence remain open for STEP-008**
Completed: 2026-09-19
Design authority: [stp2.md](stp2.md) (STEP-005); [design/README.md](design/README.md)
Basis: [STEP-004-VERIFICATION.md](STEP-004-VERIFICATION.md)

## Result

STEP-005 is the performance task that answers the Bambu Studio observation for
our own pipeline. This slice implements the parts that are concrete, bounded,
and testable against the existing corpus:

1. **Where-the-time-goes instrumentation** across the real phases: Part-21
   lexical admission, `ReadStream` parse, `Transfer`, mesh-free planning,
   per-definition `BRepMesh_IncrementalMesh`, face/triangle extraction, and
   window emission.
2. **A bounded phase/progress signal** (`StepProgress`, opcode 21) so the
   viewer can render meaningful provisional status and the broker can record
   timing evidence.
3. **A corrected tessellation-parallelism finding**: the pinned `USE_TBB=OFF`
   OCCT port *does* have a parallel backend (`OSD_Parallel`'s built-in
   `OSD_ThreadPool`), so multi-threaded meshing is enabled and proven
   byte-identical to the forced-serial path.
4. **A single mapped read** for admission plus OCCT transfer, removing the
   pre-STEP-005 double full read of the source.
5. **A versioned delivery-strategy constant** recording the
   two-pass-versus-single-pass decision.

The route remains private to the broker with `ImportFormat::Step`. No public
picker, drag/drop, Open With, installer, or thumbnail path recognizes
`.step`/`.stp` yet.

## Corrected parallelism finding (work item 3)

STEP-004 recorded `parallel = false` on the premise that `USE_TBB=OFF` leaves
`BRepMesh_IncrementalMesh::InParallel` without a backend. That premise is
wrong for OCCT 7.8:

- `OSD_Parallel::For` dispatches to `forEachOcct` /
  `OSD_ThreadPool::DefaultPool()` when `ToUseOcctThreads()` is true, and the
  header documents that this is the default "if alternative library has been
  enabled while OCCT building and TRUE otherwise".
- `IMeshTools_Parameters::InParallel` is documented as "Switches on/off
  multi-thread computation".

`StepTessellationProfile` v3 therefore sets `parallel = true` and explains the
backend. Per-face triangulation is independent and extraction order is
deterministic, so thread scheduling cannot change emitted bytes.
`kImportRequestStepForceSerialForTesting` (bit 8) is a test-only seam that
forces serial meshing; `[step-005][parallel][determinism]` imports the same
nested assembly both ways and asserts identical batch count, chunk ids, byte
sizes, and checksums. Product callers never set the seam.

## Delivery-strategy decision (work item 5)

`StepDeliveryStrategy::SinglePassProgressiveDisplay` is recorded as a versioned
product constant in `StepTessellationProfile.h`. Two-pass coarse-then-display
was evaluated and rejected for this slice:

- STEP is parse/transfer-bound: the host emits nothing until `ReadStream` and
  `Transfer` complete, so a second coarse pass cannot make first geometry
  appear sooner.
- The Tier-B broker/bridge deliberately do not enable the coarse/detail
  replacement protocol for STEP; enabling it is a cross-cutting change with no
  measured benefit over the existing progressive window stream.
- Single-pass already hands each bounded window off through
  `ChunkBatchReady`/`ChunkBatchConsumed`, so time-to-first-usable-frame is the
  time to mesh and emit the first definition, not the whole scene.

## Single mapped read (work item 4)

`RunStepHostImport` now creates one read-only file mapping from the inherited
duplicated handle (`CreateFileMappingW` + `MapViewOfFile`, no path) and maps
exactly the file size:

- `StepPreflightBytes` runs lexical admission over that view.
- `RunStepXdeAdapter` consumes the same view through a product-owned
  `MappedStreamBuf`, so `ReadStream` does not re-read the file.

If mapping fails, the proven `StepPreflightHandle` + `HandleStreamBuf` path
remains the fallback. Mapping exactly the file size matters: mapping to
end-of-file exposes the zero-filled tail of the final allocation granule, which
the lexical scanner correctly rejects as a non-Part-21 control byte.

## Bounded phase/progress signal (work item 7)

| Identity | Value | Meaning |
| --- | ---: | --- |
| `model_core::ControlOpcode::StepProgress` | 21 | STEP host -> broker, non-terminal |
| `model_core::StepProgressNotice` | 48 bytes | generation, closed phase, N-of-M definitions, lexed bytes, phase/total milliseconds |
| `model_core::kStepPhase*` | 1..6 | Preflight, Read, Transfer, Plan, Mesh, Emit |
| `model_core::kImportRequestStepForceSerialForTesting` | `1u << 8` | test-only serial-meshing seam |

The host emits one event per completed phase plus throttled per-definition mesh
events (at most ~256 regardless of definition count). The broker accepts the
message only from `ImportProducer::StepHost`, validates generation/phase/order,
bounds it by `maxStepProgressPerGeneration` (default 8192, exceeding it is
`ImportStage::StepProgressLimit` -> `ResourceLimit`), invokes the optional
`ImportSessionRequest::onStepProgress`, and records the count and last event on
`ImportSessionResult`. No OCCT text, label, or path can cross this boundary.
The UI wiring of the signal belongs to STEP-006/STEP-007.

## Phase timing evidence

`StepPhaseTimings` records wall-clock milliseconds for every phase; the host
adds the preflight phase the adapter cannot see. Measured on the committed
small STEP-003 fixtures (`[step-005-measure]`):

| Fixture | Bytes | Preflight | ReadStream | Transfer | Plan | Mesh | Emit | Total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `part_ap203.stp` | 21,168 | 0 | 1 | 3 | 0 | 3 | 3 | 10 |
| `assembly_ap214.stp` | 24,428 | 0 | 1 | 3 | 0 | 3 | 3 | 10 |
| `assembly_nested_ap214.stp` | 26,404 | 0 | 1 | 3 | 0 | 3 | 4 | 12 |

Release values (Debug totals were 58/66/64 ms, dominated by OCCT Debug
instrumentation). On these tiny fixtures transfer dominates parse and mesh; the
advisor's parse/transfer-dominates-mesh split cannot be confirmed or refuted
for large files without the genuine 100 MB+ corpus, which remains STEP-008.

## Verification

`x64/<Config>/Tests.ImportIsolation.exe "[step-002],[step-003],[step-004],[step-005]"`
passes **30 cases / 530 assertions in both Debug and Release** (STEP-004 was 26
cases / 423 assertions). New `[step-005]` cases:

```text
[step-005][profile]     profile v3 records parallel meshing; importer identity
                        folds it; delivery strategy is single-pass
[step-005][parallel]    parallel and forced-serial imports emit identical
                        batch counts, chunk ids, byte sizes, and checksums
[step-005][progress]    ordered Preflight/Read/Transfer/Plan/Mesh/Emit events,
                        lexed bytes == file size, monotonic N-of-M, terminal
                        event recorded on the result
[step-005][envelope]    entity-cap abort is early: just-over files report
                        lexed bytes well under the full input
[step-005][protocol]    opcode 21 and the 48-byte notice layout are fixed
```

The opt-in `[step-005-measure]` case prints the timing table above and is not
part of the ordinary selector.

## Known limits and handoff

- **No genuine 100 MB+ corpus and no published Tier-B STEP budgets.** The
  stp2.md exit criterion requires at least one genuine 100 MB+ assembly and one
  high-triangle fixture. The current fixtures are tens of kilobytes, so the
  Tier-B time-to-first-coarse/Ready budgets are not set here and
  `design/09-quality-performance-and-security.md` is not rewritten from
  fabricated numbers. This is the primary STEP-008 obligation.
- **No progress UI.** The signal exists on the wire and is recorded by the
  broker; presenting it in the viewer is STEP-006/STEP-007 work.
- **Coarse-catalog replacement remains single-pass by decision**, recorded as
  `kStepDeliveryStrategy`. If STEP-008 measures a real benefit from two-pass,
  that is a new versioned decision.
- **Thread-pool width is OCCT's default**, not yet bounded by the Job commit
  ceiling or measured CPU headroom; the Job Object remains the hard backstop.
  Sizing the pool from measured peak commit is STEP-008.
- **No golden images, trimmed-NURBS/fillet/hole classes, or adversarial
  fixtures** were added here; they remain STEP-008/STEP-006.

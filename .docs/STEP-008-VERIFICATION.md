# STEP-008 verification: qualification and hardening

Status: **qualification slice implemented; release qualification remains open**
Completed: 2026-09-19
Design authority: [stp2.md](stp2.md) (STEP-008); [design/README.md](design/README.md);
[design/09-quality-performance-and-security.md](design/09-quality-performance-and-security.md)
Basis: [STEP-007-VERIFICATION.md](STEP-007-VERIFICATION.md)
Checked-in corpus: [tests/fixtures/step/manifest.json](../tests/fixtures/step/manifest.json)

## Result

STEP-008 turns the STEP slice's promises into checked-in, reproducible evidence
and fixes the one hardening defect that evidence exposed:

1. **Frozen corpus.** `tests/fixtures/step/manifest.json` freezes provenance,
   byte size, SHA-256, class, and expected outcome for the 16 committed fixtures
   and 12 deterministic adversarial derivations;
   `tests/fixtures/step/verify.py` re-derives the cases in memory and checks
   every pinned hash. The genuine 230 MiB assembly is recorded with its
   size/SHA-256/provenance but is not redistributed.
2. **Runtime typed-outcome oracle.** A new
   `tests/import-isolation/StepQualificationTests.cpp` gives every malformed,
   unsupported, and over-limit admission family an asserted typed status and
   exercises each cap exactly at its boundary (just under and just over).
3. **Fuzz coverage.** `prepare_step_seeds.py` now materializes the STEP-008
   adversarial families as seeds for the existing no-GPU `StepFuzz` target; a
   fresh 45-second Release ASan smoke ran 64,569 executions with no finding.
4. **Genuine 100 MB+ measurement.** The real AppContainer/Job STEP host
   imported a 241,522,213-byte AP214 assembly (4.06 M triangles, 12.17 M
   vertices, 1,314 definitions) in 95 s, and the where-time-goes split and peak
   commit are published as a Tier-B budget.
5. **Hardening fix.** The measurement exposed that the STEP emitter flushed its
   output window only on byte capacity and ignored the broker's per-section
   `maxChunkCount`, so a large many-definition assembly was rejected as
   `ResourceLimit`/`StepHostLimit` instead of being delivered progressively. The
   emitter now flushes on the chunk-count cap as well, with a regression test.

## Work item 1: provenance and SHA-256 corpus

`tests/fixtures/step/manifest.json` has four sections:

- `entries`: the 16 committed `.stp` fixtures with `sha256`, `bytes`, `class`,
  `provenance`, and the named expected outcome that
  [STEP-006-INTEROP-MATRIX.md](STEP-006-INTEROP-MATRIX.md) already asserts.
- `derived`: 12 deterministic mutations of `part_ap214.stp` (empty,
  truncate-half, truncate-terminator, NUL append, UTF-16 BOM, ZIP signature,
  wrong physical envelope, `FILE_POPULATION`, relative `DOCUMENT_FILE`,
  duplicate entity, unterminated string, unterminated comment) with pinned
  SHA-256 and expected admission status.
- `manualCorpus`: the genuine Voron 2.4r2 assembly
  (`test-models/Voron_2.4r2_Assembly.step`, 241,522,213 bytes, SHA-256
  `06b372e40217caa1a7d1f516cb74887247f809e41e1e389f740f290e98441b55`,
  AP214 `AUTOMOTIVE_DESIGN`, self-contained). It is git-ignored and supplied
  locally; the verifier records it when present but does not require it.
- `notes`: the durability rule. The XDE writer embeds a timestamp, so the
  committed bytes are the artifact and are never regenerated implicitly.

```powershell
python tests/fixtures/step/verify.py
python tests/fixtures/step/verify.py --require-manual-corpus
```

Both pass: `verified 16 sources, 12 derived cases, 1 manual corpus item(s)`.

## Work item 2: no-GPU sanitizer coverage

`StepFuzz` remains the standalone ASan/libFuzzer target for the product-owned
admission, declaration-discovery, control-frame, and normalized-output
boundaries. STEP-008 adds the frozen adversarial families to
`tests/fuzz/prepare_step_seeds.py` so the fuzzer starts from them.

```powershell
python tests/fuzz/prepare_step_seeds.py TestResults/step-008/fuzz-seeds
tests/fuzz/x64/Release/StepFuzz.exe TestResults/step-008/fuzz-seeds `
    -max_total_time=45 -timeout=5 -rss_limit_mb=1024 -max_len=1048576 `
    -print_final_stats=1 -verbosity=0
```

Result: `64,569` executions, `1,384` new units, `456 MiB` peak RSS, no finding.

The pinned OCCT `ReadStream`/`Transfer`/tessellation boundary is a separately
built DLL and is not sanitizer-instrumented by this target, consistent with the
USD/3MF/FBX targets. That boundary is covered by the real AppContainer/Job
cases and the derived malformed families; adding an instrumented OCCT harness
remains open.

## Work item 3: typed outcomes and shared invariants

`[step-008][preflight-typed]` asserts a closed status for valid, empty,
wrong-envelope, UTF-16 BOM, ZIP, XML, embedded NUL, missing terminator,
unterminated string/comment, duplicate/zero/impossible entity id, and both
external-declaration forms. `[step-008][caps]` proves the boundary is exact for
entity records, references, nesting depth, record bytes, string bytes, total
lexed bytes, DATA sections, and external documents.

The existing shared suites are unchanged and still green: the STEP-006
interoperability matrix through the real host, the STEP protocol/hostile-host
cases, AppContainer path/network/process denial, Job/timeout/worker
replacement, cancellation, and copy-then-validate. The new
`[step-008][chunk-cap]` regression imports `assembly_nested_ap214.stp` with a
4-chunk per-section cap and asserts that the scene is delivered across more
than one bounded batch instead of one oversized section.

## Work item 4: measured budgets

`[.][step-008-measure]` runs the genuine corpus through `RunImportSession` with
the real host, records the bounded `StepProgress` phases, samples the host's
private commit at each accepted batch, and prints the delivered totals.

```powershell
$env:PREVIEW3D_MANUAL_STEP_FILE = (Resolve-Path test-models/Voron_2.4r2_Assembly.step)
x64/Release/Tests.ImportIsolation.exe "[.][step-008-measure]"
```

Measured (Release, compatibility reference):

| Phase | ms |
| --- | ---: |
| Part-21 admission | 3,156 |
| `ReadStream` parse | 12,173 |
| `Transfer` | 50,829 |
| Plan (mesh-free) | 17 |
| Mesh (1,314 definitions) | 24,339 |
| Emit (8 batches) | 25,979 |
| Host total | 89,162 |
| **Time-to-first-coarse** | **66,396** |
| **Ready (broker wall)** | **94,952** |

Delivered: 4,589 chunks in 8 batches, 4,056,614 triangles, 12,169,842
vertices, 224 bounded progress events, 2,226,372,608 bytes peak host private
commit. This confirms the STEP-005 thesis for a genuine file: the file is
parse/transfer-bound, and no coarse second pass could make geometry appear
before the OCCT transfer finishes.

The budget published in
[design/09-quality-performance-and-security.md](design/09-quality-performance-and-security.md)
for a self-contained STEP file of this class (≤256 MiB, ≤5 M triangles) is
**time-to-first-coarse ≤120 s and Ready ≤180 s**, with host private commit
inside the STEP-host Job ceiling. Multi-run p95 and a separate
~20 M-triangle-class fixture remain open.

## Work item 5: remaining release qualification

Completed here: the corpus manifest, fuzz seed expansion and smoke, Debug and
Release focused reruns, and the measured budget. Still open for STEP-008
completion, and not claimed by this slice:

- static analysis and dependency vulnerability/license review of the OCCT
  closure;
- Debug/Release reproducibility of the measured corpus and a multi-run p95;
- the ~20 M-triangle-class fixture;
- an instrumented sanitizer harness for the OCCT stream/transfer/extraction
  boundary;
- the 8-hour mixed-format soak including large STEP files;
- a clean offline standard-user VM install/repair/upgrade/uninstall run;
- signed artifact/hash/SBOM inspection and an OCCT update/rollback servicing
  rehearsal.

## Verification

```text
python tests/fixtures/step/verify.py
  verified 16 sources, 12 derived cases, 1 manual corpus item(s)

x64/Release/Tests.ImportIsolation.exe "[step-002],[step-003],[step-004],[step-005],[step-006],[step-008]"
  All tests passed (746 assertions in 38 test cases)

x64/Debug/Tests.ImportIsolation.exe   "[step-002],[step-003],[step-004],[step-005],[step-006],[step-008]"
  All tests passed (746 assertions in 38 test cases)
```

The 38 cases / 746 assertions are STEP-006's 35 cases / 706 assertions plus
STEP-008's `[preflight-typed]`, `[caps]`, and `[chunk-cap]`.

## Hardening finding

The genuine measurement first failed as `ResourceLimit`/`StepHostLimit` with no
batch delivered. The cause was in the STEP host, not the broker: `SceneEmitter`
flushed its window only when the next chunk would exceed the byte capacity, so a
scene whose geometry is small relative to its definition count accumulated more
descriptors than `request.maxChunkCount`, and the broker correctly rejected the
section. `SceneEmitter` now takes `maxChunkCount` and flushes-and-publishes
before adding a chunk that would exceed it, exactly as it already did for bytes.
The `[step-008][chunk-cap]` regression exercises the split with a 4-chunk cap,
and the genuine 230 MiB assembly now completes. No broker, protocol, or wire
change was needed; the emitter simply honors a limit it was already given.

## Known limits and handoff

- **External STEP documents stay out of scope** and healing stays off, per
  ADR-017; nothing in STEP-008 changes that.
- **No thumbnails.** Explorer STEP thumbnails remain STEP-009.
- **The budget is a measured class, not a multi-gigabyte promise.** It is backed
  by one genuine 230 MiB / 4.06 M-triangle assembly and must be re-measured for
  the ~20 M-triangle class before it is presented as a broad claim.
- **The OCCT boundary is not sanitizer-instrumented.** It is covered by the real
  host containment cases; an instrumented harness remains open.

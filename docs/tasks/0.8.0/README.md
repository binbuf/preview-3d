# 0.8.0 — valid-file coverage

This is the execution plan for the `0.8.0` task set: make Preview 3D open the
valid files it already claims to support, rather than only the happy-path
subset each importer was built against. It runs alongside the base/MVP,
`security`, and `0.7.0` sets without touching them.

The work is deliberately split into small, independently reviewable sessions.
A task should leave the build and its named verification command green, and its
hand-off should tell the next task what changed.

```powershell
./.symphony/symphony.ps1 doctor --set 0.8.0
./.symphony/symphony.ps1 run    --set 0.8.0
./.symphony/symphony.ps1 status --set 0.8.0
```

- Roadmap: [`../../0.8.0/ROADMAP.md`](../../0.8.0/ROADMAP.md)
- Task files: this directory.
- Shared design docs: [`../../design/`](../../design/)
- Progress notebook: [`../../0.8.0/PROGRESS.md`](../../0.8.0/PROGRESS.md)

## Read first

- [`docs/FORMAT-SUPPORT.md`](../../FORMAT-SUPPORT.md) — the exact supported
  subset, resource ceilings and known limitations.
- [`docs/design/03-file-formats-and-ingestion.md`](../../design/03-file-formats-and-ingestion.md)
  — the catalog, sniffer, wire contract, budgets and sandbox boundary.
- [`docs/design/01-product-scope.md`](../../design/01-product-scope.md) —
  requirements and exclusions.
- [`docs/tasks/0.7.0/README.md`](../0.7.0/README.md) — the previous task-set
  conventions and the compatibility corpus foundation this set extends.
- [`docs/tasks/mvp/TEMPLATE.md`](../mvp/TEMPLATE.md) — the task shape used here.

## Ground rules

1. **Parsers never enter `Preview3D.exe`.** Every parser or decoder runs in
   `Preview3DImportWorker.exe`, `Preview3DImportHost.exe`, or the dedicated
   CAD host, under the existing AppContainer/Job Object/broker policy. A test
   that needs a parser links the worker test binary, not the viewer.
2. **No path or network authority is added.** New importers request sidecars
   through the existing broker; they never open a path, URL, or child process.
3. **A format is not “supported” until the whole chain lands:** catalog entry,
   UI/Open With registration, worker/host adapter, preflight, typed errors,
   fixtures, fuzz target, docs, and installer. Do not claim it earlier.
4. **Wire changes are additive and validated.** New chunk topologies, material
   fields, or format IDs must update `SharedSectionValidator`, the hostile-worker
   suite, and cache/importer identity. A new public limit needs an ADR.
5. **Valid-file evidence is end-to-end.** A unit test can prove a parser
   decision, but a claim that “this file opens” must run through the real
   broker/worker or host path and record geometry kind and typed outcome.
6. **Do not conflate fidelity with openability.** Animation playback, cameras,
   lights, and NURBS are documented fidelity limits. A valid file that contains
   usable geometry should open (possibly degraded) even when those features are
   ignored; a file whose only visible geometry is unsupported may fail typed.
7. **Every task updates its hand-off.** Record what landed, deviations,
   commands run, and what the next task must know.
8. **F3D is a reference, not a dependency.** It is useful for reader metadata,
   scoring/force-reader, per-reader options, primitive breadth, and corpus
   design. Do not import its trusted-process parser/plugin architecture.
   Preserve licenses and provenance for any reused fixture.

## Security architecture invariants

Every 0.8.0 task must preserve the existing boundary. A task that cannot do so
is out of scope and needs a separate ADR.

- **Parsers stay out of `Preview3D.exe`.** No new parser, decoder, or codec is
  linked into the viewer. Assimp work stays in `Preview3DImportWorker.exe`;
  OCCT work stays in `Preview3DStepHost.exe`; OpenUSD work stays in
  `Preview3DImportHost.exe`.
- **Path authority stays in the trusted process.** The viewer classifies the
  primary path, opens it, and duplicates a read-only handle across the process
  boundary. Workers and hosts receive handles, never paths. They may request
  sidecars only through the broker by relative reference.
- **No new network, URI, child-process, or arbitrary-file authority.** The
  existing sidecar allowlist, canonical containment, size caps, and
  AppContainer/Job Object commit limits are unchanged.
- **Reader selection and options are closed product-owned values.** A reader
  name or option is never passed to a parser as free-form text. Forcing a
  reader does not bypass `ClassifySourcePath`, broker canonicalization, the
  worker/host admission, or any budget.
- **Wire changes remain validated.** Any new field or topology updates
  `SharedSectionValidator`, the hostile-worker suite, and cache/importer
  identity before it is accepted.
- **Compatibility fallback is not a more permissive retry.** It is allowed only
  for the closed compatibility-class errors in T08. It never retries
  `UnsafeReference`, `FileUnavailable`, `FileChanged`, `ResourceLimit`,
  `ArchiveLimit`, or the other containment/resource failures, and it never
  retries after a batch has been published.
- **The thumbnail provider stays its own bounded COM component.** It does not
  launch the viewer, a worker, or a host, and it does not gain a new family or
  CLSID without an ADR. Adding an alias to the existing STEP family is not a
  new parser.
- **No F3D runtime dependency or plugin loader.** F3D source is a reference
  only; the shipped architecture remains the sandboxed worker/host model.

## Phase map

| Phase | Tasks | Purpose |
| --- | --- | --- |
| 0 — baseline | T01 | Measure valid-file failures by class before changing behaviour |
| 1 — extension/detection | T02, T03 | Close extension aliases and make content sniffing catalog-driven |
| 2 — primitives | T04, T05 | Stop discarding valid Assimp line/point and CAD edge/wire geometry |
| 3 — options/selection | T06, T07, T08 | Tune per-format imports and select alternate readers |
| 4 — evidence/qualification | T09, T10 | Real-world corpus, coverage report, support matrix and docs |

## Task index

| ID | Task | Depends on | Verify |
| --- | --- | --- | --- |
| [T01](T01-valid-file-coverage-baseline.md) | Valid-file coverage baseline and failure triage | — | `x64\Release\Tests.ImportIsolation.exe "[valid-file]"` |
| [T02](T02-extension-aliases-registration.md) | Extension aliases and catalog/installer drift | T01 | `x64\Release\Tests.Unit.exe "[format-aliases],[format-catalog],[format-installer]"` |
| [T03](T03-catalog-driven-sniffer.md) | Catalog-driven content sniffer | T01 | `x64\Release\Tests.Unit.exe "[format-sniffer],[format-catalog]"` |
| [T04](T04-assimp-point-line-primitives.md) | Assimp point and line primitives | T01, T03 (soft) | `x64\Release\Tests.ImportIsolation.exe "[assimp-primitives]"` |
| [T05](T05-cad-edge-wire-geometry.md) | CAD edge and wire geometry | T01 | `x64\Release\Tests.ImportIsolation.exe "[cad-wires]"` |
| [T06](T06-per-format-import-options.md) | Per-format import options and quality surface | T05 | `x64\Release\Tests.ImportIsolation.exe "[import-options]"` |
| [T07](T07-reader-registry-force-reader.md) | Reader registry, scores and force-reader | T02, T03 | `x64\Release\Tests.Unit.exe "[reader-selection]"` |
| [T08](T08-compatibility-readers-fallback.md) | Compatibility readers and bounded fallback | T04, T07 | `x64\Release\Tests.ImportIsolation.exe "[reader-fallback]"` |
| [T09](T09-real-world-coverage-corpus.md) | Real-world per-format corpus and coverage report | T01–T08 | `x64\Release\Tests.ImportIsolation.exe "[format-coverage]"` |
| [T10](T10-support-matrix-qualification.md) | Support matrix, docs and 0.8.0 qualification | T01–T09 | `x64\Release\Tests.ImportIsolation.exe "[compat],[valid-file],[format-coverage]"` |

## Import options (T06)

`--import-option <FORMAT>.<name>=<value>` is repeatable; format and name are
ASCII case-insensitive. The same record is validated by the viewer, broker,
worker and host, and every output-changing value is part of the import cache
identity. Unknown names, non-finite/out-of-range values, and options that do
not apply to the selected format fail typed before a parser runs.

| Format(s) | Option | Default | Range/values |
| --- | --- | --- | --- |
| STEP, IGES, BREP, XBF, FCStd | `quality` | `balanced` | `draft`, `balanced`, `full` |
| STEP, IGES, BREP, XBF, FCStd | `read_wire` | fallback | `0`/`1`; absent = edges only when no faces |
| STEP, IGES, BREP, XBF, FCStd | `linear_deflection` | quality profile | finite `> 0`; `<= 0.5` relative or `<= 1e6` absolute |
| STEP, IGES, BREP, XBF, FCStd | `angular_deflection` | quality profile | finite `> 0` and `<= pi` radians |
| STEP, IGES, BREP, XBF, FCStd | `relative_deflection` | `true` | `0`/`1` |
| STEP, IGES, BREP, XBF, FCStd | `unit_scale` | format resolution | finite `> 0` and `<= 1e9` metres/unit |
| USD | `subdivision_level` | `0` | `0..4`; `> 0` rejected as unsupported in this build |

`read_wire=1` adds edges alongside faces; `read_wire=0` suppresses both the
T05 edge-only fallback and the overlay. `unit_scale` still runs the format's
authored-unit checks and records the override as a compatibility diagnostic.
The UI exposes the same surface under **Settings → Import options…**.

## Reader selection (T07)

`shared/model-core/include/model_core/ReaderCatalog.h` is the compile-time
table of readers this build ships. `--list-readers` prints
`name<TAB>extensions<TAB>importer<TAB>score` in stable name order;
`--list-formats` remains the family view. `--reader <name>` is ASCII,
case-insensitive and bounded to 64 characters; unknown or unshipped names fail
typed before any open, and `--reader` wins over `--format` when both are given.

| Reader | Extensions | Importer | Score |
| --- | --- | --- | --- |
| `3mf` | `3mf` | `3mf-worker` | 90 |
| `3mf-assimp` | `3mf` | `assimp-compat` | 35 |
| `assimp` | `dae,3ds,dxf,x,off,amf,x3d` | `assimp-compat` | 30 |
| `cad` | `igs,iges,brep,xbf,fcstd` | `cad-host` | 100 |
| `fbx` | `fbx` | `fbx-worker` | 90 |
| `fbx-assimp` | `fbx` | `assimp-compat` | 35 |
| `gltf` | `gltf,glb` | `gltf-worker` | 100 |
| `gltf-assimp` | `gltf,glb` | `assimp-compat` | 35 |
| `obj` | `obj` | `obj-worker` | 50 |
| `obj-assimp` | `obj` | `assimp-compat` | 35 |
| `ply` | `ply` | `ply-worker` | 70 |
| `step` | `step,stp,stpnc,p21,210` | `step-host` | 100 |
| `stl` | `stl` | `stl-worker` | 60 |
| `usd` | `usd,usda,usdc,usdz` | `usd-fast` | 95 |
| `usd-compat` | `usd,usda,usdc,usdz` | `usd-compat-host` | 40 |

The default reader for a family is the highest score, with ties by stable name;
a compatibility reader always scores below its native counterpart. A forced
reader replaces candidate selection but not the worker/host parse: the trusted
viewer still classifies the path, the broker still canonicalizes and duplicates
the handle, and the worker/host still applies admission, containment and every
budget. The reader name never crosses the process boundary. Open As lists the
default reader (marked) and shipped alternates for each family; selecting the
default clears the forced-reader state. T08 adds the same-family compatibility
readers and the closed retry policy on top of this selection foundation; it
does not add a new parser here.

## Compatibility readers and fallback (T08)

`gltf-assimp`, `obj-assimp`, `fbx-assimp` and `3mf-assimp` are lower-scored
readers that use the existing bounded Assimp worker adapter. They can be forced
explicitly with `--reader`/Open As, or selected by the closed broker fallback
after a default native open fails with `UnsupportedFormat`,
`UnsupportedRequiredFeature`, or `EmptyGeometry`. The retry is at most one per
generation, only before any batch is published, and runs in the same sandbox
with the same budgets and sidecar policy. `UnsafeReference`, `FileUnavailable`,
`FileChanged`, `ResourceLimit`, `ArchiveLimit`, `PrimarySourceLimit`,
`AggregateSourceLimit`, `ScratchLimit`, `Cancelled`, `WorkerCrashed`,
`WorkerTimedOut`, and `ImportProtocolViolation` never retry. A forced native
reader does not switch silently; the failure card offers **Try compatibility
reader**. The diagnostic names the native reader, the compatibility reader and
the final reader. See
[ADR-0102](../../design/adr/0102-compatibility-reader-fallback.md).

## Execution notes

- T01 is the measurement gate. Do not fix importer behaviour in it; later tasks
  flip the expected outcome from `failed` to `opened` or `degraded`.
- T02 and T03 are independent after T01 and can run in parallel.
- T04 and T05 are independent after T01 and can run in parallel.
- T06 depends on T05 because it exposes the `read_wire` behaviour T05 adds.
- T07 adds the reader table, `--list-readers`/`--reader` and Open As
  alternates; T08 adds new compatibility readers and the bounded retry policy.
- T09 should run after all behaviour changes so the coverage report is stable.
- T10 is the final qualification and documentation gate.

## Definition of done for the series

- Every catalog extension has at least one real-world valid fixture from two
  exporters and one edge/negative fixture.
- Line-only, point-only, and wireframe-only fixtures open with visible geometry
  instead of `EmptyGeometry`.
- STEP files named `.stpnc`, `.p21`, or `.210` open through the same STEP host.
- Renamed files and cross-family mismatches are detected from content, not only
  from the suffix.
- Users can force a reader and see the actual build-specific reader list.
- Per-format import options are documented, validated, and included in cache
  identity.
- The support matrix, README, installer, SBOM, and design docs agree with the
  shipped behaviour.
- No parser runs in the trusted viewer, and no path/network authority was added.

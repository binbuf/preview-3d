# 0.7.0 — format breadth and rendering quality

This is the execution plan derived from [`docs/proposal.md`](../../proposal.md).
It is deliberately split into small, independently reviewable sessions: a task
should leave the build and its named verification command green, and its
hand-off should tell the next task what changed.

The original 20 tasks completed at T19. T20–T35 extend this same set with
quality work in **Studio, Clay and Directional**. Read the
[post-qualification investigation and shared acceptance contract](../../0.7.0/RENDER-QUALITY-FOLLOWUP.md)
for measured observations, source findings, reference versions and the
implementation order. This update is a plan; the new renderer fixes and
verification runner have not yet been implemented.

This plan is registered as the `0.7.0` symphony task set; run it without
disturbing the base/MVP or `security` sets:

```powershell
./.symphony/symphony.ps1 doctor --set 0.7.0
./.symphony/symphony.ps1 run    --set 0.7.0
./.symphony/symphony.ps1 status --set 0.7.0
```

The harness roadmap is [`docs/0.7.0/ROADMAP.md`](../../0.7.0/ROADMAP.md); the
progress notebook and run logs live beside it.

## Read first

- [`docs/proposal.md`](../../proposal.md) — the study and ranked rationale.
- [`docs/design/01-product-scope.md`](../../design/01-product-scope.md) —
  requirements and exclusions that several tasks amend.
- [`docs/design/03-file-formats-and-ingestion.md`](../../design/03-file-formats-and-ingestion.md)
  — the normalized scene, budgets and wire contract.
- [`docs/design/04-rendering-and-streaming.md`](../../design/04-rendering-and-streaming.md)
  — the D3D12 ownership/streaming model the Studio tasks must preserve.
- [`docs/tasks/mvp/TEMPLATE.md`](../mvp/TEMPLATE.md) — the task shape used here.

## Ground rules

1. **Parsers never enter `Preview3D.exe`.** Every new format or codec runs in
   `Preview3DImportWorker.exe`, `Preview3DImportHost.exe` or the CAD host, with
   the existing AppContainer/Job Object/broker policy. A test that needs a
   parser links the worker test binary, not the viewer.
2. **No path or network authority is added.** New importers request sidecars
   through the existing broker; they never open a path, URL or child process.
3. **A format is not “supported” until the whole chain lands:** catalog entry,
   UI/Open With registration, worker/host adapter, preflight, typed errors,
   fixtures, fuzz target, docs and installer. Do not claim it earlier.
4. **Wire changes are additive and validated.** New chunk topologies, material
   fields or format IDs must update `SharedSectionValidator`, the hostile-worker
   suite and cache/importer identity. A new public limit needs an ADR.
5. **Every task updates its hand-off.** Record what landed, deviations,
   commands run and what the next task must know.
6. **Visual quality requires actual image evidence.** T20–T35 use
   image-capable review, immutable references, local detail regions and
   motion checks. Missing baselines and self-created goldens cannot pass.
   Camera lens appearance is a mandatory nonregression case alongside Car glass.
7. **Capture only completed loads.** Visible geometry may precede a terminal
   failure; verify generation, final success/degradation and texture readiness.
   The observed microscope ResourceLimit is assigned to T23.
8. **Keep source and binary references distinct.** Local F3D source is 4.0.0;
   the inspected installed renderer is 3.5.0. Record effective presets, DPI
   and viewport pixels before comparing. Keep restricted corpus assets local.

## Phase map

| Phase | Tasks | Purpose |
| --- | --- | --- |
| 0 — baseline | T01 | Build the compatibility scoreboard before changing behaviour |
| 1 — foundation | T02, T03, T04, T06 | Catalog, content routing, Open As, compatibility reports, per-format defaults |
| 2 — breadth | T05, T07, T08, T09, T10, T10b, T11 | glTF extensions, STEP tolerance, IGES/BREP/XBF/FCStd, Assimp, 3MF compatibility, USD primitives |
| 3 — Studio | T12, T13, T14, T15, T16 | Textures, TGA/DDS/HDR/EXR, IBL/tonemap, HDR/exposure/presets, ground shadow/AO |
| 4 — depth | T17, T18 | Lines/strips/fans, material lobes and transparency |
| 5 — qualify | T19 | Run the corpus, close docs/SBOM/installer, record evidence |
| 6 — evidence | T20, T21 | Reproducible comparisons, complete-load capture and diagnostics |
| 7 — inputs/budgets | T22, T23 | Authored material semantics and bounded, complete textured loads |
| 8 — shading | T24, T25 | Correct BRDF/IBL/coat layering and lobe textures |
| 9 — glass | T26, T27 | Bounded OIT, independent coverage and scene-based transmission |
| 10 — modes | T28, T29, T30 | Calibrate Studio, coherent Clay override and Directional sun |
| 11 — depth/sampling | T31, T32, T33 | Geometry shadows, SSAO and edge/specular/texture sampling |
| 12 — qualify again | T34, T35 | Complete cross-format matrix and independent release gate |

T02 and T04 are foundations; do not start dependent tasks until they land. T01
can run in parallel with T02. T14/T15/T16 touch graphics and can run in parallel
with the import-side breadth tasks after T02 is stable.

## Task index

| ID | Task | Depends on | Verify |
| --- | --- | --- | --- |
| [T01](T01-compatibility-corpus.md) | Compatibility corpus and baseline scoreboard | — | `x64\Release\Tests.ImportIsolation.exe "[compat]"` |
| [T02](T02-format-catalog-router.md) | Format catalog and content-aware router | T01 (soft) | `x64\Release\Tests.Unit.exe "[format-catalog]"` |
| [T03](T03-registration-filters-open-as.md) | Catalog-driven registration, filters and Open As | T02 | `x64\Release\Tests.Unit.exe "[open-as],[format-installer]"` |
| [T04](T04-compatibility-report.md) | Compatibility severity model and report chunk | T02 | `x64\Release\Tests.Unit.exe "[compatibility]"` |
| [T05](T05-gltf-extension-compatibility.md) | glTF required-extension compatibility | T04 | `x64\Release\Tests.ImportIsolation.exe "[gltf-extensions]"` |
| [T06](T06-presentation-defaults-orientation.md) | Per-format presentation defaults and orientation | T02 | `x64\Release\Tests.Unit.exe "[presentation]"` |
| [T07](T07-step-tolerance-import-quality.md) | STEP tolerance and import quality | T04, T06 (soft) | `x64\Release\Tests.ImportIsolation.exe "[step-tolerance]"` |
| [T08](T08-cad-host-iges-brep.md) | OCCT host: IGES and BREP | T02, T07 (soft; shared host conventions) | `x64\Release\Tests.ImportIsolation.exe "[cad-iges],[cad-brep]"` |
| [T09](T09-cad-host-xbf-fcstd.md) | OCCT host: XBF and FCStd | T08 | `x64\Release\Tests.ImportIsolation.exe "[cad-xbf],[cad-fcstd]"` |
| [T10](T10-assimp-compatibility-adapter.md) | Bounded Assimp compatibility adapter | T02, T04 | `x64\Release\Tests.ImportIsolation.exe "[assimp]"` |
| [T10b](T10b-three-mf-compatibility.md) | 3MF best-effort compatibility | T04 | `x64\Release\Tests.ImportIsolation.exe "[3mf-compat]"` |
| [T11](T11-usd-primitives-points-creases.md) | USD primitives, points and crease tolerance | T04 | `x64\Release\Tests.ImportIsolation.exe "[usd-primitives]"` |
| [T12](T12-textures-tga-dds-ao.md) | TGA/DDS textures and ORM ambient occlusion | T04 (soft; warnings) | `x64\Release\Tests.ImportIsolation.exe "[texture-tga],[texture-dds]"` |
| [T13](T13-textures-hdr-exr-environment.md) | HDR/EXR textures and environment assets | T12 | `x64\Release\Tests.ImportIsolation.exe "[texture-hdr],[texture-exr]"` |
| [T14](T14-studio-ibl-neutral-tonemap.md) | Studio IBL and neutral tone map (first slice) | T02 (soft) | `x64\Release\Tests.Unit.exe "[studio-ibl]"` |
| [T15](T15-hdr-target-exposure-presets.md) | HDR target, exposure, background and presets | T14 | `x64\Release\Tests.Unit.exe "[tonemap],[studio-presets]"` |
| [T16](T16-ground-shadow-ao.md) | Ground contact shadow and ambient occlusion | T14, T15 (soft) | `x64\Release\Tests.Unit.exe "[ground-shadow]"` |
| [T17](T17-line-strip-fan-point-topology.md) | Line/strip/fan/point topology | T02 | `x64\Release\Tests.ImportIsolation.exe "[topology]"` |
| [T18](T18-material-lobes-transparency.md) | Material lobes and transparency | T15 | `x64\Release\Tests.Unit.exe "[material-lobes]"` |
| [T19](T19-qualification-docs.md) | 0.7.0 compatibility qualification and docs | All | `x64\Release\Tests.ImportIsolation.exe "[compat]"` |

### Rendering-quality follow-up tasks

Execute these tasks in order after T19. T20 **creates** the new verification
runner; every later task implements its profile. The commands below are
planned interfaces and have not run at planning time. Their profiles must
include meaningful unit/import checks **and** actual GPU images, failing
on missing evidence or unimplemented/empty lanes.

| ID | Task | Depends on | Verify |
| --- | --- | --- | --- |
| [T20](T20-visual-comparison-harness.md) | Deterministic visual comparison harness and observed baseline | T19 | `python tests/render-quality/verify.py --task T20 --configuration Debug --output TestResults/render-quality/T20` |
| [T21](T21-render-diagnostics.md) | Material and light-contribution diagnostics | T20 | `python tests/render-quality/verify.py --task T21 --configuration Debug --output TestResults/render-quality/T21` |
| [T22](T22-material-semantics.md) | Preserve authored material, UV, normal and AO semantics | T20, T21 | `python tests/render-quality/verify.py --task T22 --configuration Debug --output TestResults/render-quality/T22` |
| [T23](T23-texture-budgets-load-readiness.md) | Texture-budget accounting and complete-load readiness | T20, T21, T22 | `python tests/render-quality/verify.py --task T23 --configuration Debug --output TestResults/render-quality/T23` |
| [T24](T24-brdf-ibl-layering.md) | Correct BRDF, environment integration and clearcoat layering | T20, T21, T22, T23 | `python tests/render-quality/verify.py --task T24 --configuration Debug --output TestResults/render-quality/T24` |
| [T25](T25-textured-material-lobes.md) | Texture-driven clearcoat, specular, sheen and transmission inputs | T22, T24 | `python tests/render-quality/verify.py --task T25 --configuration Debug --output TestResults/render-quality/T25` |
| [T26](T26-layered-transparency.md) | Bounded order-independent transparency | T20, T22, T24, T25 | `python tests/render-quality/verify.py --task T26 --configuration Debug --output TestResults/render-quality/T26` |
| [T27](T27-scene-transmission.md) | Scene-based transmission and tinted or rough glass | T22, T24, T25, T26 | `python tests/render-quality/verify.py --task T27 --configuration Debug --output TestResults/render-quality/T27` |
| [T28](T28-studio-lighting-quality.md) | Calibrate Studio environment and first-open presentation | T24, T25, T26, T27 | `python tests/render-quality/verify.py --task T28 --configuration Debug --output TestResults/render-quality/T28` |
| [T29](T29-clay-lighting-quality.md) | Clay as a coherent opaque geometry-review mode | T22, T24, T26, T28 | `python tests/render-quality/verify.py --task T29 --configuration Debug --output TestResults/render-quality/T29` |
| [T30](T30-directional-lighting-quality.md) | Directional lighting, reflection and sun consistency | T24, T27, T28, T29 | `python tests/render-quality/verify.py --task T30 --configuration Debug --output TestResults/render-quality/T30` |
| [T31](T31-geometry-ground-shadows.md) | Geometry-derived ground and contact shadows | T26, T27, T28, T29, T30 | `python tests/render-quality/verify.py --task T31 --configuration Debug --output TestResults/render-quality/T31` |
| [T32](T32-screen-space-ambient-occlusion.md) | Scale-aware screen-space ambient occlusion | T24, T26, T27, T31 | `python tests/render-quality/verify.py --task T32 --configuration Debug --output TestResults/render-quality/T32` |
| [T33](T33-antialiasing-texture-filtering.md) | Edge, specular and texture sampling quality | T24, T25, T26, T27, T31, T32 | `python tests/render-quality/verify.py --task T33 --configuration Debug --output TestResults/render-quality/T33` |
| [T34](T34-full-corpus-render-regressions.md) | Complete corpus, cross-format and hostile-input render qualification | T20–T33 | `python tests/render-quality/verify.py --task T34 --configuration Debug --output TestResults/render-quality/T34` |
| [T35](T35-render-quality-release-gate.md) | Independent visual release gate and final 0.7.0 evidence | T20–T34 | `python tests/render-quality/verify.py --task T35 --configuration Debug --output TestResults/render-quality/T35` |

## Status

The original 20 tasks are complete. The 16 tasks T20–T35 are pending, so the
extended set is **20/36 complete**. T19's historical evidence is recorded in
[`docs/0.7.0-QUALIFICATION.md`](../../0.7.0-QUALIFICATION.md); the corpus
report is `artifacts/compat/Release.json`, and the app-smoke/visual reports are
under `TestResults/qualification/`.

| Task | Status | Evidence |
| --- | --- | --- |
| T01–T18 | done | Per-task hand-offs and `docs/0.7.0/progress/` |
| T19 | done | `[compat]` 312 assertions pass; unit 465/465; import-isolation 506 cases (501 pass, 5 typed skips); visual and app-smoke lanes recorded; installer/SBOM/dependency checks pass |
| T20–T35 | pending | [Follow-up investigation](../../0.7.0/RENDER-QUALITY-FOLLOWUP.md); per-task implementation and visual evidence still required |

Use the scoped Catch2 filters above during the task. Before declaring the
series done, run the unfiltered unit and import-isolation suites and record any
pre-existing failures separately.

## Definition of done for the series

- The T01 corpus reports no unexplained failures; every failure is typed and
  classified.
- A valid file with a wrong or missing extension can be opened through sniffing
  or **Open As**.
- glTF/3MF/USD/STEP files that previously failed only because of an optional
  visual feature or a host-supported primitive open with a specific warning.
- IGES, BREP, XBF, FCStd, DAE, 3DS, OFF and AMF have real fixtures, installer
  entries and fuzz coverage.
- Studio has a real environment, neutral tone mapping, exposure, ground contact
  and AO; metals, glass, whites and CAD parts have goldens.
- Studio, Clay and Directional each pass the completed real-model matrix and
  image-quality rubric; Car glass improves while Camera lens appearance is
  preserved. No severe ordering, visibility, normal, exposure or aliasing
  regression remains hidden by an aggregate image metric.
- Ordinary verification never accepts its own reference images, never counts
  missing evidence as success, and rejects late-failed/partial-load captures.
- `docs/FORMAT-SUPPORT.md`, `README.md`, the installer, the SBOM/license file and
  the design docs agree with the shipped behaviour.
- No new parser runs in the trusted viewer, and no path/network authority was
  added.

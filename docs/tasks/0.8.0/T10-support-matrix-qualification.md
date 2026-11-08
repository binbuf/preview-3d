---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[compat],[valid-file],[format-coverage]"
---
# T10 — Support matrix, docs and 0.8.0 qualification

## Goal
Close the series with evidence and honest documentation. The support matrix
must say what each format can actually do (open, thumbnail, animation, cameras,
lights, export) rather than implying full format conformance. README, design
docs, installer, SBOM/license notes, and the shipped behavior must agree, and
the full valid-file/compatibility/coverage lanes must pass.

## Context (read first)
- `docs/FORMAT-SUPPORT.md` — the current support matrix and limitations.
- `README.md` — the high-level format claims and known gaps.
- `docs/design/01-product-scope.md`,
  `docs/design/03-file-formats-and-ingestion.md`,
  `docs/design/05-thumbnail-provider.md` — the contracts this task reconciles.
- `docs/tasks/0.8.0/README.md` — the series definition of done.
- `packaging/installer/Preview3D.nsi`,
  `thumbnail-provider/FamilyRouting.h`, `THIRD-PARTY-LICENSES.md` — the
  registration and license surfaces that must match the catalog.
- F3D reference: `doc/user/02-SUPPORTED_FORMATS.md` uses explicit columns for
  full scene, animation, streaming, plugin, and reader name. Use that
  multi-dimensional honesty, not its runtime.

## Scope
- [ ] Rewrite the support matrix with explicit columns:
  `Open`, `Thumbnail`, `Animation`, `Cameras/lights`, `Export`, `Reader`, and
  `Known limits`. Do not use a single “supported” checkmark for a format whose
  parser only covers a subset.
- [ ] Record the 0.8.0 changes:
  - STEP aliases `.stpnc`, `.p21`, `.210`;
  - catalog-driven sniffing and the formats now detected by content;
  - Assimp line/point primitives;
  - CAD edge/wire fallback and `read_wire`;
  - per-format import options;
  - reader list/force-reader and compatibility readers.
- [ ] Reconcile the README format list, `docs/FORMAT-SUPPORT.md`, the design
  docs, the installer registration, the thumbnail table, and the license/SBOM
  notes. Remove or qualify any claim that the shipped adapters cannot meet.
- [ ] Add a short “why a valid file may still fail” section that distinguishes:
  unsupported visible geometry, a fidelity-only feature (animation/cameras/
  lights), a resource ceiling, a containment rejection, and a malformed file.
- [ ] Run and record:
  - `x64\Release\Tests.ImportIsolation.exe "[compat]"`
  - `x64\Release\Tests.ImportIsolation.exe "[valid-file]"`
  - `x64\Release\Tests.ImportIsolation.exe "[format-coverage]"`
  - the unfiltered Release and Debug unit and import-isolation suites, with
    any pre-existing failures recorded separately.
- [ ] Update the roadmap/progress status and write the series hand-off:
  what landed, what remains, and which failures are accepted/known.
- [ ] Verify no parser runs in `Preview3D.exe` and no path/network authority
  was added by any 0.8.0 task.

## Out of scope
- New format breadth (VRML/IFC/LAS/Alembic/VDB) — deferred.
- Thumbnail-provider expansion beyond the frozen eight families — needs a new
  ADR and provider adapters.
- Tier B progressive/streaming — deferred.
- Render-quality image qualification; 0.7.0 owns that lane.

## Design notes
- “Open” means geometry is published through the real broker/worker/host path,
  possibly with a compatibility warning. It does not mean full-fidelity import.
- “Thumbnail” is the installed provider’s actual family scope, not a promise
  that the viewer can render it.
- “Animation” and “Cameras/lights” are fidelity columns. A static start pose
  is not animation playback; an ignored camera is not a failed open.
- Keep the docs generated or drift-checked where possible. A hardcoded list
  that can disagree with the catalog is a bug.
- Do not turn a known failure into a success by changing the expected outcome
  without an owning task and evidence.

## Done when
- [ ] The three scoped lanes above pass in Release.
- [ ] The unfiltered Release and Debug unit/import suites pass, or every
  pre-existing failure is recorded with evidence.
- [ ] The support matrix has explicit Open/Thumbnail/Animation/Cameras/
  lights/Export/Reader columns.
- [ ] README, FORMAT-SUPPORT, design docs, installer, thumbnail table, and
  license/SBOM notes agree with the shipped behavior.
- [ ] The hand-off lists accepted limitations and follow-up tasks.
- [ ] No parser entered the trusted viewer; no path/network authority was
  added.

## Hand-off
_(filled in by the implementing session: final matrix, commands/results, accepted limitations, and follow-ups)_

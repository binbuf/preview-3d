---
timeoutMin: 180
verify: x64\Release\Tests.Unit.exe "[format-aliases],[format-catalog],[format-installer]"
---
# T02 — Extension aliases and catalog/installer drift

## Goal
Every valid suffix a supported format actually uses is recognized by the
catalog, the Open dialog, the command line, the installer, and the thumbnail
registration. The immediate gap is STEP: `.stpnc`, `.p21`, and `.210` are valid
STEP interchange names, but the catalog only owns `.step` and `.stp`.

## Context (read first)
- `shared/model-core/include/model_core/FormatCatalog.h` — `kStepExtensions`,
  the descriptor table, and the stable-name/extension lookups.
- `shared/model-core/include/model_core/FormatSelection.h` —
  `DirectOpenExtensions`, `ImportableFormatDescriptors`, `ProgIdForFamily`.
- `interactive-viewer/src/app/OpenOptions.h` and `Preview3D.cpp` — the
  catalog-driven Open As and Open dialog surfaces.
- `packaging/installer/Preview3D.nsi` — the hardcoded `RegisterExtension` /
  `UnregisterExtension` lists that must match the catalog.
- `thumbnail-provider/FamilyRouting.h` — the frozen family-to-extension table;
  STEP is one of the eight supported thumbnail families.
- `tests/unit/FormatRegistrationTests.cpp` — the existing installer drift and
  Open As tests.
- F3D reference: `plugins/occt/CMakeLists.txt:85-95` declares STEP as
  `.stp`, `.step`, `.stpnc`, `.p21`, `.210`; `doc/user/02-SUPPORTED_FORMATS.md`
  documents the same list.

## Scope
- [ ] Add `stpnc`, `p21`, and `210` to `kStepExtensions` in the catalog.
- [ ] Audit every catalog family against `docs/FORMAT-SUPPORT.md`, the README,
  and the installer. Add any other suffix the shipped adapter actually accepts;
  do not add suffixes the parser cannot validate.
- [ ] Add the new STEP aliases to the NSIS install and uninstall lists.
- [ ] Add the new STEP aliases to `thumbnail-provider/FamilyRouting.h` if the
  STEP thumbnail adapter accepts them. Do not add a new family or CLSID.
- [ ] Add `[format-aliases]` cases asserting:
  - `FindByExtension` resolves each alias case-insensitively;
  - `DirectOpenExtensions` contains each alias exactly once;
  - the Open As format list includes the STEP stable name and all aliases;
  - the installer register/unregister lists match the catalog;
  - the thumbnail route contains the aliases.
- [ ] Add small valid-file entries for `.stpnc`, `.p21`, and `.210` (copies of
  a small STEP fixture are enough) and confirm they reach the STEP host.
- [ ] Update `docs/FORMAT-SUPPORT.md`, `README.md`, and the task README if the
  alias list changes.

## Out of scope
- Adding a new primary format or a new thumbnail family. The eight-family
  provider roster is frozen by ADR-0001.
- Adding suffixes for formats that have no parser (for example `.wrl`/`.vrml`
  while VRML remains a reserved `FormatId` with no descriptor).
- Guessing aliases from third-party tools; only add suffixes the adapter
  actually accepts and the tests can open.

## Design notes
- The catalog is the single authority. The installer and thumbnail tables must
  be derived from it or checked against it; do not add a second independent
  list that can drift.
- The thumbnail provider remains its own bounded in-process COM component. This
  task only adds suffixes to the existing STEP family; it does not add a new
  family, CLSID, parser, or process launch path.
- `EqualExtension` already folds case, so the catalog stores lowercase aliases
  only. `.FCStd` works through the existing case-insensitive lookup.
- Keep the STEP stable name and ProgID unchanged; aliases belong to the same
  family and must not create a new ProgID.
- The installer test reads `Preview3D.nsi`; do not invoke `makensis`.

## Done when
- [ ] `x64\Release\Tests.Unit.exe "[format-aliases],[format-catalog],[format-installer]"` passes.
- [ ] `x64\Debug\Tests.Unit.exe "[format-aliases],[format-catalog],[format-installer]"` passes.
- [ ] The `.stpnc`, `.p21`, and `.210` valid-file cases open through the STEP
  host (or are recorded as a known importer failure with a typed error).
- [ ] The installer register and uninstall lists contain every catalog alias.
- [ ] Docs and README list the same suffixes as the catalog.

## Hand-off
_(filled in by the implementing session: aliases added, files touched, drift-test result, and any alias deliberately rejected)_

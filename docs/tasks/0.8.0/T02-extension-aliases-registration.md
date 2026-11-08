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
- [x] Add `stpnc`, `p21`, and `210` to `kStepExtensions` in the catalog.
- [x] Audit every catalog family against `docs/FORMAT-SUPPORT.md`, the README,
  and the installer. Add any other suffix the shipped adapter actually accepts;
  do not add suffixes the parser cannot validate.
- [x] Add the new STEP aliases to the NSIS install and uninstall lists.
- [x] Add the new STEP aliases to `thumbnail-provider/FamilyRouting.h` if the
  STEP thumbnail adapter accepts them. Do not add a new family or CLSID.
- [x] Add `[format-aliases]` cases asserting:
  - `FindByExtension` resolves each alias case-insensitively;
  - `DirectOpenExtensions` contains each alias exactly once;
  - the Open As format list includes the STEP stable name and all aliases;
  - the installer register/unregister lists match the catalog;
  - the thumbnail route contains the aliases.
- [x] Add small valid-file entries for `.stpnc`, `.p21`, and `.210` (copies of
  a small STEP fixture are enough) and confirm they reach the STEP host.
- [x] Update `docs/FORMAT-SUPPORT.md`, `README.md`, and the task README if the
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
- [x] `x64\Release\Tests.Unit.exe "[format-aliases],[format-catalog],[format-installer]"` passes.
- [x] `x64\Debug\Tests.Unit.exe "[format-aliases],[format-catalog],[format-installer]"` passes.
- [x] The `.stpnc`, `.p21`, and `.210` valid-file cases open through the STEP
  host (or are recorded as a known importer failure with a typed error).
- [x] The installer register and uninstall lists contain every catalog alias.
- [x] Docs and README list the same suffixes as the catalog.

## Hand-off

**Aliases added.** `catalog_detail::kStepExtensions` now owns `step`, `stp`,
`stpnc`, `p21`, and `210` (lowercase; `EqualExtension` folds case). They share
`FormatId::Step`, the `step` stable name, `Binbuf.Preview3D.STEP.1`, the STEP
thumbnail CLSID and the existing host. No new format, family, CLSID, parser or
process path was added.

**Files touched.** Catalog (`shared/model-core/include/model_core/FormatCatalog.h`);
NSIS install/uninstall and test-association reset; thumbnail routing and its
staged registration table (`thumbnail-provider/FamilyRouting.h`,
`packaging/installer/Preview3DThumbnailRegistration.ps1`); Open With ranking
(`interactive-viewer/src/platform/OpenWithRanking.cpp`); tests
(`tests/unit/FormatRegistrationTests.cpp`, `FormatCatalogTests.cpp`,
`OpenWithRankingTests.cpp`, `tests/app-smoke/thumbnail_registration.py`,
`tests/app-smoke/step_package.py`);
valid-file manifest; README, `docs/FORMAT-SUPPORT.md`, installer/portable
READMEs, design docs, and ADR-0096.

**Audit result.** The STEP aliases were the only catalog/doc/installer suffix
gap. Every other family already matched `docs/FORMAT-SUPPORT.md`, the README
and the NSIS lists. No other suffix the shipped adapters validate was found;
`.wrl`/`.vrml` remain deliberately unowned because VRML is a reserved
`FormatId` with no descriptor/parser. The audit also completed stale
direct-extension lists in `docs/design/07-user-experience.md`,
`docs/design/08-installation-and-registration.md`, and the installer/portable
READMEs; the ProgID table in design/08 now covers every catalog family.

**Valid-file evidence.** The three T01 fixtures already exist as byte-identical
copies of `tests/fixtures/stp-spike/part_ap214.stp`; only the manifest
expectations changed from `failed`/`extension` to `opened`/`triangles` with
`requiresStepHost`. The report observes 64 triangles, stage `Completed`, no
warnings for each alias.

**Checks run.**
- `x64\Release\Tests.Unit.exe "[format-aliases],[format-catalog],[format-installer]"` → all passed (1072 assertions, 16 cases).
- `x64\Debug\Tests.Unit.exe "[format-aliases],[format-catalog],[format-installer]"` → all passed (1072 assertions, 16 cases).
- `x64\Release\Tests.ImportIsolation.exe "[valid-file]"` and the Debug equivalent → all passed (163 assertions, 2 cases); `[write-valid-file-report]` records the aliases as opened.
- `python tests\app-smoke\thumbnail_registration.py` → static contract plus sandboxed install/conflict/repair/uninstall lifecycle passed.
- `x64\Release\Tests.Unit.exe "[provider]"` → all passed (60949 assertions, 225 cases).
- `x64\Release\Tests.Unit.exe "[open-with]"` → all passed (101 assertions, 19 cases).

**Deviations.** None from the task scope. A few consistency surfaces not named
in the task were updated because they carry the same suffix list: Open With
ranking affinity, `Reset-Preview3DTestAssociations.ps1`, and the STEP
app-smoke registration loop. The T01 fixtures were reused rather than copied
again. `docs/tasks/0.8.0/README.md` already named the three aliases in the
series definition of done and needed no edit. No `makensis` run was needed.

**Remaining work.** None for T02. T03's sniffer work is independent; T10 should
keep the support matrix aligned if any later task adds another alias.

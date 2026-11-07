---
timeoutMin: 240
verify: x64\Release\Tests.Unit.exe "[reader-selection]"
---
# T07 — Reader registry, scores and force-reader

## Goal
Make reader selection explicit and inspectable. A user must be able to list the
readers this build actually ships, force a specific reader for a file, and see
alternate readers in Open As. This is the selection foundation for T08; it does
not add a new parser.

## Context (read first)
- `shared/model-core/include/model_core/FormatCatalog.h` — the current
  one-descriptor-per-format table and `priority` field (currently unused).
- `shared/model-core/include/model_core/FormatSelection.h` —
  `ImportableFormatDescriptors`, `ProgIdForFamily`, and the open policy.
- `interactive-viewer/src/app/OpenOptions.h` — `--format` parsing and
  `OpenAsFormats`.
- `interactive-viewer/src/app/D3D12ImportBridge.cpp` —
  `SourceFormatForFormatDescriptor` and `RunImport`.
- `interactive-viewer/src/app/Preview3D.cpp` — the CLI parser, Open As menu,
  and failure card.
- `shared/import-broker/include/import_broker/ImportSession.h` — the closed
  `ImportFormat` and request fields the broker sends to the worker/host.
- F3D reference: `library/src/factory.cxx.in` (`pickReader` by score),
  `library/plugin/reader.h` (`getScore`, `canRead`), and
  `application/F3DStarter.cxx:2323-2356` (`--force-reader` diagnostics). F3D
  has multiple readers per extension; Preview 3D currently has one.

## Scope
- [x] Introduce a product-owned reader descriptor table (a new
  `ReaderCatalog.h` is fine) with at least:
  `name`, `displayName`, `family`, `importerKind`, `extensions`, `score`,
  `isCompatibility`, and the supported option names.
- [x] Populate it from the existing shipped readers: glTF/GLB, STL, PLY, OBJ,
  FBX, 3MF, USD fast path, USD compatibility host, STEP host, CAD host, and
  Assimp compatibility. Do not invent a reader that is not shipped.
- [x] Add `--list-readers`, printing one
  `name<TAB>extensions<TAB>importer<TAB>score` line per reader, in stable
  name order. Keep `--list-formats` as the family-oriented view.
- [x] Add `--reader <name>` (case-insensitive, bounded length) that forces a
  reader for the next open. A forced reader still goes through the normal
  worker/host validation; it never bypasses admission, containment, or
  budgets. Unknown or unshipped names fail typed before any open.
- [x] Make Open As show alternate readers for the selected family, with the
  default reader marked. Selecting an alternate sets the same forced-reader
  state; selecting the default clears it.
- [x] Keep extension/content classification as the default selector. A forced
  reader replaces candidate selection but not the worker’s own parse.
- [x] Add `[reader-selection]` cases:
  - `--list-readers` includes every shipped reader exactly once and no
    reserved/unshipped identity;
  - `--reader` is case-insensitive, rejects unknown names, and preserves the
    default path when absent;
  - Open As lists the default and alternates for each family;
  - a forced reader that cannot parse the file fails typed with the reader
    name in the diagnostic, not a crash;
  - the diagnostic/info panel names the selected reader.
- [x] Update `docs/FORMAT-SUPPORT.md`, `README.md`, and the task README.

## Out of scope
- Adding new compatibility readers or new parsers — T08.
- Changing the default reader for any family.
- Automatic retry after a parse failure — T08 owns the closed retry policy.
- Removing `--format`; it remains the family alias.

## Design notes
- Reader names are stable ASCII product identities, like stable format names.
  Do not localize them or derive them from a file path.
- Scores are product policy, not parser output. A compatibility reader must
  score below its native counterpart. Ties resolve deterministically by name.
- The trusted process chooses a reader; the worker/host still receives a
  closed discriminator, never free-form text. Do not pass the reader name as
  an unchecked parser hint.
- A forced reader is applied only after `ClassifySourcePath`, the broker’s
  canonicalization and handle duplication, and the normal worker/host
  admission. It changes importer selection, not the security boundary; it must
  not bypass sidecar policy, AppContainer access, Job Object limits, or any
  budget.
- The reader table is a compile-time product table, not a plugin loader. It
  lists only readers shipped in this build and cannot load code from disk or
  the network.
- `--reader` must not allow a reader to open a file whose extension it does
  not own unless the user explicitly forces it; that is the point of the
  override. The worker still validates the actual bytes.
- Keep the Open As list short: default plus shipped alternates, not every
  internal host.

## Done when
- [x] `x64\Release\Tests.Unit.exe "[reader-selection]"` passes.
- [x] `x64\Debug\Tests.Unit.exe "[reader-selection]"` passes.
- [x] `--list-readers` reflects the build’s shipped readers.
- [x] `--reader` forces a reader and reports an unknown name cleanly.
- [x] Open As exposes the alternates without changing the default open path.
- [x] No new parser or path authority was added.

## Hand-off

### Reader table

`shared/model-core/include/model_core/ReaderCatalog.h` is the compile-time,
product-owned reader registry (ADR-0101). `ReaderDescriptor` carries `name`,
`displayName`, primary `family`, `ImporterKind`, owned `extensions`, `score`,
`isCompatibility` and `optionNames`. Eleven shipped readers, in stable name
order:

| Reader | Extensions | Importer | Score | Kind |
| --- | --- | --- | --- | --- |
| `3mf` | `3mf` | `3mf-worker` | 90 | native |
| `assimp` | `dae,3ds,dxf,x,off,amf,x3d` | `assimp-compat` | 30 | compatibility |
| `cad` | `igs,iges,brep,xbf,fcstd` | `cad-host` | 100 | native |
| `fbx` | `fbx` | `fbx-worker` | 90 | native |
| `gltf` | `gltf,glb` | `gltf-worker` | 100 | native |
| `obj` | `obj` | `obj-worker` | 50 | native |
| `ply` | `ply` | `ply-worker` | 70 | native |
| `step` | `step,stp,stpnc,p21,210` | `step-host` | 100 | native |
| `stl` | `stl` | `stl-worker` | 60 | native |
| `usd` | `usd,usda,usdc,usdz` | `usd-fast` | 95 | native/default |
| `usd-compat` | `usd,usda,usdc,usdz` | `usd-compat-host` | 40 | compatibility |

Scores are product policy. The default reader for a family is the highest
score, ties by stable name; a compatibility reader is always below its native
counterpart. `ReaderFamilies`, `ReadersForFamily`, `DefaultReaderForFamily`,
`ReaderChoicesForFamily` and `ResolveForcedReader` are the pure lookups.

### CLI grammar

- `--list-readers` prints one
  `name<TAB>extensions<TAB>importer<TAB>score` line per reader in stable name
  order and exits 0. `--list-formats` remains the family view.
- `--reader <name>` and `--reader=<name>` accept an ASCII case-insensitive
  reader name, bounded to 64 characters. Empty, over-long, non-ASCII and
  unknown names set `commandLineInvalid`, print a bounded stderr diagnostic and
  exit 2 before any file is opened. `ReaderParseError` is the typed parser
  result.
- `--reader` wins over `--format` when both are present. `--format` remains the
  family alias and is unchanged.

### Forced selection

`BeginOpen` resolves a forced reader against the same `ClassifyForOpen`
classification as the default path. It opens the exact descriptor when the
reader owns the suffix or content family; a single-family reader falls back to
its own family and lets the worker/host reject the bytes; a multi-family reader
(`cad`, `assimp`) applied to an unowned suffix fails typed
`ReaderCannotOpenFormat` before any open, with the reader name in the failure
text. The default open path (no forced reader/format) is unchanged.

The reader name never crosses the process boundary. The broker still receives
the closed `SourceFormat`/`ImportFormat` value. The only new broker field is
`ImportSessionRequest::forceCompatibilityHost` (a boolean) for `--reader
usd-compat`; `RunImportSession` rejects it on any non-USD format as
`ValidateOptions`/`ImportProtocolViolation` and fails typed
`CompatibilityHostFailure` when no host path is configured. All admission,
canonicalization, sidecar, AppContainer, Job Object and budget checks are
unchanged.

### Open As and info panel

`OpenOptions::OpenAsFamilyEntries()` returns one entry per importable family,
default reader first plus shipped alternates (only USD has `usd-compat`).
`BuildOpenAsReaderText()` is the testable `family<TAB>reader<TAB>default|
alternate` form. The menu marks the default `[default]`, checks the currently
forced reader, sets `forcedReader` for an alternate and clears it for the
default. `BuildInfoPanelSections` gained an optional reader label and appends a
`Reader` row to Scene Data after the existing rows (so `rows[2]` stays Format).
A forced open's notice and failure details name the reader; the info panel
names the selected reader for every open.

### Checks run (2026-11-07)

- `x64\Release\Tests.Unit.exe "[reader-selection]"` — 660 assertions, 8 cases,
  passed.
- `x64\Debug\Tests.Unit.exe "[reader-selection]"` — 660 assertions, 8 cases,
  passed.
- Full `x64\Release\Tests.Unit.exe` — 166,248 assertions, 535 cases, passed.
- `x64\Release\Tests.ImportIsolation.exe "*forced compatibility-host*"` —
  6 assertions, 1 case, passed.
- `x64\Release\Tests.ImportIsolation.exe "[usd-006]"` — 27 assertions, passed
  (one fault case skipped in Release).
- `Preview3D.vcxproj` Release and Debug build; `Tests.Unit.vcxproj` Release and
  Debug build; `Tests.ImportIsolation.vcxproj` Release build.
- `Preview3D.exe --list-readers` (Release and Debug) — 11 lines, exit 0.
- `Preview3D.exe --reader nope` — exit 2 with a bounded stderr diagnostic.

### Docs

`docs/FORMAT-SUPPORT.md` (Readers and forced selection), `README.md` (Reader
selection), `docs/tasks/0.8.0/README.md` (T07 section),
`docs/design/03-file-formats-and-ingestion.md` (Reader selection + input
boundary), and ADR-0101.

### What T08 must add

- New compatibility readers go into the same `ReaderCatalog.h` table with
  stable names, scores below their native counterparts, `isCompatibility=true`
  and their closed option names; the `[reader-selection]` table/coverage test
  will then require them to be listed and covered.
- The closed retry policy and any compatibility fallback remain T08 work. A
  forced reader today changes importer selection only; there is no automatic
  retry after a parse failure.
- Do not pass the reader name to a parser, and do not let a forced reader skip
  `ClassifySourcePath`, broker canonicalization/handle duplication, sidecar
  policy, AppContainer access, Job Object limits or any budget.
- Keep the default reader for every existing family unchanged. If a new
  alternate is added for a family, update `OpenAsFamilyEntries` expectations
  and the docs table.

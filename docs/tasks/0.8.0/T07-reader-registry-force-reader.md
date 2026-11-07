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
- [ ] Introduce a product-owned reader descriptor table (a new
  `ReaderCatalog.h` is fine) with at least:
  `name`, `displayName`, `family`, `importerKind`, `extensions`, `score`,
  `isCompatibility`, and the supported option names.
- [ ] Populate it from the existing shipped readers: glTF/GLB, STL, PLY, OBJ,
  FBX, 3MF, USD fast path, USD compatibility host, STEP host, CAD host, and
  Assimp compatibility. Do not invent a reader that is not shipped.
- [ ] Add `--list-readers`, printing one
  `name<TAB>extensions<TAB>importer<TAB>score` line per reader, in stable
  name order. Keep `--list-formats` as the family-oriented view.
- [ ] Add `--reader <name>` (case-insensitive, bounded length) that forces a
  reader for the next open. A forced reader still goes through the normal
  worker/host validation; it never bypasses admission, containment, or
  budgets. Unknown or unshipped names fail typed before any open.
- [ ] Make Open As show alternate readers for the selected family, with the
  default reader marked. Selecting an alternate sets the same forced-reader
  state; selecting the default clears it.
- [ ] Keep extension/content classification as the default selector. A forced
  reader replaces candidate selection but not the worker’s own parse.
- [ ] Add `[reader-selection]` cases:
  - `--list-readers` includes every shipped reader exactly once and no
    reserved/unshipped identity;
  - `--reader` is case-insensitive, rejects unknown names, and preserves the
    default path when absent;
  - Open As lists the default and alternates for each family;
  - a forced reader that cannot parse the file fails typed with the reader
    name in the diagnostic, not a crash;
  - the diagnostic/info panel names the selected reader.
- [ ] Update `docs/FORMAT-SUPPORT.md`, `README.md`, and the task README.

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
- [ ] `x64\Release\Tests.Unit.exe "[reader-selection]"` passes.
- [ ] `x64\Debug\Tests.Unit.exe "[reader-selection]"` passes.
- [ ] `--list-readers` reflects the build’s shipped readers.
- [ ] `--reader` forces a reader and reports an unknown name cleanly.
- [ ] Open As exposes the alternates without changing the default open path.
- [ ] No new parser or path authority was added.

## Hand-off
_(filled in by the implementing session: reader table, CLI grammar, Open As behavior, and what T08 must add)_

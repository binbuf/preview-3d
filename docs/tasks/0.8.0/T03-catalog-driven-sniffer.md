---
timeoutMin: 240
verify: x64\Release\Tests.Unit.exe "[format-sniffer],[format-catalog]"
---
# T03 — Catalog-driven content sniffer

## Goal
Make `FormatSniffer` derive its bounded content probe from the format catalog
instead of a hand-maintained switch. Every signature the catalog declares is
actually implemented, and formats that currently have no signature get one
where a bounded head or central-directory probe can identify them. This makes
renamed files, content-only opens, and cross-family mismatch prompts work for
the whole catalog rather than only glTF/STL/PLY/OBJ/FBX/USD/STEP.

## Context (read first)
- `shared/model-core/include/model_core/FormatCatalog.h` — `MagicKind`,
  `MagicSignature`, `FormatDescriptor::signatures` and `priority`.
- `shared/model-core/src/FormatSniffer.cpp` — the current hardcoded probe.
  It never reads `descriptor.signatures` or `priority`.
- `shared/model-core/include/model_core/FormatSniffer.h` — the bounded limits
  (64 KiB head, 1 MiB central directory, 8,192 entries) and
  `SniffConfidence`.
- `tests/unit/FormatCatalogTests.cpp` — existing signature, ZIP and
  classification cases.
- `docs/design/03-file-formats-and-ingestion.md` — the rule that sniffing is a
  dispatch hint, never a trust decision.
- F3D reference: `library/plugin/reader.h` (`canRead`) and the per-reader
  `CanReadFile` tests show the useful pattern of one bounded probe per reader,
  tested with positive and negative inputs. Keep Preview 3D’s central,
  path-free probe and sandbox boundary.

## Scope
- [ ] Refactor `SniffHead` to iterate `model_core::Formats()` and evaluate each
  descriptor’s declared signatures in priority order. Keep the result a
  `SniffResult` candidate only; never parse model structure or decompress.
- [ ] Implement every declared `MagicKind`:
  `HeadPrefix`, `HeadPrefixCaseInsensitive`, `GltfJsonAsset`, `BinaryStl`,
  `AsciiStlText`, `PlyHeader`, `ObjText`, `ZipEntryEquals`,
  `ZipEntrySuffix`.
- [ ] Make `ProbeZipCentralDirectory` evaluate catalog ZIP signatures instead
  of hardcoding 3MF/USDZ. Preserve the current bounded EOCD/central-directory
  limits and reject ZIP64/spanned archives.
- [ ] Add missing signatures where a bounded probe is reliable:
  - 3DS: `MM` (`0x4D4D`) main chunk plus a declared length that fits the file;
  - AMF: `<amf` after optional XML declaration/comments;
  - DAE: `<COLLADA` after optional XML declaration/comments/BOM;
  - DXF: `0`/`SECTION` in the first four lines;
  - DirectX: `xof `;
  - OFF: `OFF` after optional whitespace;
  - X3D: `<X3D` after optional XML declaration/comments/BOM;
  - IGES: a complete 80-column record with `S`/`G`/`D`/`P`/`T` in column 73;
  - BREP: `CASCADE Topology V1/V2/V3` or `DBRep_DrawableShape`;
  - XBF: `BINFILE` plus a supported document-version prefix;
  - FCStd: ZIP with `Document.xml` and at least one `.brp` entry (add a
    bounded ZIP-entry pair/suffix kind if the current `MagicKind` cannot
    express the conjunction).
- [ ] Add `[format-sniffer]` cases for every signature: positive, BOM/
  whitespace/comment variants, truncated/malformed negatives, a generic ZIP
  that must stay `Unknown`, a renamed file that should be content-only, and a
  cross-family mismatch that should ask the user.
- [ ] Add a unit test that every descriptor with a non-empty `signatures` span
  is actually reachable by the table-driven probe, so adding a catalog entry
  cannot silently leave a dead signature.
- [ ] Update `docs/design/03-file-formats-and-ingestion.md` and the format
  support notes if the sniffing contract changes.

## Out of scope
- Opening or parsing the file in the sniffer. It remains a candidate selector.
- Adding signatures for formats with no bounded, reliable probe; leave them
  extension-only and document that.
- Changing `DecideOpenFormat` policy or the Open As prompt.
- Adding new formats; this task only detects the catalog’s existing families.

## Design notes
- Keep the probe allocation-bounded and path-free. It reads only the bounded
  head window and, for ZIP containers, the bounded EOCD/central directory.
- `priority` breaks ties when several descriptors match. A strong structural
  match (GLB header, checked binary STL, ZIP central directory) outranks a
  textual token. A weak textual match must not displace a strong one.
- `SniffHead` cannot identify ZIP-based formats; `SniffFile` owns the central
  directory path. Do not make `SniffHead` claim a package from the local
  header alone.
- Do not add a signature that a malformed or arbitrary file can satisfy with a
  few bytes unless the downstream parser still owns acceptance. The sniffer is
  advisory; false positives only produce an Open As prompt, not a trust
  decision.
- Keep the catalog data-only; do not include parser or platform headers.

## Done when
- [ ] `x64\Release\Tests.Unit.exe "[format-sniffer],[format-catalog]"` passes.
- [ ] `x64\Debug\Tests.Unit.exe "[format-sniffer],[format-catalog]"` passes.
- [ ] Every declared signature has at least one positive and one negative case.
- [ ] A renamed DAE, DXF, OFF, X3D, 3DS, IGES, BREP, XBF, and FCStd file is
  detected as content-only and routed through the Open As prompt.
- [ ] No parser, path, network or decompression code was added to the sniffer.

## Hand-off
_(filled in by the implementing session: signatures added, false-positive limits, test coverage, and any format deliberately left extension-only)_

---
timeoutMin: 300
verify: x64\Release\Tests.Unit.exe "[format-catalog]"
---
# T02 — Format catalog and content-aware router

## Goal
A single compile-time format catalog declares every supported format's
extensions, magic signatures, importer, priority, default orientation/units,
presentation profile and thumbnail eligibility, and the trusted process can
classify a file by extension **or** bounded content sniffing. Existing formats
move to the table; no new parser is added in this task.

## Context (read first)
- `docs/proposal.md` §3.1 — the catalog and Open As design.
- `interactive-viewer/src/app/D3D12ImportBridge.h:28-40` — `SourceFormat`.
- `interactive-viewer/src/app/D3D12ImportBridge.cpp:268-289, 425-441` —
  `ToBrokerFormat` and `ClassifyByExtension`.
- `shared/model-core/include/model_core/WireFormat.h:120-140` —
  `SourceFormatId` (wire identity; append-only).
- `interactive-viewer/src/platform/ShellIntegration.cpp:40-43`,
  `interactive-viewer/src/app/ActiveInstance.cpp:323-326`,
  `interactive-viewer/src/app/Preview3D.cpp:2230-2238` — duplicate extension
  lists.
- `docs/design/03-file-formats-and-ingestion.md` § Input boundary — the
  extension/magic sniff is only a dispatch hint, never a trust decision.

## Scope
- [ ] Add `shared/model-core/include/model_core/FormatCatalog.h` with:
  - `FormatId` stable names (start with the existing eight families; leave room
    for later additions);
  - `FormatDescriptor` fields: stable name, display/localization key,
    extensions, `MagicSignature` list, importer kind, priority, default up
    axis/unit policy, presentation profile, thumbnail family, capabilities;
  - a compile-time `Formats()` table and lookup functions
    (`FindByExtension`, `FindByStableName`, `FindById`).
- [ ] Add `shared/model-core/include/model_core/FormatSniffer.h` and a source
  file (or header-only if simpler) that reads only a bounded head window and,
  for ZIP-based formats, a bounded central-directory probe. Implement
  signatures for the currently supported formats:
  glTF/GLB, STL, PLY, OBJ, FBX, 3MF, USD/USDA/USDC/USDZ, STEP/STP.
- [ ] Replace the extension switch in `ClassifyByExtension` with the catalog.
  Add `ClassifyByContent` and a combined `ClassifyForOpen` that returns the
  candidate plus the reason (`extension`, `content`, `both`, `mismatch`).
- [ ] Keep the current broker behavior: unknown extensions still fail with the
  existing Open dialog/active-instance guidance until T03 adds override. For
  now, expose the classifier result to the caller and add diagnostics.
- [ ] Replace `SourceFormatLabel` / `ToBrokerFormat` switches with catalog
  lookups; keep wire `SourceFormatId` values unchanged.
- [ ] Add `tests/unit/FormatCatalogTests.cpp` tagged `[format-catalog]`:
  every extension unique; every descriptor has a display key and an importer;
  every existing extension resolves; all wire IDs have a descriptor; a GLB
  renamed `.bin` is identified by content; a malformed `.glb` is not.
- [ ] Add a tiny `--list-formats` **stub** only if the existing CLI parser makes
  it cheap; otherwise defer the command to T03 and keep this task to the table.
- [ ] Update `docs/design/03-file-formats-and-ingestion.md` with the catalog as
  the authority for the support matrix's extension list.

## Out of scope
- New formats (T08/T09/T10).
- The Open As UI/menu and installer generation (→ T03).
- Per-format behaviour changes such as OBJ orientation (→ T06).
- Changing any parser or the wire format.

## Design notes
- The catalog lives in `model_core` but must not include third-party headers or
  link a parser. It is data plus pure lookup functions.
- `FormatId`/stable names are append-only. Do not renumber existing
  `SourceFormatId` values.
- The sniffer is a **candidate selector**, not validation. The worker/host still
  performs the real parse and all containment checks.
- A ZIP central-directory probe must be bounded in both bytes and entry count;
  never decompress an archive to sniff it.
- If a file's extension and content disagree, prefer the strong content
  signature and record the mismatch; T03 decides whether to auto-open or ask.
- Do not let the catalog become a second list of parser behavior. Behaviour
  stays in the adapters; the catalog describes identity and defaults.

## Done when
- [ ] `x64\Release\Tests.Unit.exe "[format-catalog]"` passes.
- [ ] `x64\Debug\Tests.Unit.exe "[format-catalog]"` passes.
- [ ] The existing open path still opens all current fixtures (run the app
  smoke or the relevant import tests).
- [ ] A GLB renamed `.bin` produces a content-classified glTF result even though
  the open path still rejects it until T03.
- [ ] `docs/design/03-file-formats-and-ingestion.md` names the catalog as the
  extension authority.
- [ ] Hand-off records the descriptor names reserved for later formats.

## Hand-off

### Landed

- `shared/model-core/include/model_core/FormatCatalog.h` is the compile-time
  authority: `FormatId`, `FormatDescriptor` (stable name, display key/name,
  extensions, magic signatures, importer, priority, default up axis/unit
  policy, presentation profile, thumbnail family, capabilities, wire id), a
  `Formats()` table, and `FindByExtension` / `FindByStableName` / `FindById` /
  `FindBySourceFormatId`.
- `FormatId` 1..13 mirror the existing `SourceFormatId` 1..13. Reserved
  append-only ids with no descriptor yet: `Iges=15`, `Brep=16`, `Xbf=17`,
  `Fcstd=18`, `Dae=19`, `ThreeDs=20`, `Off=21`, `Amf=22`, `Vrml=23`,
  `X3d=24`. `Usd=14` is the generic `.usd` extension identity.
- ASCII STL/PLY are wire-only descriptors (no extension), so every extension
  has exactly one owner while every wire id still has a descriptor.
- `shared/model-core/include/model_core/FormatSniffer.h` and
  `src/FormatSniffer.cpp` implement `SniffHead` and `SniffFile` with a 64 KiB
  head bound, a 1 MiB / 8,192-entry central-directory bound, and no
  decompression. Signatures: GLB header (v2, declared length bound), glTF JSON
  asset/version, PLY header, binary/ASCII STL, OBJ statements, FBX binary and
  ASCII, USDA, USDC, STEP, 3MF content-types entry, USDZ USD-layer entry.
- `D3D12ImportBridge` now exposes `ClassifyByContent`, `ClassifyForOpen`,
  `ExtensionSourceFormat` and `ClassificationDiagnostic`.
  `ClassifyByExtension`, `SourceFormatLabel` and `ToBrokerFormat` read the
  catalog; `SourceFormatId` wire values are unchanged.
- The open path still admits files by extension only (so a GLB renamed `.bin`
  is still rejected), but it runs the bounded probe once and logs a path-free
  content/mismatch diagnostic so T03 can act on it.
- Docs: `docs/design/03-file-formats-and-ingestion.md` names the catalog as the
  extension authority; ADR `0057-format-catalog-and-bounded-sniff-routing.md`
  records the identity/dispatch boundary.

### Decisions / deviations

- T03 owns the Open As UI/CLI, `--list-formats`, installer registration and
  replacing the remaining duplicate lists in `ShellIntegration.cpp`,
  `ActiveInstance.cpp` and the open-dialog filters. T02 deliberately did not
  touch them.
- `Tests.Unit` Release opts out of whole-program optimization. MSVC 14.51's c2
  ICEs (C1001 in `GltfDataUriPreflightTests.cpp`) during LTCG for this test-only
  link; the same failure reproduces with all T02 sources removed, so it is
  pre-existing and unrelated. Shipped projects keep LTCG.
- The catalog's display key/name are `const char*`/`const wchar_t*` literals:
  `Loc()` caches fallbacks by pointer, so a temporary key would alias.

### Checks

- `msbuild Preview3D.slnx /t:Tests_Unit /p:Configuration=Release /p:Platform=x64`
  — pass.
- `x64\Release\Tests.Unit.exe "[format-catalog]"` — 350 assertions, 8 cases,
  all pass.
- `x64\Debug\Tests.Unit.exe "[format-catalog]"` — 350 assertions, 8 cases, all
  pass.
- `msbuild Preview3D.slnx /t:Tests_ImportIsolation /p:Configuration=Release /p:Platform=x64`
  then `x64\Release\Tests.ImportIsolation.exe "[recovery]"` — 2312 assertions,
  37 cases (34 pass, 3 fault-harness skips).
- `x64\Release\Tests.ImportIsolation.exe "[format-catalog]"` — 8 assertions;
  the bridge-level case proves a GLB renamed `.bin` has a content candidate but
  the extension-only gate still rejects it.
- `x64\Release\Tests.ImportIsolation.exe "[compat]"` — 312 assertions, all
  pass; existing fixtures still open through the unchanged open path.
- `msbuild Preview3D.slnx /t:Preview3D /p:Configuration=Release /p:Platform=x64`
  — pass.

### Extension/content disagreements observed

- A `.gltf` extension over GLB content is the same family, so it classifies as
  `both` with the GLB candidate selected.
- `.usd` is intentionally a generic extension descriptor; `#usda` or
  `PXR-USDC` content selects the concrete USDA/USDC descriptor as `both`.
- A GLB renamed `.bin`, or any supported content under an unrelated extension,
  is `content`/`mismatch` and is still rejected by the open path until T03.

### Open As hooks for T03

- Use `d3d12_import_bridge::ClassifyForOpen(path)`; it returns `extension`,
  `content`, `selected`, `reason` (`Extension`/`Content`/`Both`/`Mismatch`) and
  the content confidence/detail.
- `ClassificationDiagnostic` produces a path-free string for the info panel or
  logs; `ExtensionSourceFormat` recovers the legacy gate without re-reading.
- Catalog iteration (`Formats()`, `FindByStableName`) is the source for
  `--format <stableName>`, `--list-formats`, filters, registration and the
  Open As list. `--skip-content-check` should keep using the extension
  candidate.

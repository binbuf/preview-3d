# ADR-0101 — Reader catalog, scores and forced selection

**Status:** Accepted

## Context

The format catalog identifies what a file is (`FormatId`, family, extensions,
importer kind, presentation, capabilities), but it has no user-visible reader
identity. A family currently maps to exactly one importer, so `priority` is
only a sniffer tie-breaker and there is no way to list, inspect or override the
reader a build actually ships. Open As lists catalog formats, not readers, and
the USD family has two real implementations (TinyUSDZ fast path and OpenUSD
compatibility host) that a user cannot distinguish.

F3D models readers as named objects with `canRead`/`getScore` and exposes
`--force-reader` diagnostics. Preview 3D needs the same inspectability without
adopting F3D's plugin loader, its trusted-process parser architecture, or
free-form reader names crossing the process boundary.

## Decision

- `shared/model-core/include/model_core/ReaderCatalog.h` is a compile-time,
  product-owned table of readers shipped in this build. A `ReaderDescriptor`
  carries a stable ASCII `name`, `displayName`, primary `family`,
  `ImporterKind`, owned `extensions`, product-policy `score`,
  `isCompatibility`, and the closed `optionNames` it honors. The table cannot
  load code, a path, a URL or a plugin.
- The table is populated from the existing shipped readers only: glTF/GLB,
  STL, PLY, OBJ, FBX, 3MF, the USD fast path, the USD OpenUSD compatibility
  host, the STEP host, the OCCT CAD host (IGES/BREP/XBF/FCStd) and the bounded
  Assimp compatibility adapter. Names are listed in stable name order.
- A compatibility reader always scores below its native counterpart
  (`usd-compat` 40 < `usd` 95; `assimp` 30 is below every native reader). The
  default reader for a family is the highest score, with ties resolved by
  stable name. The default is derived, never a second hand-written switch.
- `--list-readers` prints `name<TAB>extensions<TAB>importer<TAB>score` per
  reader in stable name order. `--list-formats` stays the family-oriented view.
- `--reader <name>` is ASCII, case-insensitive and bounded to 64 characters.
  Unknown or unshipped names fail typed before any open. `--reader` wins over
  `--format` when both are present.
- A forced reader is resolved in the trusted process against the same
  extension/content classification used by the default path. If it owns the
  suffix (or the content family) it opens that exact descriptor; a
  single-family reader may fall back to its own family and let the worker/host
  reject the bytes; a multi-family reader has no honest fallback and is a typed
  `ReaderCannotOpenFormat` failure before any open. The reader name is never
  passed to a parser.
- The worker/host still receives the existing closed
  `SourceFormat`/`ImportFormat` discriminator. The one additional closed value
  is `ImportSessionRequest::forceCompatibilityHost`, which routes a USD
  generation to the OpenUSD compatibility host instead of the fast path. It is
  a boolean, not a reader name, and the host repeats admission, containment and
  every budget check.
- Open As lists one entry per importable family: the default reader marked
  `[default]` plus shipped alternates (currently only `usd-compat`). Selecting
  an alternate sets the forced-reader state; selecting the default clears it.
  Selecting any family still forces that family, which is what Open As already
  meant. The default open path is unchanged when no reader is forced.
- The selected reader is named in the info panel's Scene Data section and in
  the forced-reader notice/failure text. All strings are stable product names;
  no path or source text is added.

## Consequences

- T08 adds compatibility readers and the closed retry policy to this table and
  resolver; it must not add a new parser or a free-form reader hint.
- A reader can only be forced after `ClassifySourcePath`, broker
  canonicalization/handle duplication and normal worker/host admission. Forced
  selection changes importer choice, not the security boundary, sidecar policy,
  AppContainer access, Job Object limits or any budget.
- `--format` remains the family alias. A forced reader and a forced format are
  both trusted-process decisions; the reader is the more specific one.
- The unit suite enumerates the reader table, its extension coverage, the
  default/alternate resolution and the CLI grammar, so adding a shipped reader
  without listing it (or listing an unshipped identity) fails the gate.

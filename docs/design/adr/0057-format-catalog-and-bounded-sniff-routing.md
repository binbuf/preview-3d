# 0057 — The format catalog is the identity authority; sniffing is a bounded dispatch hint

Status: accepted

## Context

0.7.0 needs one place that says which extensions a format owns, which importer
serves it, and what its defaults are. Before T02 that metadata was duplicated in
`ClassifyByExtension`, `SourceFormatLabel`, `ToBrokerFormat`, the Shell
integration, the active-instance guard and the open dialog. Adding a format
therefore required a scavenger hunt, and a renamed-but-valid file was rejected
before any parser saw it.

At the same time the security model requires that extension and content checks
never become trust decisions. All parsing stays in the sandboxed worker or a
dedicated host; only normalized, validated chunks cross the process boundary.

## Decision

- `shared/model-core/include/model_core/FormatCatalog.h` is the compile-time
  authority for format identity: append-only `FormatId`, stable name, display
  key, extensions, magic signatures, importer kind, priority, default up
  axis/unit policy, presentation profile, thumbnail family, capabilities and
  wire `SourceFormatId`. Existing wire identities keep their numeric values.
- The catalog is data plus pure lookups. It includes no parser, platform or
  third-party header and carries no path/network authority.
- `FormatSniffer` produces a candidate from at most a 64 KiB head window and,
  for ZIP-based formats, a bounded EOCD plus central-directory probe (1 MiB and
  at most 8,192 entries). It never decompresses an archive and never parses
  model structure.
- `ClassifyFormat` combines extension and content into a reason
  (`extension`, `content`, `both`, `mismatch`). Same-family variants such as
  `.gltf`/`.glb` and `.usd`/`.usdc` count as `both`. A cross-family
  disagreement prefers the strong content candidate and is recorded as
  `mismatch`.
- T02 does not change the open path's extension-only admission. The combined
  result is exposed to the caller and logged path-free so T03 can add
  `--format`, `--skip-content-check` and the Open As UI without reworking the
  catalog.

## Consequences

- Later tasks add formats by appending a descriptor and adapter; duplicate
  extension lists are removed incrementally as T03 moves registration and
  filters onto the table.
- A valid file with the wrong suffix remains rejected until T03 wires the
  override, but the content classification is already available and tested.
- The sniffer is advisory. A malformed file with a correct extension still
  reaches the parser and fails with its existing typed error.
- Wire `SourceFormatId` values are unchanged; ASCII STL/PLY and the USD
  encodings are catalog descriptors without duplicate extension ownership.

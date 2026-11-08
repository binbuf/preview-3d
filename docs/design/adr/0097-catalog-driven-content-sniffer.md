# 0097 — Catalog-driven content sniffer

Status: accepted (T03, 0.8.0).

## Context

`FormatSniffer.cpp` kept a hand-written `if`/`else` probe for seven families
while `FormatCatalog.h` already declared `MagicKind` signatures and a
`priority` for every descriptor. The two drifted: the catalog declared
signatures the probe never evaluated (DAE, DXF, DirectX, OFF, X3D), several
catalog families had no signature at all (3DS, AMF, IGES, BREP, XBF, FCStd), and
the ZIP probe hard-coded 3MF/USDZ entry names. A valid renamed DAE or DXF file
therefore failed as `UnsupportedFormat` instead of reaching the Open As prompt,
and adding a catalog entry could silently leave a dead signature.

The security boundary is unchanged: the sniffer is an advisory candidate
selector in the trusted process. It must not parse model structure, open a
path, touch the network, or decompress an archive, and the worker/host still
owns acceptance.

## Decision

- `SniffHead` iterates `model_core::Formats()` and evaluates every non-ZIP
  `MagicSignature` declared by each descriptor. A match is scored by
  confidence first (strong structural over weak textual) and then by the
  descriptor's `priority`; a recognized GLB envelope that fails its own header
  check vetoes weaker fallbacks.
- Every declared `MagicKind` is implemented: `HeadPrefix`,
  `HeadPrefixCaseInsensitive`, `GltfJsonAsset`, `BinaryStl`, `AsciiStlText`,
  `PlyHeader`, `ObjText`, `ZipEntryEquals`, `ZipEntrySuffix`, plus the bounded
  kinds added here: `GlbHeader`, `ThreeDsChunk`, `XmlPrefixCaseInsensitive`,
  `DxfSection`, `OffHeader`, `IgesRecord`, `BrepHeader`, `XbfHeader`, and
  `ZipEntryEqualsAndSuffix`.
- `ProbeZipCentralDirectory` evaluates the catalog's ZIP signatures against
  the bounded central-directory entry names. The existing EOCD limits and the
  ZIP64/spanned-archive rejection remain. `SniffHead` never claims a package
  from the local header.
- The catalog stays data-only. Signatures are bounded, path-free tokens or
  structural checks; the sniffer never includes a parser, platform, network,
  or decompression header. A false positive only produces the Open As prompt.
- Wire-only content variants (ASCII STL/PLY) and the generic `.usd` extension
  identity declare no separate signature; the family descriptor owns the
  signature and the concrete encoding is resolved from content. Formats with
  no bounded, reliable probe stay extension-only.
- A unit case enumerates every catalog signature and supplies a positive and
  negative probe, and asserts every descriptor with a non-empty signature span
  is selected by the table-driven probe. Adding a catalog signature without an
  implementation or test therefore fails the build gate.

## Consequences

- A renamed DAE, DXF, OFF, X3D, 3DS, IGES, BREP, XBF, or FCStd file is
  content-detected and routed through Open As; a generic ZIP, a truncated head,
  and an ambiguous XML/JSON document stay `Unknown`.
- Adding a catalog format with a signature requires a matching kind
  implementation and positive/negative coverage; otherwise the reachability
  test fails. Adding a format without a bounded probe is an explicit
  extension-only decision, not a silent gap.
- The probe still reads only the 64 KiB head window and, for ZIP packages, the
  bounded EOCD/1 MiB central directory. The worker/host remains the only place
  that parses or decompresses the file.

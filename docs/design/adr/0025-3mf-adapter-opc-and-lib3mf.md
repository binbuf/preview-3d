# 0025 — 3MF adapter: product OPC/required-extension preflight plus provider-local lib3mf

## Status
accepted

## Context
T32 must render the supported static `.3mf` preview subset in Explorer from the Shell's
`IInitializeWithStream` stream using the pinned lib3mf 2.5 reader, never the viewer or the import
worker (3MF-008). lib3mf handles Core/Production/Materials/Beam-Lattice parsing, but two properties
of the pinned version force product-owned boundaries: lib3mf's compatible reader mode (required for
current slicer packages, which lib3mf's strict mode rejects) does **not** fail an unknown
`requiredextensions` entry; and the frozen `VertexSample`/`MaterialPayload` contracts have no texture
sampling, so a decoded contained image cannot change the thumbnail. The worker already owns a bounded
OPC/ZIP preflight and an XML `requiredextensions` scan, but the provider must stay free of COM/WIC and
cannot link the worker binary.

## Decision
1. **Package boundary:** reuse the worker's product OPC/ZIP preflight (`ThreeMfOpcPreflight.cpp`,
   compiled source-not-state into the provider) under provider ceilings — 256 MiB stream, 128 MiB
   aggregate expansion, 100:1 ratio, 4096 entries, 32-level paths. It rejects encrypted/multi-disk
   archives, unsupported compression, unsafe/duplicate paths, overlapping records and malformed
   headers before lib3mf reads a byte.
2. **Required-extension policy:** after preflight, a bounded product byte-level scan of every `.model`
   part rejects a `requiredextensions` prefix that resolves to a namespace outside the allowlist
   (Core, Materials, Production, Beam Lattice, Beam Lattice balls) and rejects DTD/entity content.
   This replaces the worker's COM `IXmlReader` scan with a self-contained scanner so the provider
   stays COM/WIC-free. Rejected: running lib3mf in strict mode, which rejects ordinary slicer
   packages, and trusting lib3mf's compatible mode, which ignores unknown required extensions.
3. **Reader:** lib3mf is statically linked and fed the bounded in-memory bytes through read/seek
   callbacks (never `ReadFromFile`/a path), with a progress callback that aborts on the cooperative
   deadline; the adapter launches no worker and performs no filesystem, sidecar, network or
   persistent write. lib3mf's allocations are outside the product allocation ledger (recorded for
   T51), as the worker's 3MF-001 spike established.
4. **Scene and appearance:** the standard root build is traversed deterministically across build
   items and component graphs with checked double-precision transforms (row-vector composition,
   256-level depth cap, 10 000 occurrences, 2 M inspected triangles). Bare-mesh occurrences emit
   triangle samples with a flat geometric normal and the object/triangle/per-corner 3MF color
   resolved to a linear vertex color that modulates a single registered white material. Supported
   property types are base materials, color groups, texture-coordinate groups, composites and
   multi-properties; a texture-coordinate group is validated against the aggregate pixel budget but
   not decoded. A lattice occurrence prefers a bounded tessellation of its beams (Butt/Hemisphere/
   Sphere caps, tapered radii) and balls, `inside`-clips against a closed axis-aligned 8-vertex/
   12-triangle box, and fails to the generic icon for `outside` or non-box clipping without a valid
   representation mesh.
5. **Failure policy:** an unsupported required extension, an over-budget scene, an unclipped
   parametric lattice or a malformed package fails closed to the generic icon; a supported scene is
   never rendered only in part.

## Consequences
- The provider is import-clean (lib3mf/zip/zlib/bz2 static; no viewer/worker/host/core imports) and
  stream-only; the crash-containment boundary is unchanged.
- The byte-level required-extension scan only reads the root `<model>` start tag and does not resolve
  arbitrary XML prefixes outside it; a required extension declared only in a region the scanner does
  not read would rely on lib3mf's own handling. Corpus fixtures (`production-boxes` with
  `requiredextensions="s p"`) and the derived `unknown-required` case cover the documented shape.
- Contained textures are structurally validated, not decoded; if a later task adds texture sampling
  it must promote this to a real bounded decoder and revisit the COM/WIC boundary (as in ADR-0024).
- The reproduction of the worker's OPC preflight as a provider translation unit is a deliberate
  source-not-state reuse; a future refactor may move it into the shared parser subset.
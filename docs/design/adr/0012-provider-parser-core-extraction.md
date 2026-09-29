# 0012 — The provider shares the format-agnostic parser primitives, not the wire adapters

## Status
accepted

## Context
ADR-0004 and ADR-0009 commit `Preview3DThumbnailProvider.dll` to compiling the same *source files*
the worker uses for the bounded product parsers, and interfaces.md enumerated an exact subset. But
the STL/PLY adapters are not format-agnostic: `StlAdapter.cpp`/`PlyAdapter.cpp` interleave parsing
with `BoundedChunkWriter`/`ChunkBatchSink` (wire batches, preview/coarse/refinement and requested-
source replay) and `MappedFile` windowed reads. Compiling those translation units into the DLL would
drag in `model_core/WireFormat.h`, `Checksum.h`, `VertexLayouts.h`, `ControlProtocol.h`,
`ControlChannelIo.h`, `CoarseSampler.h` and `windows.h` — every one of which T04 excludes. A literal
"move the adapters" would therefore either break the no-IPC invariant or force a wire-protocol
refactor that risks the proven worker suite.

## Decision
Extract only the genuinely format-agnostic parser primitives into a new
`shared/parser-core/` source set and compile it into both consumers:

- `AsciiTokenizer` (std-only bounded tokenizer), `StlParserCore` (binary-STL layout/limits, native
  reads, per-facet validation and supplied/flat-normal policy, ASCII/binary detection) and
  `PlyParserCore` (scalar-type model, endian-aware scalar reads, color normalization, bounded header
  parser). None reference an IPC, mapping, broker, worker or viewer header.
- The worker's `StlAdapter.*`/`PlyAdapter.*` keep their wire-emitting Tier A/B streaming shells and
  call the shared primitives; they are reclassified from **move** to **duplicate** in interfaces.md.
- The provider project compiles the three `shared/parser-core` translation units with its
  precompiled header disabled (the worker has none), proving the DLL boundary; T21–T34 add only
  their `IFamilyAdapter` on top.

## Consequences
- A change to a shared parser primitive is intentionally a change to both the worker and the
  provider; the per-consumer wire/policy shell may still diverge.
- The DLL compiles no worker/broker/host/viewer header, and `Tests.Unit.exe` compiles the same
  three files (no worker header) as the `[parser]` regression set.
- The worker's Tier A protocol, coarse proxy and mapped streaming stay byte-for-byte unchanged; the
  filtered `Tests.ImportIsolation.exe "[stl-import],[ply-import],[gltf-import]"` verify command is
  the acceptance evidence.
- Family tasks T21/T23 reuse `parser_core::StlParserCore`/`PlyParserCore`; a future shared static
  library must preserve ADR-0004's no-shared-state invariant and is itself ADR-worthy.
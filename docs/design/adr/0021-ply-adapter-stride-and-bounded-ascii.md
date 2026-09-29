# 0021 — The PLY adapter addresses binary vertices by stride and bounds ASCII tables

## Status
accepted

## Context
T23 adds the product PLY adapter (`thumbnail-provider/PlyFamilyAdapter.{h,cpp}`) for ASCII and binary
little/big-endian `.ply`, meshes and point clouds, with vertex colors. The shared parser subset (T07,
ADR-0012) provides only header parsing, scalar reads and color normalization; the geometry walk is
the adapter's. The provider must sample a source without materializing every position in private
memory (design/05, "Geometry sampling"; design/03, "PLY"), must fail hostile counts/lists before
allocation, and must feed triangles to the sampler and points to the depth-tested splat path.

The worker's PLY adapter retains a full `meshVertices` vector for its indexed faces. That is acceptable
for a bounded Tier-B import but not for the provider's tighter 192 MiB scratch / 384 MiB ledger budget,
and it conflicts with the provider's "never all source positions in memory" requirement for binary
meshes, where every vertex record has a known fixed byte stride.

## Decision
- **Binary mesh: stride random access, no vertex table.** The header parser gives a fixed record
  stride when no vertex property is a list (`FixedStride`). `ComputeBinaryOffsets` locates the vertex
  and face bodies with checked arithmetic and proves the vertex extent against the source length in
  `Parse`; `EnumerateGeometry` reads each vertex a face references from
  `vertexStart + index * stride` through a 256-entry direct-mapped cache. The only retained storage is
  one record buffer plus the cache, both charged to the T06 ledger. A binary vertex element with a
  list-typed property (no fixed stride) fails closed with `UnsupportedRequiredFeature`.
- **ASCII mesh: bounded, charged vertex table.** `AsciiTokenizer` has no fixed record offsets, so the
  adapter retains a `VertexSample` table in header order and resolves faces from it. The table is
  rejected in `Parse` when it would exceed `ProviderLimits::kAccountedScratchMaxBytes` and charged to
  the caller's ledger before allocation. ASCII is Tier B, so the tighter budget is acceptable.
- **Both dialects share one schema/decoder.** `ResolveSchema` identifies `x/y/z`, optional
  `nx/ny/nz`, and `red/green/blue[/alpha]` (or `r/g/b[/a]`); `ApplyVertexProperty` maps values into
  one `DecodedVertex`; `EmitPoint`/`EmitTriangle` convert to product-owned samples. A face list is
  fan-triangulated; out-of-range indices and non-finite positions drop the affected triangle locally
  rather than failing the file (PLY vertices are not shared state once dropped).
- **Vertex colors use a white base material.** When colors are present, material 1 is the neutral
  palette with `baseColorFactor = (1,1,1,1)` so `vertexColor * baseColor` preserves the source color
  (CpuRasterizer.h); otherwise it stays neutral.
- **Bounds over declawed counts.** Per-face vertex lists are capped at 255, unknown list lengths at
  65 536, skipped elements at 6 M records, the vertex count at 6 M, and a hostile count is rejected
  before any allocation. Every read polls `AdapterInput::deadline`.

## Consequences
- A binary PLY mesh touches only one record at a time, so a 2 M-triangle mesh stays inside the ledger;
  a source that cannot be positioned (list-typed vertex element, or a list-bearing element before the
  face body) returns the generic-icon fallback rather than reading unbounded memory.
- ASCII meshes are capped by the accounted-scratch ceiling; larger ASCII PLY files get the fallback and
  are the natural candidate if a later task needs streaming ASCII face resolution.
- T24 (OBJ) and the remaining families can follow the same "prove extents in Parse, stream in
  EnumerateGeometry" shape; the registry already routes `Family::Ply` to `PlyAdapter`.
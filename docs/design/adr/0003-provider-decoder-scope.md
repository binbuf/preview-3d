# 0003 — Provider decoder scope for embedded glTF and image assets

## Status
accepted

## Context
The baseline disagrees with itself. `05-thumbnail-provider.md` permits "bounded Draco geometry and
KTX2/WebP textures" in the provider, and the thumbnail budget table reserves a Draco primitive working
set (96 MiB / 1 million triangles) and 32 megapixels of decoded texture. But the TSK-209 decision in
`11-decisions-and-risks.md` says the thumbnail provider "does not link either decoder or claim
compressed-glTF thumbnail support in this limited MVP", referring to meshopt and WebP. The provider
must know exactly which decoders it links before T25 is scoped.

## Decision
The provider links only the bounded decoders needed to render a **stream-contained** thumbnail, each
independently budget-bounded and optional except where the geometry itself requires it:

- glTF geometry: embedded uncompressed accessors, plus bounded **Draco** decode and bounded
  **meshopt** (`EXT_meshopt_compression`) decode. Required compressed geometry that cannot be decoded
  within the provider budget returns the generic icon.
- glTF images: embedded data-URI images in PNG/JPEG, plus bounded **KTX2/Basis** transcode and
  bounded **WebP** decode. An unavailable *optional* image uses the default material with no failure.
- FBX/OBJ/3MF/USD: embedded/contained images only, using the same bounded image decoders.
- No external sidecar, path, filesystem, environment-selected codec or arbitrary WIC enumeration is
  ever used.

Decode uses provider limits (decoded pixels, per-primitive byte/triangle caps and
the cooperative 2 s stop point), never the viewer's larger worker budgets. Calls without progress or
allocator callbacks need conservative admission plus measured time and process commit; these limits
do not guarantee an in-call timeout. This supersedes the TSK-209 "provider
does not link either decoder" sentence, which described the scope-limited MVP, not full thumbnail
support.

## Consequences
- The DLL's dependency closure and SBOM include Draco, KTX2/Basis, libwebp and meshoptimizer in
  addition to the family parsers. T42 owns that closure.
- Each decoder contributes provider-limit and fuzz tests (T43) and a golden image where applicable.
- If any decoder cannot meet the provider budget on the reference corpus, T25 records the exact
  fallback and the reader is narrowed in this ADR rather than silently exceeding a limit.

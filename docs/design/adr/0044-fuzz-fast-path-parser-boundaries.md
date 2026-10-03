# 0044 — Fuzz-target boundaries for the STL, PLY, and OBJ/MTL fast paths

## Status
accepted

## Context
`docs/design/09-quality-performance-and-security.md` ("Fuzzing") promises a
standalone, no-GPU target per format that runs bounded bytes through the
normalized output boundary. SEC-13 added the `fuzz-smoke` lane and the
STEP/3MF targets; SEC-15 owns the product-owned STL/PLY parsers and the
Wavefront OBJ/MTL path. The STL/PLY parsers are product-owned
(`shared/parser-core` plus `import-worker/src/{Stl,Ply}Adapter.cpp`); OBJ/MTL is
parsed by the pinned `ufbx` library that `ObjAdapter.cpp` drives. The design
already scopes the OBJ/FBX target as "adapter options/callbacks", not the full
adapter's wire-emission or image-decode stages.

## Decision
- `StlFuzz` and `PlyFuzz` link and drive the real adapter entry points
  (`ImportStl`, `ImportPly`) with a null `MappedFile` and a bounded in-memory
  source, plus a direct `parser_core` domain for the shared header/scalar/
  normalization primitives.
- `ObjFuzz` drives pinned `ufbx` directly with OBJ and MTL file formats and a
  single-entry, in-memory virtual sidecar served through the open-file
  callback. Every other external open (missing, traversal, absolute, UNC,
  network) is denied without touching the filesystem.
- Each target caps its standalone input at 2 MiB (1 MiB sidecar for OBJ),
  allocates a 4 MiB output window, and passes null `ChunkBatchSink`; the real
  AppContainer/Job and product size limits stay covered by the
  import-isolation and provider lanes.
- `ObjAdapter`'s texture/image decode stages are not linked into `ObjFuzz`
  because compressed codecs are SEC-16's boundary; the provider/worker lanes
  cover them.

## Consequences
- A finding minimizes into the target's immutable seed directory; a fix lands
  with a regression seed.
- The targets must be built individually (one per runner) because the fuzz
  projects share `tests\fuzz\x64\Release` as an intermediate directory
  (MSB8028).
- `ufbx` is header-only here, so `ObjFuzz` needs the root vcpkg manifest
  restored; `StlFuzz`/`PlyFuzz` need no vcpkg dependency.
- The fuzz lane remains opt-in until SEC-15/16/17 promote it into the required
  gate; SEC-16 extends the same matrix with glTF/codec targets.
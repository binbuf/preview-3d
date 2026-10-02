# T16 — Fuzz targets: glTF + compressed codecs

## Goal
The primary format (glTF/GLB) and the compressed paths named in `SECURITY.md` have fuzzing
coverage, so the admissibility checks, range validation, and decoder boundaries are exercised
continuously rather than by inspection.

## Context (read first)
- `tests/fuzz/README.md` — target conventions; existing targets do not instrument their linked
  third-party libraries (documented limitation).
- `import-worker/src/GltfAdapter.cpp` — container/JSON/accessor/sparse validation, the node walk,
  Draco/meshopt/KTX2/WebP branches; `GltfImportWorker.cpp` entry.
- `import-worker/src/DracoDecodeAdapter.cpp`, `MeshoptDecodeAdapter.cpp`,
  `TextureTranscodeAdapter.cpp`, `WebpDecodeAdapter.cpp`, `WicImageDecodeAdapter.cpp`.
- `SECURITY.md:39-40` explicitly lists "compressed glTF (Draco, meshopt, KTX2/Basis, WebP) paths"
  as in scope; `docs/design/09...md:177-192` promises the targets.
- SEC-02 will tighten decode ordering; fuzz seeds should cover the new checks.

## Scope
- [ ] Add `GltfFuzz` driving the GLB container parser, JSON preflight, accessor/bufferView range
      validation, sparse accessors, node graph, and the adapter normalization with a bounded virtual
      sidecar map (mirroring the FbxFuzz virtual-sidecar pattern).
- [ ] Add a codec-domain target (or domain in `GltfFuzz`) for Draco bitstreams, meshopt streams,
      KTX2/Basis headers/levels, and WebP/PNG/JPEG metadata paths, including the decoder-input
      preflights added by SEC-02.
- [ ] Seed preparers per target; minimize any finding into the immutable corpus and add regression
      tests.
- [ ] If feasible without destabilizing the build, build Draco/meshopt/KTX/WebP from the pinned
      source with sanitizer instrumentation for the target; if not, document exactly what is not
      instrumented (as the existing README does) and keep the real-worker Job boundary as the
      containment evidence.
- [ ] Document the targets and a bounded smoke run; wire into the SEC-13 fuzz-smoke step.

## Out of scope
- STL/PLY/OBJ (→ SEC-15); provider pipeline (→ SEC-17).
- Upstream library patches.

## Design notes
- The GLB/JSON surface can be driven without vcpkg where possible, but the real adapter code is the
  target of record; prefer instrumenting product code even when a library is not.
- Keep the envelope compact (format selector + bytes + bounded virtual sidecar map) exactly like
  `FbxFuzz`/`UsdFuzz`, so corpus entries are portable across domains.
- A finding is not closed until a minimized seed and a regression test exist.

## Done when
- [ ] The target(s) build with ASan and complete a bounded run with no findings.
- [ ] Seeds for GLB truncation, hostile counts, sparse ranges, Draco mismatch, KTX2 over-declared
      dimensions, and WebP metadata are committed.
- [ ] `tests/fuzz/README.md` states coverage and limitations; Hand-off records runtimes/findings.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
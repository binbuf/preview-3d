---
verify: x64\Release\Tests.Unit.exe "~[graphics]"
---

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

**Landed.** `tests/fuzz/GltfFuzz.{cpp,vcxproj}` — a standalone ASan+libFuzzer target linking the real
`import_worker::ImportGltf` and the Draco/meshopt/KTX2/WebP/WIC codec adapters, with seven envelope
domains (`Adapter`, `Draco`, `Meshopt`, `Ktx2`, `WebP`, `WicRaster`, `Sniff`). `prepare_gltf_seeds.py`
materializes 34 deterministic seeds (GLB truncation, hostile counts, sparse out-of-range, Draco
declared-vs-expected mismatch, KTX2 over-declared dimensions, WebP/PNG/JPEG metadata, plus the
committed Draco/meshopt/WebP/Basis fixtures). `tests/fuzz/README.md` documents the target, domains,
build/run, and limitations. ADR-0045 records the boundary decision; design/09's Fuzzing section and
`docs/security/PROGRESS.md` point at it.

**Deviation — two upstream decoder findings block the "no findings" criterion and CI promotion.**
A bounded run found two deterministic crashes in the pinned, non-instrumented third-party decoders,
reachable from the real worker path:

1. **KTX-Software 4.4.2 / basisu ETC1S** — `TranscodeKtx2BasisImage` (via `ImportGltf` with an
   embedded `KHR_texture_basisu` image) crashes in
   `basist::basisu_lowlevel_etc1s_transcoder::transcode_slice` (`basisu_transcoder.cpp:8013`). The
   input is a structurally valid KTX2 that passes `model_core::PreflightKtx2`; a two-byte mutation of
   the frozen `interactive-viewer/test-assets/basisu_sample.ktx2` is enough. Minimized:
   `tests/fuzz/corpus/gltf/basislz-etc1s-crash.env` + `.ktx2`.
2. **fastgltf 0.9.0 base64** — `ImportGltf`'s `fastgltf::Parser::loadGltf` heap-overflows in
   `fastgltf::base64::fallback_decode_inplace` (`base64.cpp:413`) on a `.gltf` data URI whose base64
   length is not a multiple of four. Minimized: `tests/fuzz/corpus/gltf/fastgltf-base64-overflow.env`.

Upstream patches are out of scope (task Out-of-scope). Because of this, the `Ktx2` domain calls
`PreflightKtx2` for BasisLZ/ETC1S and only transcodes UASTC/uncompressed containers, the valid
BasisLZ GLB is excluded from the generated adapter seeds (the crash seed lives only in the findings
corpus), and **`GltfFuzz` was deliberately not added to the `fuzz-smoke` matrix** — a smoke run
rediscovers both crashes within ~20–60 s. Adding a guaranteed-red nightly was judged worse than
recording the finding. This deviates from the Scope bullet "wire into the SEC-13 fuzz-smoke step";
see the follow-up below.

**Check results.**
- `MSBuild tests\fuzz\GltfFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=<root>\" /p:VcpkgRoot=C:\vcpkg /p:VcpkgManifestInstall=false /m:1` → success (ASan + `/fsanitize=fuzzer`).
- `python tests/fuzz/prepare_gltf_seeds.py TestResults/security-t16/gltf-seeds-2` → 34 seeds.
- Bounded run (`-max_total_time=60`): exited non-zero on the two findings above; per-domain/no-crash
  confirmation is not claimable while these seeds exist, and the target is intentionally kept out of CI.
- `x64\Release\Tests.Unit.exe "~[graphics]"` → All tests passed (131,544 assertions in 307 cases).
- No product code changed.

**Next task must know.** The two findings need a human decision: patch/upgrade the pinned decoders,
add product-owned mitigations (an ETC1S global-data guard in `Ktx2Preflight`; a data-URI base64
length/size check before `loadGltf`), or formally accept them as contained worker crashes. Only after
that should the findings seeds join the smoke corpus and `GltfFuzz` be added to `fuzz-smoke` (one
target per runner, then promote the lane). `Tests.Unit.exe` is green and untouched.
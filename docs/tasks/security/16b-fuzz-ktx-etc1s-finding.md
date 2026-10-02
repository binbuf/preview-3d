---
verify: x64\Release\Tests.Unit.exe "~[graphics]"
---

# T16b — KTX2/BasisLZ ETC1S decoder finding and GltfFuzz smoke promotion

## Goal
Close the one remaining SEC-16 fuzzing blocker: a deterministic null-deref in the pinned
KTX-Software 4.4.2 / basisu ETC1S transcoder reachable from the real glTF adapter. Once it (and
the already-fixed fastgltf base64 finding) is mitigated, promote `GltfFuzz` and its seed preparer
into the `fuzz-smoke` matrix so glTF and the compressed codecs are exercised continuously.

## Context (read first)
- `docs/tasks/security/16-fuzz-gltf-and-codecs.md` Hand-off — what SEC-16 landed and the exact split
  of the two findings.
- `docs/design/adr/0045-gltf-codec-fuzz-findings.md` — the current "smoke promotion blocked" record.
- `tests/fuzz/corpus/gltf/README.md` and the minimized seed `basislz-etc1s-crash.{env,ktx2}`.
- `shared/model-core/include/model_core/Ktx2Preflight.h` — where the ETC1S/BasisLZ structural guard
  belongs if the chosen policy is a preflight rejection.
- `import-worker/src/TextureTranscodeAdapter.cpp` — `TranscodeKtx2BasisImage`, the product path that
  calls `ktxTexture2_TranscodeBasis` and enters the crashing `transcode_slice`.
- `tests/import-isolation/TextureTranscodeAdapterTests.cpp` — in-process KTX2 adapter test pattern.
- `tests/fuzz/README.md` (glTF section) and `.github/workflows/ci.yml` (existing `fuzz-smoke`
  entries from SEC-15) — what promotion requires.

## Finding to close
`basist::basisu_lowlevel_etc1s_transcoder::transcode_slice` (`basisu_transcoder.cpp:8013`) null-derefs
on an embedded `KHR_texture_basisu` ETC1S image whose BasisLZ supercompression global data is mutated
(two bytes off the frozen `interactive-viewer/test-assets/basisu_sample.ktx2`). The container passes
`model_core::PreflightKtx2` (the header, level index and metadata ranges are all valid); the fault is
in the third-party ETC1S bitstream decode, where the selector Huffman model is uninitialized for the
mutated global data. Today the only containment is the AppContainer/Job process boundary, which
violates design/09's "corrupt optional texture uses a deterministic fallback" contract.

## Scope
- [ ] Characterize the exact malformed field (which BasisLZ global-data value defeats the decoder's
      initialization) and choose a mitigation. Record the decision in a new ADR (0045 is SEC-16's).
      Candidate policies, in preference order:
      1. product-owned ETC1S global-data structural guard in `PreflightKtx2` (validate the ETC1S
         global header: endpoint/selector counts, codebook byte-length sums vs `sgdLength`, and the
         derived Huffman-table extent) before `ktxTexture2_TranscodeBasis`; or
      2. fail-closed rejection of BasisLZ/ETC1S containers (capability regression — justify in the
         ADR and update the `basisu_*` corpus expectations); or
      3. an upstream KTX-Software/basisu fix or version bump via the vcpkg port (outside the original
         SEC-16 scope; escalate if chosen).
- [ ] Implement the chosen guard/policy and convert the minimized seed into a regression that asserts
      the product rejects (or contains) it without a crash: a `[texture-transcode][security]` case and
      a real-worker `GltfImportTests.cpp` case.
- [ ] Move `basislz-etc1s-crash.env`/`.ktx2` (and the now-safe `fastgltf-base64-overflow.env`) into
      the generated smoke seeds, add `GltfFuzz` + `prepare_gltf_seeds.py` to the `fuzz-smoke` matrix in
      `.github/workflows/ci.yml`, and run a bounded smoke with no findings.
- [ ] Reconcile `ADR-0045`, `tests/fuzz/README.md`, `tests/fuzz/corpus/gltf/README.md`, and
      `docs/design/09-quality-performance-and-security.md` to drop the "smoke promotion blocked" note.

## Out of scope
- STL/PLY/OBJ fuzzing (→ SEC-15); provider pipeline fuzz + surrogate soak (→ SEC-17).
- The fastgltf 0.9.0 base64 data-URI overflow, already fixed in SEC-16 (`model_core::ValidateGltfDataUri`
  in `GltfDataUriPreflight.h`, wired into the `GltfAdapter.cpp` simdjson preflight).

## Design notes
- Prefer a bounded, allocation-free preflight in `model_core` (the `DracoPreflight.h`/`Ktx2Preflight.h`
  pattern) over a post-hoc catch; a null-deref inside the library is not catchable.
- Never claim per-payload cancellation for the KTX2 transcode (ADR-0030); that is unchanged.
- `GltfFuzz` must not enter `fuzz-smoke` until the crash is actually fixed — a guaranteed-red nightly
  is worse than a deferred lane.

## Done when
- [ ] The chosen mitigation has an ADR and lands with a regression that fails on the old code.
- [ ] `GltfFuzz` builds and a bounded smoke over the promoted corpus exits 0 with no ASan findings.
- [ ] `GltfFuzz` is in the `fuzz-smoke` matrix; `Tests.Unit "~[graphics]"` and the targeted
      `Tests.ImportIsolation` cases are green; docs reconciled.
- [ ] Hand-off filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_

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

**Landed.** The remaining SEC-16 fuzzing blocker is closed and `GltfFuzz` is
promoted into the smoke lane.

- **Characterized the malformed field.** KTX-Software 4.4.2's
  `ktxTexture2_transcodeLzEtc1s` ignores the `bool` returns of basisu's
  `decode_palettes`/`decode_tables` and then transcodes. The minimized two-byte
  mutation (`basislz-etc1s-crash.ktx2`, SGD offsets 109/110 of the 208-byte
  BasisLZ global data in `basisu_sample.ktx2`) corrupts the Huffman-table blob so
  the third `read_huffman_table` reads `num_codelength_codes == 0`; `decode_tables`
  returns false, leaving `m_selector_model` empty, and `transcode_slice` indexes
  its empty lookup (`basisu_transcoder.cpp:8013`). All KTX2 structural fields stay
  valid, so a bounds-only guard cannot detect it.
- **Found a second class while validating the promoted corpus.** A one-byte
  mutation of the selector codebook (file offset 296) makes `decode_palettes`
  reject an unsupported selector-codebook variant; KTX ignores that too, leaving
  the selector objects sized but never `init_flags()`-initialized, so
  `transcode_slice` feeds a garbage `m_lo_selector`/`m_hi_selector` to
  `convert_etc1s_to_bc7_m5_color` and indexes a `[4][4]` table out of bounds.
- **Chose policy #1 and implemented it.** KTX 4.4.2 vendors basisu under
  `external/basisu` (BASISD_LIB_VERSION 116) inside `ktx.lib`; the standalone
  `basisu` vcpkg headers are a different ABI, so the product cannot instantiate the
  class. New `model_core::ValidateEtc1sGlobalData`
  (`shared/model-core/include/model_core/Etc1sTablePreflight.h`) is a bounded,
  allocation-free port of `bitwise_decoder` + `huffman_decoding_table::init` +
  `read_huffman_table`; it replays both `decode_palettes`' deciding reads (the four
  endpoint tables and the selector-codebook variant flags) and `decode_tables`'
  shape (four tables, 13-bit selector-history-buffer size), plus SGD
  header/count/codebook-length checks. It is called from `PreflightKtx2` for
  `vkFormat==0 && supercompression==1`, before `ktxTexture2_CreateFromMemory`, so
  both the worker and the provider reject the container. No ETC1S capability
  regression. ADR: `docs/design/adr/0046-etc1s-global-data-preflight.md`.
- **Regressions.** `tests/import-isolation/TextureTranscodeAdapterTests.cpp`
  `[texture-transcode][security]` asserts the crash KTX2 is rejected before the KTX
  library (`Ktx2DecoderInvocations` unchanged) while `basisu_sample.ktx2` still
  transcodes. `GltfImportTests.cpp` `[gltf-import][texture][security]` strips the
  `.env` envelope and drives the real worker, asserting the deterministic
  fallback. `Tests.ImportIsolation.vcxproj` defines `PREVIEW3D_FUZZ_CORPUS_DIR`.
- **Smoke promotion.** `prepare_gltf_seeds.py` promotes both minimized `.env`
  findings, the extracted `.ktx2`, and the valid `basisu_textured_triangle.glb`
  (38 seeds); `GltfFuzz`'s `Ktx2` domain now drives the real ETC1S transcode for
  every accepted container; `.github/workflows/ci.yml` `fuzz-smoke` has a `glTF`
  entry.

**Deviation — one extra promoted-corpus finding fixed.** The re-enabled real
transcode and the promoted seeds found an unrelated product bug: `GltfAdapter.cpp`
`ConvertPrimitive` indexed `asset.materials[*primitive.materialIndex]` for
`normalTexture` without a bounds check. fastgltf keeps a primitive's `material`
index even when the top-level `materials` array is absent (a corrupted JSON key is
enough), so an empty array null-derefs. Fixed by bounding the index against
`asset.materials.size()`; real-worker regression
`GltfImportTests.cpp [gltf-import][security]`. This was required for the lane to be
green.

**Check results.**
- `GltfFuzz.exe tests/fuzz/corpus/gltf/basislz-etc1s-crash.env -runs=1` → exit 0
  (was an ASan null-deref before the guard).
- `python tests/fuzz/prepare_gltf_seeds.py TestResults/security-t16b/gltf-seeds` →
  38 seeds.
- `GltfFuzz.exe <38 seeds> -max_total_time=60 -timeout=5 -rss_limit_mb=1024
  -max_len=1048576 -print_final_stats=1 -verbosity=0` → exit 0, 55,945 units,
  peak RSS 351 MB, no ASan finding. A separate 180 s run executed 187,491 units
  with no finding.
- `Tests.Unit.exe "~[graphics]"` → All tests passed (131,557 assertions in 312
  test cases).
- `Tests.ImportIsolation.exe "[texture-transcode]"` → 7 cases green (the
  `[security]` subset now also mutates the selector codebook and asserts
  rejection); `"[gltf-import][security]"` → 3 cases green.
- Release rebuilds: `Preview3DImportWorker`, `Preview3DThumbnailProvider`,
  `GltfFuzz`, `Tests.Unit`, `Tests.ImportIsolation` all succeeded.

**Next task must know.** `GltfFuzz` is in the scheduled/nightly `fuzz-smoke`
matrix but still not a required merge gate; SEC-17 adds the provider-soak entry
and promotes the lane. Any KTX-Software/basisu bump must re-validate the
`Etc1sTablePreflight.h` port against the new `read_huffman_table`/`decode_tables`
and rerun the promoted corpus before sign-off. `Tests.Unit` is green.

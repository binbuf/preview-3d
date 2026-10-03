# 0046 — ETC1S/BasisLZ global-data preflight and GltfFuzz smoke promotion

## Status
accepted

## Context
SEC-16 found that the pinned KTX-Software 4.4.2 / vendored basisu 1.16 ETC1S
transcoder reachable from `import_worker::TranscodeKtx2BasisImage` (and the
thumbnail provider's `TryDecodeKtx2`) null-dereferences in
`basisu_lowlevel_etc1s_transcoder::transcode_slice` on a structurally valid
BasisLZ KTX2. The root cause is in KTX-Software's
`ktxTexture2_transcodeLzEtc1s`: it calls `decode_palettes`/`decode_tables` but
ignores their `bool` results and then transcodes unconditionally. When
`decode_tables` fails, `m_selector_model` stays empty and `transcode_slice`
indexes the empty lookup table.

Characterization of the minimized two-byte mutation (offsets 109/110 of the
208-byte supercompression-global-data blob in the frozen
`interactive-viewer/test-assets/basisu_sample.ktx2`): it corrupts the Huffman
table blob so that the third `read_huffman_table` reads
`num_codelength_codes == 0`, which basisu rejects. Every KTX2 structural field
(header, level index, metadata ranges, SGD lengths) remains valid, so a
bounds-only guard cannot detect it; only replaying the decoder's own read can.

A second class was found while validating the promoted corpus: a one-byte
mutation of the selector codebook (file offset 296) makes `decode_palettes`
reject an unsupported selector-codebook variant. KTX ignores that failure too,
leaving the selector objects sized but never `init_flags()`-initialized, so
`transcode_slice` passes a garbage `m_lo_selector`/`m_hi_selector` into
`convert_etc1s_to_bc7_m5_color` and indexes a `[4][4]` table out of bounds.
Both classes have to be rejected at preflight; neither is a structural-field
problem.

Calling the transcoder class directly from product code is not possible: the
low-level symbols live inside `ktx.lib`, compiled from KTX-Software's vendored
`external/basisu` (BASISD_LIB_VERSION 116), while the standalone `basisu`
vcpkg package exposes a different, newer ABI. Instantiating that class from a
product TU against the wrong header would be an object-layout mismatch.

## Decision
- Add `model_core::ValidateEtc1sGlobalData`
  (`shared/model-core/include/model_core/Etc1sTablePreflight.h`), a bounded,
  allocation-free port of the pinned decoder's reads. It parses the BasisLZ SGD
  header and image-descriptor offset, validates the endpoint and selector counts
  and the codebook byte-length sums against `sgdLength`, then re-implements
  `bitwise_decoder`, `huffman_decoding_table::init` and `read_huffman_table`.
  It replays the four endpoint-table reads and the selector-codebook variant
  flags from `decode_palettes`, and the four table reads plus the 13-bit
  selector-history-buffer read from `decode_tables`, returning false exactly
  when either would. (The two codebook decode loops have no failure returns of
  their own, so their bits need not be replayed; the selector reader is
  re-initialized, so the endpoint loop cannot affect the decision.)
- Call it from `model_core::PreflightKtx2` for
  `vkFormat == 0 && supercompression == KTX_SS_BASIS_LZ`, before
  `ktxTexture2_CreateFromMemory`/`ktxTexture2_TranscodeBasis`. Both the import
  worker and the thumbnail provider share the guard. This is candidate policy
  #1; the fail-closed rejection of all ETC1S (policy #2) was not needed and
  would have been a capability regression.
- Promote `GltfFuzz`: add it to the `fuzz-smoke` matrix in
  `.github/workflows/ci.yml`; add the valid `basisu_textured_triangle.glb` and
  both minimized findings (`fastgltf-base64-overflow`,
  `basislz-etc1s-crash`) to the generated smoke seeds; drive the real ETC1S
  transcode in the `Ktx2` domain now that accepted containers are safe.
- Fix the out-of-range `primitive.materialIndex` access found by the promoted
  corpus (`GltfAdapter.cpp` `ConvertPrimitive`): bound it against
  `asset.materials.size()` before reading `normalTexture`, matching
  `ResolveMaterial`'s existing check.

## Consequences
- A corrupt embedded ETC1S texture now fails closed at preflight and falls back
  deterministically, meeting `docs/design/09`'s "corrupt optional texture uses
  a deterministic fallback" contract without the real-worker Job boundary.
- Only the ETC1S read path (`decode_palettes`' deciding reads and
  `decode_tables`) is replicated. Any KTX-Software/basisu version change must
  re-validate this port against the new `basisu_transcoder_internal.h` and
  rerun the promoted corpus before sign-off (add to the dependency-update
  checklist in design/09).
- `GltfFuzz` is in the `fuzz-smoke` lane (one target per runner), which
  SEC-17/T25 promoted into the required merge gate.
- The promoted-corpus material-index fix is a product hardening change with a
  real-worker regression in `GltfImportTests.cpp`.
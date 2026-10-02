# GltfFuzz findings corpus

Immutable, minimized reproducers for `GltfFuzz`. Both classes are now fixed and
`prepare_gltf_seeds.py` promotes the `.env` seeds (and the extracted
`basislz-etc1s-crash.ktx2`) into the generated smoke seeds, so libFuzzer
exercises the fixed rejection continuously. They are retained here as the
canonical minimal reproducers and drive the real-worker regressions in
`tests/import-isolation/`.

## basislz-etc1s-crash

`basislz-etc1s-crash.env` is a `GltfFuzz` Adapter-domain envelope carrying a
self-contained GLB whose embedded KTX2 image is a two-byte mutation of the
committed `interactive-viewer/test-assets/basisu_sample.ktx2` (and of the KTX2
inside `interactive-viewer/test-assets/corpus/basisu_textured_triangle.glb`).
`basislz-etc1s-crash.ktx2` is the same mutated KTX2 extracted, for the Ktx2
domain and for independent `model_core::PreflightKtx2` tests.

Reproduce (Release + ASan build of `GltfFuzz.vcxproj`):

```powershell
tests/fuzz/x64/Release/GltfFuzz.exe tests/fuzz/corpus/gltf/basislz-etc1s-crash.env -runs=1
```

Result: a deterministic `AddressSanitizer: access-violation` (null read) in
`basist::basisu_lowlevel_etc1s_transcoder::transcode_slice`
(`basisu_transcoder.cpp:8013`, `sym_codec.decode_huffman(m_selector_model)`)
called from `import_worker::TranscodeKtx2BasisImage`
(`import-worker/src/TextureTranscodeAdapter.cpp`). The header/level preflight
(`model_core::PreflightKtx2`) accepts the container; the crash is in the
third-party ETC1S bitstream decoder, whose selector Huffman model is
uninitialized for this global-data mutation. This is reachable from the real
worker path (`import_worker::ImportGltf` with an embedded `KHR_texture_basisu`
image) and is contained only by the AppContainer/Job process boundary, matching
the "corrupt optional texture uses a deterministic fallback" contract being
violated.

Before SEC-16b, the mutation also crashed via the Ktx2 domain on the extracted
container without the GLB wrapper.

Disposition: **fixed in SEC-16b** by a product-owned ETC1S global-data guard,
`model_core::ValidateEtc1sGlobalData` (`shared/model-core/include/model_core/Etc1sTablePreflight.h`),
called from `PreflightKtx2` before `ktxTexture2_TranscodeBasis`. It replays basisu's bounded
`decode_palettes`/`decode_tables` deciding reads and rejects the container before the library is
entered; the real-worker
regression is the `[gltf-import][texture][security]` case in `tests/import-isolation/GltfImportTests.cpp`
and the `[texture-transcode][security]` case in `TextureTranscodeAdapterTests.cpp`. See
`docs/design/adr/0046-etc1s-global-data-preflight.md`.

## fastgltf-base64-overflow

`fastgltf-base64-overflow.env` is an Adapter-domain envelope carrying a plain
`.gltf` whose `buffers[0].uri` is a base64 data URI whose encoded payload
length is not a multiple of four. `fastgltf::base64::fallback_decode_inplace`
(`fastgltf` 0.9.0 `base64.cpp:413`, reached via
`Parser::decodeDataUri` -> `parseBuffers` -> `loadGltfJson`) writes past the
destination the product's `setBufferAllocationCallback` sized from
`base64::getOutputSize`. AddressSanitizer reports a
`heap-buffer-overflow` (via the intercepted `memcpy`/store) with the
allocation in the product's `std::vector<std::byte>`.

Reproduce:

```powershell
tests/fuzz/x64/Release/GltfFuzz.exe tests/fuzz/corpus/gltf/fastgltf-base64-overflow.env -runs=1
```

The product's simdjson preflight accepts the JSON and the product's data-URI cap is not reached.
**Fixed in SEC-16.** `model_core::ValidateGltfDataUri`
(`shared/model-core/include/model_core/GltfDataUriPreflight.h`) now rejects a base64 `data:` payload
whose encoded length is not a multiple of four (and over-cap/mis-padded payloads) in the
`GltfAdapter.cpp` simdjson preflight before `loadGltf`, so this seed is a safe regression:
`GltfFuzz.exe tests/fuzz/corpus/gltf/fastgltf-base64-overflow.env -runs=1` exits 0. It no longer
blocks CI smoke promotion; the ETC1S finding above is also closed (SEC-16b).
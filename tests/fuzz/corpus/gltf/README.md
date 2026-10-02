# GltfFuzz findings corpus

Immutable, minimized reproducers for `GltfFuzz`. These are **not** fed to the
bounded CI smoke: a finding must be fixed (or its class mitigated) before its
seed joins the smoke training set, otherwise libFuzzer rediscovers it and the
run fails. They are retained so the crash is reproducible and so a future
regression test can assert the intended rejection.

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

The mutation also crashes via the Ktx2 domain on the extracted container
without the GLB wrapper.

Disposition: upstream KTX-Software / basisu fix, or a product-owned ETC1S
global-data guard, is required before this seed can join the smoke corpus. See
`docs/security/PROGRESS.md` ("T16 — SEC-16") and the task Hand-off.

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

The product's simdjson preflight accepts the JSON and the product's data-URI
cap is not reached. Disposition: upstream fastgltf fix, or a product-owned
data-URI base64 preflight (length multiple-of-four and decoded-size cap) before
`loadGltf`. Same containment note as above. This is a second, independent
reason the CI smoke promotion is blocked.
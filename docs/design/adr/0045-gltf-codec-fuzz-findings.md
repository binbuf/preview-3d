# 0045 — glTF/codec fuzz target and third-party decoder findings

## Status
accepted

## Context
`docs/design/09-quality-performance-and-security.md` ("Fuzzing") promises a
standalone, no-GPU target covering "GLB/glTF JSON/accessor/range validation"
and "Draco bitstreams, KTX2/Basis level metadata/transcode boundary, WebP, and
expanded texture metadata". SEC-16 supplies it. The real adapter
(`import_worker::ImportGltf`) owns container/JSON/accessor/sparse validation and
routes compressed payloads through product-owned preflights
(`model_core::PreflightKtx2`, `model_core::ValidateDracoCounts`) into the pinned
third-party decoders (fastgltf/simdjson/draco/meshoptimizer/ktx/basisu/libwebp).
Those libraries are restored from the root vcpkg manifest without sanitizer
instrumentation.

## Decision
- `GltfFuzz` links the real adapter and codec adapters. One 8-byte envelope
  selects `Adapter`, `Draco`, `Meshopt`, `Ktx2`, `WebP`, `WicRaster`, or
  `Sniff`; codec domains carry a small count/stride header so the SEC-02
  preflights are reachable.
- The `Adapter` domain runs only the always-self-contained source path
  (`sidecarClient`/`batchSink` null). External sidecar resolution uses the
  pipe-based `SidecarFileClient`; its reference validation stays in SEC-04 and
  the real worker.
- The `Ktx2` domain drives `PreflightKtx2` for BasisLZ/ETC1S containers and the
  real transcode for UASTC/uncompressed containers. The third-party ETC1S
  transcode is excluded from the bounded smoke because it crashes on malformed
  global data.
- The target sets `_DISABLE_STL_ANNOTATION` so an ASan TU can link the pinned
  non-ASan static libraries without `LNK2038` (`annotate_*=0` vs `1`).
- Two deterministic findings are minimized under `tests/fuzz/corpus/gltf/`
  rather than folded into the smoke corpus: a KTX-Software/basisu ETC1S
  `transcode_slice` null-deref, and a fastgltf base64 data-URI heap overflow.
  They are kept out of the generated seeds and out of the `fuzz-smoke` matrix
  until a product mitigation or upstream fix exists.

## Consequences
- The glTF/codec target is built and runnable but is not promoted into the
  `fuzz-smoke` matrix; doing so is blocked on the two third-party decoder
  findings.
- The real AppContainer/Job worker remains the production containment for
  worker-process crashes; the design's "corrupt optional texture falls back"
  contract is not met for the ETC1S case until it is fixed.
- A findings seed must not join the smoke corpus before its class is mitigated
  or libFuzzer rediscovers it.
- The target needs the root vcpkg manifest restored and must be built
  individually (MSB8028 shared intermediate directory).
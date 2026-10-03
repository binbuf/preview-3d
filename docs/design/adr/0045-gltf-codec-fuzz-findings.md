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
- The `Ktx2` domain drives `PreflightKtx2` and the real transcode for every
  container. (Originally the ETC1S transcode was excluded because it crashed on
  malformed global data; SEC-16b added `model_core::ValidateEtc1sGlobalData` to
  `PreflightKtx2`, so the real ETC1S transcode is safe for accepted containers;
  see [ADR-0046](./0046-etc1s-global-data-preflight.md).)
- The target sets `_DISABLE_STL_ANNOTATION` so an ASan TU can link the pinned
  non-ASan static libraries without `LNK2038` (`annotate_*=0` vs `1`).
- Two deterministic findings were minimized under `tests/fuzz/corpus/gltf/`
  rather than folded into the smoke corpus: a fastgltf 0.9.0 base64 data-URI
  heap overflow and a KTX-Software/basisu ETC1S `transcode_slice` null-deref.
  The fastgltf class is fixed in-product by `model_core::ValidateGltfDataUri`
  (`GltfDataUriPreflight.h`), called from the `GltfAdapter.cpp` simdjson
  preflight before `loadGltf`; its seed is now a safe regression. The ETC1S
  class was closed by SEC-16b, which added the ETC1S global-data preflight and
  promoted `GltfFuzz` into `fuzz-smoke` (ADR-0046).

## Consequences
- The glTF/codec target is promoted into the `fuzz-smoke` matrix; both SEC-16
  decoder findings (fastgltf base64 and KTX-Software/basisu ETC1S) are closed.
- The real AppContainer/Job worker remains the production containment for
  worker-process crashes; the design's "corrupt optional texture falls back"
  contract is now met for the ETC1S case by the product-owned preflight.
- A findings seed must not join the smoke corpus before its class is mitigated
  or libFuzzer rediscovers it.
- The target needs the root vcpkg manifest restored and must be built
  individually (MSB8028 shared intermediate directory).
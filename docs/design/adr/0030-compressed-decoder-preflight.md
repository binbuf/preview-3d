# 0030 — Compressed-decoder preflight before library allocation

## Status
accepted

## Context
`KHR_draco_mesh_compression` and KTX2/Basis payloads are decoded inside the zero-capability
import worker and inside the Explorer thumbnail provider. Both handed attacker-controlled bytes
straight to the third-party decoder and only checked the decoded result against the product
budget afterwards:

- `draco::Decoder::DecodeMeshFromBuffer` reads the declared connectivity preamble and, in the
  edgebreaker path, resets a `CornerTable` sized by the declared face count *before* it validates
  that the stream contains that many symbols. A tiny bitstream could therefore drive a
  multi-gigabyte working set.
- `ktxTexture2_CreateFromMemory` walks the level index and can allocate or Zstd-expand storage
  from file-declared sizes before the provider looked at `baseWidth`/`baseHeight`.

Neither decoder exposes a progress or cancellation callback, so a decode that starts cannot be
interrupted except by process termination.

## Decision
- Add shared, header-only preflights in `shared/model-core/include/model_core/`:
  `DracoPreflight.h` and `Ktx2Preflight.h`. They are the single source of truth for the header
  layout and are called by both the worker adapter and the provider adapter; the layouts are not
  forked.
- `PreflightKtx2` validates the fixed 80-byte header, depth/layer/face counts, mip count, the
  metadata ranges, the level index (in-bounds, non-overlapping), the declared uncompressed sizes
  and the worst-case RGBA8 expansion against the caller's encoded/decoded/pixel/dimension
  budgets, and maps `vkFormat` through the closed product allowlist.
- `ValidateDracoCounts` parses the fixed header, the optional metadata block (via Draco's own
  `MetadataDecoder`, so the layout is reused, not reimplemented), the edgebreaker traversal
  selector and the declared vertex/face/symbol counts. It rejects a declared face count that
  disagrees with the glTF accessors (`MalformedData`) and a declared working set over budget
  (typed limit) before the decoder is invoked. The existing post-decode checks remain as
  defense in depth.
- A mismatch is `MalformedData`; an over-budget declaration is the caller's typed limit
  (`DracoPrimitiveLimit`). Draco uses the worker's 512 MiB / 10 M-triangle budget and the
  thumbnail host's 96 MiB / 1 M-triangle budget; KTX2 uses each path's existing byte/pixel caps.
- The worker's pre-existing inline KTX2 preflight is replaced by the shared function; the
  provider gains the same preflight.
- **Uninterruptible window.** Draco and KTX expose no cancellation. A payload that passes the
  preflight still decodes to completion; the working-set/triangle caps above bound its cost and
  the adapters' existing compressed-span caps bound the input. A request deadline is therefore
  honoured between payloads, not inside one third-party decode. `docs/design/02`'s "bounded
  cancellable decode jobs" is narrowed to "bounded decode jobs; cancellation is cooperative
  between payloads".

## Consequences
- `Tests.Unit` and `Tests.ImportIsolation` call the preflight directly and through the adapters
  with hostile Draco count preambles and extreme KTX2 headers, and use the exported invocation
  counters to assert the third-party decoder was never reached.
- Later fuzz work (SEC-16) can seed the preflight functions directly; they never allocate from a
  file-declared size.
- Any new compressed codec adopted behind these adapters must add the same preflight + counter
  pattern rather than calling the library first.
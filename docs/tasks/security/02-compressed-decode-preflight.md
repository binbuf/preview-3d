# T02 — Preflight compressed decoders before allocation

## Goal
Draco and KTX2 data is validated against product budgets before the decoder is allowed to allocate
from attacker-controlled header fields, and long decodes are cancellable, so a tiny hostile
bitstream cannot drive a multi-gigabyte working set or an uninterruptible CPU burn.

## Context (read first)
- `import-worker/src/DracoDecodeAdapter.cpp:50-81` — bounds are computed from *glTF accessor*
  counts, then `DecodeMeshFromBuffer` (`:62`) runs; the decoder's own declared point/face counts are
  checked only after it returns (`:71-81`). No cancellation hook exists.
- `thumbnail-provider/GltfFamilyAdapter.cpp:403-445` — same order; `pointCount`/`faceCount` are
  validated after `DecodeMeshFromBuffer`.
- `thumbnail-provider/GltfFamilyAdapter.cpp:536-570` — `TryDecodeKtx2` calls
  `ktxTexture2_CreateFromMemory` before checking dimensions and ignores level/layer/face counts.
  The worker KTX2 path is already correct: `import-worker/src/TextureTranscodeAdapter.cpp:80-119`
  validates the fixed header, level index, and expanded bytes before the library call — copy that
  pattern.
- `docs/design/02-system-architecture.md:164` claims "bounded cancellable decode jobs";
  `docs/design/03-file-formats-and-ingestion.md` owns the 512 MiB / 10 M-triangle decoded bound.
- Tests: `tests/import-isolation/DracoDecodeAdapterTests.cpp`,
  `tests/import-isolation/TextureTranscodeAdapterTests.cpp`, `tests/provider-host`.

## Scope
- [x] Worker Draco: parse the Draco header (magic and declared attribute/point/face counts) from the
      compressed span and reject a mismatch/over-budget before `DecodeMeshFromBuffer`; keep the
      existing post-check as defense in depth.
- [x] Worker/provider KTX2: pre-validate magic, pixel dimensions, level count, layer/face counts,
      and the worst-case expanded size from the span before `ktxTexture2_CreateFromMemory`.
- [x] Add the same KTX2 preflight to the provider path or call a shared helper; do not fork the
      header layout.
- [x] Cancellation: if the decoder exposes progress/cancel, wire it; otherwise document the
      uninterruptible window and keep the span cap small enough to bound it.
- [x] Regression tests with a Draco stream declaring counts far larger than its glTF accessors and a
      KTX2 header declaring extreme dimensions/levels; assert rejection before allocation (ledger or
      instrumentation evidence).

## Out of scope
- Patching upstream Draco/KTX behavior (record via ADR if a real cap cannot be enforced → SEC-18).
- Full fuzz lanes (→ SEC-16; add seeds there).

## Design notes
- Do not trust `mesh->num_points()` as the budget source; it is a decoder output.
- Keep the worker and provider limits in one shared header where practical
  (`thumbnail-provider/ProviderLimits.h` vs `import-worker` constants) and record any intentional
  difference.
- A mismatch is `MalformedData`; an over-budget declaration is a typed limit failure.

## Done when
- [x] `Tests.ImportIsolation` and `Tests.ProviderHost` pass with the new cases.
- [x] Evidence shows the reject happens before library allocation (ledger delta or equivalent).
- [x] Hand-off filled in.

## Hand-off

### What landed
- New shared, header-only preflights: `shared/model-core/include/model_core/DracoPreflight.h` and
  `.../Ktx2Preflight.h`. Both are called from the worker and provider adapters, so the Draco/KTX2
  header layouts are defined once.
  - `ValidateDracoCounts` parses the fixed header, skips any metadata block with Draco's own
    `MetadataDecoder`, reads the edgebreaker traversal selector (the byte `MeshEdgebreakerDecoder::InitializeDecoder`
    consumes before connectivity) and the declared vertex/face/symbol counts, then rejects:
    declared faces != accessor faces → `Malformed`; declared working set / triangles over budget →
    `OverLimit`. It uses `PointCloudDecoder::DecodeHeader` + `DecodeVarint`, so no bitstream layout
    is reimplemented.
  - `PreflightKtx2` validates the 80-byte header, depth/layer/face counts, mip count, metadata
    ranges, the level index (bounds + overlap) and worst-case RGBA8 expansion against the caller's
    encoded/decoded/pixel/dimension budgets, and maps `vkFormat` through the closed allowlist.
- `import-worker/src/DracoDecodeAdapter.cpp` and `thumbnail-provider/GltfFamilyAdapter.cpp` call the
  preflight before `DecodeMeshFromBuffer`; the existing post-decode checks remain.
- `import-worker/src/TextureTranscodeAdapter.cpp` now calls the shared preflight (its inline copy was
  removed); `TryDecodeKtx2` in the provider does the same before `ktxTexture2_CreateFromMemory`.
- Evidence seam: `model_core::DracoDecoderInvocations()` / `Ktx2DecoderInvocations()` are incremented
  immediately before each third-party call, so tests assert the library was never reached.
- Cancellation: neither decoder exposes a cancel hook; ADR-0030 records the uninterruptible window
  and design/02's "bounded cancellable decode jobs" was narrowed accordingly.

### Tests
- `tests/unit/ProviderGltfAdapterTests.cpp` (`[provider][gltf][security]`): direct preflight
  classification, a GLB whose Draco stream declares counts inconsistent with its accessors
  (`MalformedData`), a GLB declaring an over-budget vertex count (`DracoPrimitiveLimit`), and a GLB
  with an extreme KTX2 header (image dropped). Each asserts the invocation counter is unchanged.
- `tests/import-isolation/DracoDecodeAdapterTests.cpp` (`[draco][security]`) and
  `.../TextureTranscodeAdapterTests.cpp` (`[texture-transcode][security]`): the same hostile inputs
  through the worker adapters, asserting typed errors and unchanged counters.

### Check results
- `x64\Debug\Tests.ImportIsolation.exe`: 376/381 pass; 5 failures are the documented pre-existing
  baseline (SidecarPathResolver, ThreeMfSpike, UsdSpike). New `[security]`, `[draco]`,
  `[texture-transcode]` cases pass.
- `x64\Debug\Tests.ProviderHost.exe`: passes on rerun; the `[parallel]` case is a pre-existing
  concurrency flake (counts one transient failure in some orderings, passes alone and on rerun).
  None of its fixtures use Draco/KTX2.
- `npm test` (`x64\Release\Tests.Unit.exe`): all 350 cases / 134956 assertions pass.

### Deviations / verify-gate repairs (out of SEC-02 scope)
Two pre-existing defects made `npm test` red and had to be repaired to reach a green gate:
- `interactive-viewer/src/ui/InfoPanel.cpp` `FormatDimension` never inserted `value` into its stream,
  so every dimension row rendered as just the unit. Added the missing `<< value`.
- `tests/unit/LocalizationTests.cpp` loaded the French pack and leaked the process-global active
  locale into later cases (order-dependent `WireFormatTests` failures). Added an RAII
  `EnglishLocaleGuard` so the reset runs even when a REQUIRE aborts.
The harness stages/commits these with the task; they are unrelated to compressed decoding.

### Changed docs
- `docs/design/adr/0030-compressed-decoder-preflight.md` (new).
- `docs/design/03-file-formats-and-ingestion.md` (glTF Draco paragraph).
- `docs/design/02-system-architecture.md` (Draco boundary row).
- `docs/security/PROGRESS.md` (T02 section + follow-ups).

### Remaining work / blockers
- None blocking. `docs/design/02`'s provider row for KTX is unchanged because the KTX boundary
  already said "bounded"; only the Draco row/ADR needed the cancellation clarification.
- SEC-16 should add fuzz seeds for `PreflightKtx2` / `ValidateDracoCounts`; SEC-18 should re-read the
  ADR-0030 wording if a future codec gains a real cancel hook.
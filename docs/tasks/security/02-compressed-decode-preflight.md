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
- [ ] Worker Draco: parse the Draco header (magic and declared attribute/point/face counts) from the
      compressed span and reject a mismatch/over-budget before `DecodeMeshFromBuffer`; keep the
      existing post-check as defense in depth.
- [ ] Worker/provider KTX2: pre-validate magic, pixel dimensions, level count, layer/face counts,
      and the worst-case expanded size from the span before `ktxTexture2_CreateFromMemory`.
- [ ] Add the same KTX2 preflight to the provider path or call a shared helper; do not fork the
      header layout.
- [ ] Cancellation: if the decoder exposes progress/cancel, wire it; otherwise document the
      uninterruptible window and keep the span cap small enough to bound it.
- [ ] Regression tests with a Draco stream declaring counts far larger than its glTF accessors and a
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
- [ ] `Tests.ImportIsolation` and `Tests.ProviderHost` pass with the new cases.
- [ ] Evidence shows the reject happens before library allocation (ledger delta or equivalent).
- [ ] Hand-off filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
---
verify: x64\Release\Tests.ImportIsolation.exe
---

# T28 — Worker WIC native-copy buffer invariant

## Goal
The WIC JPEG decode path cannot overrun its tile buffer if `IWICBitmapSourceTransform::CopyPixels`
ever honors the requested extent over `cbBufferSize`.

## Context (read first)
- Follow-up audit evidence: `import-worker/src/WicImageDecodeAdapter.cpp:121-127` allocates
  `tile(size_t(outputW) * channels * 32)` (only 32 rows) but calls
  `native->CopyPixels(&rectangle, outputW, outputH, &nativeFormat, ..., outputW*channels,
  rows*outputW*channels, tile.data())` with the full image `outputH` as `uiHeight`.
- The provider raster-source path may share this helper; check both call sites before changing it.
- `outputW`/`outputH` are bounded by `maxDimension` and the byte chain is checked at `:103-105`;
  the concern is only the `uiWidth`/`uiHeight` vs buffer mismatch.

## Scope
- [ ] Confirm whether the WIC contract makes this safe; if it does, document the invariant in a
      comment. If it does not, pass a `uiHeight`/rectangle consistent with the rows written (or use
      the tile-row `IWICBitmapSourceTransform` size).
- [ ] Add a worker regression with a tall JPEG (more than one 32-row tile) that asserts the decoded
      pixel bytes stay within the allocated buffer.
- [ ] Re-check `outputW`/`outputH`/`rows` and the byte chain while there.

## Out of scope
- Other image formats (PNG/Basis/WebP) — already covered.

## Design notes
- Prefer making the copy extent explicitly consistent with the buffer over relying on the library
  honoring `cbBufferSize`.
- This is a worker path; no sandbox-boundary change.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe` passes with the new case.
- [ ] Hand-off records the contract decision.

## Hand-off
_(filled in by the implementing session)_
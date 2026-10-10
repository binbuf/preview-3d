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

### Contract decision
`IWICBitmapSourceTransform::CopyPixels` requires `uiWidth`/`uiHeight` to equal the value returned by
`GetClosestSize` (the *scaled full-image* size); `prc` then clips that already-scaled image, and
`nStride`/`cbBufferSize` describe the destination. The code already verifies
`GetClosestSize(...) == {outputW, outputH}` before entering the native path, so passing the full
`outputH` as `uiHeight` is contract-correct and must **not** be changed to the tile row count (that
would mis-scale the image). The real gap was that the scratch held only 32 rows while the call
described an `outputW x outputH` extent, so the buffer was not visibly large enough for the extent
it passed. Fixed by sizing the scratch for the full requested extent
`size_t(outputW)*channels*outputH`; the 32-row loop remains only for cancellation latency. This makes
the no-overrun property independent of the codec honoring `cbBufferSize`/`prc`. The extent is bounded:
`channels <= 4`, so the scratch is at most the base RGBA image, which the `ComputeImagePixelBytes(...)
<= maxDecodedBytes` chain check at the top of the function already bounds.

### Changes
- `import-worker/src/WicImageDecodeAdapter.cpp`: native JPEG scratch is now
  `outputW*channels*outputH` (was `outputW*channels*32`) with the contract/invariant comment.
  Re-checked `outputW`/`outputH`/`rows` and the byte chain: halving preserves aspect and is capped at
  `kMaxImageDimension`; `rows <= 32`; `outputW*channels` (stride) and `rows*outputW*channels`
  (`cbBufferSize`) both fit `UINT`; `result.pixelBytes` size is `outputW*outputH*4`.
- `tests/import-isolation/TexturePathTests.cpp`: added `EncodeJpegVerticalGradient` and a
  `[texture-decode]` case that decodes a 40x160 JPEG (five 32-row tiles) and asserts the full-height
  ramp is correct and alpha stays 255.
- `docs/design/adr/0054-wic-native-copy-buffer-invariant.md`: new accepted ADR.
- `docs/design/03-file-formats-and-ingestion.md`: texture policy sentence records the invariant.
- `docs/security/PROGRESS.md`: T28 reusable-facts section.

### Checks (Release, exit 0)
- `x64\Release\Tests.ImportIsolation.exe "[texture-decode]"` → 4 cases, 239430 assertions, all passed.
- `x64\Release\Tests.ImportIsolation.exe` full → 409 cases / 404 passed / 5 skipped / 0 failed;
  302036 assertions passed.
- Build: `MSBuild tests\import-isolation\Tests.ImportIsolation.vcxproj /p:Configuration=Release
  /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\" /p:VcpkgRoot=C:\vcpkg
  /p:VcpkgManifestInstall=false /m:1`.

### Remaining
None.
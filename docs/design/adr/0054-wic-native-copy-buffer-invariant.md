# 0054 — Size the WIC native-decode scratch for the requested transform extent

## Status
accepted

## Context
`import_worker::DecodeRasterImageWic` (`import-worker/src/WicImageDecodeAdapter.cpp`) uses the inbox
JPEG codec's `IWICBitmapSourceTransform` for native downscaling. That interface's `CopyPixels`
requires `uiWidth`/`uiHeight` to be the *scaled full-image* size returned by `GetClosestSize`; the
`prc` rectangle then clips the already-scaled image, and `nStride`/`cbBufferSize` describe the
destination. The worker copied in 32-row tiles (`prc` height `rows`) but allocated the scratch for
only those 32 rows while passing the full `outputH` as `uiHeight`. A conforming codec writes only the
clipped `rows`, so the existing code never overflowed in practice — but the buffer was not, on its
face, large enough for the transform extent the call described. A codec that honored
`uiWidth*uiHeight` over `cbBufferSize` would overrun the scratch on the first tile.

## Decision
- Keep the contract-required `uiWidth`/`uiHeight` at the `GetClosestSize` full-image size and keep
  clipping each iteration with `prc`; do not shrink `uiHeight` to the tile row count, which would
  violate the WIC contract and mis-scale the image.
- Make the buffer invariant independent of codec behavior: size the scratch for the full requested
  `outputW * channels * outputH` extent. The 32-row loop remains only to bound cancellation latency.
- The extra allocation is bounded: `channels <= 4`, so the scratch is at most the base RGBA image,
  which the existing `ComputeImagePixelBytes(...) <= maxDecodedBytes` chain check at the top of the
  function already rejects if oversized.

## Consequences
- A `CopyPixels` implementation that writes the requested extent instead of the clipped extent can
  no longer overrun the scratch; the peak scratch for the native JPEG path rises to at most one
  extra decoded image, still under `maxDecodedBytes`.
- `tests/import-isolation/TexturePathTests.cpp` decodes a 40x160 JPEG (five 32-row tiles) and asserts
  the full grayscale ramp is placed correctly, exercising the multi-iteration native path.
- No provider/thumbnail path shares this helper; the fix is confined to the import worker's WIC
  adapter, and no sandbox-boundary change is involved.
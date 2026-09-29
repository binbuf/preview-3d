#pragma once

// T13 HBITMAP boundary.
//
// The frozen rasterizer contract (CpuRasterizer.h, T04) returns a GDI-free
// `RasterImage` so it is unit-testable without Explorer; design/05 ("CPU
// renderer") makes GetThumbnail return a *top-down 32-bit premultiplied BGRA
// DIB section* whose ownership passes to Explorer. This header is that single
// Windows/GDI boundary:
//
//   ProviderOutcome CreatePremultipliedDib(const RasterImage&, HBITMAP& out);
//
// It copies the raster bytes into a DIBSection and returns its HBITMAP; it
// creates no other GDI object, so the caller has nothing but that bitmap to
// release. It is free of COM and of the provider precompiled header so the same
// translation unit compiles into Tests.Unit.exe (compiled with no GDI ownership
// assumptions).

#include "CpuRasterizer.h"
#include "ProviderErrors.h"

#include <windows.h>

namespace preview3d::provider {

// Builds a top-down (negative-height) 32-bpp BI_RGB DIB section from `image` and
// hands back its HBITMAP in `out` (null on failure). The image must be tightly
// packed premultiplied BGRA with a non-empty width/height match, otherwise
// `BadFormat` is returned. A failed DIB allocation is `OutOfMemory`. WTS_ALPHATYPE
// is set by the caller, not here.
ProviderOutcome CreatePremultipliedDib(const RasterImage& image, HBITMAP& out) noexcept;

} // namespace preview3d::provider
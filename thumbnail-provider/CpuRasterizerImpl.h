#pragma once

// T15 product-owned CPU tile rasterizer entry point.
//
// The frozen contract lives in CpuRasterizer.h (T04). This header names the one
// implementation the provider links and Tests.Unit exercises directly; it is
// PCH/COM/GDI-free so the same translation unit compiles into
// Preview3DThumbnailProvider.dll and Tests.Unit.exe (design/05, "CPU renderer").
//
// `RenderCpuTileRaster` consumes the T14 `SampledGeometry`, clamps the
// resolution to min(cx, 512), frames the verified bounds in the fixed isometric
// view, depth-tests opaque/masked triangles and round point splats, shades with
// ambient plus two fixed lights and the neutral material palette, downsamples in
// linear space and returns top-down premultiplied BGRA. On any failure `out` is
// left empty (a fabricated success image is prohibited).

#include "CpuRasterizer.h"

namespace preview3d::provider {

// The single production rasterizer implementation.
ErrorCode RenderCpuTileRaster(const RasterRequest& request, RasterImage& out) noexcept;

// `ICpuRasterizer` adapter over RenderCpuTileRaster, for call sites that prefer
// the interface.
class CpuTileRasterizer final : public ICpuRasterizer {
public:
    ErrorCode Render(const RasterRequest& request, RasterImage& out) noexcept override
    {
        return RenderCpuTileRaster(request, out);
    }
};

} // namespace preview3d::provider
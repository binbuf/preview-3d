#pragma once

// Frozen CPU rasterizer contract (T04). T15 implements the product-owned tile
// rasterizer behind it; no GPU device is created inside Explorer and no
// third-party renderer is involved.
//
// The rasterizer consumes the sampler's representative set, frames verified
// bounds in a fixed isometric view, depth-tests tiles and returns top-down,
// premultiplied BGRA pixels. It charges raster targets/scratch against the T06
// ledger and stops at a deadline checkpoint. HBITMAP/DIB creation and
// WTS_ALPHATYPE are the caller's (T13/T15) concern; this contract stays GDI-
// and Windows-type-free so it is unit-testable without Explorer.

#include "ProviderTypes.h"
#include "GeometrySampler.h"
#include "model_core/MaterialPayload.h"

#include <cstdint>
#include <span>
#include <vector>

namespace preview3d::provider {

// One render request. `requestedSize` is the caller's cx (physical pixels); the
// rasterizer clamps it independently (min(cx, 512) for nonzero cx). `materials`
// is indexed by the sample's 1-based materialIndex; a zero or out-of-range
// index selects NeutralMaterial(). `deadline` and `ledger` are owned by the
// caller and outlive the call.
struct RasterRequest {
    const SampledGeometry* geometry = nullptr;
    std::span<const model_core::MaterialPayload> materials;
    std::uint32_t requestedSize = 0;
    bool allowSupersample = true;
    Deadline* deadline = nullptr;
    AllocationLedger* ledger = nullptr;
};

// A finished raster: top-down, tightly packed, premultiplied BGRA (4 bytes per
// pixel), width * height * 4 bytes. Construction is the rasterizer's; the
// caller copies it into a DIB section and owns the resulting HBITMAP.
struct RasterImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bgraPremultiplied;
};

class ICpuRasterizer {
public:
    ICpuRasterizer() = default;
    ICpuRasterizer(const ICpuRasterizer&) = delete;
    ICpuRasterizer& operator=(const ICpuRasterizer&) = delete;
    virtual ~ICpuRasterizer() noexcept = default;

    // Renders request.geometry into `out`. Returns None on success, or a typed
    // failure. On failure `out` is left empty; the provider never returns a
    // partial or fabricated image.
    virtual ErrorCode Render(const RasterRequest& request, RasterImage& out) noexcept = 0;
};

} // namespace preview3d::provider
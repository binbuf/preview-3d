#pragma once

// Shared bounded RGBA8 finalization for the product-owned TGA/DDS decoders.
// Both decoders produce tightly packed, top-left-origin RGBA8 at their native
// size; this helper applies the same maxDimension downscale policy and
// color-aware mip generation as the WIC path, without allocating beyond the
// decoded-texture budget. Normal maps keep the GenerateRasterMips
// renormalization rule.

#include "model_core/PixelFormats.h"
#include "TextureDecodePolicy.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace import_worker {

namespace raster_rgba_detail {

inline float DecodeChannel(std::byte value, model_core::ColorSpaceId space, unsigned channel)
{
    float v = float(std::to_integer<uint8_t>(value)) / 255.0f;
    if (space == model_core::ColorSpaceId::Srgb && channel < 3) {
        v = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    }
    return v;
}

inline std::byte EncodeChannel(float v, model_core::ColorSpaceId space, unsigned channel)
{
    if (space == model_core::ColorSpaceId::Srgb && channel < 3) {
        v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    }
    return std::byte(uint8_t(std::clamp(std::lround(v * 255), 0l, 255l)));
}

} // namespace raster_rgba_detail

// Downsamples `rgba` area-averaged in linear light to fit options.maxDimension,
// then generates the full color-aware mip chain. Returns false (leaving the
// caller to fall back) on cancellation, budget overflow, or a malformed input
// size. `width`/`height` are updated to the retained base dimensions.
inline bool FinalizeRgba8(std::vector<std::byte>& rgba, uint32_t& width, uint32_t& height,
                          model_core::ColorSpaceId space, const TextureDecodeOptions& options,
                          uint32_t& mipLevels)
{
    using namespace raster_rgba_detail;
    mipLevels = 1;
    if (options.Cancelled() || width == 0 || height == 0
        || rgba.size() != size_t(width) * height * 4) {
        return false;
    }
    uint32_t outputW = width, outputH = height;
    // Bounded progressive halving, matching the WIC adapter's policy.
    while (outputW > options.maxDimension || outputH > options.maxDimension) {
        outputW = (std::max)(1u, outputW / 2);
        outputH = (std::max)(1u, outputH / 2);
    }
    if (outputW == width && outputH == height) {
        return GenerateRasterMips(rgba, width, height, space, options, mipLevels);
    }
    const uint64_t targetPixels = uint64_t(outputW) * outputH;
    if (targetPixels > options.maxPixels) return false;
    std::vector<std::byte> reduced(size_t(outputW) * outputH * 4);
    for (uint32_t outY = 0; outY < outputH; ++outY) {
        if (options.Cancelled()) return false;
        const uint32_t y0 = outY * height / outputH;
        const uint32_t y1 = (outY + 1) * height / outputH;
        for (uint32_t outX = 0; outX < outputW; ++outX) {
            const uint32_t x0 = outX * width / outputW;
            const uint32_t x1 = (outX + 1) * width / outputW;
            float sum[4]{};
            for (uint32_t sy = y0; sy < y1; ++sy) {
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    for (unsigned c = 0; c < 4; ++c) {
                        sum[c] += DecodeChannel(rgba[(size_t(sy) * width + sx) * 4 + c], space, c);
                    }
                }
            }
            const float weight = float((x1 - x0) * (y1 - y0));
            for (unsigned c = 0; c < 4; ++c) {
                reduced[(size_t(outY) * outputW + outX) * 4 + c] =
                    EncodeChannel(sum[c] / weight, space, c);
            }
        }
    }
    rgba = std::move(reduced);
    width = outputW;
    height = outputH;
    return GenerateRasterMips(rgba, width, height, space, options, mipLevels);
}

} // namespace import_worker

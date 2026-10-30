#include "HdrFloatDecode.h"

#include "RasterRgbaSupport.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace import_worker {

namespace {

// Deterministic display policy for scene-linear HDR data. Exposure stays at 1.0
// so the decode is a pure function of the file; T14/T15 own any user exposure.
constexpr float kReinhardExposure = 1.0f;

float ToneMapChannel(float value)
{
    if (!std::isfinite(value)) return 0.0f;
    value = (std::max)(0.0f, value) * kReinhardExposure;
    return value / (1.0f + value);
}

float Clamp01(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

// Halves both axes until the image fits maxDimension, matching
// RasterRgbaSupport::FinalizeRgba8. Area-averages kept texels in linear light.
std::optional<LinearRgbaImage> DownscaleToLimit(LinearRgbaImage image, uint32_t maxDimension,
                                                uint64_t maxPixels, const TextureDecodeOptions& options)
{
    if (options.Cancelled() || image.width == 0 || image.height == 0
        || image.rgba.size() != size_t(image.width) * image.height * 4
        || maxDimension == 0) {
        return std::nullopt;
    }
    uint32_t outputW = image.width;
    uint32_t outputH = image.height;
    while (outputW > maxDimension || outputH > maxDimension) {
        outputW = (std::max)(1u, outputW / 2);
        outputH = (std::max)(1u, outputH / 2);
    }
    if (outputW == image.width && outputH == image.height) return image;
    if (uint64_t(outputW) * outputH > maxPixels) return std::nullopt;

    LinearRgbaImage reduced;
    reduced.width = outputW;
    reduced.height = outputH;
    reduced.rgba.assign(size_t(outputW) * outputH * 4, 0.0f);
    for (uint32_t outY = 0; outY < outputH; ++outY) {
        if (options.Cancelled()) return std::nullopt;
        const uint32_t y0 = outY * image.height / outputH;
        const uint32_t y1 = (outY + 1) * image.height / outputH;
        for (uint32_t outX = 0; outX < outputW; ++outX) {
            const uint32_t x0 = outX * image.width / outputW;
            const uint32_t x1 = (outX + 1) * image.width / outputW;
            float sum[4]{};
            for (uint32_t sy = y0; sy < y1; ++sy) {
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    const float* texel = image.rgba.data() + (size_t(sy) * image.width + sx) * 4;
                    for (unsigned c = 0; c < 4; ++c) sum[c] += texel[c];
                }
            }
            const float weight = float((x1 - x0) * (y1 - y0));
            float* target = reduced.rgba.data() + (size_t(outY) * outputW + outX) * 4;
            for (unsigned c = 0; c < 4; ++c) target[c] = sum[c] / weight;
        }
    }
    return reduced;
}

} // namespace

std::optional<LinearRgbaImage> DownscaleLinearRgba(LinearRgbaImage image, uint32_t maxDimension,
                                                   uint64_t maxPixels,
                                                   const TextureDecodeOptions& options)
{
    return DownscaleToLimit(std::move(image), maxDimension, maxPixels, options);
}

std::optional<TranscodedImage> FinalizeLinearImage(LinearRgbaImage image,
                                                   model_core::ColorSpaceId colorSpace,
                                                   const TextureDecodeOptions& options)
{
    auto reduced = DownscaleToLimit(std::move(image), options.maxDimension, options.maxPixels, options);
    if (!reduced) return std::nullopt;

    const uint64_t pixelCount = uint64_t(reduced->width) * reduced->height;
    if (pixelCount == 0 || pixelCount > options.maxPixels
        || pixelCount > options.maxDecodedBytes / 4) {
        return std::nullopt;
    }

    const bool displayCurve = options.semantic != TextureSemantic::Normal
        && options.semantic != TextureSemantic::Data;
    std::vector<std::byte> rgba(size_t(pixelCount) * 4);
    for (uint64_t i = 0; i < pixelCount; ++i) {
        if (options.Cancelled()) return std::nullopt;
        const float* source = reduced->rgba.data() + size_t(i) * 4;
        std::byte* target = rgba.data() + size_t(i) * 4;
        for (unsigned c = 0; c < 3; ++c) {
            const float value = displayCurve ? ToneMapChannel(source[c]) : Clamp01(source[c]);
            target[c] = raster_rgba_detail::EncodeChannel(value, colorSpace, c);
        }
        target[3] = raster_rgba_detail::EncodeChannel(Clamp01(source[3]), colorSpace, 3);
    }

    TranscodedImage result;
    result.pixelFormat = model_core::PixelFormatId::RGBA8_UNORM;
    result.width = reduced->width;
    result.height = reduced->height;
    uint32_t levels = 1;
    if (!GenerateRasterMips(rgba, result.width, result.height, colorSpace, options, levels)) {
        return std::nullopt;
    }
    result.mipLevels = levels;
    result.pixelBytes = std::move(rgba);
    return result;
}

} // namespace import_worker

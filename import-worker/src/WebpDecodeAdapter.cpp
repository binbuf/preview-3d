#include "WebpDecodeAdapter.h"

#include "ImageFormatSniff.h"
#include "platform/CheckedMath.h"

#include <webp/decode.h>

namespace import_worker {

std::optional<DecodedRasterImage> DecodeWebpImage(
    std::span<const std::byte> encodedBytes, model_core::ColorSpaceId colorSpace,
    const TextureDecodeOptions& options)
{
    using model_core::PixelFormatId;
    if (options.Cancelled() || encodedBytes.empty() || encodedBytes.size() > options.maxEncodedBytes
        || options.maxDimension == 0 || SniffImageFormat(encodedBytes) != SniffedImageFormat::WebP)
        return std::nullopt;

    WebPDecoderConfig config{};
    if (!WebPInitDecoderConfig(&config)) return std::nullopt;
    const auto* data = reinterpret_cast<const uint8_t*>(encodedBytes.data());
    if (WebPGetFeatures(data, encodedBytes.size(), &config.input) != VP8_STATUS_OK
        || config.input.width <= 0 || config.input.height <= 0 || config.input.has_animation)
        return std::nullopt;

    const auto sourcePixels = platform::CheckedMultiply(uint64_t(config.input.width),
                                                         uint64_t(config.input.height));
    if (!sourcePixels || *sourcePixels > options.maxPixels) return std::nullopt;
    uint32_t width = static_cast<uint32_t>(config.input.width);
    uint32_t height = static_cast<uint32_t>(config.input.height);
    while (width > options.maxDimension || height > options.maxDimension) {
        width = (std::max)(1u, width / 2);
        height = (std::max)(1u, height / 2);
    }
    const auto chainBytes = model_core::ComputeImagePixelBytes(
        PixelFormatId::RGBA8_UNORM, width, height, model_core::FullImageMipCount(width, height));
    const auto baseBytes = platform::CheckedMultiply(uint64_t(width) * height, 4);
    if (!chainBytes || !baseBytes || *chainBytes > options.maxDecodedBytes || *baseBytes > SIZE_MAX)
        return std::nullopt;

    DecodedRasterImage result;
    result.width = width;
    result.height = height;
    result.colorSpace = colorSpace;
    result.pixelBytes.resize(static_cast<size_t>(*baseBytes));

    config.output.colorspace = MODE_RGBA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = reinterpret_cast<uint8_t*>(result.pixelBytes.data());
    config.output.u.RGBA.stride = static_cast<int>(width * 4);
    config.output.u.RGBA.size = result.pixelBytes.size();
    if (width != static_cast<uint32_t>(config.input.width)
        || height != static_cast<uint32_t>(config.input.height)) {
        config.options.use_scaling = 1;
        config.options.scaled_width = static_cast<int>(width);
        config.options.scaled_height = static_cast<int>(height);
    }
    if (!WebPValidateDecoderConfig(&config) || options.Cancelled()
        || WebPDecode(data, encodedBytes.size(), &config) != VP8_STATUS_OK) {
        WebPFreeDecBuffer(&config.output);
        return std::nullopt;
    }
    WebPFreeDecBuffer(&config.output);
    if (options.Cancelled()
        || !GenerateRasterMips(result.pixelBytes, width, height, colorSpace, options, result.mipLevels))
        return std::nullopt;
    return result;
}

} // namespace import_worker

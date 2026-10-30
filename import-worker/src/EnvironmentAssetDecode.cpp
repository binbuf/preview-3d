#include "EnvironmentAssetDecode.h"

#include "HdrFloatDecode.h"
#include "ImageFormatSniff.h"
#include "OpenExrDecode.h"
#include "RadianceHdrDecode.h"
#include "TextureDecodePolicy.h"

#include <utility>

namespace import_worker {

std::optional<EnvironmentAsset> LoadEnvironmentAsset(std::span<const std::byte> bytes,
                                                     const EnvironmentAssetOptions& options)
{
    if (options.Cancelled() || bytes.empty() || bytes.size() > options.maxEncodedBytes) {
        return std::nullopt;
    }

    TextureDecodeOptions decode;
    decode.maxEncodedBytes = options.maxEncodedBytes;
    decode.maxDecodedBytes = options.maxDecodedBytes;
    decode.maxPixels = options.maxPixels;
    decode.maxDimension = options.maxDimension;
    decode.semantic = TextureSemantic::Data; // no material tone curve
    decode.isCancelled = options.isCancelled;

    std::optional<LinearRgbaImage> linear;
    switch (SniffImageFormat(bytes)) {
    case SniffedImageFormat::Hdr:
        linear = DecodeRadianceHdrToLinear(bytes, decode);
        break;
    case SniffedImageFormat::Exr:
        linear = DecodeOpenExrToLinear(bytes, decode);
        break;
    default:
        return std::nullopt;
    }
    if (!linear) return std::nullopt;

    auto reduced = DownscaleLinearRgba(std::move(*linear), options.maxDimension,
                                       options.maxPixels, decode);
    if (!reduced || !reduced->width || !reduced->height) return std::nullopt;
    if (reduced->rgba.size() > options.maxDecodedBytes / sizeof(float)) return std::nullopt;

    EnvironmentAsset asset;
    asset.width = reduced->width;
    asset.height = reduced->height;
    asset.rgba = std::move(reduced->rgba);
    if (!asset.Valid()) return std::nullopt;
    return asset;
}

} // namespace import_worker

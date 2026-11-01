#include "AssimpTextureDecode.h"

#include "ImageFormatSniff.h"
#include "DdsImageDecode.h"
#include "OpenExrDecode.h"
#include "RadianceHdrDecode.h"
#include "TgaImageDecode.h"
#include "TextureTranscodeAdapter.h"
#include "WebpDecodeAdapter.h"
#include "WicImageDecodeAdapter.h"

namespace import_worker {

std::optional<AssimpDecodedImage> DecodeAssimpTexture(
    std::span<const std::byte> encoded, model_core::ColorSpaceId space,
    TextureSemantic semantic, const TextureDecodeOptions& options)
{
    if (encoded.empty() || options.Cancelled()) return std::nullopt;
    TextureDecodeOptions local = options;
    local.semantic = semantic;

    AssimpDecodedImage image;
    image.space = space;
    switch (SniffImageFormat(encoded)) {
    case SniffedImageFormat::Ktx2:
        if (auto decoded = TranscodeKtx2BasisImage(encoded, local)) {
            image.format = decoded->pixelFormat;
            image.width = decoded->width;
            image.height = decoded->height;
            image.levels = decoded->mipLevels;
            image.pixels = std::move(decoded->pixelBytes);
        }
        break;
    case SniffedImageFormat::Tga:
    case SniffedImageFormat::Dds:
        if (auto decoded = SniffImageFormat(encoded) == SniffedImageFormat::Tga
                ? DecodeTgaImage(encoded, space, local)
                : DecodeDdsImage(encoded, space, local)) {
            image.format = decoded->pixelFormat;
            image.space = space;
            image.width = decoded->width;
            image.height = decoded->height;
            image.levels = decoded->mipLevels;
            image.pixels = std::move(decoded->pixelBytes);
        }
        break;
    case SniffedImageFormat::Hdr:
    case SniffedImageFormat::Exr:
        if (auto decoded = SniffImageFormat(encoded) == SniffedImageFormat::Hdr
                ? DecodeRadianceHdrImage(encoded, space, local)
                : DecodeOpenExrImage(encoded, space, local)) {
            image.format = decoded->pixelFormat;
            image.space = space;
            image.width = decoded->width;
            image.height = decoded->height;
            image.levels = decoded->mipLevels;
            image.pixels = std::move(decoded->pixelBytes);
        }
        break;
    case SniffedImageFormat::WebP:
        if (auto decoded = DecodeWebpImage(encoded, space, local)) {
            image.format = decoded->pixelFormat;
            image.space = decoded->colorSpace;
            image.width = decoded->width;
            image.height = decoded->height;
            image.levels = decoded->mipLevels;
            image.pixels = std::move(decoded->pixelBytes);
        }
        break;
    case SniffedImageFormat::Png:
    case SniffedImageFormat::Jpeg:
    case SniffedImageFormat::Bmp:
    case SniffedImageFormat::Tiff:
        if (auto decoded = DecodeRasterImageWic(encoded, space, local)) {
            image.format = decoded->pixelFormat;
            image.space = decoded->colorSpace;
            image.width = decoded->width;
            image.height = decoded->height;
            image.levels = decoded->mipLevels;
            image.pixels = std::move(decoded->pixelBytes);
        }
        break;
    default:
        break;
    }
    if (image.pixels.empty()) return std::nullopt;
    return image;
}

} // namespace import_worker

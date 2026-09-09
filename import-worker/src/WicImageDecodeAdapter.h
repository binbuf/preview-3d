#pragma once

// Plain raster image decode (PNG/JPEG/BMP/TIFF) via Windows Imaging
// Component -- the "inbox WIC paths" .docs/design/03-file-formats-and-
// ingestion.md's texture policy names, as opposed to KTX-Software/Basis
// (TextureTranscodeAdapter.h) for KTX2/KHR_texture_basisu. Per ADR-006,
// this only ever links into Preview3DImportWorker.exe, never the trusted
// viewer process.
//
// Deliberately soft-failing, mirroring TextureTranscodeAdapter.h's
// TranscodedImage/std::optional convention exactly: any problem here
// (corrupt bytes, unsupported WIC pixel format, oversized declared
// dimensions) returns std::nullopt, never an ImportErrorCode -- an image
// texture is never geometry-required.

#include "model_core/PixelFormats.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace import_worker {

struct DecodedRasterImage {
    model_core::PixelFormatId pixelFormat = model_core::PixelFormatId::RGBA8_UNORM; // always RGBA8 this adapter
    uint32_t width = 0;
    uint32_t height = 0;
    model_core::ColorSpaceId colorSpace = model_core::ColorSpaceId::Linear;
    std::vector<std::byte> pixelBytes; // level 0 only, tightly packed per PixelFormats.h's layout
};

// encodedBytes: raw PNG/JPEG/BMP/TIFF container bytes (already sniffed by
// the caller via ImageFormatSniff.h -- this function trusts WIC's own
// container detection for the decode itself, but the caller's sniff is what
// decides whether to call this adapter at all). colorSpace is a pass-
// through label the caller already determined from the material slot
// (baseColor/emissive = sRGB, metallicRoughness/normal = linear) -- this
// adapter does not infer it from pixel content.
std::optional<DecodedRasterImage> DecodeRasterImageWic(std::span<const std::byte> encodedBytes,
                                                          model_core::ColorSpaceId colorSpace);

} // namespace import_worker

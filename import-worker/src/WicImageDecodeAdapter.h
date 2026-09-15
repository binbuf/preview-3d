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
#include "TextureDecodePolicy.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace import_worker {

struct DecodedRasterImage {
    model_core::PixelFormatId pixelFormat = model_core::PixelFormatId::RGBA8_UNORM; // always RGBA8 this adapter
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    model_core::ColorSpaceId colorSpace = model_core::ColorSpaceId::Linear;
    std::vector<std::byte> pixelBytes; // complete chain, tightly packed, level 0 first
};

// Explicit inbox PNG/JPEG/BMP/TIFF decoder CLSIDs selected from sniffed bytes;
// never invoke stream-based codec discovery. BMP/TIFF remain adapter-only;
// glTF enables PNG/JPEG here. JPEG native scaling uses verified size/format
// transforms. Other expansion and all mip generation use cancellable tiles.
// Material semantics supply colorSpace; container metadata does not override it.
std::optional<DecodedRasterImage> DecodeRasterImageWic(std::span<const std::byte> encodedBytes,
                                                          model_core::ColorSpaceId colorSpace,
                                                          const TextureDecodeOptions& options = {});

} // namespace import_worker

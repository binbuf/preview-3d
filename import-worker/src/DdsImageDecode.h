#pragma once

// Product-owned bounded DDS decoder. Supports the closed GPU block set
// already carried by PixelFormatId (BC1/BC3/BC5/BC7) plus uncompressed 24/32
// bit RGB(A) that is converted to RGBA8. Cubemaps, arrays, volumes and every
// other DXGI format are rejected rather than flattened. All declared sizes are
// cross-checked against the actual payload before a byte is copied.

#include "TextureTranscodeAdapter.h"

#include <cstddef>
#include <optional>
#include <span>

namespace import_worker {

std::optional<TranscodedImage> DecodeDdsImage(std::span<const std::byte> bytes,
                                              model_core::ColorSpaceId colorSpace,
                                              const TextureDecodeOptions& options = {});

} // namespace import_worker

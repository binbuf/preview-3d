#pragma once

#include "WicImageDecodeAdapter.h"

namespace import_worker {

// Bounded static WebP decode into the same tightly-packed RGBA mip-chain
// contract as the inbox WIC raster adapter. Animated WebP is deliberately
// outside the MVP and soft-fails like every optional image decode.
std::optional<DecodedRasterImage> DecodeWebpImage(
    std::span<const std::byte> encodedBytes, model_core::ColorSpaceId colorSpace,
    const TextureDecodeOptions& options = {});

} // namespace import_worker

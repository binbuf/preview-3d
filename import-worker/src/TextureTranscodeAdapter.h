#pragma once

// Worker-only bounded KTX2/Basis decode. Color/emissive Basis textures use
// BC7 with reload-and-RGBA fallback; data/normal textures use RGBA to preserve
// all channels (BC5 requires green/alpha swizzles ordinary glTF does not promise).
// Supported non-array 2D RGBA8/BC1/BC3/BC5/BC7 containers retain validated mips.
// Corrupt, oversized or cancelled optional images return nullopt to GltfAdapter,
// which supplies checker/neutral fallbacks and one bounded generation warning.

#include "model_core/PixelFormats.h"
#include "TextureDecodePolicy.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace import_worker {

struct TranscodedImage {
    model_core::PixelFormatId pixelFormat = model_core::PixelFormatId::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    std::vector<std::byte> pixelBytes; // all retained mips, level 0 first, tightly packed
};

// ktx2Bytes: raw bytes of a KTX2 container (GLB-embedded, per
// KHR_texture_basisu). Returns nullopt on any failure -- never throws past
// this boundary.
std::optional<TranscodedImage> TranscodeKtx2BasisImage(std::span<const std::byte> ktx2Bytes,
                                                    const TextureDecodeOptions& options = {});

} // namespace import_worker

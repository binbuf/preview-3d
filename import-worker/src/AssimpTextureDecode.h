#pragma once

// Product-owned texture decode boundary for the bounded Assimp compatibility
// adapter. The adapter owns broker requests, budgets and Image chunk emission;
// this unit only turns bounded encoded bytes into the same tightly packed
// RGBA (or retained block-compressed) contract the native adapters use.
//
// Kept in its own translation unit so the Assimp fuzz target can link the
// adapter with a deterministic no-decode stub: compressed-codec coverage has
// dedicated targets and is not the compatibility container's attack surface.

#include "TextureDecodePolicy.h"
#include "model_core/PixelFormats.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace import_worker {

struct AssimpDecodedImage {
    model_core::PixelFormatId format = model_core::PixelFormatId::Unknown;
    model_core::ColorSpaceId space = model_core::ColorSpaceId::Linear;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 1;
    std::vector<std::byte> pixels;
};

std::optional<AssimpDecodedImage> DecodeAssimpTexture(
    std::span<const std::byte> encoded, model_core::ColorSpaceId space,
    TextureSemantic semantic, const TextureDecodeOptions& options);

} // namespace import_worker

// Deterministic no-decode stub for the Assimp fuzz target. Compressed codec
// coverage lives in the dedicated codec/provider targets; this target owns the
// compatibility container and normalized scene path only.

#include "AssimpTextureDecode.h"

namespace import_worker {

std::optional<AssimpDecodedImage> DecodeAssimpTexture(
    std::span<const std::byte>, model_core::ColorSpaceId, TextureSemantic,
    const TextureDecodeOptions&)
{
    return std::nullopt;
}

} // namespace import_worker

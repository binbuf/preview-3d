#pragma once

// Product-owned bounded TGA decoder. TGA has no magic; callers select it from
// the decodable-extension candidate filter and this decoder validates every
// header field from bytes. Only the common true-color forms are supported:
// type 2 (uncompressed) and type 10 (RLE), 24/32 bpp, no palette, no
// interleaving. Everything else soft-fails to std::nullopt so an optional
// texture degrades to a warning instead of failing the model.

#include "TextureTranscodeAdapter.h"

#include <cstddef>
#include <optional>
#include <span>

namespace import_worker {

std::optional<TranscodedImage> DecodeTgaImage(std::span<const std::byte> bytes,
                                              model_core::ColorSpaceId colorSpace,
                                              const TextureDecodeOptions& options = {});

} // namespace import_worker

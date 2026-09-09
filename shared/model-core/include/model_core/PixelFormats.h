#pragma once

// Pixel-format contract for ChunkTopology::Image payloads (see WireFormat.h).
// Mirrors VertexLayouts.h's pattern: a closed numeric ID crosses the wire,
// never a raw format/stride the receiver must interpret unchecked.

#include "platform/CheckedMath.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace model_core {

// Closed enumeration. The validator must never guess a block size for an
// unrecognized value -- see PixelFormatBlockInfo below.
enum class PixelFormatId : uint32_t {
    Unknown = 0,
    RGBA8_UNORM = 1,
    BC7_UNORM = 2,
    BC5_UNORM = 3,
    BC3_UNORM = 4,
    BC1_UNORM = 5,
};

enum class ColorSpaceId : uint32_t {
    Linear = 0,
    Srgb = 1,
};

#pragma pack(push, 1)

// Fixed-width header immediately at the start of an Image chunk's payload
// region; pixelDataByteSize bytes of tightly-packed pixel data (all mip
// levels, level 0 first) follow it. See SharedSectionValidator.cpp for the
// fail-closed cross-checks applied to every field here before any of it is
// trusted.
struct ImagePayloadHeader {
    uint32_t pixelFormat;      // PixelFormatId
    uint32_t width;             // mip 0 texel width
    uint32_t height;            // mip 0 texel height
    uint32_t mipLevels;         // >= 1
    uint32_t colorSpace;        // ColorSpaceId
    uint32_t reserved0;         // must be 0
    uint64_t pixelDataByteSize; // bytes following this header; redundant
                                  // cross-check against ChunkDescriptor::byteSize
};
static_assert(sizeof(ImagePayloadHeader) == 32, "ImagePayloadHeader wire layout changed");

#pragma pack(pop)

// Block-compressed formats operate on 4x4 texel blocks; RGBA8 is 1x1 (plain
// per-texel). Returns {texelsPerBlockEdge, bytesPerBlock}, or nullopt for
// Unknown/any value outside the closed enumeration -- never a guess.
constexpr std::optional<std::pair<uint32_t, uint32_t>> PixelFormatBlockInfo(PixelFormatId format) noexcept
{
    switch (format) {
    case PixelFormatId::RGBA8_UNORM:
        return std::make_pair(1u, 4u);
    case PixelFormatId::BC7_UNORM:
    case PixelFormatId::BC5_UNORM:
        return std::make_pair(4u, 16u);
    case PixelFormatId::BC3_UNORM:
        return std::make_pair(4u, 16u);
    case PixelFormatId::BC1_UNORM:
        return std::make_pair(4u, 8u);
    default:
        return std::nullopt;
    }
}

// Sums tightly-packed pixel bytes across mip levels [0, mipLevels), each
// level halving (floor, minimum 1 texel) from the previous. All arithmetic
// is overflow-checked; returns nullopt on any overflow, an unrecognized
// format, or a zero width/height/mipLevels.
constexpr std::optional<uint64_t> ComputeImagePixelBytes(PixelFormatId format, uint32_t width, uint32_t height,
                                                            uint32_t mipLevels) noexcept
{
    const auto block = PixelFormatBlockInfo(format);
    if (!block || width == 0 || height == 0 || mipLevels == 0) {
        return std::nullopt;
    }

    uint64_t total = 0;
    for (uint32_t level = 0; level < mipLevels; ++level) {
        const uint64_t mipW = (static_cast<uint64_t>(width) >> level) != 0 ? (static_cast<uint64_t>(width) >> level) : 1;
        const uint64_t mipH = (static_cast<uint64_t>(height) >> level) != 0 ? (static_cast<uint64_t>(height) >> level) : 1;
        const uint64_t unitsW = (block->first == 1) ? mipW : (mipW + 3) / 4;
        const uint64_t unitsH = (block->first == 1) ? mipH : (mipH + 3) / 4;

        const auto rowBytes = platform::CheckedMultiply(unitsW, static_cast<uint64_t>(block->second));
        if (!rowBytes) {
            return std::nullopt;
        }
        const auto levelBytes = platform::CheckedMultiply(*rowBytes, unitsH);
        if (!levelBytes) {
            return std::nullopt;
        }
        const auto next = platform::CheckedAdd(total, *levelBytes);
        if (!next) {
            return std::nullopt;
        }
        total = *next;
    }
    return total;
}

} // namespace model_core

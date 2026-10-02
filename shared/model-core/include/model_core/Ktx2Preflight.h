#pragma once

// Shared KTX2 container preflight (SEC-02).
//
// The import worker and the Explorer thumbnail provider both hand attacker-
// controlled KTX2/Basis bytes to KTX-Software's ktxTexture2_CreateFromMemory.
// That call decodes the fixed header, walks the level index and can allocate/
// Zstd-expand storage from fields the file itself declares, so the product owns
// the checks that must run before the library is allowed to touch the data.
//
// This header is the single source of truth for the KTX2 header layout and the
// pre-allocation checks. Both adapters call PreflightKtx2 rather than forking
// the offsets; the worker's prior inline copy is now this function. The layout
// is the Khronos KTX2 specification's 80-byte header + level index (24 bytes
// per level), little-endian.

#include "model_core/PixelFormats.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

namespace model_core {

// Test-only evidence seam: incremented immediately before the KTX library is
// invoked, so a regression test can prove a hostile header was rejected before
// any library allocation.
inline std::atomic<std::uint64_t>& Ktx2DecoderInvocations() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

// KTX2 spec identifier: "<<KTX 20>>\r\n\x1A\n". Deliberately not sourced from
// ktx.h -- that header exposes the KTX1 identifier only.
inline bool LooksLikeKtx2(std::span<const std::byte> bytes) noexcept
{
    static constexpr std::uint8_t kMagic[12]
        = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    return bytes.size() >= sizeof(kMagic)
        && std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) == 0;
}

// Acceptance ceilings for one KTX2 container. Callers derive these from their
// own policy (worker TextureDecodeOptions; provider remaining pixel budget).
struct Ktx2Limits {
    std::uint64_t maxEncodedBytes = 0;
    std::uint64_t maxDecodedBytes = 0;
    std::uint64_t maxPixels = 0;
    std::uint32_t maxDimension = 0;
};

// Fixed header + metadata offsets for the level index. Populated only after the
// container passed every check, so a caller never trusts a field that was not
// validated in the same pass.
struct Ktx2HeaderFields {
    std::uint32_t vkFormat = 0;
    std::uint32_t typeSize = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t layerCount = 0;
    std::uint32_t faceCount = 0;
    std::uint32_t levelCount = 0;
    std::uint32_t supercompression = 0;
    std::uint64_t dfdOffset = 0;
    std::uint64_t dfdLength = 0;
    std::uint64_t kvdOffset = 0;
    std::uint64_t kvdLength = 0;
    std::uint64_t sgdOffset = 0;
    std::uint64_t sgdLength = 0;
};

struct Ktx2Preflight {
    Ktx2HeaderFields header;
    // PixelFormatId::Unknown when vkFormat == 0 (Basis Universal, needs
    // transcoding); otherwise the validated non-Basis container format.
    PixelFormatId format = PixelFormatId::Unknown;
    // Sum of the declared uncompressed level sizes.
    std::uint64_t expandedLevelBytes = 0;
    // Worst case RGBA8 bytes for [width, height, levelCount] -- the largest
    // buffer a transcoding decode could materialize.
    std::uint64_t worstCaseDecodedBytes = 0;
};

// Validates the fixed KTX2 header, metadata ranges, level index and worst-case
// expansion against `limits`. Returns nullopt on any malformed field, an
// unsupported container format, or a declaration over budget -- before the KTX
// library is invoked. Never allocates from a file-declared size.
inline std::optional<Ktx2Preflight> PreflightKtx2(std::span<const std::byte> bytes,
                                                  const Ktx2Limits& limits) noexcept
{
    if (bytes.size() < 80 || bytes.size() > limits.maxEncodedBytes
        || limits.maxDimension == 0 || !LooksLikeKtx2(bytes)) {
        return std::nullopt;
    }
    const auto u32 = [&bytes](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    const auto u64 = [&bytes](std::size_t offset) {
        std::uint64_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };

    Ktx2HeaderFields header;
    header.vkFormat = u32(12);
    header.typeSize = u32(16);
    header.width = u32(20);
    header.height = u32(24);
    header.depth = u32(28);
    header.layerCount = u32(32);
    header.faceCount = u32(36);
    header.levelCount = u32(40);
    header.supercompression = u32(44);
    header.dfdOffset = u32(48);
    header.dfdLength = u32(52);
    header.kvdOffset = u32(56);
    header.kvdLength = u32(60);
    header.sgdOffset = u64(64);
    header.sgdLength = u64(72);

    if (header.width == 0 || header.height == 0
        || header.width > limits.maxDimension || header.height > limits.maxDimension
        || header.depth != 0 || header.layerCount != 0 || header.faceCount != 1
        || header.levelCount == 0
        || header.levelCount > FullImageMipCount(header.width, header.height)
        || bytes.size() < 80 + std::size_t(header.levelCount) * 24
        || std::uint64_t(header.width) * header.height > limits.maxPixels) {
        return std::nullopt;
    }

    // Metadata ranges are independently bounded and contained before any
    // library allocation.
    const std::pair<std::uint64_t, std::uint64_t> metadataRanges[]
        = {{header.dfdOffset, header.dfdLength},
           {header.kvdOffset, header.kvdLength},
           {header.sgdOffset, header.sgdLength}};
    for (const auto& range : metadataRanges) {
        if (range.second > 1024 * 1024 || range.first > bytes.size()
            || range.second > bytes.size() - range.first) {
            return std::nullopt;
        }
    }

    // Only Basis (transcode) or the closed set of supported Vulkan UNORM/SRGB
    // formats; an unrecognized vkFormat is never guessed.
    PixelFormatId format = PixelFormatId::Unknown;
    switch (header.vkFormat) {
    case 0:
        break;
    case 37:
    case 43:
        format = PixelFormatId::RGBA8_UNORM;
        break;
    case 131:
    case 132:
    case 133:
    case 134:
        format = PixelFormatId::BC1_UNORM;
        break;
    case 137:
    case 138:
        format = PixelFormatId::BC3_UNORM;
        break;
    case 141:
        format = PixelFormatId::BC5_UNORM;
        break;
    case 145:
    case 146:
        format = PixelFormatId::BC7_UNORM;
        break;
    default:
        return std::nullopt;
    }

    std::uint64_t expanded = 0;
    for (std::uint32_t level = 0; level < header.levelCount; ++level) {
        const std::size_t index = 80 + std::size_t(level) * 24;
        const std::uint64_t offset = u64(index);
        const std::uint64_t length = u64(index + 8);
        const std::uint64_t uncompressed = u64(index + 16);
        if (offset < 80 + std::size_t(header.levelCount) * 24 || offset > bytes.size()
            || length == 0 || length > bytes.size() - offset
            || uncompressed > limits.maxDecodedBytes
            || expanded > limits.maxDecodedBytes - uncompressed) {
            return std::nullopt;
        }
        expanded += uncompressed;
        for (std::uint32_t prior = 0; prior < level; ++prior) {
            const std::uint64_t priorOffset = u64(80 + std::size_t(prior) * 24);
            const std::uint64_t priorLength = u64(88 + std::size_t(prior) * 24);
            if (offset < priorOffset + priorLength && priorOffset < offset + length) {
                return std::nullopt;
            }
        }
    }

    const auto worst = ComputeImagePixelBytes(PixelFormatId::RGBA8_UNORM, header.width,
                                              header.height, header.levelCount);
    if (!worst || *worst > limits.maxDecodedBytes) {
        return std::nullopt;
    }

    Ktx2Preflight result;
    result.header = header;
    result.format = format;
    result.expandedLevelBytes = expanded;
    result.worstCaseDecodedBytes = *worst;
    return result;
}

} // namespace model_core
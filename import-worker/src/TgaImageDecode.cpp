#include "TgaImageDecode.h"

#include "RasterRgbaSupport.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace import_worker {

namespace {

uint16_t ReadLe16(const std::byte* p) noexcept
{
    return uint16_t(std::to_integer<uint8_t>(p[0]))
        | uint16_t(uint16_t(std::to_integer<uint8_t>(p[1])) << 8);
}

// Decodes true-color BGR(A) rows into top-left-origin RGBA8. The origin bits
// are applied while writing so the rest of the worker always sees the same
// orientation contract as WIC decodes.
bool DecodeTrueColor(std::span<const std::byte> payload, uint32_t width, uint32_t height,
                     uint32_t bytesPerPixel, bool topOrigin, bool rightOrigin, bool rle,
                     std::vector<std::byte>& rgba)
{
    const uint64_t pixelCount = uint64_t(width) * height;
    if (pixelCount == 0 || pixelCount > (std::numeric_limits<size_t>::max)() / 4) return false;
    rgba.assign(size_t(pixelCount) * 4, std::byte{0});

    uint64_t produced = 0;
    size_t pos = 0;
    auto writePixel = [&](const std::byte* source) {
        if (produced >= pixelCount) return false;
        const uint32_t x = uint32_t(produced % width);
        const uint32_t y = uint32_t(produced / width);
        const uint32_t dx = rightOrigin ? width - 1 - x : x;
        const uint32_t dy = topOrigin ? y : height - 1 - y;
        auto* out = rgba.data() + (size_t(dy) * width + dx) * 4;
        // TGA true-color stores blue, green, red (and alpha) in that order.
        out[0] = source[2];
        out[1] = source[1];
        out[2] = source[0];
        out[3] = bytesPerPixel == 4 ? source[3] : std::byte{255};
        ++produced;
        return true;
    };

    if (!rle) {
        const uint64_t needed = pixelCount * bytesPerPixel;
        if (needed > payload.size()) return false;
        for (uint64_t i = 0; i < pixelCount; ++i) {
            if (!writePixel(payload.data() + i * bytesPerPixel)) return false;
        }
        return true;
    }

    while (produced < pixelCount) {
        if (pos >= payload.size()) return false;
        const uint8_t header = std::to_integer<uint8_t>(payload[pos++]);
        const uint32_t count = uint32_t(header & 0x7Fu) + 1;
        if (count > pixelCount - produced) return false; // packet overruns the image
        if ((header & 0x80u) != 0) {
            if (payload.size() - pos < bytesPerPixel) return false;
            const std::byte* pixel = payload.data() + pos;
            pos += bytesPerPixel;
            for (uint32_t i = 0; i < count; ++i) {
                if (!writePixel(pixel)) return false;
            }
        } else {
            const uint64_t literalBytes = uint64_t(count) * bytesPerPixel;
            if (literalBytes > payload.size() - pos) return false;
            for (uint32_t i = 0; i < count; ++i) {
                if (!writePixel(payload.data() + pos + size_t(i) * bytesPerPixel)) return false;
            }
            pos += size_t(literalBytes);
        }
    }
    return true;
}

} // namespace

std::optional<TranscodedImage> DecodeTgaImage(std::span<const std::byte> bytes,
                                              model_core::ColorSpaceId colorSpace,
                                              const TextureDecodeOptions& options)
{
    // Fixed 18-byte TGA header. Reject every field this product does not
    // support before touching the payload or allocating anything.
    constexpr size_t kHeaderBytes = 18;
    if (options.Cancelled() || bytes.size() < kHeaderBytes
        || bytes.size() > options.maxEncodedBytes) {
        return std::nullopt;
    }
    const std::byte* h = bytes.data();
    const uint8_t idLength = std::to_integer<uint8_t>(h[0]);
    const uint8_t colorMapType = std::to_integer<uint8_t>(h[1]);
    const uint8_t imageType = std::to_integer<uint8_t>(h[2]);
    // Color-map spec is meaningless without a palette; reject a palette or a
    // nonzero spec so indexed/colormapped forms never masquerade as truecolor.
    if (colorMapType != 0) return std::nullopt;
    for (size_t i = 3; i < 8; ++i) {
        if (std::to_integer<uint8_t>(h[i]) != 0) return std::nullopt;
    }
    if (imageType != 2 && imageType != 10) return std::nullopt;
    const uint16_t xOrigin = ReadLe16(h + 8);
    const uint16_t yOrigin = ReadLe16(h + 10);
    const uint16_t width = ReadLe16(h + 12);
    const uint16_t height = ReadLe16(h + 14);
    const uint8_t pixelDepth = std::to_integer<uint8_t>(h[16]);
    const uint8_t descriptor = std::to_integer<uint8_t>(h[17]);
    // Bits 6-7 select interleaving, which this decoder does not implement.
    if ((descriptor & 0xC0u) != 0) return std::nullopt;
    if (pixelDepth != 24 && pixelDepth != 32) return std::nullopt;
    const uint8_t alphaBits = descriptor & 0x0Fu;
    if (alphaBits > (pixelDepth == 32 ? 8u : 0u)) return std::nullopt;
    if (width == 0 || height == 0 || width > model_core::kMaxTextureDimension
        || height > model_core::kMaxTextureDimension) {
        return std::nullopt;
    }
    const uint64_t pixelCount = uint64_t(width) * height;
    if (pixelCount > options.maxPixels || pixelCount > options.maxDecodedBytes / 4) {
        return std::nullopt;
    }
    const uint64_t headerAndId = uint64_t(kHeaderBytes) + idLength;
    if (headerAndId > bytes.size()) return std::nullopt;
    const uint32_t bytesPerPixel = pixelDepth / 8;
    const auto payload = bytes.subspan(size_t(headerAndId));
    // The origin is a canvas placement, not a pixel offset. This decoder does
    // not composite into a larger canvas, so only a zero origin is meaningful;
    // a nonzero one is rejected rather than silently mis-cropped.
    if (xOrigin != 0 || yOrigin != 0) return std::nullopt;

    std::vector<std::byte> rgba;
    if (!DecodeTrueColor(payload, width, height, bytesPerPixel,
                         (descriptor & 0x20u) != 0, (descriptor & 0x10u) != 0,
                         imageType == 10, rgba)) {
        return std::nullopt;
    }

    TranscodedImage result;
    result.pixelFormat = model_core::PixelFormatId::RGBA8_UNORM;
    result.width = width;
    result.height = height;
    uint32_t levels = 0;
    if (!FinalizeRgba8(rgba, result.width, result.height, colorSpace, options, levels)) {
        return std::nullopt;
    }
    result.mipLevels = levels;
    result.pixelBytes = std::move(rgba);
    return result;
}

} // namespace import_worker

#include "DdsImageDecode.h"

#include "RasterRgbaSupport.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace import_worker {

namespace {

constexpr uint32_t kDdsMagic = 0x20534444u; // "DDS "
constexpr size_t kHeaderBytes = 124;
constexpr size_t kPixelFormatBytes = 32;
constexpr size_t kDx10HeaderBytes = 20;
constexpr size_t kHeaderStart = 4;
constexpr size_t kDataStart = kHeaderStart + kHeaderBytes;
constexpr size_t kDx10DataStart = kDataStart + kDx10HeaderBytes;

constexpr uint32_t kDdsdMipMapCount = 0x00020000u;
constexpr uint32_t kDdpfAlphaPixels = 0x00000001u;
constexpr uint32_t kDdpfFourCc = 0x00000004u;
constexpr uint32_t kDdpfRgb = 0x00000040u;
constexpr uint32_t kDdpfLuminance = 0x00020000u;
constexpr uint32_t kDdpfYuv = 0x00000200u;
constexpr uint32_t kCaps2Cubemap = 0x00000200u;
constexpr uint32_t kCaps2Volume = 0x00200000u;
constexpr uint32_t kResourceDimensionTexture2D = 3u;
constexpr uint32_t kMiscFlagTextureCube = 0x00000004u;

uint32_t ReadLe32(const std::byte* p) noexcept
{
    return uint32_t(std::to_integer<uint8_t>(p[0]))
        | (uint32_t(std::to_integer<uint8_t>(p[1])) << 8)
        | (uint32_t(std::to_integer<uint8_t>(p[2])) << 16)
        | (uint32_t(std::to_integer<uint8_t>(p[3])) << 24);
}

uint32_t FourCc(char a, char b, char c, char d) noexcept
{
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8)
        | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}

// The closed block-format mapping. Nothing outside this table is guessed.
std::optional<model_core::PixelFormatId> BlockFormatFromFourCc(uint32_t fourCc) noexcept
{
    if (fourCc == FourCc('D', 'X', 'T', '1')) return model_core::PixelFormatId::BC1_UNORM;
    if (fourCc == FourCc('D', 'X', 'T', '5')) return model_core::PixelFormatId::BC3_UNORM;
    if (fourCc == FourCc('A', 'T', 'I', '2')
        || fourCc == FourCc('B', 'C', '5', 'U')) return model_core::PixelFormatId::BC5_UNORM;
    return std::nullopt;
}

struct UncompressedLayout {
    uint32_t bytesPerPixel = 0;
    bool rgbaOrder = true; // false = BGRA/BGR byte order
    bool hasAlpha = false;
};

std::optional<UncompressedLayout> LegacyUncompressedLayout(uint32_t flags, uint32_t bits,
                                                           uint32_t r, uint32_t g, uint32_t b,
                                                           uint32_t a) noexcept
{
    if ((flags & kDdpfRgb) == 0 || (flags & (kDdpfFourCc | kDdpfLuminance | kDdpfYuv)) != 0) {
        return std::nullopt;
    }
    if (bits == 32) {
        if (r == 0x000000FFu && g == 0x0000FF00u && b == 0x00FF0000u) {
            if (a != 0 && a != 0xFF000000u) return std::nullopt;
            return UncompressedLayout{4, true, a == 0xFF000000u};
        }
        if (r == 0x00FF0000u && g == 0x0000FF00u && b == 0x000000FFu) {
            if (a != 0 && a != 0xFF000000u) return std::nullopt;
            return UncompressedLayout{4, false, a == 0xFF000000u};
        }
        return std::nullopt;
    }
    if (bits == 24) {
        if (a != 0) return std::nullopt;
        if (r == 0x00FF0000u && g == 0x0000FF00u && b == 0x000000FFu) {
            return UncompressedLayout{3, false, false};
        }
        if (r == 0x000000FFu && g == 0x0000FF00u && b == 0x00FF0000u) {
            return UncompressedLayout{3, true, false};
        }
    }
    return std::nullopt;
}

uint32_t AlignedRowPitch(uint32_t width, uint32_t bytesPerPixel) noexcept
{
    const uint64_t row = uint64_t(width) * bytesPerPixel;
    return uint32_t((row + 3) & ~uint64_t(3));
}

} // namespace

std::optional<TranscodedImage> DecodeDdsImage(std::span<const std::byte> bytes,
                                              model_core::ColorSpaceId colorSpace,
                                              const TextureDecodeOptions& options)
{
    if (options.Cancelled() || bytes.size() < kDataStart || bytes.size() > options.maxEncodedBytes) {
        return std::nullopt;
    }
    if (ReadLe32(bytes.data()) != kDdsMagic) return std::nullopt;
    const std::byte* h = bytes.data() + kHeaderStart;
    if (ReadLe32(h) != kHeaderBytes) return std::nullopt;
    const uint32_t flags = ReadLe32(h + 4);
    const uint32_t height = ReadLe32(h + 8);
    const uint32_t width = ReadLe32(h + 12);
    const uint32_t pitchOrLinear = ReadLe32(h + 16);
    const uint32_t depth = ReadLe32(h + 20);
    const uint32_t declaredMips = ReadLe32(h + 24);
    if (width == 0 || height == 0 || width > model_core::kMaxTextureDimension
        || height > model_core::kMaxTextureDimension) {
        return std::nullopt;
    }
    const uint64_t basePixels = uint64_t(width) * height;
    if (basePixels > options.maxPixels) return std::nullopt;

    const std::byte* pf = h + 72;
    if (ReadLe32(pf) != kPixelFormatBytes) return std::nullopt;
    const uint32_t pfFlags = ReadLe32(pf + 4);
    const uint32_t fourCc = ReadLe32(pf + 8);
    const uint32_t bits = ReadLe32(pf + 12);
    const uint32_t rMask = ReadLe32(pf + 16);
    const uint32_t gMask = ReadLe32(pf + 20);
    const uint32_t bMask = ReadLe32(pf + 24);
    const uint32_t aMask = ReadLe32(pf + 28);
    const uint32_t caps2 = ReadLe32(h + 108);

    // Resource shapes this product does not render are rejected outright.
    if ((caps2 & kCaps2Cubemap) != 0 || (caps2 & kCaps2Volume) != 0) return std::nullopt;
    if (depth > 1) return std::nullopt;

    uint32_t mipCount = (flags & kDdsdMipMapCount) != 0 ? declaredMips : 1;
    if (mipCount == 0) mipCount = 1;
    mipCount = (std::min)(mipCount, model_core::FullImageMipCount(width, height));
    if (mipCount == 0) return std::nullopt;

    bool usesDx10 = false;
    std::optional<model_core::PixelFormatId> blockFormat;
    std::optional<UncompressedLayout> uncompressed;
    uint64_t dataStart = kDataStart;

    if ((pfFlags & kDdpfFourCc) != 0 && fourCc == FourCc('D', 'X', '1', '0')) {
        if (bytes.size() < kDx10DataStart) return std::nullopt;
        const std::byte* dx10 = bytes.data() + kDataStart;
        const uint32_t dxgiFormat = ReadLe32(dx10);
        const uint32_t resourceDimension = ReadLe32(dx10 + 4);
        const uint32_t miscFlag = ReadLe32(dx10 + 8);
        const uint32_t arraySize = ReadLe32(dx10 + 12);
        if (resourceDimension != kResourceDimensionTexture2D) return std::nullopt;
        if ((miscFlag & kMiscFlagTextureCube) != 0 || arraySize != 1) return std::nullopt;
        usesDx10 = true;
        dataStart = kDx10DataStart;        switch (dxgiFormat) {
        // BC1/BC3/BC5/BC7 UNORM; the _SRGB variants use the same block layout
        // and are marked by the material semantic's color space, not by a
        // second PixelFormatId.
        case 71: case 72: blockFormat = model_core::PixelFormatId::BC1_UNORM; break;
        case 77: case 78: blockFormat = model_core::PixelFormatId::BC3_UNORM; break;
        case 83: case 84: blockFormat = model_core::PixelFormatId::BC5_UNORM; break;
        case 98: case 99: blockFormat = model_core::PixelFormatId::BC7_UNORM; break;
        case 28: case 29: uncompressed = UncompressedLayout{4, true, true}; break;
        case 87: case 91: uncompressed = UncompressedLayout{4, false, true}; break;
        default: return std::nullopt; // every other DXGI format is unsupported
        }
    } else if ((pfFlags & kDdpfFourCc) != 0) {
        blockFormat = BlockFormatFromFourCc(fourCc);
        if (!blockFormat) return std::nullopt;
    } else {
        uncompressed = LegacyUncompressedLayout(pfFlags, bits, rMask, gMask, bMask, aMask);
        if (!uncompressed) return std::nullopt;
    }

    const auto payload = bytes.subspan(size_t(dataStart));
    TranscodedImage result;

    if (blockFormat) {
        // Preserve the authored mip chain as far as the payload truly supports
        // it. A header that claims more levels than the bytes contain is
        // clamped, never trusted.
        while (mipCount > 1) {
            const auto total = model_core::ComputeImagePixelBytes(*blockFormat, width, height, mipCount);
            if (total && *total <= payload.size()) break;
            --mipCount;
        }
        const auto baseBytes = model_core::ComputeImagePixelBytes(*blockFormat, width, height, 1);
        if (!baseBytes || *baseBytes > payload.size()) return std::nullopt;
        // Drop authored levels that exceed the requested preview dimension.
        uint32_t first = 0;
        while (((std::max)(1u, width >> first) > options.maxDimension
                || (std::max)(1u, height >> first) > options.maxDimension)
               && first + 1 < mipCount) {
            ++first;
        }
        const uint32_t retainedWidth = (std::max)(1u, width >> first);
        const uint32_t retainedHeight = (std::max)(1u, height >> first);
        if (retainedWidth > options.maxDimension || retainedHeight > options.maxDimension) {
            return std::nullopt;
        }
        const auto skipped = first == 0
            ? std::optional<uint64_t>(0)
            : model_core::ComputeImagePixelBytes(*blockFormat, width, height, first);
        const uint32_t retained = mipCount - first;
        const auto retainedBytes = model_core::ComputeImagePixelBytes(
            *blockFormat, retainedWidth, retainedHeight, retained);
        if (!skipped || !retainedBytes || *skipped > payload.size()
            || *retainedBytes > payload.size() - *skipped
            || *retainedBytes > options.maxDecodedBytes) {
            return std::nullopt;
        }
        result.pixelFormat = *blockFormat;
        result.width = retainedWidth;
        result.height = retainedHeight;
        result.mipLevels = retained;
        result.pixelBytes.assign(payload.begin() + ptrdiff_t(*skipped),
                                 payload.begin() + ptrdiff_t(*skipped + *retainedBytes));
        return result;
    }

    // Uncompressed: decode level 0 only, then regenerate the mip chain with the
    // shared color-aware path. Source mips are not trusted as a memory source.
    const UncompressedLayout layout = *uncompressed;
    const uint32_t pitch = AlignedRowPitch(width, layout.bytesPerPixel);
    const uint64_t levelBytes = uint64_t(pitch) * height;
    if (width > UINT32_MAX / layout.bytesPerPixel || levelBytes > payload.size()) return std::nullopt;
    // A declared pitch must at least hold one row; a smaller value is a lie.
    if (pitchOrLinear != 0 && pitchOrLinear < uint64_t(width) * layout.bytesPerPixel) {
        return std::nullopt;
    }
    if (levelBytes > options.maxDecodedBytes) return std::nullopt;

    std::vector<std::byte> rgba(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        if (options.Cancelled()) return std::nullopt;
        const std::byte* source = payload.data() + size_t(y) * pitch;
        auto* out = rgba.data() + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            const std::byte* pixel = source + size_t(x) * layout.bytesPerPixel;
            if (layout.rgbaOrder) {
                out[0] = pixel[0]; out[1] = pixel[1]; out[2] = pixel[2];
            } else {
                out[0] = pixel[2]; out[1] = pixel[1]; out[2] = pixel[0];
            }
            out[3] = (layout.bytesPerPixel == 4 && layout.hasAlpha) ? pixel[3] : std::byte{255};
            out += 4;
        }
    }
    result.pixelFormat = model_core::PixelFormatId::RGBA8_UNORM;
    result.width = width;
    result.height = height;
    uint32_t levels = 0;
    if (!FinalizeRgba8(rgba, result.width, result.height, colorSpace, options, levels)) {
        return std::nullopt;
    }
    result.mipLevels = levels;
    result.pixelBytes = std::move(rgba);
    (void)usesDx10;
    return result;
}

} // namespace import_worker

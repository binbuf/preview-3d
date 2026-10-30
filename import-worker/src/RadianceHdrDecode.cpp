#include "RadianceHdrDecode.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace import_worker {

namespace {

constexpr size_t kMaxHeaderLine = 4096;
constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr uint32_t kMaxHeaderLines = 64;

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
    return text;
}

bool ParseDimension(std::string_view token, uint32_t& value)
{
    if (token.empty() || token.size() > 10) return false;
    uint64_t parsed = 0;
    for (char c : token) {
        if (c < '0' || c > '9') return false;
        parsed = parsed * 10 + uint64_t(c - '0');
        if (parsed > 0xFFFFFFFFull) return false;
    }
    value = uint32_t(parsed);
    return true;
}

// Reads one '\n'-terminated line from `pos`. Rejects an over-long line rather
// than scanning the whole file for a newline.
bool ReadLine(std::span<const std::byte> bytes, size_t& pos, std::string_view& line, uint64_t& consumed)
{
    if (pos >= bytes.size()) return false;
    size_t end = pos;
    while (end < bytes.size() && bytes[end] != std::byte{'\n'}) {
        if (end - pos >= kMaxHeaderLine) return false;
        ++end;
    }
    line = std::string_view(reinterpret_cast<const char*>(bytes.data()) + pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    consumed += line.size() + 1;
    pos = end < bytes.size() ? end + 1 : end;
    return consumed <= kMaxHeaderBytes;
}

bool DecodeNewRleScanline(std::span<const std::byte> bytes, size_t& pos, uint32_t width,
                          std::vector<uint8_t>& channels)
{
    // channels is 4 * width. Each of the four components is an independent run
    // stream and must fill exactly one row.
    for (unsigned channel = 0; channel < 4; ++channel) {
        uint32_t x = 0;
        while (x < width) {
            if (pos >= bytes.size()) return false;
            const uint8_t count = std::to_integer<uint8_t>(bytes[pos++]);
            if (count == 0) return false;
            if (count > 128) {
                const uint32_t run = count - 128u;
                if (run > width - x || pos >= bytes.size()) return false;
                const uint8_t value = std::to_integer<uint8_t>(bytes[pos++]);
                for (uint32_t i = 0; i < run; ++i) {
                    channels[size_t(channel) * width + x++] = value;
                }
            } else {
                if (count > width - x || count > bytes.size() - pos) return false;
                for (uint32_t i = 0; i < count; ++i) {
                    channels[size_t(channel) * width + x++] =
                        std::to_integer<uint8_t>(bytes[pos++]);
                }
            }
        }
    }
    return true;
}

bool DecodeFlatScanline(std::span<const std::byte> bytes, size_t& pos, uint32_t width,
                        std::vector<uint8_t>& channels)
{
    const uint64_t needed = uint64_t(width) * 4;
    if (needed > bytes.size() - pos) return false;
    for (uint32_t channel = 0; channel < 4; ++channel) {
        for (uint32_t x = 0; x < width; ++x) {
            channels[size_t(channel) * width + x] =
                std::to_integer<uint8_t>(bytes[pos + size_t(x) * 4 + channel]);
        }
    }
    pos += size_t(needed);
    return true;
}

struct Resolution {
    uint32_t width = 0;
    uint32_t height = 0;
    bool topToBottom = true;
    bool leftToRight = true;
};

bool ParseResolution(std::string_view line, Resolution& resolution)
{
    std::string_view tokens[4];
    size_t count = 0;
    size_t start = 0;
    while (start < line.size() && count < 4) {
        while (start < line.size() && IsSpace(line[start])) ++start;
        if (start >= line.size()) break;
        size_t end = start;
        while (end < line.size() && !IsSpace(line[end])) ++end;
        tokens[count++] = line.substr(start, end - start);
        start = end;
    }
    while (start < line.size() && IsSpace(line[start])) ++start;
    if (count != 4 || start != line.size()) return false;
    if (tokens[0].size() != 2 || tokens[2].size() != 2) return false;
    const char ySign = tokens[0][0];
    const char xSign = tokens[2][0];
    if ((ySign != '+' && ySign != '-') || (xSign != '+' && xSign != '-')) return false;
    if ((tokens[0][1] != 'Y' && tokens[0][1] != 'y')
        || (tokens[2][1] != 'X' && tokens[2][1] != 'x')) {
        return false;
    }
    if (!ParseDimension(tokens[1], resolution.height)
        || !ParseDimension(tokens[3], resolution.width)) {
        return false;
    }
    resolution.topToBottom = ySign == '-';
    resolution.leftToRight = xSign == '+';
    return resolution.width != 0 && resolution.height != 0;
}

std::optional<LinearRgbaImage> DecodeLinear(std::span<const std::byte> bytes,
                                            const TextureDecodeOptions& options)
{
    if (options.Cancelled() || bytes.size() < 8 || bytes.size() > options.maxEncodedBytes) {
        return std::nullopt;
    }

    size_t pos = 0;
    uint64_t consumed = 0;
    std::string_view line;
    if (!ReadLine(bytes, pos, line, consumed)) return std::nullopt;
    if (line != "#?RADIANCE" && line != "#?RGBE") return std::nullopt;

    bool sawFormat = false;
    uint32_t headerLines = 0;
    for (;;) {
        if (!ReadLine(bytes, pos, line, consumed)) return std::nullopt;
        if (++headerLines > kMaxHeaderLines) return std::nullopt;
        if (line.empty()) break;
        if (line.substr(0, 7) == "FORMAT=") {
            if (sawFormat) return std::nullopt;
            sawFormat = true;
            if (Trim(line.substr(7)) != "32-bit_rle_rgbe") return std::nullopt;
        }
    }
    if (!sawFormat) return std::nullopt;

    if (!ReadLine(bytes, pos, line, consumed)) return std::nullopt;
    Resolution resolution;
    if (!ParseResolution(Trim(line), resolution)) return std::nullopt;
    if (resolution.width > model_core::kMaxTextureDimension
        || resolution.height > model_core::kMaxTextureDimension) {
        return std::nullopt;
    }
    const uint64_t pixelCount = uint64_t(resolution.width) * resolution.height;
    if (pixelCount == 0 || pixelCount > options.maxPixels
        || pixelCount * 16 > LinearFloatTransientLimit(options)) {
        return std::nullopt;
    }

    LinearRgbaImage image;
    image.width = resolution.width;
    image.height = resolution.height;
    image.rgba.assign(size_t(pixelCount) * 4, 0.0f);

    std::vector<uint8_t> channels(size_t(resolution.width) * 4);
    for (uint32_t scanline = 0; scanline < resolution.height; ++scanline) {
        if (options.Cancelled()) return std::nullopt;
        const size_t start = pos;
        bool newRle = false;
        if (resolution.width >= 8 && resolution.width <= 32767
            && bytes.size() - pos >= 4) {
            newRle = std::to_integer<uint8_t>(bytes[pos]) == 2
                && std::to_integer<uint8_t>(bytes[pos + 1]) == 2
                && ((uint32_t(std::to_integer<uint8_t>(bytes[pos + 2])) << 8)
                    | uint32_t(std::to_integer<uint8_t>(bytes[pos + 3])))
                    == resolution.width;
            if (newRle) pos += 4;
        }
        const bool ok = newRle
            ? DecodeNewRleScanline(bytes, pos, resolution.width, channels)
            : DecodeFlatScanline(bytes, pos, resolution.width, channels);
        if (!ok || pos <= start) return std::nullopt;

        const uint32_t row = resolution.topToBottom
            ? scanline : resolution.height - 1 - scanline;
        for (uint32_t x = 0; x < resolution.width; ++x) {
            const uint32_t column = resolution.leftToRight ? x : resolution.width - 1 - x;
            const uint8_t exponent = channels[size_t(3) * resolution.width + x];
            float* target = image.rgba.data() + (size_t(row) * resolution.width + column) * 4;
            if (exponent == 0) {
                target[0] = target[1] = target[2] = 0.0f;
            } else {
                const float scale = std::ldexp(1.0f, int(exponent) - (128 + 8));
                target[0] = float(channels[size_t(0) * resolution.width + x]) * scale;
                target[1] = float(channels[size_t(1) * resolution.width + x]) * scale;
                target[2] = float(channels[size_t(2) * resolution.width + x]) * scale;
            }
            target[3] = 1.0f;
        }
    }
    return image;
}

} // namespace

std::optional<LinearRgbaImage> DecodeRadianceHdrToLinear(std::span<const std::byte> bytes,
                                                         const TextureDecodeOptions& options)
{
    return DecodeLinear(bytes, options);
}

std::optional<TranscodedImage> DecodeRadianceHdrImage(std::span<const std::byte> bytes,
                                                      model_core::ColorSpaceId colorSpace,
                                                      const TextureDecodeOptions& options)
{
    auto linear = DecodeLinear(bytes, options);
    if (!linear) return std::nullopt;
    return FinalizeLinearImage(std::move(*linear), colorSpace, options);
}

} // namespace import_worker

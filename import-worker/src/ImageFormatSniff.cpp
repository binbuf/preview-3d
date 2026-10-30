#include "ImageFormatSniff.h"

#include "model_core/PixelFormats.h"

#include <array>
#include <cctype>
#include <cstring>

namespace import_worker {

namespace {

bool StartsWith(std::span<const std::byte> bytes, std::initializer_list<uint8_t> magic)
{
    if (bytes.size() < magic.size()) {
        return false;
    }
    size_t i = 0;
    for (uint8_t b : magic) {
        if (static_cast<uint8_t>(bytes[i]) != b) {
            return false;
        }
        ++i;
    }
    return true;
}

uint16_t Le16(std::span<const std::byte> bytes, size_t offset) noexcept
{
    return uint16_t(static_cast<uint8_t>(bytes[offset]))
        | uint16_t(uint16_t(static_cast<uint8_t>(bytes[offset + 1])) << 8);
}

// TGA has no printable magic. This is a candidate check on the fixed header,
// not a promise of decodability: the decoder revalidates every field and the
// payload. It exists so a TGA-named file with invalid header bytes is never
// treated as a valid texture.
bool LooksLikeTga(std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() < 18) return false;
    const uint8_t idLength = static_cast<uint8_t>(bytes[0]);
    const uint8_t colorMapType = static_cast<uint8_t>(bytes[1]);
    const uint8_t imageType = static_cast<uint8_t>(bytes[2]);
    if (colorMapType != 0 || (imageType != 2 && imageType != 10)) return false;
    for (size_t i = 3; i < 8; ++i) {
        if (static_cast<uint8_t>(bytes[i]) != 0) return false;
    }
    if (Le16(bytes, 8) != 0 || Le16(bytes, 10) != 0) return false;
    const uint32_t width = Le16(bytes, 12);
    const uint32_t height = Le16(bytes, 14);
    const uint8_t depth = static_cast<uint8_t>(bytes[16]);
    const uint8_t descriptor = static_cast<uint8_t>(bytes[17]);
    if (width == 0 || height == 0 || width > model_core::kMaxTextureDimension
        || height > model_core::kMaxTextureDimension) return false;
    if (depth != 24 && depth != 32) return false;
    if ((descriptor & 0xC0u) != 0) return false;
    if ((descriptor & 0x0Fu) > (depth == 32 ? 8u : 0u)) return false;
    const uint64_t headerAndId = 18ull + idLength;
    if (headerAndId > bytes.size()) return false;
    if (imageType == 2) {
        const uint64_t needed = headerAndId + uint64_t(width) * height * (depth / 8);
        if (needed > bytes.size()) return false;
    }
    return true;
}

} // namespace

SniffedImageFormat SniffImageFormat(std::span<const std::byte> bytes) noexcept
{
    // PNG: fixed 8-byte signature.
    if (StartsWith(bytes, { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A })) {
        return SniffedImageFormat::Png;
    }
    // JPEG: SOI marker.
    if (StartsWith(bytes, { 0xFF, 0xD8 })) {
        return SniffedImageFormat::Jpeg;
    }
    // BMP: 'BM'.
    if (StartsWith(bytes, { 0x42, 0x4D })) {
        return SniffedImageFormat::Bmp;
    }
    // TIFF: little- or big-endian byte-order marker + magic 42.
    if (StartsWith(bytes, { 0x49, 0x49, 0x2A, 0x00 }) || StartsWith(bytes, { 0x4D, 0x4D, 0x00, 0x2A })) {
        return SniffedImageFormat::Tiff;
    }
    // WebP: 'RIFF'....'WEBP' -- bytes 0-3 and 8-11, not contiguous.
    if (bytes.size() >= 12 && StartsWith(bytes, { 0x52, 0x49, 0x46, 0x46 })
        && static_cast<uint8_t>(bytes[8]) == 0x57 && static_cast<uint8_t>(bytes[9]) == 0x45
        && static_cast<uint8_t>(bytes[10]) == 0x42 && static_cast<uint8_t>(bytes[11]) == 0x50) {
        return SniffedImageFormat::WebP;
    }
    // KTX2's 12-byte spec identifier ("«KTX 20»\r\n\x1A\n"). Not sourced from
    // ktx.h -- that header only exposes KTX_IDENTIFIER_REF, the KTX *1*
    // identifier ("«KTX 11»", bytes 5-6 = 0x31 0x31); KTX2's is a separate,
    // equally fixed/standardized 12-byte constant per the Khronos KTX2 spec,
    // differing only in bytes 5-6 (0x32 0x30 = "20").
    if (StartsWith(bytes, { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A })) {
        return SniffedImageFormat::Ktx2;
    }
    // DDS: fixed "DDS " magic followed by the 124-byte header. The decoder
    // proves the rest of the container before any block is copied.
    if (StartsWith(bytes, { 0x44, 0x44, 0x53, 0x20 })) {
        return SniffedImageFormat::Dds;
    }
    // TGA: no magic; a strict fixed-header candidate check. Callers still use
    // the extension allowlist to decide whether TGA is worth attempting.
    if (LooksLikeTga(bytes)) {
        return SniffedImageFormat::Tga;
    }
    return SniffedImageFormat::Unknown;
}

bool HasDecodableImageExtension(std::string_view path) noexcept
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= path.size()) {
        return false;
    }
    std::string_view extension = path.substr(dot + 1);
    // The extension ends at the first separator or alternate-stream/query
    // character, so "inside.png:stream" and "tex/a.png?x" report "png" rather
    // than an unrecognized blob. This keeps the request filter from being
    // fooled into treating a syntactically unsafe reference as an unsupported
    // optional format.
    size_t length = 0;
    while (length < extension.size()) {
        const unsigned char ch = static_cast<unsigned char>(extension[length]);
        if (!std::isalnum(ch)) break;
        ++length;
    }
    extension = extension.substr(0, length);
    if (extension.size() > 5) {
        return false;
    }
    char folded[6]{};
    for (size_t i = 0; i < extension.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(extension[i]);
        folded[i] = static_cast<char>(ch < 0x80 ? std::tolower(ch) : ch);
    }
    const std::string_view lower(folded, extension.size());
    return lower == "png" || lower == "jpg" || lower == "jpeg" || lower == "bmp"
        || lower == "tif" || lower == "tiff" || lower == "webp" || lower == "ktx2"
        || lower == "tga" || lower == "dds";
}

} // namespace import_worker

#include "ImageFormatSniff.h"

#include <array>
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
    return SniffedImageFormat::Unknown;
}

} // namespace import_worker

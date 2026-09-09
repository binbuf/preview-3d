#pragma once

// Detects an encoded image container from its own bytes, never from a
// declared extension/MIME type -- per .docs/design/03-file-formats-and-
// ingestion.md's texture policy: "Encoded type is verified from bytes and
// declared MIME/container metadata rather than extension alone."

#include <cstddef>
#include <span>

namespace import_worker {

enum class SniffedImageFormat {
    Unknown,
    Png,
    Jpeg,
    Bmp,
    Tiff,
    WebP,
    Ktx2,
};

SniffedImageFormat SniffImageFormat(std::span<const std::byte> bytes) noexcept;

} // namespace import_worker

#pragma once

// Detects an encoded image container from its own bytes, never from a
// declared extension/MIME type -- per .docs/design/03-file-formats-and-
// ingestion.md's texture policy: "Encoded type is verified from bytes and
// declared MIME/container metadata rather than extension alone."

#include <cstddef>
#include <span>
#include <string_view>

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

// True when an asset reference names an encoded image container the decode
// path can attempt (PNG/JPEG/BMP/TIFF/WebP/KTX2). This is a request filter, not
// a type decision: it lets callers skip sidecar requests for formats no decoder
// handles (for example EXR normal/roughness maps) instead of surfacing the
// sidecar resolver's disallowed-extension rejection as a fatal import error.
// The actual container is still verified from bytes by SniffImageFormat.
bool HasDecodableImageExtension(std::string_view path) noexcept;

} // namespace import_worker

#pragma once

// Shared glTF data-URI admissibility preflight (SEC-16).
//
// glTF 2.0 permits `buffers[].uri` and `images[].uri` to be RFC 2397 data URIs
// with a base64 payload (glTF 2.0 §3.6.1.3). fastgltf 0.9.0's fallback base64
// decoder (`fastgltf::base64::fallback_decode_inplace`) sizes its destination
// from `base64::getOutputSize`, which truncates a trailing partial 4-character
// group, yet still writes the trailing bytes -- so a payload whose encoded
// length is not a multiple of four over-writes the product's allocation. RFC
// 4648 base64 is emitted in 4-character groups, so such a payload is malformed
// and can be rejected before the JSON buffer is handed to fastgltf.
//
// The worker walks the bounded simdjson DOM and calls this for each `uri`; the
// provider/worker preflights follow the DracoPreflight/Ktx2Preflight pattern:
// header-only, allocation-free, and directly unit-testable.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace model_core {

enum class GltfDataUriStatus {
    NotDataUri, // relative path / buffer index / non-base64 data URI; not ours
    Ok,
    Malformed, // structurally invalid base64 (e.g. length % 4 != 0)
    TooLarge,  // decoded size exceeds the caller's cap
};

// Validates one glTF `uri` value. Only base64 data URIs are inspected; any
// other value returns NotDataUri so the caller keeps its existing handling.
// Never allocates and never reads past the view.
inline GltfDataUriStatus ValidateGltfDataUri(std::string_view uri,
                                             std::uint64_t maxDecodedBytes) noexcept
{
    constexpr std::string_view kDataScheme = "data:";
    constexpr std::string_view kBase64Marker = ";base64,";
    if (uri.size() < kDataScheme.size() || uri.substr(0, kDataScheme.size()) != kDataScheme)
        return GltfDataUriStatus::NotDataUri;
    const std::size_t marker = uri.find(kBase64Marker, kDataScheme.size());
    if (marker == std::string_view::npos)
        return GltfDataUriStatus::NotDataUri; // mediatype without base64: leave to fastgltf
    const std::string_view payload = uri.substr(marker + kBase64Marker.size());

    // RFC 4648 base64 is emitted in 4-character groups; a payload length not a
    // multiple of four is the exact fastgltf 0.9.0 overflow trigger.
    if (payload.size() % 4 != 0)
        return GltfDataUriStatus::Malformed;

    std::size_t padding = 0;
    bool sawPadding = false;
    for (const char c : payload) {
        if (c == '=') {
            sawPadding = true;
            ++padding; // padding is only legal at the very end
        } else if (sawPadding) {
            return GltfDataUriStatus::Malformed;
        } else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                     || c == '+' || c == '/')) {
            return GltfDataUriStatus::Malformed;
        }
    }
    if (padding > 2)
        return GltfDataUriStatus::Malformed;

    const std::uint64_t decoded = std::uint64_t(payload.size() / 4) * 3 - padding;
    if (decoded > maxDecodedBytes)
        return GltfDataUriStatus::TooLarge;
    return GltfDataUriStatus::Ok;
}

} // namespace model_core

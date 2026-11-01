#pragma once

// Bounded content sniffing for the trusted-process router.
//
// The sniffer reads at most a fixed head window and, for ZIP containers, an
// End-Of-Central-Directory record plus a bounded central-directory window. It
// never decompresses an archive and never parses model structure. A match is a
// *candidate selector*: the worker/host still performs the real parse and all
// containment checks. The catalog remains the authority for extensions,
// identities and defaults.

#include "model_core/FormatCatalog.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace model_core {

inline constexpr uint32_t kSniffHeadBytes = 64 * 1024;
inline constexpr uint32_t kSniffMaxCentralDirectoryBytes = 1024 * 1024;
inline constexpr uint32_t kSniffMaxZipEntries = 8192;
inline constexpr uint32_t kSniffEocdSearchBytes = 64 * 1024;

enum class SniffConfidence : uint8_t {
    None = 0,
    Weak,   // textual/token evidence only
    Strong, // fixed magic or checked structural envelope
};

struct SniffResult {
    FormatId format = FormatId::Unknown;
    SniffConfidence confidence = SniffConfidence::None;
    std::string_view detail{};

    explicit operator bool() const noexcept { return format != FormatId::Unknown; }
};

// Pure bounded-head probe. This cannot identify ZIP-based formats (3MF/USDZ)
// because their identity lives in the central directory; use SniffFile for
// those.
SniffResult SniffHead(std::span<const std::byte> head, uint64_t totalBytes) noexcept;

// Reads the bounded head window and, when the head is a ZIP local header, the
// bounded EOCD/central-directory window. Returns Unknown on any read error,
// malformed archive, or unsupported ZIP64/spanned archive.
SniffResult SniffFile(const std::wstring& path);

enum class ClassificationReason : uint8_t {
    None = 0,
    Extension,
    Content,
    Both,
    Mismatch,
};

struct FormatClassification {
    const FormatDescriptor* extension = nullptr;
    const FormatDescriptor* content = nullptr;
    const FormatDescriptor* selected = nullptr;
    ClassificationReason reason = ClassificationReason::None;
    SniffConfidence contentConfidence = SniffConfidence::None;
    std::string_view contentDetail{};

    bool HasExtension() const noexcept { return extension != nullptr; }
    bool HasContent() const noexcept { return content != nullptr; }
};

// Combines the extension lookup with a content candidate. Families (gltf/glb,
// the USD encodings, binary/ASCII STL) are compatible; a cross-family
// disagreement is a mismatch. Strong content wins over the extension in either
// case, and the caller (T03) decides whether to auto-open or ask.
inline FormatClassification ClassifyFormat(std::wstring_view extension, const SniffResult& content) noexcept
{
    FormatClassification result;
    result.extension = FindByExtension(extension);
    result.content = FindById(content.format);
    result.contentConfidence = content.confidence;
    result.contentDetail = content.detail;
    if (result.content != nullptr && result.content->id == FormatId::Unknown) result.content = nullptr;

    if (result.extension != nullptr && result.content != nullptr) {
        result.selected = result.content;
        result.reason = (result.extension->family == result.content->family)
            ? ClassificationReason::Both : ClassificationReason::Mismatch;
    } else if (result.extension != nullptr) {
        result.selected = result.extension;
        result.reason = ClassificationReason::Extension;
    } else if (result.content != nullptr) {
        result.selected = result.content;
        result.reason = ClassificationReason::Content;
    }
    return result;
}

} // namespace model_core

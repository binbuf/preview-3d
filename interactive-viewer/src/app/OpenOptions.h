#pragma once

// Header-only helpers for the catalog-driven Open As / --format surface.
// Kept out of Preview3D.cpp so Tests.Unit can exercise the CLI grammar,
// selection list and list-formats text without a window or GPU.

#include "model_core/FormatCatalog.h"
#include "model_core/FormatSelection.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace open_options {

struct FormatOption {
    const model_core::FormatDescriptor* descriptor = nullptr;
    bool valid = true;
};

// A stable name is ASCII and case-insensitive. Only formats with a shipped
// importer are accepted, so reserved catalog identities stay unclaimable.
inline FormatOption ParseFormatName(std::wstring_view value)
{
    FormatOption result;
    if (value.empty() || value.size() > 64) { result.valid = false; return result; }
    std::string name;
    name.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < 0 || character > 0x7f) { result.valid = false; return result; }
        name.push_back(static_cast<char>(character));
    }
    result.descriptor = model_core::FindByStableName(name);
    if (!model_core::IsImportableFormat(result.descriptor)) {
        result.descriptor = nullptr;
        result.valid = false;
    }
    return result;
}

inline std::vector<const model_core::FormatDescriptor*> OpenAsFormats()
{
    return model_core::ImportableFormatDescriptors();
}

// `stable-name<TAB>ext1,ext2<TAB>display-name` per catalog format, newline
// terminated. Stable names are ASCII; display names come from the catalog and
// are localized by the caller.
inline std::wstring BuildFormatListText(
    const std::function<std::wstring(const model_core::FormatDescriptor&)>& displayName)
{
    std::wstring text;
    for (const auto* descriptor : OpenAsFormats()) {
        text += std::wstring(descriptor->stableName.begin(), descriptor->stableName.end());
        text += L'\t';
        bool first = true;
        for (const auto extension : descriptor->extensions) {
            if (!first) text += L',';
            text += std::wstring(extension);
            first = false;
        }
        text += L'\t';
        text += displayName(*descriptor);
        text += L'\n';
    }
    return text;
}

} // namespace open_options

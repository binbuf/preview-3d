#pragma once

// Header-only helpers for the catalog-driven Open As / --format surface.
// Kept out of Preview3D.cpp so Tests.Unit can exercise the CLI grammar,
// selection list and list-formats text without a window or GPU.

#include "model_core/FormatCatalog.h"
#include "model_core/FormatSelection.h"
#include "model_core/ReaderCatalog.h"

#include <cstdint>
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

// ---------------------------------------------------------------------------
// T07 reader selection
// ---------------------------------------------------------------------------

enum class ReaderParseError : uint8_t {
    None = 0,
    Empty,
    TooLong,
    NonAscii,
    Unknown,
};

struct ReaderOption {
    const model_core::ReaderDescriptor* reader = nullptr;
    ReaderParseError error = ReaderParseError::None;
    bool valid = true;
};

// A reader name is a stable ASCII product identity, case-insensitive and
// bounded to the same 64-character envelope as a stable format name. Names
// that this build does not ship are typed rejections; there is no dynamic
// discovery or fallback.
inline ReaderOption ParseReaderName(std::wstring_view value)
{
    ReaderOption result;
    if (value.empty()) { result.valid = false; result.error = ReaderParseError::Empty; return result; }
    if (value.size() > 64) { result.valid = false; result.error = ReaderParseError::TooLong; return result; }
    std::string name;
    name.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < 0 || character > 0x7f) {
            result.valid = false;
            result.error = ReaderParseError::NonAscii;
            return result;
        }
        name.push_back(static_cast<char>(character));
    }
    result.reader = model_core::FindReaderByName(name);
    if (!result.reader) {
        result.valid = false;
        result.error = ReaderParseError::Unknown;
    }
    return result;
}

inline std::wstring AsciiToWide(std::string_view text)
{
    return std::wstring(text.begin(), text.end());
}

// `name<TAB>ext1,ext2<TAB>importer<TAB>score` per shipped reader, newline
// terminated, in stable name order. --list-formats remains the family view.
inline std::wstring BuildReaderListText()
{
    std::wstring text;
    for (const auto& reader : model_core::Readers()) {
        text += AsciiToWide(reader.name);
        text += L'\t';
        bool first = true;
        for (const auto extension : reader.extensions) {
            if (!first) text += L',';
            text += std::wstring(extension);
            first = false;
        }
        text += L'\t';
        text += AsciiToWide(model_core::ImporterKindName(reader.importer));
        text += L'\t';
        text += std::to_wstring(reader.score);
        text += L'\n';
    }
    return text;
}

struct OpenAsReaderEntry {
    const model_core::ReaderDescriptor* reader = nullptr;
    bool isDefault = true;
};

struct OpenAsFamilyEntry {
    model_core::FormatId family = model_core::FormatId::Unknown;
    const model_core::FormatDescriptor* format = nullptr;
    std::vector<OpenAsReaderEntry> readers; // default first, then alternates
};

// One entry per importable family, in catalog order. Every family lists its
// default reader and any shipped alternates (currently only the USD family has
// an alternate). Internal hosts are never listed on their own.
inline std::vector<OpenAsFamilyEntry> OpenAsFamilyEntries()
{
    std::vector<OpenAsFamilyEntry> result;
    for (const auto* descriptor : OpenAsFormats()) {
        bool known = false;
        for (const auto& entry : result) {
            if (entry.family == descriptor->family) { known = true; break; }
        }
        if (known) continue;
        OpenAsFamilyEntry entry;
        entry.family = descriptor->family;
        entry.format = descriptor;
        for (const auto& choice : model_core::ReaderChoicesForFamily(descriptor->family))
            entry.readers.push_back({ choice.reader, choice.isDefault });
        if (!entry.readers.empty()) result.push_back(std::move(entry));
    }
    return result;
}

// `family<TAB>reader<TAB>default|alternate` per family/reader pair. A testable
// text form of the Open As list.
inline std::wstring BuildOpenAsReaderText()
{
    std::wstring text;
    for (const auto& entry : OpenAsFamilyEntries()) {
        const auto* family = model_core::FindById(entry.family);
        const std::wstring familyName = family
            ? AsciiToWide(family->stableName) : std::wstring(L"unknown");
        for (const auto& reader : entry.readers) {
            text += familyName;
            text += L'\t';
            text += AsciiToWide(reader.reader->name);
            text += L'\t';
            text += reader.isDefault ? L"default" : L"alternate";
            text += L'\n';
        }
    }
    return text;
}

// "Display (stable-name)" used by the Open As menu, the info panel and failure
// diagnostics.
inline std::wstring ReaderLabel(const model_core::ReaderDescriptor& reader)
{
    std::wstring label = reader.displayName ? reader.displayName : L"";
    label += L" (";
    label += AsciiToWide(reader.name);
    label += L')';
    return label;
}

// Typed selection failure text for the failure card. The reader name is always
// present; the path is never included.
inline std::wstring ReaderSelectionDiagnostic(const model_core::ReaderSelection& selection)
{
    std::wstring text;
    if (selection.reader) {
        text = L"Reader: ";
        text += ReaderLabel(*selection.reader);
    }
    if (!selection.diagnostic.empty()) {
        if (!text.empty()) text += L"\r\n\r\n";
        text += AsciiToWide(selection.diagnostic);
    }
    return text;
}

} // namespace open_options

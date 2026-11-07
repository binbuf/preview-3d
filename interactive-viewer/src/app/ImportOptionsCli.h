#pragma once

// Header-only wide-character adapter for the shared, closed import-option
// grammar. Kept out of Preview3D.cpp so Tests.Unit/Tests.ImportIsolation can
// exercise the exact `--import-option` path without a window or GPU.

#include "model_core/ImportOptions.h"

#include <string>
#include <string_view>

namespace import_options_cli {

// `--import-option <FORMAT>.<name>=<value>` is ASCII by contract. A non-ASCII
// or over-long argument is a typed invalid value rather than a narrowed byte
// sequence that could alias a different name.
inline bool ParseImportOptionArgument(std::wstring_view value,
                                      model_core::ImportOptionsTable& table,
                                      model_core::ImportOptionError& error)
{
    error = model_core::ImportOptionError::None;
    if (value.empty() || value.size() > 256) {
        error = model_core::ImportOptionError::InvalidValue;
        return false;
    }
    std::string narrow;
    narrow.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < 0 || character > 0x7f) {
            error = model_core::ImportOptionError::InvalidValue;
            return false;
        }
        narrow.push_back(static_cast<char>(character));
    }
    return model_core::ParseImportOptionAssignment(narrow, table, error);
}

} // namespace import_options_cli

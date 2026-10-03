#pragma once

// Pre-open classification of an untrusted primary-source path (T26, SEC-04
// follow-up). The trusted viewer's open guard and the broker's
// OpenAndCanonicalizeSourceFile both refuse UNC and device namespaces *before*
// any CreateFileW, so a crafted path cannot make a trusted process initiate an
// SMB/device open that is only rejected after the connection is already made.
// A primary source must also be drive-qualified absolute, matching the
// requirement the sidecar resolver already enforces on its references.
//
// Reject, do not sanitize: this classifies a normalized copy of the text and
// never returns a rewritten path for the caller to open. Forward slashes are
// valid Win32 separators, so they are normalized before classification --
// otherwise "//server/share" (a UNC) would be read as a relative path.

#include <string>
#include <string_view>

namespace platform {

enum class SourcePathKind {
    // "X:\..." or the extended-length "\\?\X:\..." form for a drive path.
    LocalAbsolute,
    // UNC ("\\server\share", "//server/share"), the device namespace
    // ("\\.\PhysicalDrive0"), or an extended non-drive namespace
    // ("\\?\UNC\...", "\\?\GLOBALROOT\...", "\\?\Volume{...}\").
    RemoteOrDevice,
    // Relative, root-relative ("\foo"), or drive-relative ("C:foo").
    Relative,
};

inline bool IsAsciiAlpha(wchar_t character)
{
    return (character >= L'A' && character <= L'Z') || (character >= L'a' && character <= L'z');
}

// Replaces '/' with '\' so a forward-slash path is classified by the same
// rules as its backslash form. The result is only ever classified, never
// opened, by ClassifySourcePath's callers.
inline std::wstring NormalizeSourcePathSeparators(std::wstring_view path)
{
    std::wstring normalized(path);
    for (wchar_t& character : normalized) {
        if (character == L'/') {
            character = L'\\';
        }
    }
    return normalized;
}

inline SourcePathKind ClassifySourcePath(std::wstring_view rawPath)
{
    if (rawPath.empty()) {
        return SourcePathKind::Relative;
    }
    const std::wstring path = NormalizeSourcePathSeparators(rawPath);

    const bool extendedPrefix = path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\'
        && path[2] == L'?' && path[3] == L'\\';
    if (extendedPrefix) {
        // Only "\\?\<drive>:\..." is a local drive path. "\\?\UNC\...",
        // "\\?\GLOBALROOT\..." and "\\?\Volume{...}\" are remote/device namespaces.
        const bool extendedDrive = path.size() >= 7 && IsAsciiAlpha(path[4])
            && path[5] == L':' && path[6] == L'\\';
        return extendedDrive ? SourcePathKind::LocalAbsolute : SourcePathKind::RemoteOrDevice;
    }
    // Any other leading double separator is a UNC ("\\server\share") or the
    // device namespace ("\\.\").
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return SourcePathKind::RemoteOrDevice;
    }
    const bool driveQualifiedAbsolute = path.size() >= 3 && IsAsciiAlpha(path[0])
        && path[1] == L':' && path[2] == L'\\';
    return driveQualifiedAbsolute ? SourcePathKind::LocalAbsolute : SourcePathKind::Relative;
}

} // namespace platform
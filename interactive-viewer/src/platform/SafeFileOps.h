#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// Local attack-surface primitives shared by the trusted viewer and its unit
// tests (SEC-11 / T11). Everything here is deliberately free of app state so
// the exact shipped code can be exercised in-process.
namespace preview3d::safeio
{
// Returns text safe to place in a Win32 dialog/control: C0/C1/DEL controls and
// Unicode bidi/format controls are replaced with a space, the result is
// trimmed, and it is bounded to maxLength UTF-16 code units (an ellipsis is
// appended on truncation). Worker-supplied asset references go through this
// before they reach a dialog so control characters cannot spoof dialog text.
std::wstring SanitizeDisplayText(std::wstring_view text, std::size_t maxLength = 512);

// Fills outRoot with the app-owned per-user data directory,
// %LOCALAPPDATA%\Binbuf\Preview 3D. Returns false if the known folder cannot
// be resolved.
bool AppDataDirectory(std::wstring& outRoot);

// Validates that `path` is a local, absolute, drive-letter path suitable as an
// output file: no UNC/device prefix, no alternate data stream or extra colon,
// no "."/".." traversal component, no control characters, and (when
// requiredExtension is non-empty) exactly that extension. `appOwnedRoot`, when
// non-empty, must be a case-insensitive prefix of the path (the caller owns the
// directory and its tree). Returns false and sets `error` otherwise.
bool IsSafeOutputPath(std::wstring_view path, std::wstring_view requiredExtension,
                      std::wstring_view appOwnedRoot, std::wstring& error);

// Writes `bytes` to `finalPath` through a uniquely-named sibling temporary so a
// pre-planted fixed-name link (the old "<final>.tmp" vector) can never redirect
// the write. The temporary is created with CREATE_NEW and
// FILE_FLAG_OPEN_REPARSE_POINT, its handle is re-validated (no reparse point),
// flushed, and then atomically moved over `finalPath`. `replaceExisting` maps
// to MOVEFILE_REPLACE_EXISTING; when false an existing destination fails
// closed. On any failure the temporary is deleted and `error` is set.
bool WriteFileAtomically(std::wstring_view finalPath, std::span<const std::byte> bytes,
                         bool replaceExisting, std::wstring& error);
}
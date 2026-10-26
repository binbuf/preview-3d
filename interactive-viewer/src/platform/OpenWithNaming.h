#pragma once

#include <string>

namespace open_with
{
// True when `uiName` looks like a raw executable name or path rather than a
// friendly application name. Shell handler UI names are not reliably friendly:
// some registrations return things such as "f3d.exe" or "meshlab.exe".
bool LooksLikeFileName(const std::wstring& uiName, const std::wstring& handlerName);

// Returns the label the Open With popup should draw. `fileDescription` and
// `productName` are optional version-resource strings read by the discovery
// worker. Friendly shell names are returned unchanged; filename-like names are
// replaced by the best available friendly description or a cleaned file stem.
std::wstring NormalizeDisplayName(const std::wstring& uiName, const std::wstring& handlerName,
    const std::wstring& fileDescription, const std::wstring& productName);

// Exposed for tests and for the rare caller that only has a handler path.
std::wstring FriendlyFileStem(const std::wstring& handlerName);
}

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// OS shell interop only — no UI. Open With discovery is asynchronous and its
// bounded local cache contains application-handler metadata only (never model
// paths or launch history). Both features degrade gracefully.

enum class OpenWithGroup
{
    Cad,
    Modeling,
    Printing,
    Recommended,
    Status,
    Fallback,
};

// One entry in the "Open With" list for the current file — the same
// recommended-handler list Explorer's own Open With submenu shows, via
// SHAssocEnumHandlers, plus a trailing entry that always falls back to the
// system picker (SHOpenWithDialog).
struct OpenWithEntry
{
    std::wstring displayName;
    OpenWithGroup group = OpenWithGroup::Recommended;
    bool enabled = true;
    std::function<bool(const std::wstring& path)> invoke;
};

// Starts a background load/refresh. Cached handlers are not revalidated on
// startup; discovery runs at most weekly to add newly registered apps. A
// failed invocation invalidates that entry and requests an immediate rescan.
void InitializeOpenWithCatalog(HWND notificationWindow, UINT launchFailureMessage);
void ShutdownOpenWithCatalog();

std::vector<OpenWithEntry> EnumerateOpenWithHandlers(const std::wstring& path);

// Shows the Windows Share flyout (DataTransferManager), anchored to `window`,
// offering `path` as the shared file.
bool ShowWindowsShare(HWND window, const std::wstring& path, std::wstring& error);

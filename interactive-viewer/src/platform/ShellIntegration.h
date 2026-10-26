#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "OpenWithIcon.h"
#include "OpenWithRanking.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// OS shell interop only — no UI. Open With discovery is asynchronous and its
// bounded local cache contains application-handler metadata only (never model
// paths or launch history). Both features degrade gracefully.

// One application in the custom "Open With" strip. The fallback picker is a
// UI-owned action, not an entry here.
struct OpenWithEntry
{
    std::wstring displayName;
    std::wstring handlerKey;
    std::wstring iconPath;
    int iconIndex = 0;
    OpenWithGroup group = OpenWithGroup::Recommended;
    bool recommended = false;
    bool enabled = true;
    std::function<bool(const std::wstring& path)> invoke;
};

struct OpenWithCatalog
{
    std::vector<OpenWithEntry> apps; // ranked, self-filtered, deduplicated
    bool loading = false;            // discovery is running with no cached apps
};

// Startup notification contract for the discovery worker. Any field may be 0
// when the caller does not need that notification.
struct OpenWithNotifications
{
    UINT launchFailed = 0;
    UINT catalogChanged = 0;
    UINT iconsReady = 0;
};

struct OpenWithIconRequest
{
    std::wstring handlerKey;
    std::wstring iconPath;
    int iconIndex = 0;
    int pixelSize = 32;
};

struct OpenWithIconResult
{
    std::wstring handlerKey;
    open_with::IconPixels pixels;
};

// Starts a background load/refresh. Cached handlers are not revalidated on
// startup; discovery runs at most weekly to add newly registered apps. A
// failed invocation invalidates that entry and requests an immediate rescan.
void InitializeOpenWithCatalog(HWND notificationWindow, OpenWithNotifications notifications);
void ShutdownOpenWithCatalog();

// Reads the cache and returns the current ranked list. Never enumerates the
// shell or extracts icons; both happen on the discovery worker.
OpenWithCatalog EnumerateOpenWithHandlers(const std::wstring& path);

// Queues icon extraction for the given handlers. Results arrive through the
// iconsReady notification and are drained with TakeReadyOpenWithIcons.
void RequestOpenWithIcons(std::vector<OpenWithIconRequest> requests);
std::vector<OpenWithIconResult> TakeReadyOpenWithIcons();

// The system picker remains the fallback for browsing arbitrary executables.
bool ShowOpenWithDialog(const std::wstring& path);

// Shows the Windows Share flyout (DataTransferManager), anchored to `window`,
// offering `path` as the shared file.
bool ShowWindowsShare(HWND window, const std::wstring& path, std::wstring& error);

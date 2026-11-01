#include "framework.h"
#include "ShellIntegration.h"
#include "OpenWithCache.h"
#include "OpenWithNaming.h"
#include "SafeFileOps.h"
#include "model_core/FormatSelection.h"

#include <shellapi.h>
#include <shlobj.h>
#include <span>
#include <shobjidl.h>
#include <wincodec.h>
#include <winver.h>
#include <wrl/client.h>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr std::uint32_t kCatalogRevision = 8;
constexpr std::uint64_t kDiscoveryInterval = 7ull * 24 * 60 * 60 * 10'000'000;
constexpr std::size_t kMaximumCacheBytes = 256 * 1024;
constexpr wchar_t kCacheFileName[] = L"open-with-apps-v1.dat";

// Direct-open extensions come from the format catalog; this list must never be
// hand-maintained alongside it (the [format-installer] test covers the
// installer and the catalog-level test covers this helper).
const std::vector<std::wstring>& SupportedExtensions()
{
    static const std::vector<std::wstring> extensions = model_core::DirectOpenExtensions();
    return extensions;
}

struct CatalogApp
{
    const wchar_t* id;
    const wchar_t* displayName;
    OpenWithGroup group;
    std::initializer_list<const wchar_t*> matchTokens;
};

// Ordering here is ordering in the menu. Registration remains authoritative:
// an application is shown only if Windows exposes a handler for this file
// type, so the viewer never fabricates a vendor-specific command line.
const std::array<CatalogApp, 20>& CuratedCatalog()
{
    static const std::array<CatalogApp, 20> catalog{{
        { L"freecad", L"FreeCAD", OpenWithGroup::Cad, { L"freecad" } },
        { L"openscad", L"OpenSCAD", OpenWithGroup::Cad, { L"openscad" } },
        { L"fusion360", L"Autodesk Fusion", OpenWithGroup::Cad, { L"autodesk fusion", L"fusion 360", L"fusionlauncher" } },
        { L"rhino", L"Rhino", OpenWithGroup::Cad, { L"rhinoceros", L"rhino.exe" } },
        { L"solidworks", L"SOLIDWORKS", OpenWithGroup::Cad, { L"solidworks", L"sldworks" } },
        { L"inventor", L"Autodesk Inventor", OpenWithGroup::Cad, { L"autodesk inventor", L"inventor.exe" } },
        { L"blender", L"Blender", OpenWithGroup::Modeling, { L"blender" } },
        { L"sketchup", L"SketchUp", OpenWithGroup::Modeling, { L"sketchup" } },
        { L"3dsmax", L"Autodesk 3ds Max", OpenWithGroup::Modeling, { L"3ds max", L"3dsmax" } },
        { L"maya", L"Autodesk Maya", OpenWithGroup::Modeling, { L"autodesk maya", L"maya.exe" } },
        { L"zbrush", L"ZBrush", OpenWithGroup::Modeling, { L"zbrush" } },
        { L"meshlab", L"MeshLab", OpenWithGroup::Modeling, { L"meshlab" } },
        { L"plasticity", L"Plasticity", OpenWithGroup::Modeling, { L"plasticity" } },
        { L"prusaslicer", L"PrusaSlicer", OpenWithGroup::Printing, { L"prusaslicer", L"prusa-slicer" } },
        { L"orcaslicer", L"OrcaSlicer", OpenWithGroup::Printing, { L"orcaslicer", L"orca-slicer" } },
        { L"bambustudio", L"Bambu Studio", OpenWithGroup::Printing, { L"bambu studio", L"bambu-studio" } },
        { L"cura", L"UltiMaker Cura", OpenWithGroup::Printing, { L"ultimaker cura", L"ultimaker-cura", L"cura.exe" } },
        { L"lychee", L"Lychee Slicer", OpenWithGroup::Printing, { L"lychee slicer" } },
        { L"chitubox", L"CHITUBOX", OpenWithGroup::Printing, { L"chitubox" } },
        { L"simplify3d", L"Simplify3D", OpenWithGroup::Printing, { L"simplify3d" } },
    }};
    return catalog;
}

std::wstring Fold(std::wstring value)
{
    for (auto& character : value) character = static_cast<wchar_t>(towlower(character));
    return value;
}

const CatalogApp* CatalogById(const std::wstring& id)
{
    for (const auto& app : CuratedCatalog())
        if (_wcsicmp(app.id, id.c_str()) == 0) return &app;
    return nullptr;
}

const CatalogApp* MatchCatalog(const std::wstring& handlerName, const std::wstring& displayName)
{
    const std::wstring haystack = Fold(handlerName + L"\n" + displayName);
    for (const auto& app : CuratedCatalog())
        for (const auto* token : app.matchTokens)
            if (haystack.find(token) != std::wstring::npos) return &app;
    return nullptr;
}

std::wstring ExtensionOf(const std::wstring& path)
{
    const std::size_t dot = path.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring{} : Fold(path.substr(dot));
}

bool SupportedExtension(const std::wstring& extension)
{
    for (const auto& supported : SupportedExtensions())
        if (_wcsicmp(supported.c_str(), extension.c_str()) == 0) return true;
    return false;
}

std::wstring SafeMenuLabel(std::wstring value)
{
    if (value.size() > 128) value.resize(128);
    for (auto& character : value)
        if (character < L' ' || character == 0x7f) character = L' ';
    return value;
}

std::wstring HandlerKey(const std::wstring& extension, const std::wstring& handlerName)
{
    return Fold(extension) + L"\n" + Fold(handlerName);
}

std::uint64_t CurrentFileTime()
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

bool IsLocalAbsolutePath(const std::wstring& path)
{
    if (path.size() < 3) return false;
    if (!iswalpha(path[0]) || path[1] != L':') return false;
    return path[2] == L'\\' || path[2] == L'/';
}

std::wstring ModulePath()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size())
        {
            buffer.resize(length);
            return buffer;
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring CanonicalIdentity(const std::wstring& path)
{
    if (path.empty()) return {};
    std::wstring full = path;
    if (IsLocalAbsolutePath(path))
    {
        std::wstring buffer(MAX_PATH, L'\0');
        const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()),
            buffer.data(), nullptr);
        if (length > 0 && length < buffer.size())
        {
            buffer.resize(length);
            full = std::move(buffer);
        }
    }
    return Fold(full);
}

// Reads FileDescription/ProductName from an executable's version resource.
// Used only to clean filename-like shell UI names; failure is normal.
void ReadVersionStrings(const std::wstring& path, std::wstring& description, std::wstring& product)
{
    description.clear();
    product.clear();
    if (!IsLocalAbsolutePath(path)) return;

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0 || size > 1024 * 1024) return;
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return;

    struct Translation { WORD language; WORD codePage; };
    Translation* translations = nullptr;
    UINT translationBytes = 0;
    std::wstring prefix = L"\\StringFileInfo\\040904B0\\";
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
        reinterpret_cast<void**>(&translations), &translationBytes) &&
        translations && translationBytes >= sizeof(Translation))
    {
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"\\StringFileInfo\\%04x%04x\\", translations[0].language, translations[0].codePage);
        prefix = buffer;
    }

    auto query = [&](const wchar_t* name) -> std::wstring
    {
        const std::wstring key = prefix + name;
        wchar_t* value = nullptr;
        UINT bytes = 0;
        if (!VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&value), &bytes) ||
            !value || bytes < sizeof(wchar_t)) return {};
        const std::size_t length = bytes / sizeof(wchar_t);
        std::wstring result(value, wcsnlen(value, length));
        return result;
    };
    description = query(L"FileDescription");
    product = query(L"ProductName");
}

bool ExtractIconPixels(const std::wstring& iconPath, int iconIndex, int pixelSize,
    open_with::IconPixels& output)
{
    output = {};
    if (!IsLocalAbsolutePath(iconPath) || pixelSize < 16 || pixelSize > 64) return false;

    HICON icon = nullptr;
    const UINT size = static_cast<UINT>(pixelSize);
    if (FAILED(SHDefExtractIconW(iconPath.c_str(), iconIndex, 0, &icon, nullptr, MAKELONG(size, size))) ||
        !icon) return false;

    ComPtr<IWICImagingFactory> factory;
    const HRESULT created = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(created))
    {
        DestroyIcon(icon);
        return false;
    }

    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICFormatConverter> converter;
    bool ok = SUCCEEDED(factory->CreateBitmapFromHICON(icon, &bitmap)) && bitmap &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) && converter &&
        SUCCEEDED(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
    DestroyIcon(icon);
    if (!ok) return false;

    UINT width = 0;
    UINT height = 0;
    if (FAILED(bitmap->GetSize(&width, &height)) || width == 0 || height == 0 ||
        width > 64 || height > 64) return false;

    output.width = static_cast<int>(width);
    output.height = static_cast<int>(height);
    output.bgra.resize(static_cast<std::size_t>(width) * height * 4);
    const UINT stride = width * 4;
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(output.bgra.size()),
        output.bgra.data())))
    {
        output = {};
        return false;
    }
    return true;
}

std::wstring IconCacheKey(const std::wstring& handlerKey, int pixelSize)
{
    return Fold(handlerKey) + L"\n" + std::to_wstring(pixelSize);
}

bool CachePath(std::wstring& path)
{
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) || !localAppData)
    {
        if (localAppData) CoTaskMemFree(localAppData);
        return false;
    }
    path = localAppData;
    CoTaskMemFree(localAppData);
    path += L"\\Binbuf\\Preview 3D";
    CreateDirectoryW((path.substr(0, path.find_last_of(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\";
    path += kCacheFileName;
    return true;
}

bool LoadCache(open_with::CacheState& state)
{
    std::wstring path;
    if (!CachePath(path)) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > static_cast<LONGLONG>(kMaximumCacheBytes))
    {
        CloseHandle(file);
        return false;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    return ok && read == bytes.size() && open_with::DecodeCache(bytes, state);
}

void SaveCache(const open_with::CacheState& state)
{
    std::vector<std::byte> bytes;
    if (!open_with::EncodeCache(state, bytes)) return;
    std::wstring finalPath;
    if (!CachePath(finalPath)) return;
    std::wstring error;
    preview3d::safeio::WriteFileAtomically(finalPath, std::as_bytes(std::span(bytes)), true, error);
}

// Launches the handler's executable directly with the model path as its
// argument. The shell's IAssocHandler::Invoke path is deliberately avoided:
// when the extension has a UserChoice, invoking a non-default handler shows the
// OS "How do you want to open this file?" picker instead of launching it.
bool LaunchExecutableWithFile(const std::wstring& executable, const std::wstring& path)
{
    if (!IsLocalAbsolutePath(executable) || path.empty()) return false;
    const std::wstring parameters = L"\"" + path + L"\"";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"open";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return false;
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

void AddDiscoveredHandlers(const wchar_t* extension, ASSOC_FILTER filter, bool popularOnly,
    std::unordered_map<std::wstring, open_with::CachedHandler>& discovered)
{
    ComPtr<IEnumAssocHandlers> enumeration;
    if (FAILED(SHAssocEnumHandlers(extension, filter, &enumeration)) || !enumeration) return;
    ComPtr<IAssocHandler> handler;
    ULONG fetched = 0;
    while (enumeration->Next(1, &handler, &fetched) == S_OK && fetched == 1)
    {
        PWSTR rawName = nullptr;
        PWSTR rawDisplayName = nullptr;
        PWSTR rawIconPath = nullptr;
        int rawIconIndex = 0;
        const HRESULT nameResult = handler->GetName(&rawName);
        const HRESULT displayResult = handler->GetUIName(&rawDisplayName);
        if (SUCCEEDED(nameResult) && rawName && *rawName &&
            SUCCEEDED(displayResult) && rawDisplayName && *rawDisplayName &&
            // Only executable-path handlers can be launched directly. Package
            // (AUMID) and legacy OpenWithList handlers expose a friendly name
            // here and can only be started through the OS picker, so they are
            // not offered as tiles.
            IsLocalAbsolutePath(rawName))
        {
            if (FAILED(handler->GetIconLocation(&rawIconPath, &rawIconIndex)))
            {
                rawIconPath = nullptr;
                rawIconIndex = 0;
            }
            const HRESULT recommendedResult = handler->IsRecommended();

            std::wstring description;
            std::wstring product;
            ReadVersionStrings(rawName, description, product);

            open_with::CachedHandler record{};
            record.extension = extension;
            record.handlerName = rawName;
            record.displayName = SafeMenuLabel(open_with::NormalizeDisplayName(
                rawDisplayName, rawName, description, product));
            record.iconPath = rawIconPath ? rawIconPath : L"";
            record.iconIndex = std::clamp(rawIconIndex, -32768, 32767);
            record.recommended = recommendedResult == S_OK;
            if (!IsLocalAbsolutePath(record.iconPath))
            {
                record.iconPath = record.handlerName;
                record.iconIndex = 0;
            }
            if (record.displayName.empty()) record.displayName = L"Unknown app";

            const CatalogApp* catalog = MatchCatalog(record.handlerName, record.displayName);
            if (catalog) record.catalogId = catalog->id;
            if (!popularOnly || catalog)
            {
                const std::wstring key = HandlerKey(record.extension, record.handlerName);
                auto found = discovered.find(key);
                if (found == discovered.end() ||
                    (!record.catalogId.empty() && found->second.catalogId.empty()))
                    discovered[key] = std::move(record);
            }
        }
        if (rawName) CoTaskMemFree(rawName);
        if (rawDisplayName) CoTaskMemFree(rawDisplayName);
        if (rawIconPath) CoTaskMemFree(rawIconPath);
        handler.Reset();
    }
}

std::vector<open_with::CachedHandler> DiscoverHandlers()
{
    std::unordered_map<std::wstring, open_with::CachedHandler> discovered;
    for (const auto& extension : SupportedExtensions())
    {
        AddDiscoveredHandlers(extension.c_str(), ASSOC_FILTER_RECOMMENDED, false, discovered);
        AddDiscoveredHandlers(extension.c_str(), ASSOC_FILTER_NONE, true, discovered);
    }
    std::vector<open_with::CachedHandler> records;
    records.reserve(discovered.size());
    for (auto& [key, record] : discovered)
    {
        (void)key;
        records.push_back(std::move(record));
    }
    return records;
}

struct CatalogRuntime
{
    struct Invocation
    {
        std::wstring extension;
        std::wstring handlerName;
        std::wstring path;
    };

    std::mutex mutex;
    std::condition_variable condition;
    open_with::CacheState cache;
    bool initialized = false;
    bool loaded = false;
    bool scanning = false;
    bool rescanRequested = false;
    bool stopping = false;
    HWND notificationWindow = nullptr;
    OpenWithNotifications notifications;
    std::wstring selfIdentity;
    std::wstring selfFileName;
    std::vector<std::wstring> selfNames;
    std::jthread worker;
    std::unordered_set<std::wstring> failedHandlers;
    std::deque<Invocation> invocations;
    std::deque<OpenWithIconRequest> iconRequests;
    std::unordered_set<std::wstring> iconInFlight;
    std::unordered_map<std::wstring, open_with::IconPixels> iconCache;
    std::vector<OpenWithIconResult> readyIcons;
};

CatalogRuntime& Runtime()
{
    static auto* runtime = new CatalogRuntime();
    return *runtime;
}

bool DiscoveryDue(const open_with::CacheState& cache)
{
    if (cache.catalogRevision != kCatalogRevision || cache.lastDiscoveryFileTime == 0) return true;
    const std::uint64_t now = CurrentFileTime();
    return now < cache.lastDiscoveryFileTime || now - cache.lastDiscoveryFileTime >= kDiscoveryInterval;
}

void MergeNewHandlers(open_with::CacheState& cache, std::vector<open_with::CachedHandler> discovered,
    const std::unordered_set<std::wstring>& failedHandlers)
{
    std::unordered_map<std::wstring, std::size_t> known;
    for (std::size_t index = 0; index < cache.handlers.size(); ++index)
        known.emplace(HandlerKey(cache.handlers[index].extension, cache.handlers[index].handlerName), index);
    for (auto& handler : discovered)
    {
        const auto key = HandlerKey(handler.extension, handler.handlerName);
        if (failedHandlers.contains(key)) continue;
        const auto found = known.find(key);
        if (found == known.end() && cache.handlers.size() < 256)
        {
            known.emplace(key, cache.handlers.size());
            cache.handlers.push_back(std::move(handler));
        }
        else if (found != known.end())
        {
            auto& existing = cache.handlers[found->second];
            if (existing.catalogId.empty() && !handler.catalogId.empty())
                existing.catalogId = std::move(handler.catalogId);
            if (!handler.displayName.empty()) existing.displayName = std::move(handler.displayName);
            if (!handler.iconPath.empty())
            {
                existing.iconPath = std::move(handler.iconPath);
                existing.iconIndex = handler.iconIndex;
            }
            existing.recommended = existing.recommended || handler.recommended;
        }
    }
    cache.catalogRevision = kCatalogRevision;
    cache.lastDiscoveryFileTime = CurrentFileTime();
}

bool InvokeNamedHandler(const std::wstring& handlerName, const std::wstring& path);

void DiscoveryWorker()
{
    // Shell handler enumeration and icon extraction stay off the UI thread.
    // Launching a resolved executable is a direct process start, not a shell
    // handler invocation.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    open_with::CacheState loaded;
    LoadCache(loaded);
    bool sanitized = false;
    std::erase_if(loaded.handlers, [&](auto& handler) {
        if (!SupportedExtension(handler.extension)) { sanitized = true; return true; }
        if (!IsLocalAbsolutePath(handler.handlerName))
        {
            // Legacy caches may hold package-style or OpenWithList identities
            // that cannot be launched directly; drop them before ranking.
            sanitized = true;
            return true;
        }
        if (!handler.catalogId.empty() && !CatalogById(handler.catalogId))
        {
            handler.catalogId.clear();
            sanitized = true;
        }
        if (open_with::LooksLikeFileName(handler.displayName, handler.handlerName))
        {
            handler.displayName = SafeMenuLabel(open_with::NormalizeDisplayName(
                handler.displayName, handler.handlerName, {}, {}));
            sanitized = true;
        }
        if (!handler.iconPath.empty() && !IsLocalAbsolutePath(handler.iconPath))
        {
            handler.iconPath.clear();
            handler.iconIndex = 0;
            sanitized = true;
        }
        if (handler.iconIndex < -32768 || handler.iconIndex > 32767)
        {
            handler.iconIndex = 0;
            sanitized = true;
        }
        return false;
    });
    if (sanitized) loaded.lastDiscoveryFileTime = 0;
    auto& runtime = Runtime();
    HWND loadedNotificationWindow = nullptr;
    UINT loadedChangedMessage = 0;
    {
        std::lock_guard lock(runtime.mutex);
        runtime.cache = std::move(loaded);
        runtime.loaded = true;
        runtime.rescanRequested = DiscoveryDue(runtime.cache);
        loadedNotificationWindow = runtime.notificationWindow;
        loadedChangedMessage = runtime.notifications.catalogChanged;
        runtime.condition.notify_all();
    }
    if (loadedNotificationWindow && loadedChangedMessage)
        PostMessageW(loadedNotificationWindow, loadedChangedMessage, 0, 0);

    std::unique_lock lock(runtime.mutex);
    while (!runtime.stopping)
    {
        runtime.condition.wait(lock, [&] {
            return runtime.stopping || !runtime.invocations.empty() || !runtime.iconRequests.empty() ||
                runtime.rescanRequested;
        });
        if (runtime.stopping) break;
        if (!runtime.invocations.empty())
        {
            auto invocation = std::move(runtime.invocations.front());
            runtime.invocations.pop_front();
            lock.unlock();
            const bool invoked = InvokeNamedHandler(invocation.handlerName, invocation.path);
            lock.lock();
            if (!invoked)
            {
                const auto key = HandlerKey(invocation.extension, invocation.handlerName);
                std::erase_if(runtime.cache.handlers, [&](const auto& record) {
                    return HandlerKey(record.extension, record.handlerName) == key;
                });
                runtime.failedHandlers.insert(key);
                runtime.rescanRequested = true;
                const auto invalidated = runtime.cache;
                const HWND notificationWindow = runtime.notificationWindow;
                const UINT failureMessage = runtime.notifications.launchFailed;
                const UINT changedMessage = runtime.notifications.catalogChanged;
                lock.unlock();
                SaveCache(invalidated);
                if (notificationWindow && failureMessage)
                    PostMessageW(notificationWindow, failureMessage, 0, 0);
                if (notificationWindow && changedMessage)
                    PostMessageW(notificationWindow, changedMessage, 0, 0);
                lock.lock();
            }
            continue;
        }
        if (!runtime.iconRequests.empty())
        {
            auto request = std::move(runtime.iconRequests.front());
            runtime.iconRequests.pop_front();
            const std::wstring key = IconCacheKey(request.handlerKey, request.pixelSize);
            if (const auto cached = runtime.iconCache.find(key); cached != runtime.iconCache.end())
            {
                runtime.readyIcons.push_back({ request.handlerKey, cached->second });
                runtime.iconInFlight.erase(key);
                const HWND notificationWindow = runtime.notificationWindow;
                const UINT iconsMessage = runtime.notifications.iconsReady;
                lock.unlock();
                if (notificationWindow && iconsMessage)
                    PostMessageW(notificationWindow, iconsMessage, 0, 0);
                lock.lock();
                continue;
            }
            lock.unlock();
            open_with::IconPixels pixels;
            ExtractIconPixels(request.iconPath, request.iconIndex, request.pixelSize, pixels);
            lock.lock();
            if (runtime.stopping) break;
            if (runtime.iconCache.size() >= 64) runtime.iconCache.erase(runtime.iconCache.begin());
            runtime.iconCache[key] = pixels;
            runtime.iconInFlight.erase(key);
            runtime.readyIcons.push_back({ request.handlerKey, std::move(pixels) });
            const HWND notificationWindow = runtime.notificationWindow;
            const UINT iconsMessage = runtime.notifications.iconsReady;
            lock.unlock();
            if (notificationWindow && iconsMessage)
                PostMessageW(notificationWindow, iconsMessage, 0, 0);
            lock.lock();
            continue;
        }
        runtime.rescanRequested = false;
        runtime.scanning = true;
        const auto beforeScan = runtime.cache;
        lock.unlock();
        SaveCache(beforeScan);
        auto discovered = DiscoverHandlers();
        lock.lock();
        if (runtime.stopping) break;
        MergeNewHandlers(runtime.cache, std::move(discovered), runtime.failedHandlers);
        runtime.scanning = false;
        const auto afterScan = runtime.cache;
        const HWND notificationWindow = runtime.notificationWindow;
        const UINT changedMessage = runtime.notifications.catalogChanged;
        lock.unlock();
        SaveCache(afterScan);
        if (notificationWindow && changedMessage)
            PostMessageW(notificationWindow, changedMessage, 0, 0);
        lock.lock();
    }
    runtime.scanning = false;
    lock.unlock();
    if (SUCCEEDED(com)) CoUninitialize();
}

bool InvokeNamedHandler(const std::wstring& handlerName, const std::wstring& path)
{
    return LaunchExecutableWithFile(handlerName, path);
}

bool QueueHandlerInvocation(const std::wstring& extension, const std::wstring& handlerName,
    const std::wstring& path)
{
    auto& runtime = Runtime();
    std::lock_guard lock(runtime.mutex);
    if (!runtime.initialized || runtime.stopping) return false;
    runtime.invocations.push_back({ extension, handlerName, path });
    runtime.condition.notify_one();
    return true;
}
}

void InitializeOpenWithCatalog(HWND notificationWindow, OpenWithNotifications notifications)
{
    auto& runtime = Runtime();
    std::lock_guard lock(runtime.mutex);
    if (runtime.initialized) return;
    runtime.initialized = true;
    runtime.stopping = false;
    runtime.notificationWindow = notificationWindow;
    runtime.notifications = notifications;
    const std::wstring selfPath = ModulePath();
    runtime.selfIdentity = CanonicalIdentity(selfPath);
    runtime.selfFileName = Fold(open_with::FileNameOf(selfPath));
    runtime.selfNames = { L"Preview 3D", L"3D Preview" };
    runtime.worker = std::jthread(DiscoveryWorker);
}

void ShutdownOpenWithCatalog()
{
    auto& runtime = Runtime();
    std::jthread worker;
    {
        std::lock_guard lock(runtime.mutex);
        if (!runtime.initialized) return;
        runtime.stopping = true;
        runtime.condition.notify_all();
        worker = std::move(runtime.worker);
    }
    if (worker.joinable()) worker.join();
    std::lock_guard lock(runtime.mutex);
    runtime.failedHandlers.clear();
    runtime.invocations.clear();
    runtime.iconRequests.clear();
    runtime.iconInFlight.clear();
    runtime.iconCache.clear();
    runtime.readyIcons.clear();
    runtime.notificationWindow = nullptr;
    runtime.notifications = {};
    runtime.selfIdentity.clear();
    runtime.selfFileName.clear();
    runtime.selfNames.clear();
    runtime.initialized = false;
}

OpenWithCatalog EnumerateOpenWithHandlers(const std::wstring& path)
{
    const std::wstring extension = ExtensionOf(path);
    std::vector<open_with::CachedHandler> records;
    open_with::RankOptions options;
    bool waiting = false;
    {
        auto& runtime = Runtime();
        std::lock_guard lock(runtime.mutex);
        waiting = !runtime.loaded || runtime.scanning;
        options.selfIdentity = runtime.selfIdentity;
        options.selfFileName = runtime.selfFileName;
        options.selfNames = runtime.selfNames;
        for (const auto& record : runtime.cache.handlers)
            if (_wcsicmp(record.extension.c_str(), extension.c_str()) == 0)
                records.push_back(record);
    }

    std::vector<open_with::RankedHandler> candidates;
    candidates.reserve(records.size());
    for (auto& record : records)
    {
        open_with::RankedHandler candidate;
        candidate.extension = record.extension;
        candidate.handlerName = record.handlerName;
        candidate.displayName = SafeMenuLabel(record.displayName);
        candidate.catalogId = record.catalogId;
        candidate.identity = CanonicalIdentity(record.handlerName);
        candidate.iconPath = record.iconPath;
        candidate.iconIndex = record.iconIndex;
        candidate.recommended = record.recommended;
        if (const auto* catalog = CatalogById(record.catalogId))
        {
            candidate.curated = true;
            candidate.group = catalog->group;
            candidate.displayName = catalog->displayName;
            candidate.affinity = open_with::ExtensionAffinity(extension, catalog->group);
            const auto& curated = CuratedCatalog();
            for (std::size_t index = 0; index < curated.size(); ++index)
                if (_wcsicmp(curated[index].id, catalog->id) == 0)
                {
                    candidate.catalogOrder = static_cast<int>(index);
                    break;
                }
        }
        else
        {
            candidate.curated = false;
            candidate.group = OpenWithGroup::Recommended;
            candidate.affinity = 1;
            candidate.catalogOrder = INT_MAX;
        }
        candidates.push_back(std::move(candidate));
    }

    auto ranked = open_with::RankHandlers(std::move(candidates), options);
    OpenWithCatalog result;
    result.loading = ranked.empty() && waiting;
    result.apps.reserve(ranked.size());
    for (auto& handler : ranked)
    {
        OpenWithEntry entry;
        entry.displayName = SafeMenuLabel(handler.displayName);
        entry.handlerKey = handler.identity.empty() ? Fold(handler.handlerName) : handler.identity;
        entry.iconPath = handler.iconPath;
        entry.iconIndex = handler.iconIndex;
        entry.group = handler.group;
        entry.recommended = handler.recommended;
        const auto handlerName = handler.handlerName;
        entry.invoke = [extension, handlerName](const std::wstring& filePath) {
            return QueueHandlerInvocation(extension, handlerName, filePath);
        };
        result.apps.push_back(std::move(entry));
    }
    return result;
}

void RequestOpenWithIcons(std::vector<OpenWithIconRequest> requests)
{
    auto& runtime = Runtime();
    bool notify = false;
    HWND notificationWindow = nullptr;
    UINT iconsMessage = 0;
    {
        std::lock_guard lock(runtime.mutex);
        if (!runtime.initialized || runtime.stopping) return;
        notificationWindow = runtime.notificationWindow;
        iconsMessage = runtime.notifications.iconsReady;
        for (auto& request : requests)
        {
            if (request.handlerKey.empty() || request.iconPath.empty() ||
                request.pixelSize < 16 || request.pixelSize > 64) continue;
            const std::wstring key = IconCacheKey(request.handlerKey, request.pixelSize);
            if (runtime.iconInFlight.contains(key)) continue;
            if (const auto cached = runtime.iconCache.find(key); cached != runtime.iconCache.end())
            {
                runtime.readyIcons.push_back({ request.handlerKey, cached->second });
                notify = true;
                continue;
            }
            runtime.iconInFlight.insert(key);
            runtime.iconRequests.push_back(std::move(request));
        }
        if (!runtime.iconRequests.empty()) runtime.condition.notify_one();
    }
    if (notify && notificationWindow && iconsMessage)
        PostMessageW(notificationWindow, iconsMessage, 0, 0);
}

std::vector<OpenWithIconResult> TakeReadyOpenWithIcons()
{
    auto& runtime = Runtime();
    std::lock_guard lock(runtime.mutex);
    std::vector<OpenWithIconResult> result;
    result.swap(runtime.readyIcons);
    return result;
}

bool ShowOpenWithDialog(const std::wstring& path)
{
    OPENASINFO info{};
    info.pcszFile = path.c_str();
    info.oaifInFlags = OAIF_EXEC | OAIF_HIDE_REGISTRATION;
    return SUCCEEDED(SHOpenWithDialog(nullptr, &info));
}

bool ShowWindowsShare(HWND window, const std::wstring& path, std::wstring& error)
{
    using namespace winrt::Windows::ApplicationModel::DataTransfer;
    using namespace winrt::Windows::Storage;
    try
    {
        auto interop = winrt::get_activation_factory<DataTransferManager, IDataTransferManagerInterop>();
        DataTransferManager manager{ nullptr };
        winrt::check_hresult(interop->GetForWindow(window, winrt::guid_of<DataTransferManager>(), winrt::put_abi(manager)));

        // DataRequested fires when the flyout opens; StorageFile lookup is
        // async, so the handler takes a deferral and completes it once the
        // file (and its share payload) is ready.
        manager.DataRequested([path](DataTransferManager const&, DataRequestedEventArgs const& args) -> winrt::fire_and_forget
        {
            auto request = args.Request();
            auto deferral = request.GetDeferral();
            try
            {
                StorageFile file = co_await StorageFile::GetFileFromPathAsync(path);
                request.Data().Properties().Title(file.Name());
                auto items = winrt::single_threaded_vector<IStorageItem>();
                items.Append(file);
                request.Data().SetStorageItems(items.GetView());
            }
            catch (...)
            {
                request.FailWithDisplayText(L"This file could not be shared.");
            }
            deferral.Complete();
        });

        winrt::check_hresult(interop->ShowShareUIForWindow(window));
        return true;
    }
    catch (const winrt::hresult_error& ex)
    {
        error = ex.message().c_str();
        return false;
    }
}

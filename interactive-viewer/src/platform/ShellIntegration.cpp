#include "framework.h"
#include "ShellIntegration.h"
#include "OpenWithCache.h"

#include <shlobj.h>
#include <shobjidl.h>
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
constexpr std::uint32_t kCatalogRevision = 7;
constexpr std::uint64_t kDiscoveryInterval = 7ull * 24 * 60 * 60 * 10'000'000;
constexpr std::size_t kMaximumCacheBytes = 256 * 1024;
constexpr wchar_t kCacheFileName[] = L"open-with-apps-v1.dat";
constexpr std::array<const wchar_t*, 13> kSupportedExtensions{
    L".glb", L".gltf", L".stl", L".ply", L".obj", L".fbx", L".3mf",
    L".usd", L".usda", L".usdc", L".usdz", L".step", L".stp"
};

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
    for (const auto* supported : kSupportedExtensions)
        if (_wcsicmp(supported, extension.c_str()) == 0) return true;
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
    const std::wstring tempPath = finalPath + L".tmp";
    HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || written != bytes.size() ||
        !MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(tempPath.c_str());
}

ComPtr<IDataObject> CreateFileDataObject(const std::wstring& path)
{
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return nullptr;
    ComPtr<IDataObject> dataObject;
    if (FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dataObject)))) return nullptr;
    return dataObject;
}

bool InvokeAssocHandler(const ComPtr<IAssocHandler>& handler, const std::wstring& path)
{
    ComPtr<IDataObject> dataObject = CreateFileDataObject(path);
    return dataObject && SUCCEEDED(handler->Invoke(dataObject.Get()));
}

bool ShowOpenWithDialog(const std::wstring& path)
{
    OPENASINFO info{};
    info.pcszFile = path.c_str();
    info.oaifInFlags = OAIF_EXEC | OAIF_HIDE_REGISTRATION;
    return SUCCEEDED(SHOpenWithDialog(nullptr, &info));
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
        const HRESULT nameResult = handler->GetName(&rawName);
        const HRESULT displayResult = handler->GetUIName(&rawDisplayName);
        if (SUCCEEDED(nameResult) && rawName && *rawName && SUCCEEDED(displayResult) && rawDisplayName && *rawDisplayName)
        {
            open_with::CachedHandler record{ extension, rawName, rawDisplayName, {} };
            const CatalogApp* catalog = MatchCatalog(record.handlerName, record.displayName);
            if (catalog) record.catalogId = catalog->id;
            if (!popularOnly || catalog)
            {
                const std::wstring key = HandlerKey(record.extension, record.handlerName);
                auto found = discovered.find(key);
                if (found == discovered.end() || (!record.catalogId.empty() && found->second.catalogId.empty()))
                    discovered[key] = std::move(record);
            }
        }
        if (rawName) CoTaskMemFree(rawName);
        if (rawDisplayName) CoTaskMemFree(rawDisplayName);
        handler.Reset();
    }
}

std::vector<open_with::CachedHandler> DiscoverHandlers()
{
    std::unordered_map<std::wstring, open_with::CachedHandler> discovered;
    for (const auto* extension : kSupportedExtensions)
    {
        AddDiscoveredHandlers(extension, ASSOC_FILTER_RECOMMENDED, false, discovered);
        AddDiscoveredHandlers(extension, ASSOC_FILTER_NONE, true, discovered);
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
    UINT launchFailureMessage = 0;
    std::jthread worker;
    std::unordered_set<std::wstring> failedHandlers;
    std::deque<Invocation> invocations;
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
        else if (found != known.end() && cache.handlers[found->second].catalogId.empty() && !handler.catalogId.empty())
        {
            cache.handlers[found->second].catalogId = std::move(handler.catalogId);
        }
    }
    cache.catalogRevision = kCatalogRevision;
    cache.lastDiscoveryFileTime = CurrentFileTime();
}

bool InvokeNamedHandler(std::unordered_map<std::wstring, ComPtr<IAssocHandler>>& liveHandlers,
    const std::wstring& extension, const std::wstring& handlerName, const std::wstring& path);

void DiscoveryWorker()
{
    // Keep handler enumeration and invocation on one dedicated STA. Some Shell
    // handlers are apartment-bound; none of their work belongs on the UI STA.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    open_with::CacheState loaded;
    LoadCache(loaded);
    bool sanitized = false;
    std::erase_if(loaded.handlers, [&](auto& handler) {
        if (!SupportedExtension(handler.extension)) { sanitized = true; return true; }
        if (!handler.catalogId.empty() && !CatalogById(handler.catalogId))
        {
            handler.catalogId.clear();
            sanitized = true;
        }
        return false;
    });
    if (sanitized) loaded.lastDiscoveryFileTime = 0;
    auto& runtime = Runtime();
    {
        std::lock_guard lock(runtime.mutex);
        runtime.cache = std::move(loaded);
        runtime.loaded = true;
        runtime.rescanRequested = DiscoveryDue(runtime.cache);
        runtime.condition.notify_all();
    }

    std::unordered_map<std::wstring, ComPtr<IAssocHandler>> liveHandlers;
    std::unique_lock lock(runtime.mutex);
    while (!runtime.stopping)
    {
        runtime.condition.wait(lock, [&] {
            return runtime.stopping || !runtime.invocations.empty() || runtime.rescanRequested;
        });
        if (runtime.stopping) break;
        if (!runtime.invocations.empty())
        {
            auto invocation = std::move(runtime.invocations.front());
            runtime.invocations.pop_front();
            lock.unlock();
            const bool invoked = InvokeNamedHandler(liveHandlers, invocation.extension,
                invocation.handlerName, invocation.path);
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
                const UINT failureMessage = runtime.launchFailureMessage;
                lock.unlock();
                SaveCache(invalidated);
                if (notificationWindow && failureMessage)
                    PostMessageW(notificationWindow, failureMessage, 0, 0);
                lock.lock();
            }
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
        lock.unlock();
        SaveCache(afterScan);
        lock.lock();
    }
    runtime.scanning = false;
    lock.unlock();
    liveHandlers.clear();
    if (SUCCEEDED(com)) CoUninitialize();
}

bool InvokeNamedHandler(std::unordered_map<std::wstring, ComPtr<IAssocHandler>>& liveHandlers,
    const std::wstring& extension, const std::wstring& handlerName, const std::wstring& path)
{
    const std::wstring key = HandlerKey(extension, handlerName);
    ComPtr<IAssocHandler> selected;
    const auto found = liveHandlers.find(key);
    if (found != liveHandlers.end()) selected = found->second;
    if (!selected)
    {
        ComPtr<IEnumAssocHandlers> enumeration;
        if (SUCCEEDED(SHAssocEnumHandlers(extension.c_str(), ASSOC_FILTER_NONE, &enumeration)) && enumeration)
        {
            ComPtr<IAssocHandler> candidate;
            ULONG fetched = 0;
            while (enumeration->Next(1, &candidate, &fetched) == S_OK && fetched == 1)
            {
                PWSTR rawName = nullptr;
                if (SUCCEEDED(candidate->GetName(&rawName)) && rawName)
                {
                    const bool matches = _wcsicmp(rawName, handlerName.c_str()) == 0;
                    CoTaskMemFree(rawName);
                    if (matches) { selected = candidate; break; }
                }
                candidate.Reset();
            }
        }
        if (selected)
            liveHandlers[key] = selected;
    }
    if (selected && InvokeAssocHandler(selected, path)) return true;
    liveHandlers.erase(key);
    return false;
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

void InitializeOpenWithCatalog(HWND notificationWindow, UINT launchFailureMessage)
{
    auto& runtime = Runtime();
    std::lock_guard lock(runtime.mutex);
    if (runtime.initialized) return;
    runtime.initialized = true;
    runtime.stopping = false;
    runtime.notificationWindow = notificationWindow;
    runtime.launchFailureMessage = launchFailureMessage;
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
    runtime.notificationWindow = nullptr;
    runtime.launchFailureMessage = 0;
    runtime.initialized = false;
}

std::vector<OpenWithEntry> EnumerateOpenWithHandlers(const std::wstring& path)
{
    const std::wstring extension = ExtensionOf(path);
    std::vector<open_with::CachedHandler> records;
    bool waiting = false;
    {
        auto& runtime = Runtime();
        std::lock_guard lock(runtime.mutex);
        waiting = !runtime.loaded || runtime.scanning;
        for (const auto& record : runtime.cache.handlers)
            if (_wcsicmp(record.extension.c_str(), extension.c_str()) == 0)
                records.push_back(record);
    }

    auto catalogOrder = [](const std::wstring& id) {
        const auto& catalog = CuratedCatalog();
        for (std::size_t index = 0; index < catalog.size(); ++index)
            if (_wcsicmp(catalog[index].id, id.c_str()) == 0) return index;
        return catalog.size();
    };
    std::ranges::stable_sort(records, [&](const auto& left, const auto& right) {
        const bool leftPopular = !left.catalogId.empty();
        const bool rightPopular = !right.catalogId.empty();
        if (leftPopular != rightPopular) return leftPopular;
        if (leftPopular) return catalogOrder(left.catalogId) < catalogOrder(right.catalogId);
        return _wcsicmp(left.displayName.c_str(), right.displayName.c_str()) < 0;
    });

    std::vector<OpenWithEntry> entries;
    std::unordered_set<std::wstring> names;
    for (const auto& record : records)
    {
        if (!names.insert(Fold(record.displayName)).second) continue;
        OpenWithEntry entry;
        if (const auto* catalog = CatalogById(record.catalogId))
        {
            entry.displayName = catalog->displayName;
            entry.group = catalog->group;
        }
        else
        {
            entry.displayName = SafeMenuLabel(record.displayName);
            entry.group = OpenWithGroup::Recommended;
        }
        const auto handlerName = record.handlerName;
        entry.invoke = [extension, handlerName](const std::wstring& filePath) {
            return QueueHandlerInvocation(extension, handlerName, filePath);
        };
        entries.push_back(std::move(entry));
    }
    if (entries.empty() && waiting)
    {
        OpenWithEntry status;
        status.displayName = L"Looking for installed apps…";
        status.group = OpenWithGroup::Status;
        status.enabled = false;
        entries.push_back(std::move(status));
    }
    OpenWithEntry chooseAnother;
    chooseAnother.displayName = L"Choose another app…";
    chooseAnother.group = OpenWithGroup::Fallback;
    chooseAnother.invoke = [](const std::wstring& filePath) { return ShowOpenWithDialog(filePath); };
    entries.push_back(std::move(chooseAnother));
    return entries;
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

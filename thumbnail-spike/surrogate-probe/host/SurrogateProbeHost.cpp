// SPIKE-8b (T03) surrogate probe host.
//
// Two activation paths are exercised:
//   com   - CoCreateInstance(CLSCTX_LOCAL_SERVER) against the probe CLSID, which
//           the AppID+DllSurrogate registration routes into a DllHost.exe COM
//           surrogate.
//   shell - IThumbnailCache::GetThumbnail on a scratch file, which exercises the
//           real Shell handler-resolution path (extension ShellEx mapping) and
//           the Shell's default out-of-process thumbnail hosting.
//
// The probe DLL logs the PID/process image, requested cx, render time and commit
// for every call. This host prints those records and reports whether the call
// ran in dllhost.exe rather than in this process.

#include <windows.h>

#include <initguid.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

const CLSID kProbeClsid = {0xC7A5B3E1, 0x9D24, 0x4F88, {0xA1, 0xB6, 0x2E, 0x5C, 0x7D, 0x9F, 0x0A, 0x31}};
constexpr wchar_t kConfigKey[] = L"Software\\Preview3DThumbnailSpike";
constexpr wchar_t kThumbnailHandlerKey[] =
    L"Software\\Classes\\CLSID\\{C7A5B3E1-9D24-4F88-A1B6-2E5C7D9F0A31}";

void SetConfigString(const wchar_t* name, const std::wstring& value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kConfigKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

bool ReadIsolationOptOut()
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kThumbnailHandlerKey, L"DisableProcessIsolation", RRF_RT_REG_DWORD, nullptr,
                        &value, &size) == ERROR_SUCCESS;
}

std::wstring ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::wstring ReadAllText(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    bytes.resize(read);
    return ToWide(bytes);
}

double NowMs()
{
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return (static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart)) * 1000.0;
}

std::wstring MakeToken()
{
    GUID guid{};
    CoCreateGuid(&guid);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}", guid.Data1, guid.Data2, guid.Data3,
               guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6],
               guid.Data4[7]);
    return buffer;
}

std::vector<std::wstring> Split(const std::wstring& csv)
{
    std::vector<std::wstring> parts;
    std::size_t start = 0;
    while (start <= csv.size()) {
        const std::size_t comma = csv.find(L',', start);
        const std::wstring part = csv.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        if (!part.empty()) parts.push_back(part);
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    return parts;
}

int Fail(const char* message)
{
    std::printf("error: %s\n", message);
    return 2;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::wstring scenario = L"com";
    std::wstring mode = L"control";
    std::wstring cxList = L"32,48,64,256";
    std::wstring file;
    std::wstring gltfPath;
    std::wstring logPath = L"";
    int runs = 3;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring { return (i + 1 < argc) ? argv[++i] : std::wstring(); };
        if (arg == L"--scenario") scenario = next();
        else if (arg == L"--mode") mode = next();
        else if (arg == L"--cx") cxList = next();
        else if (arg == L"--file") file = next();
        else if (arg == L"--gltf") gltfPath = next();
        else if (arg == L"--log") logPath = next();
        else if (arg == L"--runs") runs = _wtoi(next().c_str());
    }

    if (logPath.empty()) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        logPath = std::wstring(temp) + L"preview3d-surrogate-probe.log";
    }

    const std::wstring token = MakeToken();
    SetConfigString(L"Mode", mode);
    SetConfigString(L"LogPath", logPath);
    SetConfigString(L"RunToken", token);
    if (!gltfPath.empty()) SetConfigString(L"GltfPath", gltfPath);

    const bool isolationOptOut = ReadIsolationOptOut();

    const int hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrInit)) return Fail("CoInitializeEx failed");

    std::printf("surrogate-probe host\n");
    std::printf("  scenario=%ls mode=%ls token=%ls pid=%lu log=%ls\n", scenario.c_str(), mode.c_str(), token.c_str(),
                GetCurrentProcessId(), logPath.c_str());
    std::printf("  DisableProcessIsolation value present under probe CLSID: %s\n", isolationOptOut ? "YES" : "no");

    const std::vector<std::wstring> cxValues = Split(cxList);
    int successes = 0;
    int failures = 0;

    for (int run = 0; run < runs; ++run) {
        for (const std::wstring& cxText : cxValues) {
            const unsigned cx = static_cast<unsigned>(_wtoi(cxText.c_str()));
            const double start = NowMs();
            HRESULT hr = E_FAIL;
            std::wstring detail;

            if (scenario == L"shell") {
                IShellItem* item = nullptr;
                if (FAILED(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                    detail = L"SHCreateItemFromParsingName failed";
                } else {
                    IThumbnailCache* cache = nullptr;
                    hr = CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cache));
                    if (SUCCEEDED(hr)) {
                        ISharedBitmap* bitmap = nullptr;
                        WTS_CACHEFLAGS flags = WTS_DEFAULT;
                        WTS_THUMBNAILID id{};
                        hr = cache->GetThumbnail(item, cx, static_cast<WTS_FLAGS>(WTS_EXTRACT | WTS_FORCEEXTRACTION),
                                                 &bitmap, &flags, &id);
                        if (SUCCEEDED(hr) && bitmap != nullptr) {
                            SIZE size{};
                            bitmap->GetSize(&size);
                            detail = L"bitmap=" + std::to_wstring(size.cx) + L"x" + std::to_wstring(size.cy);
                            bitmap->Release();
                        }
                        cache->Release();
                    }
                    item->Release();
                }
            } else {
                IThumbnailProvider* provider = nullptr;
                hr = CoCreateInstance(kProbeClsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&provider));
                if (SUCCEEDED(hr)) {
                    IInitializeWithStream* init = nullptr;
                    if (SUCCEEDED(provider->QueryInterface(IID_PPV_ARGS(&init)))) {
                        static const char kPayload[] = "preview3d-surrogate-probe";
                        IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(kPayload), sizeof(kPayload));
                        if (stream != nullptr) {
                            hr = init->Initialize(stream, STGM_READ);
                            if (SUCCEEDED(hr)) {
                                HBITMAP bitmap = nullptr;
                                WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
                                hr = provider->GetThumbnail(cx, &bitmap, &alpha);
                                if (SUCCEEDED(hr) && bitmap != nullptr) {
                                    BITMAP info{};
                                    GetObjectW(bitmap, sizeof(info), &info);
                                    detail = L"bitmap=" + std::to_wstring(info.bmWidth) + L"x" +
                                             std::to_wstring(info.bmHeight) + L" alpha=" +
                                             std::to_wstring(static_cast<int>(alpha));
                                    DeleteObject(bitmap);
                                }
                            }
                            stream->Release();
                        }
                        init->Release();
                    }
                    provider->Release();
                }
            }

            const double elapsed = NowMs() - start;
            const bool ok = SUCCEEDED(hr);
            ok ? ++successes : ++failures;
            std::wstring line = L"  run=" + std::to_wstring(run) + L" cx=" + std::to_wstring(cx) + L" host_elapsed_ms=" +
                                std::to_wstring(elapsed) + L" hr=0x" + std::to_wstring(static_cast<unsigned>(hr));
            if (!detail.empty()) line += L" " + detail;
            std::printf("%s\n", ToUtf8(line).c_str());
        }
    }

    CoUninitialize();

    // Read back the probe's own records for this token and report the hosting process.
    const std::wstring log = ReadAllText(logPath);
    bool sawDllHost = false;
    bool sawHostProcess = false;
    std::wstring hostPid = std::to_wstring(GetCurrentProcessId());
    std::size_t offset = 0;
    std::printf("\nprobe records (token %ls):\n", token.c_str());
    while (offset < log.size()) {
        const std::size_t end = log.find(L'\n', offset);
        const std::wstring line = log.substr(offset, end == std::wstring::npos ? std::wstring::npos : end - offset);
        offset = (end == std::wstring::npos) ? log.size() : end + 1;
        if (line.find(token) == std::wstring::npos) continue;
        std::printf("  %s\n", ToUtf8(line).c_str());
        if (line.find(L"\\dllhost.exe") != std::wstring::npos || line.find(L"dllhost.exe") != std::wstring::npos) {
            sawDllHost = true;
        }
        if (line.find(L"SurrogateProbeHost.exe") != std::wstring::npos) sawHostProcess = true;
    }

    const char* isolation = sawDllHost && !sawHostProcess ? "dllhost"
                            : sawHostProcess                     ? "inproc-host"
                                                                 : "unobserved";
    std::printf("\nsummary: successes=%d failures=%d isolation=%s\n", successes, failures, isolation);
    return failures == 0 ? 0 : 1;
}
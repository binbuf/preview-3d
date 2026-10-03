// SEC-17 Explorer-surrogate soak implementation (see ProviderSoak.h).

#include "ProviderSoak.h"

#include <windows.h>

#include <objbase.h>
#include <objidl.h>
#include <psapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace provider_smoke {
namespace {

constexpr wchar_t kProviderModuleName[] = L"Preview3DThumbnailProvider.dll";
constexpr DWORD kGdiTolerance = 64;
constexpr DWORD kUserTolerance = 64;
constexpr DWORD kHandleTolerance = 256;
constexpr DWORD kThreadTolerance = 8;
constexpr std::uint64_t kPrivateToleranceBytes = 32ull * 1024 * 1024; // 32 MiB
constexpr unsigned kTeardownTimeoutMs = 30000;

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

struct ProcessMetrics {
    DWORD gdi = 0;
    DWORD user = 0;
    DWORD handles = 0;
    DWORD threads = 0;
    std::uint64_t privateBytes = 0;
    std::uint64_t workingSet = 0;
};

std::vector<DWORD> FindSurrogatePids()
{
    std::vector<DWORD> pids;
    const HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    if (Process32FirstW(processes, &process)) {
        do {
            if (_wcsicmp(process.szExeFile, L"dllhost.exe") != 0) continue;
            const HANDLE modules =
                CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process.th32ProcessID);
            if (modules == INVALID_HANDLE_VALUE) continue;
            MODULEENTRY32W module{};
            module.dwSize = sizeof(module);
            if (Module32FirstW(modules, &module)) {
                do {
                    if (_wcsicmp(module.szModule, kProviderModuleName) == 0) {
                        pids.push_back(process.th32ProcessID);
                        break;
                    }
                } while (Module32NextW(modules, &module));
            }
            CloseHandle(modules);
        } while (Process32NextW(processes, &process));
    }
    CloseHandle(processes);
    return pids;
}

DWORD CountThreads(DWORD pid)
{
    DWORD count = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 thread{};
    thread.dwSize = sizeof(thread);
    if (Thread32First(snapshot, &thread)) {
        do {
            if (thread.th32OwnerProcessID == pid) ++count;
        } while (Thread32Next(snapshot, &thread));
    }
    CloseHandle(snapshot);
    return count;
}

bool SamplePid(DWORD pid, ProcessMetrics& out)
{
    const HANDLE process =
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) return false;
    out.gdi = GetGuiResources(process, GR_GDIOBJECTS);
    out.user = GetGuiResources(process, GR_USEROBJECTS);
    DWORD handles = 0;
    if (GetProcessHandleCount(process, &handles)) out.handles = handles;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        out.privateBytes = counters.PrivateUsage;
        out.workingSet = counters.WorkingSetSize;
    }
    out.threads = CountThreads(pid);
    CloseHandle(process);
    return true;
}

ProcessMetrics SampleSurrogates(std::vector<DWORD>* pidsOut)
{
    ProcessMetrics total;
    const std::vector<DWORD> pids = FindSurrogatePids();
    if (pidsOut != nullptr) *pidsOut = pids;
    for (const DWORD pid : pids) {
        ProcessMetrics one;
        if (!SamplePid(pid, one)) continue;
        total.gdi += one.gdi;
        total.user += one.user;
        total.handles += one.handles;
        total.threads += one.threads;
        total.privateBytes += one.privateBytes;
        total.workingSet += one.workingSet;
    }
    return total;
}

struct WorkerStats {
    std::uint64_t attempts = 0;
    std::uint64_t ok = 0;
    std::uint64_t failed = 0;
    long firstFailure = 0;
};

void SoakWorker(const SoakOptions* options, unsigned index, WorkerStats* out)
{
    // Each worker owns one STA apartment; IThumbnailCache and the provider proxy
    // are created and released per iteration so the surrogate sees repeated
    // activation/extraction/teardown.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        out->failed = options->iterations;
        return;
    }
    const std::vector<SoakModel>& models = options->models;
    for (unsigned i = 0; i < options->iterations; ++i) {
        const SoakModel& model = models[(index + i) % models.size()];
        ++out->attempts;
        HRESULT hr = E_FAIL;
        IShellItem* item = nullptr;
        hr = SHCreateItemFromParsingName(model.path.c_str(), nullptr, IID_PPV_ARGS(&item));
        IThumbnailCache* cache = nullptr;
        if (SUCCEEDED(hr)) {
            hr = CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&cache));
        }
        if (SUCCEEDED(hr)) {
            ISharedBitmap* shared = nullptr;
            WTS_THUMBNAILID id{};
            WTS_CACHEFLAGS flags = WTS_DEFAULT;
            hr = cache->GetThumbnail(item, options->cx,
                                     static_cast<WTS_FLAGS>(WTS_EXTRACT | WTS_FORCEEXTRACTION),
                                     &shared, &flags, &id);
            if (SUCCEEDED(hr) && shared != nullptr) {
                HBITMAP bitmap = nullptr;
                if (SUCCEEDED(shared->GetSharedBitmap(&bitmap)) && bitmap != nullptr) {
                    DeleteObject(bitmap);
                } else {
                    hr = E_FAIL;
                }
                shared->Release();
            } else if (SUCCEEDED(hr)) {
                hr = E_POINTER;
            }
        }
        if (cache != nullptr) cache->Release();
        if (item != nullptr) item->Release();
        if (SUCCEEDED(hr)) {
            ++out->ok;
        } else {
            ++out->failed;
            if (out->firstFailure == 0) out->firstFailure = static_cast<long>(hr);
        }
    }
    CoUninitialize();
}

WorkerStats RunPhase(const SoakOptions& options, unsigned iterations)
{
    std::vector<std::thread> threads;
    std::vector<WorkerStats> stats(options.apartments);
    SoakOptions phase = options;
    phase.iterations = iterations;
    for (unsigned i = 0; i < options.apartments; ++i) {
        threads.emplace_back(SoakWorker, &phase, i, &stats[i]);
    }
    for (std::thread& thread : threads) thread.join();
    WorkerStats total;
    for (const WorkerStats& one : stats) {
        total.attempts += one.attempts;
        total.ok += one.ok;
        total.failed += one.failed;
        if (total.firstFailure == 0) total.firstFailure = one.firstFailure;
    }
    return total;
}

bool Exceeds(const ProcessMetrics& baseline, const ProcessMetrics& finalMetrics)
{
    return finalMetrics.gdi > baseline.gdi + kGdiTolerance
        || finalMetrics.user > baseline.user + kUserTolerance
        || finalMetrics.handles > baseline.handles + kHandleTolerance
        || finalMetrics.threads > baseline.threads + kThreadTolerance
        || finalMetrics.privateBytes > baseline.privateBytes + kPrivateToleranceBytes;
}

std::wstring JoinPids(const std::vector<DWORD>& pids)
{
    std::wstring text;
    for (const DWORD pid : pids) {
        if (!text.empty()) text += L",";
        text += std::to_wstring(pid);
    }
    return text.empty() ? L"none" : text;
}

} // namespace

int RunProviderSoak(const SoakOptions& options)
{
    if (options.dllPath.empty() || options.models.empty() || options.apartments == 0
        || options.iterations == 0) {
        return 2;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 2;
    }

    CreateDirectoryW(options.outDir.c_str(), nullptr);
    FILE* report = nullptr;
    const std::wstring reportPath = options.outDir + L"\\soak-report.txt";
    if (_wfopen_s(&report, reportPath.c_str(), L"wb") != 0) report = nullptr;
    auto say = [&](const std::string& text) {
        std::printf("%s\n", text.c_str());
        if (report != nullptr) std::fprintf(report, "%s\n", text.c_str());
    };

    std::vector<DWORD> beforePids;
    const ProcessMetrics before = SampleSurrogates(&beforePids);
    say("provider surrogate soak");
    say("  dll=" + ToUtf8(options.dllPath));
    say("  models=" + std::to_string(options.models.size())
        + " apartments=" + std::to_string(options.apartments)
        + " iterations=" + std::to_string(options.iterations)
        + " cx=" + std::to_string(options.cx));
    say("  surrogates before=" + ToUtf8(JoinPids(beforePids)));

    // Warm-up so the surrogate is loaded and the first extraction is not part of
    // the growth baseline.
    const WorkerStats warm = RunPhase(options, 1);
    std::vector<DWORD> warmPids;
    const ProcessMetrics baseline = SampleSurrogates(&warmPids);
    say("  warm-up attempts=" + std::to_string(warm.attempts)
        + " ok=" + std::to_string(warm.ok)
        + " failed=" + std::to_string(warm.failed));
    say("  baseline gdi=" + std::to_string(baseline.gdi)
        + " user=" + std::to_string(baseline.user)
        + " handles=" + std::to_string(baseline.handles)
        + " threads=" + std::to_string(baseline.threads)
        + " privateBytes=" + std::to_string(baseline.privateBytes));

    const WorkerStats main = RunPhase(options, options.iterations);
    std::vector<DWORD> finalPids;
    const ProcessMetrics finalMetrics = SampleSurrogates(&finalPids);
    say("  main attempts=" + std::to_string(main.attempts)
        + " ok=" + std::to_string(main.ok)
        + " failed=" + std::to_string(main.failed));
    say("  final gdi=" + std::to_string(finalMetrics.gdi)
        + " user=" + std::to_string(finalMetrics.user)
        + " handles=" + std::to_string(finalMetrics.handles)
        + " threads=" + std::to_string(finalMetrics.threads)
        + " privateBytes=" + std::to_string(finalMetrics.privateBytes));
    say("  surrogates after=" + ToUtf8(JoinPids(finalPids)));

    const bool growth = Exceeds(baseline, finalMetrics);
    say(std::string("  growth_within_tolerance=") + (growth ? "false" : "true"));

    // Release path: after the last cache object is released the Shell surrogate
    // must drop the module and tear down. Poll for it.
    std::vector<DWORD> afterPids = finalPids;
    const DWORD start = GetTickCount();
    while (!afterPids.empty() && GetTickCount() - start < kTeardownTimeoutMs) {
        Sleep(500);
        afterPids = FindSurrogatePids();
    }
    std::vector<DWORD> expected = beforePids;
    bool persistentNewSurrogate = false;
    for (const DWORD pid : afterPids) {
        if (std::find(expected.begin(), expected.end(), pid) == expected.end()) {
            persistentNewSurrogate = true;
            break;
        }
    }
    say("  surrogates after_teardown=" + ToUtf8(JoinPids(afterPids))
        + " waited_ms=" + std::to_string(static_cast<unsigned long>(GetTickCount() - start)));
    say(std::string("  persistent_new_surrogate=")
        + (persistentNewSurrogate ? "true" : "false"));

    const bool crashOrHang = (main.failed != 0) || (warm.failed != 0);
    if (crashOrHang) {
        say("  first_failure_hr=0x" + [&] {
            char buffer[16]{};
            std::snprintf(buffer, sizeof(buffer), "%08lX",
                          static_cast<unsigned long>(main.firstFailure != 0 ? main.firstFailure
                                                                            : warm.firstFailure));
            return std::string(buffer);
        }());
    }

    if (report != nullptr) std::fclose(report);
    CoUninitialize();

    if (crashOrHang) {
        std::printf("FAIL: the soak saw %llu failed thumbnails (crash/hang)\n",
                    static_cast<unsigned long long>(main.failed + warm.failed));
        return 1;
    }
    if (growth) {
        std::printf("FAIL: surrogate resource growth exceeded tolerance\n");
        return 1;
    }
    if (persistentNewSurrogate) {
        std::printf("FAIL: a surrogate still hosts the provider after teardown\n");
        return 1;
    }
    std::printf("PASS: surrogate soak stable over %u apartments x %u iterations\n",
                options.apartments, options.iterations);
    return 0;
}

} // namespace provider_smoke
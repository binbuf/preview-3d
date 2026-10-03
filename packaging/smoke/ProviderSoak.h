#pragma once

// SEC-17 Explorer-surrogate soak (see packaging/smoke/README.md).
//
// Runs many concurrent STA apartments through the real Shell thumbnail path
// (IThumbnailCache::GetThumbnail with WTS_FORCEEXTRACTION) so the provider is
// loaded, exercised and torn down inside the real DllHost surrogate, repeatedly,
// with thumbnail-cache churn. It asserts no crash/hang (every valid call must
// return a bitmap), no persistent surrogate after the last release, and no
// monotonic GDI/User/private-byte/handle/thread growth in the surrogate.
//
// T25 adds the SEC-08 hostile-input classification lane. A contained access
// violation is recorded as a quarantine failure-with-reason (a valid request
// refused after the boundary contained a fault), and a surrogate death from a
// re-raised stack overflow or an uncatchable `__fastfail`/stack-cookie fault is
// recorded as the documented allowed failure rather than masked as a pass
// (ADR-0037/ADR-0047). An unexpected surrogate death still fails the soak.

#include <windows.h>

#include <string>
#include <vector>

namespace provider_smoke {

struct SoakModel {
    std::string name;   // "stl", "glb", ...; only used for reporting
    CLSID clsid;        // unused by the soak (the Shell routes by extension); kept for callers
    std::wstring path;
};

struct SoakOptions {
    std::wstring dllPath;
    std::vector<SoakModel> models;   // valid fixtures: every rendered thumbnail must succeed
    std::vector<SoakModel> hostile;  // hostile inputs: a refused thumbnail is expected (fail closed)
    unsigned apartments = 4;
    unsigned iterations = 100; // per apartment
    unsigned cx = 256;
    std::wstring outDir;
};

// Returns 0 on success (including a run whose only fault was a recorded SEC-08
// allowed surrogate death), 1 on a soak failure, 2 on a setup error.
int RunProviderSoak(const SoakOptions& options);

// SEC-08 classification of a surrogate's process exit code. The "allowed" codes
// are the re-raised stack overflow and the uncatchable `__fastfail`/stack-cookie
// faults ADR-0037 documents; any other nonzero exit is an unexpected fault.
bool IsAllowedSurrogateExitCode(unsigned long exitCode) noexcept;

} // namespace provider_smoke
#pragma once

// SEC-17 Explorer-surrogate soak (see packaging/smoke/README.md).
//
// Runs many concurrent STA apartments through the real Shell thumbnail path
// (IThumbnailCache::GetThumbnail with WTS_FORCEEXTRACTION) so the provider is
// loaded, exercised and torn down inside the real DllHost surrogate, repeatedly,
// with thumbnail-cache churn. It asserts no crash/hang (every call must return a
// bitmap), no persistent surrogate after the last release, and no monotonic
// GDI/User/private-byte/handle/thread growth in the surrogate.

#include <windows.h>

#include <string>
#include <vector>

namespace provider_smoke {

struct SoakModel {
    std::string name;   // "stl", "glb", ...
    CLSID clsid;
    std::wstring path;
};

struct SoakOptions {
    std::wstring dllPath;
    std::vector<SoakModel> models;
    unsigned apartments = 4;
    unsigned iterations = 100; // per apartment
    unsigned cx = 256;
    std::wstring outDir;
};

// Returns 0 on success, 1 on a soak failure, 2 on a setup error.
int RunProviderSoak(const SoakOptions& options);

} // namespace provider_smoke
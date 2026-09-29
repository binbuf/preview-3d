#pragma once

// Frozen CLSID-to-family routing table (T04). This is the single source for
// the eight product identities and their direct extensions:
//
//   * the DLL decides the family from the object's CLSID and nothing else --
//     no content sniffing, no path recovery (T11/T13);
//   * registration (T41) consumes the same table to write the machine-level
//     CLSID/InprocServer32/AppID and extension ShellEx entries (design 08).
//
// The values below are product identities: never regenerate or rename one.
// CLSID strings are compared exactly, including braces and upper-case hex.

#include "ProviderTypes.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace preview3d::provider {

// Windows thumbnail-handler ShellEx category. T41 writes, for each direct
// extension X and its family CLSID:
//
//   HKLM\Software\Classes\X\shellex\{E357FCCD-...} = "<family CLSID>"
//
// validated in the surrogate by T03/ADR-0008.
inline constexpr char kThumbnailHandlerShellExGuid[] =
    "{E357FCCD-A995-4576-B01F-234630154E96}";

struct FamilyRoute {
    Family family;
    const char* clsid;
    std::span<const char* const> extensions;
};

inline constexpr const char* kGltfExtensions[] = {".glb", ".gltf"};
inline constexpr const char* kStlExtensions[] = {".stl"};
inline constexpr const char* kPlyExtensions[] = {".ply"};
inline constexpr const char* kObjExtensions[] = {".obj"};
inline constexpr const char* kFbxExtensions[] = {".fbx"};
inline constexpr const char* kThreeMfExtensions[] = {".3mf"};
inline constexpr const char* kUsdExtensions[] = {".usd", ".usda", ".usdc", ".usdz"};
inline constexpr const char* kStepExtensions[] = {".step", ".stp"};

inline constexpr FamilyRoute kFamilyRoutes[] = {
    {Family::Gltf, "{A592F425-EA68-4C88-BB96-020805D4BE56}", {kGltfExtensions, 2}},
    {Family::Stl, "{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}", {kStlExtensions, 1}},
    {Family::Ply, "{F4DC6119-E235-4BAC-8089-54EDD84F8492}", {kPlyExtensions, 1}},
    {Family::Obj, "{D4722752-C480-4D9C-BEBE-1A9B514A8846}", {kObjExtensions, 1}},
    {Family::Fbx, "{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}", {kFbxExtensions, 1}},
    {Family::ThreeMf, "{D8389A63-8526-454A-9892-72F3149484B9}", {kThreeMfExtensions, 1}},
    {Family::Usd, "{E938BC70-4C08-4446-A15D-EE31576BFB48}", {kUsdExtensions, 4}},
    {Family::Step, "{6EE961AC-AC3B-4958-A898-E30523FEE79D}", {kStepExtensions, 2}},
};

inline constexpr std::span<const FamilyRoute> FamilyRoutes() noexcept
{
    return {kFamilyRoutes, sizeof(kFamilyRoutes) / sizeof(kFamilyRoutes[0])};
}

// Returns the route for a CLSID, or nullptr when it is not a product identity.
// T11's class factory uses this; an unknown CLSID maps to
// CLASS_E_CLASSNOTAVAILABLE and is never sniffed.
inline constexpr const FamilyRoute* RouteForClsid(std::string_view clsid) noexcept
{
    for (const FamilyRoute& route : FamilyRoutes()) {
        if (clsid == route.clsid) return &route;
    }
    return nullptr;
}

inline constexpr Family FamilyForClsid(std::string_view clsid) noexcept
{
    const FamilyRoute* route = RouteForClsid(clsid);
    return route ? route->family : Family::Unknown;
}

} // namespace preview3d::provider
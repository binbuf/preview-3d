// T13 IThumbnailProvider COM boundary coverage.
//
// Loads the built provider DLL the way the Shell and the T17 host do (the two
// exports are PRIVATE, so there is no import library) and exercises the frozen
// GetThumbnail call contract from design/05:
//
//   - every family CLSID creates an object that answers IID_IThumbnailProvider,
//     routed from the CLSID alone;
//   - null out-params and an uninitialized object use the tabulated HRESULTs;
//   - the degenerate cx == 0 is rejected with E_INVALIDARG and a null bitmap;
//   - with no family adapter linked yet (T21-T34), a real request fails to the
//     safe generic-icon fallback (ERROR_NOT_SUPPORTED) and never fabricates a
//     bitmap; the call can be repeated because the deadline is restarted;
//   - every object/factory is released, so DllCanUnloadNow returns to S_OK.

#include <catch2/catch_test_macros.hpp>

#include "FamilyRouting.h"
#include "ProviderErrors.h"
#include "ProviderTestSupport.h"

#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <propsys.h>
#include <thumbcache.h>

#include <cstddef>
#include <vector>

namespace {

using preview3d::provider::Family;
using preview3d::test::GuidFromText;
using preview3d::test::MemoryStream;
using preview3d::test::ProviderModule;

IThumbnailProvider* CreateThumbnailProvider(ProviderModule& module, REFCLSID clsid)
{
    void* raw = nullptr;
    if (FAILED(module.getClassObject(clsid, __uuidof(IClassFactory), &raw))) {
        return nullptr;
    }
    auto* factory = static_cast<IClassFactory*>(raw);

    IThumbnailProvider* provider = nullptr;
    const HRESULT hr = factory->CreateInstance(
        nullptr, __uuidof(IThumbnailProvider), reinterpret_cast<void**>(&provider));
    factory->Release();
    return SUCCEEDED(hr) ? provider : nullptr;
}

IInitializeWithStream* AsInitializable(IThumbnailProvider* provider)
{
    IInitializeWithStream* init = nullptr;
    if (provider == nullptr) {
        return nullptr;
    }
    const HRESULT hr =
        provider->QueryInterface(__uuidof(IInitializeWithStream),
                                 reinterpret_cast<void**>(&init));
    return SUCCEEDED(hr) ? init : nullptr;
}

std::vector<std::byte> StreamBytes(std::size_t size)
{
    return std::vector<std::byte>(size, std::byte{0x2A});
}

} // namespace

TEST_CASE("every frozen family CLSID creates an IThumbnailProvider", "[provider][thumbnail]")
{
    ProviderModule module;
    REQUIRE(module.Ready());
    REQUIRE(module.canUnloadNow() == S_OK);

    for (const preview3d::provider::FamilyRoute& route : preview3d::provider::FamilyRoutes()) {
        const GUID clsid = GuidFromText(route.clsid);
        IThumbnailProvider* provider = CreateThumbnailProvider(module, clsid);
        INFO("family CLSID " << route.clsid);
        REQUIRE(provider != nullptr);

        IThumbnailProvider* again = nullptr;
        CHECK(provider->QueryInterface(__uuidof(IThumbnailProvider),
                                       reinterpret_cast<void**>(&again)) == S_OK);
        CHECK(again == provider);
        if (again != nullptr) {
            again->Release();
        }

        provider->Release();
    }

    CHECK(module.canUnloadNow() == S_OK);
}

TEST_CASE("GetThumbnail rejects null out-params and an uninitialized object",
          "[provider][thumbnail]")
{
    ProviderModule module;
    REQUIRE(module.Ready());

    IThumbnailProvider* provider =
        CreateThumbnailProvider(module, GuidFromText(
            preview3d::provider::FamilyRoutes().front().clsid));
    REQUIRE(provider != nullptr);

    HBITMAP bitmap = reinterpret_cast<HBITMAP>(1);
    WTS_ALPHATYPE alpha = WTSAT_ARGB;
    CHECK(provider->GetThumbnail(64, nullptr, &alpha) == E_POINTER);
    CHECK(provider->GetThumbnail(64, &bitmap, nullptr) == E_POINTER);

    // No stream adopted yet: the call order is invalid.
    bitmap = reinterpret_cast<HBITMAP>(1);
    alpha = WTSAT_ARGB;
    CHECK(provider->GetThumbnail(64, &bitmap, &alpha) == E_UNEXPECTED);
    CHECK(bitmap == nullptr);
    CHECK(alpha == WTSAT_UNKNOWN);

    provider->Release();
    CHECK(module.canUnloadNow() == S_OK);
}

TEST_CASE("GetThumbnail rejects cx == 0 and fails closed without an adapter",
          "[provider][thumbnail]")
{
    ProviderModule module;
    REQUIRE(module.Ready());

    // T32 links the 3MF adapter, so this case uses the USD CLSID, which still has
    // no linked adapter (T33), to prove the "no adapter" fallback.
    IThumbnailProvider* provider =
        CreateThumbnailProvider(module, preview3d::test::FamilyClsid(Family::Usd));
    REQUIRE(provider != nullptr);

    IInitializeWithStream* init = AsInitializable(provider);
    REQUIRE(init != nullptr);
    MemoryStream stream(StreamBytes(4096));
    REQUIRE(init->Initialize(&stream, STGM_READ) == S_OK);

    // The degenerate request is a bad argument and returns no bitmap.
    HBITMAP bitmap = reinterpret_cast<HBITMAP>(1);
    WTS_ALPHATYPE alpha = WTSAT_ARGB;
    CHECK(provider->GetThumbnail(0, &bitmap, &alpha) == E_INVALIDARG);
    CHECK(bitmap == nullptr);
    CHECK(alpha == WTSAT_UNKNOWN);

    // No adapter is linked for this family yet, so a real request falls back to
    // the generic icon with the tabulated code and no fabricated bitmap. A
    // request larger than today's cache sizes is still only a hint.
    for (const UINT cx : {32u, 256u, 4096u}) {
        bitmap = reinterpret_cast<HBITMAP>(1);
        alpha = WTSAT_ARGB;
        CHECK(provider->GetThumbnail(cx, &bitmap, &alpha) ==
              HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        CHECK(bitmap == nullptr);
        CHECK(alpha == WTSAT_UNKNOWN);
    }

    // A repeated call succeeds in reaching the same fallback (the source
    // deadline is restarted at every GetThumbnail entry).
    bitmap = nullptr;
    alpha = WTSAT_UNKNOWN;
    CHECK(provider->GetThumbnail(64, &bitmap, &alpha) ==
          HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    CHECK(bitmap == nullptr);

    init->Release();
    provider->Release();
    CHECK(module.canUnloadNow() == S_OK);
}
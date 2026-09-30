// T17 provider COM host harness coverage.
//
// Layers, per docs/design/testing-strategy.md:
//   [host][com]      - the real Shell activation sequence per frozen CLSID
//                      through DllGetClassObject/IClassFactory/
//                      IInitializeWithStream/IThumbnailProvider, then unload;
//   [host][golden]   - every registered fixture rendered through the real
//                      pipeline and compared with its committed PAM golden;
//   [host][parallel] - multiple STA apartments driving provider objects at once;
//   [host][leak]     - repeated load/use/unload with GDI/User/private-byte/thread
//                      sampling.
//
// The hidden [write-host-goldens] case regenerates the committed goldens, as
// Tests.Unit.exe "[write-goldens]" does for the rasterizer suite:
//   x64\Release\Tests.ProviderHost.exe "[write-host-goldens]"

#include <catch2/catch_test_macros.hpp>

#include "FamilyAdapterRegistry.h"
#include "FamilyRouting.h"
#include "ProviderHostSupport.h"
#include "RasterBitmap.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

using preview3d::provider::ProviderOutcome;
using preview3d::provider::RasterImage;
using preview3d::test::ActivateAndRender;
using preview3d::test::CompareBgraToRgba;
using preview3d::test::FamilyClsid;
using preview3d::test::GoldenDiff;
using preview3d::test::GoldenFixture;
using preview3d::test::GuidFromText;
using preview3d::test::LoadGoldenRgba;
using preview3d::test::MemoryStream;
using preview3d::test::ProviderHostFixtures;
using preview3d::test::ProviderHostGoldenDirectory;
using preview3d::test::ProviderModule;
using preview3d::test::RunFixture;
using preview3d::test::SampleProcessResources;
using preview3d::test::SaveGolden;
using preview3d::test::WithinTolerance;

namespace {

// RAII apartment for the tests that mirror the Shell's STA.
class ComApartment {
public:
    ComApartment() : okay_(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ~ComApartment()
    {
        if (okay_) {
            ::CoUninitialize();
        }
    }

private:
    bool okay_;
};

// One activate/use/unload cycle against a freshly loaded module, plus one
// in-process placeholder render and DIB round-trip so the loop touches the same
// GDI surface a real adapter will.
void LeakCycle()
{
    ProviderModule module;
    if (module.Ready()) {
        MemoryStream stream(std::vector<std::byte>(1024, std::byte{0}));
        HBITMAP bitmap = nullptr;
        WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
        ActivateAndRender(module, FamilyClsid(preview3d::provider::Family::Gltf), stream, 128,
                          bitmap, alpha);
        if (bitmap != nullptr) {
            ::DeleteObject(bitmap);
        }
    }

    if (!ProviderHostFixtures().empty()) {
        RasterImage image;
        if (RunFixture(ProviderHostFixtures().front(), image) == ProviderOutcome::Success) {
            HBITMAP dib = nullptr;
            if (preview3d::provider::CreatePremultipliedDib(image, dib) ==
                    ProviderOutcome::Success &&
                dib != nullptr) {
                ::DeleteObject(dib);
            }
        }
    }
}

} // namespace

TEST_CASE("the Shell activation sequence renders or falls back per CLSID, then unloads",
          "[host][com]")
{
    ProviderModule module;
    REQUIRE(module.Ready());
    REQUIRE(module.canUnloadNow() == S_OK);

    ComApartment apartment;

    for (const preview3d::provider::FamilyRoute& route :
         preview3d::provider::FamilyRoutes()) {
        MemoryStream stream(std::vector<std::byte>(4096, std::byte{0x2A}));
        HBITMAP bitmap = nullptr;
        WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
        const HRESULT hr =
            ActivateAndRender(module, GuidFromText(route.clsid), stream, 256, bitmap, alpha);

        INFO("family CLSID " << route.clsid);
        // A family with a linked adapter either renders a real bitmap or fails
        // closed on the garbage stream with a precise HRESULT and no bitmap. A
        // family with no adapter in this build gets the generic-icon fallback.
        const bool linked =
            preview3d::provider::CreateFamilyAdapter(route.family) != nullptr;
        if (linked) {
            if (hr == S_OK) {
                // A linked adapter produced a real, owned bitmap.
                CHECK(bitmap != nullptr);
                CHECK(alpha == WTSAT_ARGB);
            } else {
                // No fabricated bitmap on a typed parse failure.
                CHECK(bitmap == nullptr);
                CHECK(alpha == WTSAT_UNKNOWN);
            }
        } else {
            // No adapter linked: the generic-icon fallback, never a fabricated
            // bitmap.
            CHECK(hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
            CHECK(bitmap == nullptr);
            CHECK(alpha == WTSAT_UNKNOWN);
        }
        if (bitmap != nullptr) {
            ::DeleteObject(bitmap);
        }

        // Every object/factory was released, so the module may unload.
        CHECK(module.canUnloadNow() == S_OK);
    }
}

TEST_CASE("an unknown CLSID is never a product identity", "[host][com]")
{
    ProviderModule module;
    REQUIRE(module.Ready());

    const GUID unknown = {
        0x00000000, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};
    void* raw = reinterpret_cast<void*>(1);
    CHECK(module.getClassObject(unknown, __uuidof(IClassFactory), &raw) ==
          CLASS_E_CLASSNOTAVAILABLE);
    CHECK(raw == nullptr);
    CHECK(module.canUnloadNow() == S_OK);
}

TEST_CASE("every registered fixture matches its committed golden", "[host][golden]")
{
    REQUIRE_FALSE(ProviderHostFixtures().empty());

    for (const GoldenFixture& fixture : ProviderHostFixtures()) {
        RasterImage image;
        const ProviderOutcome outcome = RunFixture(fixture, image);
        INFO("fixture " << fixture.name);
        REQUIRE(outcome == ProviderOutcome::Success);
        REQUIRE(image.width != 0);
        REQUIRE(image.height != 0);

        int goldenWidth = 0;
        int goldenHeight = 0;
        std::vector<std::uint8_t> golden;
        REQUIRE(LoadGoldenRgba(fixture.goldenPath, goldenWidth, goldenHeight, golden));

        const GoldenDiff diff =
            CompareBgraToRgba(image.bgraPremultiplied, goldenWidth, goldenHeight, golden);
        INFO("fixture " << fixture.name << " meanAbs=" << diff.meanAbs
                        << " maxAbs=" << diff.maxAbs);
        CHECK(WithinTolerance(diff, fixture.tolerance));
    }
}

TEST_CASE("multiple STA apartments drive provider objects concurrently", "[host][parallel]")
{
    ProviderModule module;
    REQUIRE(module.Ready());

    // USD (T33) has no linked adapter yet, so the concurrent activation here
    // exercises the tabulated ERROR_NOT_SUPPORTED fallback; the golden case
    // above covers a real adapter concurrently through the fixtures loop.
    const GUID clsid = FamilyClsid(preview3d::provider::Family::Usd);
    constexpr int kThreads = 4;
    constexpr int kIterations = 25;
    std::atomic<int> failures{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&module, &clsid, &failures] {
            const HRESULT co = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const bool uninitialize = SUCCEEDED(co);

            for (int i = 0; i < kIterations; ++i) {
                MemoryStream stream(std::vector<std::byte>(1024, std::byte{0}));
                HBITMAP bitmap = nullptr;
                WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
                const HRESULT hr =
                    ActivateAndRender(module, clsid, stream, 128, bitmap, alpha);
                if (hr == S_OK) {
                    if (bitmap == nullptr || alpha != WTSAT_ARGB) {
                        ++failures;
                    }
                } else if (hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
                    if (bitmap != nullptr) {
                        ++failures;
                    }
                } else {
                    ++failures;
                }
                if (bitmap != nullptr) {
                    ::DeleteObject(bitmap);
                }

                for (const GoldenFixture& fixture : ProviderHostFixtures()) {
                    RasterImage image;
                    if (RunFixture(fixture, image) != ProviderOutcome::Success) {
                        ++failures;
                    }
                }
            }

            if (uninitialize) {
                ::CoUninitialize();
            }
        });
    }

    for (std::thread& thread : threads) {
        thread.join();
    }

    CHECK(failures.load() == 0);
    CHECK(module.canUnloadNow() == S_OK);
}

TEST_CASE("repeated load/unload keeps GDI, User, private bytes and threads stable",
          "[host][leak]")
{
    constexpr int kWarmup = 32;
    constexpr int kIterations = 128;
    for (int i = 0; i < kWarmup; ++i) {
        LeakCycle();
    }

    const preview3d::test::ProcessResources before = SampleProcessResources();
    for (int i = 0; i < kIterations; ++i) {
        LeakCycle();
    }
    const preview3d::test::ProcessResources after = SampleProcessResources();

    const auto delta = [](ULONGLONG a, ULONGLONG b) -> long long {
        return static_cast<long long>(b) - static_cast<long long>(a);
    };

    INFO("gdi " << before.gdiObjects << " -> " << after.gdiObjects);
    INFO("user " << before.userObjects << " -> " << after.userObjects);
    INFO("threads " << before.threads << " -> " << after.threads);
    INFO("private bytes " << before.privateBytes << " -> " << after.privateBytes);

    CHECK(delta(before.gdiObjects, after.gdiObjects) <= 4);
    CHECK(delta(before.userObjects, after.userObjects) <= 4);
    CHECK(delta(before.threads, after.threads) <= 1);
    CHECK(delta(before.privateBytes, after.privateBytes) <= 16ll * 1024 * 1024);
}

// Hidden by default; run explicitly to (re)generate the committed goldens:
//   x64\Release\Tests.ProviderHost.exe "[write-host-goldens]"
TEST_CASE("regenerate the provider host goldens", "[.][write-host-goldens]")
{
    std::filesystem::create_directories(ProviderHostGoldenDirectory());
    for (const GoldenFixture& fixture : ProviderHostFixtures()) {
        RasterImage image;
        REQUIRE(RunFixture(fixture, image) == ProviderOutcome::Success);
        REQUIRE(SaveGolden(fixture.goldenPath, image));
    }
}
#pragma once

// T17 golden-fixture registry.
//
// A family task (T21-T34) registers one `GoldenFixture` in FixtureRegistry.cpp.
// The host renders it through the *real* provider pipeline -- CLSID-routed
// adapter -> T14 deterministic sampler -> T15 CPU tile rasterizer -- and
// compares the result against the committed golden with the tolerant metric.
//
// The adapter is looked up by `family` through the same build-time
// `CreateFamilyAdapter` switch the DLL uses, so a family task must also compile
// its adapter source into Tests.ProviderHost.vcxproj for the fixture to render
// (see tests/provider-host/README.md). `Family::Unknown` selects the built-in
// placeholder adapter, which exists only to exercise the harness before any
// real family lands.

#include "GoldenImage.h"
#include "ProviderTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace preview3d::test {

struct GoldenFixture {
    // Unique fixture name, reported on failure.
    std::string_view name;

    // Family used both to route the adapter and to seed the sampler.
    preview3d::provider::Family family = preview3d::provider::Family::Unknown;

    // Bytes the bounded source exposes to the adapter. Small and committed;
    // never a multi-gigabyte binary.
    std::vector<std::byte> source;

    // Absolute path of the committed PAM golden.
    std::string goldenPath;

    // Tolerant comparison bounds.
    GoldenTolerance tolerance{};

    // Requested cx; the rasterizer clamps internally.
    std::uint32_t cx = 256;
};

// Stable storage; every registered fixture, in execution order. Additive per
// family task.
std::span<const GoldenFixture> ProviderHostFixtures();

// The directory the goldens live in, from the build's macro.
std::string ProviderHostGoldenDirectory();

} // namespace preview3d::test
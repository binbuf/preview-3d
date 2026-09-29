// T17 golden-fixture registry.
//
// Family tasks (T21-T34) register their fixture here, one entry each. The host
// renders every entry through the real pipeline and compares it with the
// committed PAM golden named by the entry. See tests/provider-host/README.md for
// the exact workflow.
//
// Today the only entry is the built-in placeholder scene (`Family::Unknown`),
// which proves the comparator, the pipeline composition and the leak loop before
// the first real family adapter exists.

#include "FixtureRegistry.h"

#include <cstdint>
#include <cstring>
#include <filesystem>

namespace preview3d::test {
namespace {

// A committed-size binary STL: a unit cube as 12 facets (two per face), each
// with a generated flat normal (the binary record's normal field is left zero,
// so the adapter computes it). 84 + 12 * 50 = 684 bytes.
std::vector<std::byte> BuildCubeStl()
{
    constexpr float kCube[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                   {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    constexpr int kQuads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                  {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};

    std::vector<std::byte> bytes(80 + 4, std::byte{0});
    const std::uint32_t facetCount = 12;
    std::memcpy(bytes.data() + 80, &facetCount, sizeof(facetCount));

    const auto appendFacet = [&bytes](const float* a, const float* b, const float* c) {
        float values[12] = {0, 0, 0, a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]};
        const auto* raw = reinterpret_cast<const std::byte*>(values);
        bytes.insert(bytes.end(), raw, raw + sizeof(values));
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{0});
    };
    for (const auto& quad : kQuads) {
        appendFacet(kCube[quad[0]], kCube[quad[1]], kCube[quad[2]]);
        appendFacet(kCube[quad[0]], kCube[quad[2]], kCube[quad[3]]);
    }
    return bytes;
}

} // namespace

std::string ProviderHostGoldenDirectory()
{
    return std::filesystem::path(PREVIEW3D_PROVIDER_HOST_GOLDEN_DIR).string();
}

std::span<const GoldenFixture> ProviderHostFixtures()
{
    static const std::vector<GoldenFixture> kFixtures = [] {
        std::vector<GoldenFixture> fixtures;

        // Placeholder: exercises the harness itself (adapter -> sampler ->
        // rasterizer -> golden). Replace/keep alongside the first real family
        // adapter; it is not a product format.
        GoldenFixture placeholder;
        placeholder.name = "placeholder-sphere";
        placeholder.family = preview3d::provider::Family::Unknown;
        placeholder.source.assign(4096, std::byte{0x2A});
        placeholder.goldenPath = ProviderHostGoldenDirectory() + "placeholder-256.pam";
        placeholder.tolerance = GoldenTolerance{2.0, 48};
        placeholder.cx = 256;
        fixtures.push_back(std::move(placeholder));

        // T21: the first real family fixture. Routes Family::Stl through the
        // linked StlAdapter, the T14 sampler and the T15 rasterizer.
        GoldenFixture stlCube;
        stlCube.name = "stl-cube";
        stlCube.family = preview3d::provider::Family::Stl;
        stlCube.source = BuildCubeStl();
        stlCube.goldenPath = ProviderHostGoldenDirectory() + "stl-cube-256.pam";
        stlCube.tolerance = GoldenTolerance{2.0, 48};
        stlCube.cx = 256;
        fixtures.push_back(std::move(stlCube));

        return fixtures;
    }();
    return kFixtures;
}

} // namespace preview3d::test
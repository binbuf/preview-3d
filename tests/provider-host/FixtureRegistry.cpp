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

#include <filesystem>

namespace preview3d::test {

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

        return fixtures;
    }();
    return kFixtures;
}

} // namespace preview3d::test
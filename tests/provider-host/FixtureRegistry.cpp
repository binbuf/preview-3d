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
#include <string>

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

// A binary little-endian PLY cube: 8 vertices with RGB colors and 6 quad faces
// (fan-triangulated by the adapter). T23's committed provider-host fixture.
std::vector<std::byte> BuildCubePly()
{
    constexpr float kCube[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                   {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    constexpr std::uint8_t kColor[8][3] = {{230, 90, 70},  {240, 170, 70}, {110, 200, 90},
                                           {80, 160, 220}, {160, 110, 220}, {230, 110, 180},
                                           {90, 200, 200}, {230, 210, 90}};
    constexpr int kQuads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                  {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};

    const std::string header =
        "ply\nformat binary_little_endian 1.0\n"
        "element vertex 8\nproperty float x\nproperty float y\nproperty float z\n"
        "property uchar red\nproperty uchar green\nproperty uchar blue\n"
        "element face 6\nproperty list uchar int vertex_indices\nend_header\n";
    std::vector<std::byte> bytes;
    const auto appendText = [&bytes](const std::string& text) {
        const auto* data = reinterpret_cast<const std::byte*>(text.data());
        bytes.insert(bytes.end(), data, data + text.size());
    };
    const auto appendFloat = [&bytes](float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const auto* data = reinterpret_cast<const std::byte*>(&bits);
        bytes.insert(bytes.end(), data, data + sizeof(bits));
    };
    const auto appendInt = [&bytes](std::int32_t value) {
        const auto* data = reinterpret_cast<const std::byte*>(&value);
        bytes.insert(bytes.end(), data, data + sizeof(value));
    };

    appendText(header);
    for (int i = 0; i < 8; ++i) {
        appendFloat(kCube[i][0]);
        appendFloat(kCube[i][1]);
        appendFloat(kCube[i][2]);
        for (int channel = 0; channel < 3; ++channel) {
            bytes.push_back(static_cast<std::byte>(kColor[i][channel]));
        }
    }
    for (const auto& quad : kQuads) {
        bytes.push_back(std::byte{4});
        for (int corner = 0; corner < 4; ++corner) {
            appendInt(quad[corner]);
        }
    }
    return bytes;
}

// An ASCII OBJ cube whose geometry references an ignored `.mtl`: six quad
// faces fan-triangulated by the T24 adapter into twelve neutral triangles. The
// `mtllib`/`usemtl` lines prove the thumbnail path never resolves the sidecar.
std::vector<std::byte> BuildCubeObj()
{
    const std::string text =
        "mtllib cube-sidecar.mtl\n"
        "o cube\n"
        "usemtl cube\n"
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "v 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
        "f 1 4 3 2\n"
        "f 5 6 7 8\n"
        "f 1 2 6 5\n"
        "f 4 8 7 3\n"
        "f 1 5 8 4\n"
        "f 2 3 7 6\n";
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
}

// A binary little-endian colored point cloud: a 4x4x4 grid with a per-axis hue.
std::vector<std::byte> BuildColoredPointsPly()
{
    constexpr int kSide = 4;
    const std::string header =
        "ply\nformat binary_little_endian 1.0\n"
        "element vertex 64\nproperty float x\nproperty float y\nproperty float z\n"
        "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n";
    std::vector<std::byte> bytes;
    const auto appendText = [&bytes](const std::string& text) {
        const auto* data = reinterpret_cast<const std::byte*>(text.data());
        bytes.insert(bytes.end(), data, data + text.size());
    };
    const auto appendFloat = [&bytes](float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const auto* data = reinterpret_cast<const std::byte*>(&bits);
        bytes.insert(bytes.end(), data, data + sizeof(bits));
    };
    appendText(header);
    for (int x = 0; x < kSide; ++x) {
        for (int y = 0; y < kSide; ++y) {
            for (int z = 0; z < kSide; ++z) {
                appendFloat(static_cast<float>(x));
                appendFloat(static_cast<float>(y));
                appendFloat(static_cast<float>(z));
                bytes.push_back(static_cast<std::byte>(40 + x * 60));
                bytes.push_back(static_cast<std::byte>(40 + y * 60));
                bytes.push_back(static_cast<std::byte>(40 + z * 60));
            }
        }
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

        // T23: PLY mesh and point-cloud fixtures through the linked PlyAdapter.
        GoldenFixture plyCube;
        plyCube.name = "ply-cube";
        plyCube.family = preview3d::provider::Family::Ply;
        plyCube.source = BuildCubePly();
        plyCube.goldenPath = ProviderHostGoldenDirectory() + "ply-cube-256.pam";
        plyCube.tolerance = GoldenTolerance{2.0, 48};
        plyCube.cx = 256;
        fixtures.push_back(std::move(plyCube));

        GoldenFixture plyPoints;
        plyPoints.name = "ply-points";
        plyPoints.family = preview3d::provider::Family::Ply;
        plyPoints.source = BuildColoredPointsPly();
        plyPoints.goldenPath = ProviderHostGoldenDirectory() + "ply-points-256.pam";
        plyPoints.tolerance = GoldenTolerance{2.0, 48};
        plyPoints.cx = 256;
        fixtures.push_back(std::move(plyPoints));

        // T24: an OBJ cube, routed through the linked ObjAdapter (provider-local
        // ufbx). Its `mtllib`/`usemtl` references are ignored, so the golden is
        // the neutral-palette cube.
        GoldenFixture objCube;
        objCube.name = "obj-cube";
        objCube.family = preview3d::provider::Family::Obj;
        objCube.source = BuildCubeObj();
        objCube.goldenPath = ProviderHostGoldenDirectory() + "obj-cube-256.pam";
        objCube.tolerance = GoldenTolerance{2.0, 48};
        objCube.cx = 256;
        fixtures.push_back(std::move(objCube));

        return fixtures;
    }();
    return kFixtures;
}

} // namespace preview3d::test
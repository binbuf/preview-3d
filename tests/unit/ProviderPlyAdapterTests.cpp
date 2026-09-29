// T23 PLY family adapter coverage.
//
// `PlyFamilyAdapter.cpp` and `FamilyAdapterRegistry.cpp` are compiled into
// Tests.Unit.exe, so these cases exercise the real adapter contract directly
// (Initialize -> Parse -> EnumerateMaterials -> EnumerateGeometry) and through
// the real routed pipeline (`RunThumbnailPipeline` + `DefaultThumbnailDependencies`,
// i.e. the shipped T14 sampler and T15 rasterizer). Malformed/hostile inputs are
// asserted as typed failures, and the declared vertex count is shown to be
// rejected before any allocation exists.
//
// Fixtures are built in-memory (no committed model file) so the ASCII, binary
// little-endian and binary big-endian dialects are all exercised against the
// same geometry, matching the layout parser_core::ParseHeader expects.

#include <catch2/catch_test_macros.hpp>

#include "AllocationLedger.h"
#include "Deadline.h"
#include "FamilyAdapter.h"
#include "FamilyAdapterRegistry.h"
#include "FamilyRouting.h"
#include "PlyFamilyAdapter.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"
#include "ThumbnailPipeline.h"

#include "parser_core/PlyParserCore.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace preview3d::provider;

namespace {

class ByteSource final : public BoundedSource {
public:
    explicit ByteSource(std::vector<std::byte> bytes, bool exposeContiguous = true)
        : bytes_(std::move(bytes)), exposeContiguous_(exposeContiguous)
    {
    }

    std::uint64_t Size() const noexcept override { return bytes_.size(); }
    bool Seekable() const noexcept override { return true; }

    bool ReadAt(std::uint64_t offset, std::span<std::byte> dest) override
    {
        if (offset > bytes_.size() || dest.size() > bytes_.size() - offset) {
            return false;
        }
        if (!dest.empty()) {
            std::memcpy(dest.data(), bytes_.data() + offset, dest.size());
        }
        return true;
    }

    std::span<const std::byte> ContiguousView() override
    {
        return exposeContiguous_ ? std::span<const std::byte>(bytes_.data(), bytes_.size())
                                 : std::span<const std::byte>{};
    }

private:
    std::vector<std::byte> bytes_;
    bool exposeContiguous_;
};

struct CollectSink final : IGeometrySink {
    std::vector<TriangleSample> triangles;
    std::vector<PointSample> points;
    int triangleLimit = -1;
    int pointLimit = -1;

    bool OnTriangle(const TriangleSample& triangle) noexcept override
    {
        if (triangleLimit >= 0 && static_cast<int>(triangles.size()) >= triangleLimit) {
            return false;
        }
        triangles.push_back(triangle);
        return true;
    }

    bool OnPoint(const PointSample& point) noexcept override
    {
        if (pointLimit >= 0 && static_cast<int>(points.size()) >= pointLimit) {
            return false;
        }
        points.push_back(point);
        return true;
    }
};

struct CollectMaterials final : IMaterialSink {
    std::vector<std::uint32_t> indices;
    std::vector<model_core::MaterialPayload> materials;

    bool OnMaterial(std::uint32_t materialIndex,
                    const model_core::MaterialPayload& material) noexcept override
    {
        indices.push_back(materialIndex);
        materials.push_back(material);
        return true;
    }
};

struct AdapterResult {
    ErrorCode initialize = ErrorCode::None;
    ErrorCode parse = ErrorCode::None;
    ErrorCode materials = ErrorCode::None;
    ErrorCode geometry = ErrorCode::None;
    bool geometryRan = false;
    std::vector<std::uint32_t> materialIndices;
    model_core::MaterialPayload material{};
};

AdapterResult RunAdapter(PlyAdapter& adapter, BoundedSource& source, Deadline& deadline,
                         AllocationLedger& ledger, IGeometrySink& sink)
{
    AdapterInput input{};
    input.source = &source;
    input.limits = &ProviderLimits::Default();
    input.deadline = &deadline;
    input.ledger = &ledger;
    input.family = Family::Ply;

    AdapterResult result;
    result.initialize = adapter.Initialize(input);
    if (result.initialize != ErrorCode::None) {
        return result;
    }
    result.parse = adapter.Parse();
    if (result.parse != ErrorCode::None) {
        return result;
    }
    CollectMaterials materials;
    result.materials = adapter.EnumerateMaterials(materials);
    result.materialIndices = materials.indices;
    if (!materials.materials.empty()) {
        result.material = materials.materials.front();
    }
    result.geometryRan = true;
    result.geometry = adapter.EnumerateGeometry(sink);
    return result;
}

ProviderOutcome RunPipeline(BoundedSource& source, std::uint32_t cx = 64)
{
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Ply;
    request.source = &source;
    request.limits = &ProviderLimits::Default();
    request.deadline = &deadline;
    request.ledger = &ledger;
    request.cx = cx;
    RasterImage image;
    return RunThumbnailPipeline(request, DefaultThumbnailDependencies(), image);
}

// --- Fixture builder --------------------------------------------------------

struct TestVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
};

struct TestFace {
    std::vector<std::uint32_t> indices;
};

struct BuildOptions {
    parser_core::PlyFormat format = parser_core::PlyFormat::Ascii;
    bool withNormals = false;
    bool withColors = false;
    bool withAlpha = false;
    bool unknownVertexScalar = false; // property float confidence
    bool unknownVertexList = false;   // property list uchar int extras
    bool unknownFaceList = false;     // property list uchar float weights
    bool unknownElement = false;      // element edge N property int a property int b
    std::uint64_t vertexCountOverride = 0;
    std::uint64_t faceCountOverride = 0;
    std::vector<TestVertex> vertices;
    std::vector<TestFace> faces; // empty => point cloud
};

void Append(std::vector<std::byte>& out, const std::string& text)
{
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    out.insert(out.end(), data, data + text.size());
}

void AppendU8(std::vector<std::byte>& out, std::uint8_t value)
{
    out.push_back(static_cast<std::byte>(value));
}

void AppendU32(std::vector<std::byte>& out, std::uint32_t value, bool bigEndian)
{
    const std::uint32_t encoded = bigEndian
        ? ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
              ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24)
        : value;
    const std::size_t offset = out.size();
    out.resize(offset + 4);
    std::memcpy(out.data() + offset, &encoded, 4);
}

void AppendI32(std::vector<std::byte>& out, std::int32_t value, bool bigEndian)
{
    AppendU32(out, static_cast<std::uint32_t>(value), bigEndian);
}

void AppendF32(std::vector<std::byte>& out, float value, bool bigEndian)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    AppendU32(out, bits, bigEndian);
}

std::string HeaderText(const BuildOptions& options, std::uint64_t vertexCount,
                       std::uint64_t faceCount)
{
    std::string header = "ply\n";
    switch (options.format) {
        case parser_core::PlyFormat::BinaryLittleEndian:
            header += "format binary_little_endian 1.0\n";
            break;
        case parser_core::PlyFormat::BinaryBigEndian:
            header += "format binary_big_endian 1.0\n";
            break;
        case parser_core::PlyFormat::Ascii:
            header += "format ascii 1.0\n";
            break;
    }
    header += "comment T23 fixture\n";
    header += "element vertex " + std::to_string(vertexCount) + "\n";
    header += "property float x\n";
    header += "property float y\n";
    header += "property float z\n";
    if (options.withNormals) {
        header += "property float nx\nproperty float ny\nproperty float nz\n";
    }
    if (options.withColors) {
        header += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
        if (options.withAlpha) {
            header += "property uchar alpha\n";
        }
    }
    if (options.unknownVertexScalar) {
        header += "property float confidence\n";
    }
    if (options.unknownVertexList) {
        header += "property list uchar int extras\n";
    }
    if (!options.faces.empty() || faceCount != 0) {
        header += "element face " + std::to_string(faceCount) + "\n";
        header += "property list uchar int vertex_indices\n";
        if (options.unknownFaceList) {
            header += "property list uchar float weights\n";
        }
    }
    if (options.unknownElement) {
        header += "element edge 3\n";
        header += "property int a\nproperty int b\n";
    }
    header += "end_header\n";
    return header;
}

std::vector<std::byte> BuildPly(const BuildOptions& options)
{
    const std::uint64_t vertexCount =
        options.vertexCountOverride != 0 ? options.vertexCountOverride : options.vertices.size();
    const std::uint64_t faceCount = options.faceCountOverride != 0
        ? options.faceCountOverride
        : options.faces.size();
    const bool bigEndian = options.format == parser_core::PlyFormat::BinaryBigEndian;
    const bool ascii = options.format == parser_core::PlyFormat::Ascii;

    std::vector<std::byte> bytes;
    Append(bytes, HeaderText(options, vertexCount, faceCount));

    const auto writeVertex = [&](const TestVertex& vertex) {
        if (ascii) {
            std::string line = std::to_string(vertex.x) + " " + std::to_string(vertex.y) + " " +
                               std::to_string(vertex.z);
            if (options.withNormals) {
                line += " " + std::to_string(vertex.nx) + " " + std::to_string(vertex.ny) + " " +
                        std::to_string(vertex.nz);
            }
            if (options.withColors) {
                line += " " + std::to_string(vertex.r) + " " + std::to_string(vertex.g) + " " +
                        std::to_string(vertex.b);
                if (options.withAlpha) {
                    line += " " + std::to_string(vertex.a);
                }
            }
            if (options.unknownVertexScalar) {
                line += " 0.5";
            }
            if (options.unknownVertexList) {
                line += " 2 7 9";
            }
            line += "\n";
            Append(bytes, line);
            return;
        }
        AppendF32(bytes, vertex.x, bigEndian);
        AppendF32(bytes, vertex.y, bigEndian);
        AppendF32(bytes, vertex.z, bigEndian);
        if (options.withNormals) {
            AppendF32(bytes, vertex.nx, bigEndian);
            AppendF32(bytes, vertex.ny, bigEndian);
            AppendF32(bytes, vertex.nz, bigEndian);
        }
        if (options.withColors) {
            AppendU8(bytes, vertex.r);
            AppendU8(bytes, vertex.g);
            AppendU8(bytes, vertex.b);
            if (options.withAlpha) {
                AppendU8(bytes, vertex.a);
            }
        }
        if (options.unknownVertexScalar) {
            AppendF32(bytes, 0.5f, bigEndian);
        }
        if (options.unknownVertexList) {
            AppendU8(bytes, 2);
            AppendI32(bytes, 7, bigEndian);
            AppendI32(bytes, 9, bigEndian);
        }
    };

    for (std::uint64_t i = 0; i < vertexCount; ++i) {
        const std::size_t index = static_cast<std::size_t>(i);
        TestVertex fallback;
        fallback.x = static_cast<float>(i);
        writeVertex(index < options.vertices.size() ? options.vertices[index] : fallback);
    }

    for (std::uint64_t f = 0; f < faceCount; ++f) {
        const std::size_t index = static_cast<std::size_t>(f);
        const TestFace fallback;
        const TestFace& face = index < options.faces.size() ? options.faces[index] : fallback;
        if (ascii) {
            std::string line = std::to_string(face.indices.size());
            for (std::uint32_t vertex : face.indices) {
                line += " " + std::to_string(vertex);
            }
            if (options.unknownFaceList) {
                line += " 2 0.25 0.75";
            }
            line += "\n";
            Append(bytes, line);
            continue;
        }
        AppendU8(bytes, static_cast<std::uint8_t>(face.indices.size()));
        for (std::uint32_t vertex : face.indices) {
            AppendI32(bytes, static_cast<std::int32_t>(vertex), bigEndian);
        }
        if (options.unknownFaceList) {
            AppendU8(bytes, 2);
            AppendF32(bytes, 0.25f, bigEndian);
            AppendF32(bytes, 0.75f, bigEndian);
        }
    }

    if (options.unknownElement) {
        for (std::uint32_t e = 0; e < 3; ++e) {
            if (ascii) {
                Append(bytes, std::to_string(e) + " " + std::to_string(e * 2) + "\n");
            } else {
                AppendI32(bytes, static_cast<std::int32_t>(e), bigEndian);
                AppendI32(bytes, static_cast<std::int32_t>(e * 2), bigEndian);
            }
        }
    }
    return bytes;
}

std::vector<TestVertex> UnitTriangle()
{
    TestVertex a;
    a.x = 0.0f; a.y = 0.0f; a.z = 0.0f; a.nz = 1.0f;
    TestVertex b;
    b.x = 2.0f; b.y = 0.0f; b.z = 0.0f; b.nz = 1.0f;
    TestVertex c;
    c.x = 0.0f; c.y = 2.0f; c.z = 0.0f; c.nz = 1.0f;
    return {a, b, c};
}

std::vector<TestVertex> CubeVertices()
{
    return {
        {0, 0, 0, 0, 0, 0, 255, 255, 255, 255}, {1, 0, 0, 0, 0, 0, 255, 255, 255, 255},
        {1, 1, 0, 0, 0, 0, 255, 255, 255, 255}, {0, 1, 0, 0, 0, 0, 255, 255, 255, 255},
        {0, 0, 1, 0, 0, 0, 255, 255, 255, 255}, {1, 0, 1, 0, 0, 0, 255, 255, 255, 255},
        {1, 1, 1, 0, 0, 0, 255, 255, 255, 255}, {0, 1, 1, 0, 0, 0, 255, 255, 255, 255},
    };
}

std::vector<TestFace> CubeFaces()
{
    return {
        {{0, 3, 2, 1}}, {{4, 5, 6, 7}}, {{0, 1, 5, 4}},
        {{3, 7, 6, 2}}, {{0, 4, 7, 3}}, {{1, 2, 6, 5}},
    };
}

BuildOptions MeshOptions(parser_core::PlyFormat format)
{
    BuildOptions options;
    options.format = format;
    options.vertices = UnitTriangle();
    options.faces = {{{0, 1, 2}}};
    return options;
}

} // namespace

// --- Header/parse -----------------------------------------------------------

TEST_CASE("an ASCII PLY triangle mesh emits one triangle and the neutral material",
          "[provider][ply]")
{
    ByteSource source(BuildPly(MeshOptions(parser_core::PlyFormat::Ascii)));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    PlyAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.initialize == ErrorCode::None);
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.materials == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE(result.materialIndices == std::vector<std::uint32_t>{1u});
    REQUIRE(sink.triangles.size() == 1u);
    CHECK(sink.points.empty());
    CHECK(sink.triangles[0].materialIndex == 1u);
    CHECK(sink.triangles[0].vertices[0].position[0] == 0.0f);
    CHECK(sink.triangles[0].vertices[1].position[0] == 2.0f);
    CHECK(sink.triangles[0].vertices[2].position[1] == 2.0f);
}

TEST_CASE("binary little-endian and big-endian meshes match the ASCII geometry",
          "[provider][ply]")
{
    for (const auto format : {parser_core::PlyFormat::BinaryLittleEndian,
                              parser_core::PlyFormat::BinaryBigEndian}) {
        CAPTURE(static_cast<int>(format));
        ByteSource source(BuildPly(MeshOptions(format)));
        Deadline deadline;
        AllocationLedger ledger;
        CollectSink sink;

        PlyAdapter adapter;
        const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
        INFO("format " << static_cast<int>(format));
        CHECK(result.parse == ErrorCode::None);
        CHECK(result.geometry == ErrorCode::None);
        REQUIRE(sink.triangles.size() == 1u);
        CHECK(sink.triangles[0].vertices[0].position[0] == 0.0f);
        CHECK(sink.triangles[0].vertices[1].position[0] == 2.0f);
        CHECK(sink.triangles[0].vertices[2].position[1] == 2.0f);
    }
}

TEST_CASE("binary meshes render through the routed pipeline without a contiguous view",
          "[provider][ply]")
{
    BuildOptions options = MeshOptions(parser_core::PlyFormat::BinaryLittleEndian);
    options.faces = CubeFaces();
    options.vertices = CubeVertices();
    ByteSource source(BuildPly(options), /*exposeContiguous=*/false);
    CHECK(RunPipeline(source) == ProviderOutcome::Success);
}

TEST_CASE("a point-only PLY emits points and never a triangle", "[provider][ply]")
{
    for (const auto format : {parser_core::PlyFormat::Ascii,
                              parser_core::PlyFormat::BinaryLittleEndian,
                              parser_core::PlyFormat::BinaryBigEndian}) {
        CAPTURE(static_cast<int>(format));
        BuildOptions options;
        options.format = format;
        options.vertices = UnitTriangle();
        ByteSource source(BuildPly(options));
        Deadline deadline;
        AllocationLedger ledger;
        CollectSink sink;

        PlyAdapter adapter;
        const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
        CHECK(result.parse == ErrorCode::None);
        CHECK(result.geometry == ErrorCode::None);
        CHECK(sink.triangles.empty());
        REQUIRE(sink.points.size() == 3u);
        CHECK(sink.points[0].materialIndex == 1u);
        CHECK(sink.points[2].vertex.position[1] == 2.0f);
    }
}

TEST_CASE("vertex colors are normalized and carried on points", "[provider][ply]")
{
    BuildOptions options;
    options.format = parser_core::PlyFormat::BinaryLittleEndian;
    options.withColors = true;
    options.vertices = {{0, 0, 0, 0, 0, 0, 255, 128, 0, 255}, {1, 0, 0, 0, 0, 0, 0, 255, 64, 255}};
    ByteSource source(BuildPly(options));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    PlyAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    REQUIRE(sink.points.size() == 2u);
    CHECK(sink.points[0].vertex.color[0] == 1.0f);
    CHECK(sink.points[0].vertex.color[1] > 0.49f);
    CHECK(sink.points[0].vertex.color[1] < 0.51f);
    CHECK(sink.points[0].vertex.color[2] == 0.0f);
    CHECK(sink.points[1].vertex.color[2] > 0.24f);
    CHECK(sink.points[1].vertex.color[2] < 0.26f);
    // The colored material keeps a white base so vertex colors survive.
    CHECK(result.material.baseColorFactor[0] == 1.0f);
    CHECK(result.material.baseColorFactor[1] == 1.0f);
}

TEST_CASE("a colored point cloud renders a bitmap through the routed pipeline",
          "[provider][ply]")
{
    BuildOptions options;
    options.format = parser_core::PlyFormat::BinaryBigEndian;
    options.withColors = true;
    options.vertices = {{0, 0, 0, 0, 0, 0, 255, 0, 0, 255}, {2, 0, 0, 0, 0, 0, 0, 255, 0, 255},
                        {0, 2, 0, 0, 0, 0, 0, 0, 255, 255}, {2, 2, 0, 0, 0, 0, 255, 255, 0, 255}};
    ByteSource source(BuildPly(options));
    CHECK(RunPipeline(source, 64) == ProviderOutcome::Success);
}

TEST_CASE("supplied normals are normalized while a mesh without them renders flat",
          "[provider][ply]")
{
    BuildOptions options = MeshOptions(parser_core::PlyFormat::BinaryLittleEndian);
    options.withNormals = true;
    options.vertices[0].nx = 0.0f; options.vertices[0].ny = 0.0f; options.vertices[0].nz = 10.0f;
    options.vertices[1].nx = 0.0f; options.vertices[1].ny = 0.0f; options.vertices[1].nz = 10.0f;
    options.vertices[2].nx = 0.0f; options.vertices[2].ny = 0.0f; options.vertices[2].nz = 10.0f;
    ByteSource source(BuildPly(options));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    PlyAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::None);
    REQUIRE(sink.triangles.size() == 1u);
    for (const VertexSample& vertex : sink.triangles[0].vertices) {
        CHECK(vertex.normal[2] == 1.0f);
    }
}

// --- Unknown properties / elements / lists ----------------------------------

TEST_CASE("unknown scalar and list properties are skipped within bounds",
          "[provider][ply]")
{
    // ASCII mesh: unknown vertex scalar + vertex list + face list.
    {
        BuildOptions options = MeshOptions(parser_core::PlyFormat::Ascii);
        options.unknownVertexScalar = true;
        options.unknownVertexList = true;
        options.unknownFaceList = true;
        ByteSource source(BuildPly(options));
        Deadline deadline;
        AllocationLedger ledger;
        CollectSink sink;
        PlyAdapter adapter;
        const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
        CHECK(result.parse == ErrorCode::None);
        CHECK(result.geometry == ErrorCode::None);
        CHECK(sink.triangles.size() == 1u);
    }
    // Binary point cloud: unknown vertex scalar/list + a whole unknown element.
    {
        BuildOptions options;
        options.format = parser_core::PlyFormat::BinaryLittleEndian;
        options.vertices = UnitTriangle();
        options.unknownVertexScalar = true;
        options.unknownVertexList = true;
        options.unknownElement = true;
        ByteSource source(BuildPly(options));
        Deadline deadline;
        AllocationLedger ledger;
        CollectSink sink;
        PlyAdapter adapter;
        const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
        CHECK(result.parse == ErrorCode::None);
        CHECK(result.geometry == ErrorCode::None);
        REQUIRE(sink.points.size() == 3u);
    }
    // Binary mesh: unknown vertex scalar + unknown face list.
    {
        BuildOptions options = MeshOptions(parser_core::PlyFormat::BinaryLittleEndian);
        options.unknownVertexScalar = true;
        options.unknownFaceList = true;
        ByteSource source(BuildPly(options));
        Deadline deadline;
        AllocationLedger ledger;
        CollectSink sink;
        PlyAdapter adapter;
        const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
        CHECK(result.parse == ErrorCode::None);
        CHECK(result.geometry == ErrorCode::None);
        CHECK(sink.triangles.size() == 1u);
    }
}

TEST_CASE("out-of-range and degenerate face indices are dropped locally",
          "[provider][ply]")
{
    BuildOptions options = MeshOptions(parser_core::PlyFormat::BinaryLittleEndian);
    options.faces = {{{0, 1, 2}}, {{0, 1, 99}}, {{0, 1}}};
    ByteSource source(BuildPly(options));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    PlyAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.size() == 1u);
}

// --- Typed failures ---------------------------------------------------------

TEST_CASE("a hostile declared vertex count fails before any allocation",
          "[provider][ply]")
{
    BuildOptions options = MeshOptions(parser_core::PlyFormat::BinaryLittleEndian);
    options.vertexCountOverride = ProviderLimits::kPointsInspectedMax + 1;
    ByteSource source(BuildPly(options));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    PlyAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::ResourceLimit);
    CHECK_FALSE(result.geometryRan);
    CHECK(sink.triangles.empty());
    CHECK(sink.points.empty());
}

TEST_CASE("a hostile face-list count is a resource limit", "[provider][ply]")
{
    std::string text = "ply\nformat ascii 1.0\n"
                       "element vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
                       "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
                       "0 0 0\n1 0 0\n0 1 0\n9999 0 1 2\n";
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    PlyAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::ResourceLimit);
}

TEST_CASE("a truncated binary PLY is malformed data", "[provider][ply]")
{
    std::vector<std::byte> bytes = BuildPly(MeshOptions(parser_core::PlyFormat::BinaryLittleEndian));
    bytes.resize(bytes.size() - 4); // declares 1 face, carries part of its index list
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    PlyAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::MalformedData);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a truncated ASCII PLY is malformed data", "[provider][ply]")
{
    std::string text = "ply\nformat ascii 1.0\n"
                       "element vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
                       "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
                       "0 0 0\n1 0 0\n";
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    PlyAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::MalformedData);
}

TEST_CASE("a PLY missing position properties is malformed data", "[provider][ply]")
{
    std::string text = "ply\nformat ascii 1.0\n"
                       "element vertex 1\nproperty float u\nproperty float v\nend_header\n"
                       "0 0\n";
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    PlyAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::MalformedData);
}

// --- Stop / deadline / limits ----------------------------------------------

TEST_CASE("a geometry-sink cap stop ends enumeration without failing", "[provider][ply]")
{
    BuildOptions options;
    options.format = parser_core::PlyFormat::BinaryLittleEndian;
    options.vertices = UnitTriangle();
    options.unknownElement = true;
    ByteSource source(BuildPly(options));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;
    sink.pointLimit = 2;

    PlyAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.points.size() == 2u);
}

TEST_CASE("an expired deadline stops the parse with a typed outcome", "[provider][ply]")
{
    ByteSource source(BuildPly(MeshOptions(parser_core::PlyFormat::BinaryLittleEndian)));
    Deadline deadline(Deadline::Clock::now() - std::chrono::seconds(10));
    AllocationLedger ledger;
    CollectSink sink;

    PlyAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::Cancelled);
    CHECK(sink.triangles.empty());
}

TEST_CASE("an exhausted allocation ledger fails the retained ASCII table safely",
          "[provider][ply]")
{
    BuildOptions options = MeshOptions(parser_core::PlyFormat::Ascii);
    options.vertices = CubeVertices();
    options.faces = {TestFace{{0, 1, 2}}};
    ByteSource source(BuildPly(options));
    Deadline deadline;
    AllocationLedger ledger(8); // smaller than the retained table
    CollectSink sink;

    PlyAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::ResourceLimit);
}

// --- Routing ----------------------------------------------------------------

TEST_CASE("the routed pipeline maps PLY failures to the tabulated outcomes",
          "[provider][ply]")
{
    {
        BuildOptions options = MeshOptions(parser_core::PlyFormat::BinaryLittleEndian);
        options.vertexCountOverride = ProviderLimits::kPointsInspectedMax + 1;
        ByteSource source(BuildPly(options));
        CHECK(RunPipeline(source) == ProviderOutcome::LimitExceeded);
    }
    {
        std::string text = "not a ply file\n";
        std::vector<std::byte> bytes(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
        ByteSource source(std::move(bytes));
        CHECK(RunPipeline(source) == ProviderOutcome::BadFormat);
    }
}

TEST_CASE("a valid PLY mesh renders a non-empty bitmap through the real dependencies",
          "[provider][ply]")
{
    BuildOptions options = MeshOptions(parser_core::PlyFormat::BinaryLittleEndian);
    options.faces = CubeFaces();
    options.vertices = CubeVertices();
    ByteSource source(BuildPly(options));
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Ply;
    request.source = &source;
    request.limits = &ProviderLimits::Default();
    request.deadline = &deadline;
    request.ledger = &ledger;
    request.cx = 64;
    RasterImage image;

    const ProviderOutcome outcome =
        RunThumbnailPipeline(request, DefaultThumbnailDependencies(), image);

    CHECK(outcome == ProviderOutcome::Success);
    CHECK(image.width != 0u);
    CHECK(image.height != 0u);
    CHECK_FALSE(image.bgraPremultiplied.empty());
}

TEST_CASE("PLY is routed from its frozen CLSID", "[provider][ply]")
{
    CHECK(FamilyForClsid("{F4DC6119-E235-4BAC-8089-54EDD84F8492}") == Family::Ply);
    CHECK(CreateFamilyAdapter(Family::Ply) != nullptr);
}
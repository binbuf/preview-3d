// T25 glTF/GLB family adapter coverage.
//
// `GltfFamilyAdapter.cpp` and `FamilyAdapterRegistry.cpp` are compiled into
// Tests.Unit.exe, so these cases exercise the real adapter contract directly
// (Initialize -> Parse -> EnumerateMaterials -> EnumerateGeometry) and through
// the real routed pipeline (`RunThumbnailPipeline` + `DefaultThumbnailDependencies`).
// Provider-local fastgltf/Draco/meshoptimizer/KTX/WebP libraries are linked here
// too, the same way the DLL links them.
//
// In-memory fixtures (a GLB and a `.gltf` data-URI asset) keep the geometry
// cases self-contained. The real compressed corpus
// (`interactive-viewer\test-assets\corpus`) supplies the Draco, meshopt,
// KTX2/Basis, WebP and sidecar/malformed cases; nothing in the provider opens a
// path -- the tests hand the raw bytes to the adapter as a bounded source.

#include <catch2/catch_test_macros.hpp>

#include "AllocationLedger.h"
#include "Deadline.h"
#include "FamilyAdapter.h"
#include "FamilyAdapterRegistry.h"
#include "FamilyRouting.h"
#include "GltfFamilyAdapter.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"
#include "ThumbnailPipeline.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    ErrorCode materialsError = ErrorCode::None;
    ErrorCode geometry = ErrorCode::None;
    bool geometryRan = false;
    bool externalReference = false;
    bool usedDraco = false;
    bool usedMeshopt = false;
    std::uint64_t decodedImages = 0;
    std::uint64_t decodedImagePixels = 0;
    std::vector<std::uint32_t> materialIndices;
    std::vector<model_core::MaterialPayload> materials;
};

AdapterResult RunAdapter(GltfAdapter& adapter, BoundedSource& source, Deadline& deadline,
                         AllocationLedger& ledger, IGeometrySink& sink)
{
    AdapterInput input{};
    input.source = &source;
    input.limits = &ProviderLimits::Default();
    input.deadline = &deadline;
    input.ledger = &ledger;
    input.family = Family::Gltf;

    AdapterResult result;
    result.initialize = adapter.Initialize(input);
    if (result.initialize != ErrorCode::None) {
        return result;
    }
    result.parse = adapter.Parse();
    result.externalReference = adapter.ExternalReferenceDetected();
    if (result.parse != ErrorCode::None) {
        return result;
    }
    CollectMaterials materials;
    result.materialsError = adapter.EnumerateMaterials(materials);
    result.materialIndices = materials.indices;
    result.materials = materials.materials;
    result.geometryRan = true;
    result.geometry = adapter.EnumerateGeometry(sink);
    result.usedDraco = adapter.UsedDraco();
    result.usedMeshopt = adapter.UsedMeshopt();
    result.decodedImages = adapter.DecodedImageCount();
    result.decodedImagePixels = adapter.DecodedImagePixels();
    return result;
}

ProviderOutcome RunPipeline(BoundedSource& source, std::uint32_t cx = 64)
{
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Gltf;
    request.source = &source;
    request.limits = &ProviderLimits::Default();
    request.deadline = &deadline;
    request.ledger = &ledger;
    request.cx = cx;
    RasterImage image;
    return RunThumbnailPipeline(request, DefaultThumbnailDependencies(), image);
}

// --- Fixture construction ---------------------------------------------------

void AppendFloat(std::vector<std::byte>& bytes, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const auto* raw = reinterpret_cast<const std::byte*>(&bits);
    bytes.insert(bytes.end(), raw, raw + sizeof(bits));
}

void AppendU32(std::vector<std::byte>& bytes, std::uint32_t value)
{
    const auto* raw = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

// One triangle in [-1,1] plus a material that exercises every product-owned
// material value the adapter must carry.
std::vector<std::byte> TriangleBuffer()
{
    std::vector<std::byte> bin;
    AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f);
    AppendFloat(bin, 1.0f); AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f);
    AppendFloat(bin, 0.0f); AppendFloat(bin, 1.0f); AppendFloat(bin, 0.0f);
    AppendU32(bin, 0);
    AppendU32(bin, 1);
    AppendU32(bin, 2);
    return bin;
}

std::string Base64(const std::vector<std::byte>& data)
{
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t value = (static_cast<std::uint32_t>(data[i]) << 16)
            | (static_cast<std::uint32_t>(data[i + 1]) << 8)
            | static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kTable[(value >> 18) & 0x3F]);
        out.push_back(kTable[(value >> 12) & 0x3F]);
        out.push_back(kTable[(value >> 6) & 0x3F]);
        out.push_back(kTable[value & 0x3F]);
    }
    if (i + 1 == data.size()) {
        const std::uint32_t value = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kTable[(value >> 18) & 0x3F]);
        out.push_back(kTable[(value >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == data.size()) {
        const std::uint32_t value = (static_cast<std::uint32_t>(data[i]) << 16)
            | (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kTable[(value >> 18) & 0x3F]);
        out.push_back(kTable[(value >> 12) & 0x3F]);
        out.push_back(kTable[(value >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

// `extraNodes`/`extraSceneNodes` let a case add an instanced or transformed node.
std::string TriangleJson(const std::string& bufferUri, const std::string& nodes,
                         const std::string& sceneNodes)
{
    return std::string("{") +
        "\"asset\":{\"version\":\"2.0\"},"
        "\"scene\":0,"
        "\"scenes\":[{\"nodes\":[" + sceneNodes + "]}],"
        "\"nodes\":[" + nodes + "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
        "\"indices\":1,\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.1,0.2,0.3,1.0],"
        "\"metallicFactor\":0.4,\"roughnessFactor\":0.5},"
        "\"emissiveFactor\":[0.6,0.0,0.0],\"alphaMode\":\"MASK\",\"alphaCutoff\":0.25,"
        "\"doubleSided\":true,\"extensions\":{\"KHR_materials_unlit\":{}}}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12}],"
        "\"buffers\":[" + bufferUri + "]}";
}

std::vector<std::byte> AsBytes(const std::string& text)
{
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
}

std::vector<std::byte> BuildGlb(const std::string& json, const std::vector<std::byte>& bin)
{
    std::vector<std::byte> jsonChunk = AsBytes(json);
    while (jsonChunk.size() % 4 != 0) {
        jsonChunk.push_back(std::byte{0x20});
    }
    std::vector<std::byte> binChunk = bin;
    while (binChunk.size() % 4 != 0) {
        binChunk.push_back(std::byte{0x00});
    }
    const std::uint32_t total = static_cast<std::uint32_t>(12 + 8 + jsonChunk.size() + 8 + binChunk.size());
    std::vector<std::byte> glb;
    auto appendU32 = [&glb](std::uint32_t value) {
        const auto* raw = reinterpret_cast<const std::byte*>(&value);
        glb.insert(glb.end(), raw, raw + sizeof(value));
    };
    appendU32(0x46546C67u); // "glTF"
    appendU32(2u);
    appendU32(total);
    appendU32(static_cast<std::uint32_t>(jsonChunk.size()));
    appendU32(0x4E4F534Au); // "JSON"
    glb.insert(glb.end(), jsonChunk.begin(), jsonChunk.end());
    appendU32(static_cast<std::uint32_t>(binChunk.size()));
    appendU32(0x004E4942u); // "BIN\0"
    glb.insert(glb.end(), binChunk.begin(), binChunk.end());
    return glb;
}

std::vector<std::byte> BuildTriangleGlb(const std::string& nodes = "{\"mesh\":0}",
                                        const std::string& sceneNodes = "0")
{
    return BuildGlb(TriangleJson("{\"byteLength\":48}", nodes, sceneNodes), TriangleBuffer());
}

std::vector<std::byte> ReadFile(const std::wstring& name);

// A GLB whose BIN chunk embeds a real WebP image (corpus `sample.webp`) that is
// referenced only by an `images` buffer view. The thumbnail does not sample
// textures, but the adapter must still decode the embedded WebP under budget.
std::vector<std::byte> BuildTriangleGlbWithWebp()
{
    const std::vector<std::byte> webp = ReadFile(L"sample.webp");
    if (webp.empty()) {
        return {};
    }
    std::vector<std::byte> bin = TriangleBuffer();
    const std::size_t imageOffset = bin.size();
    bin.insert(bin.end(), webp.begin(), webp.end());
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"images\":[{\"bufferView\":2,\"mimeType\":\"image/webp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(imageOffset)
        + ",\"byteLength\":" + std::to_string(webp.size()) + "}],"
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]}";
    return BuildGlb(json, bin);
}

std::vector<std::byte> BuildTriangleGltfDataUri()
{
    const std::vector<std::byte> bin = TriangleBuffer();
    const std::string uri = "{\"byteLength\":48,\"uri\":\"data:application/octet-stream;base64,"
        + Base64(bin) + "\"}";
    return AsBytes(TriangleJson(uri, "{\"mesh\":0}", "0"));
}

std::vector<std::byte> ReadFile(const std::wstring& name)
{
    const std::filesystem::path path =
        std::filesystem::path(PREVIEW3D_TEST_ASSETS_DIR) / L"corpus" / name;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const std::streamoff length = stream.tellg();
    if (length <= 0) {
        return {};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(bytes.data()), length);
    return bytes;
}

std::string OverBudgetMaterialsJson()
{
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"materials\":[";
    for (std::uint32_t i = 0; i <= ProviderLimits::kMaterialsMax; ++i) {
        if (i != 0) {
            json += ",";
        }
        json += "{}";
    }
    json += "]}";
    return json;
}

} // namespace

// --- Geometry / transforms / materials --------------------------------------

TEST_CASE("an embedded GLB triangle emits one transformed triangle and its material",
          "[provider][gltf]")
{
    ByteSource source(BuildTriangleGlb());
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.initialize == ErrorCode::None);
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.materialsError == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE(result.materialIndices == std::vector<std::uint32_t>{1u});
    REQUIRE(sink.triangles.size() == 1u);
    CHECK(sink.triangles[0].materialIndex == 1u);
    CHECK_FALSE(result.externalReference);

    REQUIRE(result.materials.size() == 1u);
    const model_core::MaterialPayload& material = result.materials[0];
    CHECK(material.baseColorFactor[0] > 0.09f);
    CHECK(material.baseColorFactor[0] < 0.11f);
    CHECK(material.baseColorFactor[1] > 0.19f);
    CHECK(material.baseColorFactor[1] < 0.21f);
    CHECK(material.baseColorFactor[2] > 0.29f);
    CHECK(material.baseColorFactor[2] < 0.31f);
    CHECK(material.metallicFactor > 0.39f);
    CHECK(material.metallicFactor < 0.41f);
    CHECK(material.roughnessFactor > 0.49f);
    CHECK(material.roughnessFactor < 0.51f);
    CHECK(material.emissiveFactor[0] > 0.59f);
    CHECK(material.alphaMode == static_cast<std::uint32_t>(model_core::AlphaModeId::Mask));
    CHECK(material.alphaCutoff > 0.24f);
    CHECK(material.alphaCutoff < 0.26f);
    CHECK((material.flags & model_core::kMaterialFlagDoubleSided) != 0u);
    CHECK((material.flags & model_core::kMaterialFlagUnlit) != 0u);
}

TEST_CASE("an instanced glTF emits each node instance with its world transform",
          "[provider][gltf]")
{
    ByteSource source(BuildTriangleGlb(
        "{\"mesh\":0},{\"mesh\":0,\"translation\":[10.0,0.0,0.0]}", "0,1"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE(sink.triangles.size() == 2u);
    // The two instances share one per-call origin, so their separation, not the
    // absolute coordinate, is what proves the per-instance transform.
    float minX = 1.0e9f;
    float maxX = -1.0e9f;
    for (const TriangleSample& triangle : sink.triangles) {
        minX = (std::min)(minX, triangle.vertices[0].position[0]);
        maxX = (std::max)(maxX, triangle.vertices[0].position[0]);
    }
    CHECK(maxX - minX > 9.0f);
}

TEST_CASE("a .gltf with an embedded data URI renders", "[provider][gltf]")
{
    ByteSource source(BuildTriangleGltfDataUri());
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.size() == 1u);
}

TEST_CASE("a non-contiguous glTF source parses through the bounded backing buffer",
          "[provider][gltf]")
{
    ByteSource source(BuildTriangleGlb(), /*exposeContiguous=*/false);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.size() == 1u);
}

// --- Compressed corpus ------------------------------------------------------

TEST_CASE("a Draco-compressed GLB decodes through the bounded Draco path", "[provider][gltf]")
{
    const std::vector<std::byte> bytes = ReadFile(L"draco_triangle.glb");
    REQUIRE_FALSE(bytes.empty());
    ByteSource source(bytes);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(result.usedDraco);
    CHECK_FALSE(sink.triangles.empty());
}

TEST_CASE("a meshopt-compressed GLB decodes EXT_meshopt_compression buffer views",
          "[provider][gltf]")
{
    const std::vector<std::byte> bytes = ReadFile(L"meshopt.glb");
    REQUIRE_FALSE(bytes.empty());
    ByteSource source(bytes);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(result.usedMeshopt);
    CHECK_FALSE(sink.triangles.empty());
}

TEST_CASE("a KTX2/Basis-textured GLB decodes the embedded image under budget",
          "[provider][gltf]")
{
    const std::vector<std::byte> bytes = ReadFile(L"basisu_textured_triangle.glb");
    REQUIRE_FALSE(bytes.empty());
    ByteSource source(bytes);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
    CHECK(result.decodedImages >= 1u);
    CHECK(result.decodedImagePixels > 0u);
}

TEST_CASE("a WebP-textured glTF decodes the embedded image under budget", "[provider][gltf]")
{
    const std::vector<std::byte> bytes = BuildTriangleGlbWithWebp();
    REQUIRE_FALSE(bytes.empty());
    ByteSource source(bytes);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
    CHECK(result.decodedImages >= 1u);
    CHECK(result.decodedImagePixels > 0u);
}

TEST_CASE("a corrupt embedded KTX2 image falls back to the default material",
          "[provider][gltf]")
{
    const std::vector<std::byte> bytes = ReadFile(L"basisu_corrupt_ktx2.glb");
    REQUIRE_FALSE(bytes.empty());
    ByteSource source(bytes);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    // Valid geometry survives; only the optional image is dropped.
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
    CHECK(result.decodedImages == 0u);
}

// --- Isolation / typed failures ---------------------------------------------

TEST_CASE("a sidecar-dependent glTF fails closed and never resolves a path",
          "[provider][gltf]")
{
    const std::vector<std::byte> bytes = ReadFile(L"sidecar-missing.gltf");
    REQUIRE_FALSE(bytes.empty());
    ByteSource source(bytes);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.externalReference);
    CHECK(result.parse == ErrorCode::UnsafeReference);
    CHECK(sink.triangles.empty());
}

TEST_CASE("an external-image glTF is also rejected as an unsafe reference",
          "[provider][gltf]")
{
    const std::vector<std::byte> bytes = ReadFile(L"sidecar-approved.gltf");
    REQUIRE_FALSE(bytes.empty());
    ByteSource source(bytes);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.externalReference);
    CHECK(result.parse == ErrorCode::UnsafeReference);
}

TEST_CASE("a truncated GLB is a typed malformed-data failure", "[provider][gltf]")
{
    const std::vector<std::byte> bytes = ReadFile(L"truncated.glb");
    REQUIRE_FALSE(bytes.empty());
    ByteSource source(bytes);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse != ErrorCode::None);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a glTF over the material cap is a resource limit", "[provider][gltf]")
{
    ByteSource source(AsBytes(OverBudgetMaterialsJson()));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::ResourceLimit);
    CHECK(sink.triangles.empty());
}

TEST_CASE("an expired deadline stops the parse with a typed outcome", "[provider][gltf]")
{
    ByteSource source(BuildTriangleGlb());
    Deadline deadline(Deadline::Clock::now() - std::chrono::seconds(10));
    AllocationLedger ledger;
    CollectSink sink;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::Cancelled);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a glTF sink cap stop ends enumeration without failing", "[provider][gltf]")
{
    ByteSource source(BuildTriangleGlb());
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;
    sink.triangleLimit = 0;

    GltfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.empty());
}

// --- Routing / rendering ----------------------------------------------------

TEST_CASE("the routed pipeline maps a sidecar glTF to a generic-icon outcome",
          "[provider][gltf]")
{
    const std::vector<std::byte> bytes = ReadFile(L"sidecar-missing.gltf");
    REQUIRE_FALSE(bytes.empty());
    ByteSource source(bytes);
    CHECK(RunPipeline(source) == ProviderOutcome::Unsupported);
}

TEST_CASE("a valid glTF renders a non-empty bitmap through the real dependencies",
          "[provider][gltf]")
{
    ByteSource source(BuildTriangleGlb());
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Gltf;
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

TEST_CASE("a minimal-material glTF renders at 256 px through the real dependencies",
          "[provider][gltf]")
{
    std::vector<std::byte> bin;
    AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f);
    AppendFloat(bin, 2.0f); AppendFloat(bin, 0.0f); AppendFloat(bin, 0.0f);
    AppendFloat(bin, 0.0f); AppendFloat(bin, 2.0f); AppendFloat(bin, 0.0f);
    AppendU32(bin, 0); AppendU32(bin, 1); AppendU32(bin, 2);
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
        "\"indices\":1,\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.2,0.6,0.9,1.0]},"
        "\"doubleSided\":true}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"min\":[0,0,0],\"max\":[2,2,0]},"
        "{\"bufferView\":1,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12}],"
        "\"buffers\":[{\"byteLength\":48}]}";
    ByteSource source(BuildGlb(json, bin));
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Gltf;
    request.source = &source;
    request.limits = &ProviderLimits::Default();
    request.deadline = &deadline;
    request.ledger = &ledger;
    request.cx = 256;
    RasterImage image;
    const ProviderOutcome outcome =
        RunThumbnailPipeline(request, DefaultThumbnailDependencies(), image);
    CHECK(outcome == ProviderOutcome::Success);
    CHECK(image.width != 0u);
    CHECK(image.bgraPremultiplied.size() ==
          static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u);
}

TEST_CASE("glTF is routed from its frozen CLSID", "[provider][gltf]")
{
    CHECK(FamilyForClsid("{A592F425-EA68-4C88-BB96-020805D4BE56}") == Family::Gltf);
    CHECK(CreateFamilyAdapter(Family::Gltf) != nullptr);
}
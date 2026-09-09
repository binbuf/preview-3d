#include "StlAdapter.h"

#include "AsciiTokenizer.h"
#include "model_core/Checksum.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/CheckedMath.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

namespace import_worker {

namespace {

using namespace model_core;
using platform::CheckedAdd;
using platform::CheckedMultiply;

constexpr size_t kStlHeaderBytes = 80;
constexpr size_t kStlCountBytes = 4;
constexpr size_t kStlFacetBytes = 50; // 12 (normal) + 36 (3 verts) + 2 (attribute count, unused)
constexpr size_t kStlPrefixBytes = kStlHeaderBytes + kStlCountBytes; // 84

// No full Tier-A hard-limits table enforcement yet (.docs/design/03-file-formats-and-ingestion.md)
// -- a later refinement once real large fixtures are being tested, same
// simplification GltfAdapter.cpp's kMaxVertices/kMaxIndices already made.
constexpr uint32_t kMaxFacets = 2'000'000;

// Bounds how many tokens after "solid" are skipped looking for the first
// "facet"/"endsolid" keyword -- the free-form solid name can be empty,
// one word, or several; this just stops a hostile file with neither
// keyword from scanning forever.
constexpr int kMaxSolidNameTokens = 256;

float ReadFloatLE(const std::byte* p)
{
    float value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

uint32_t ReadU32LE(const std::byte* p)
{
    uint32_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3 operator-(const Vec3& other) const { return { x - other.x, y - other.y, z - other.z }; }
};

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

float Dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool IsFinite(const Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Shared per-facet validation for both the binary and ASCII bodies: a
// non-finite facet is dropped; a degenerate (near-zero-area) facet is
// dropped; a supplied normal within ~10% of unit length is trusted (and
// re-normalized), otherwise the generated flat (cross-product) normal is
// used -- the design doc's "supplied normals or generated flat normals"
// wording. Appends nothing when the facet is dropped.
void ProcessFacet(const Vec3& suppliedNormal, const Vec3& v0, const Vec3& v1, const Vec3& v2,
                   std::vector<VertexPositionNormalUv0F32>& vertices, std::vector<uint32_t>& indices)
{
    if (!IsFinite(suppliedNormal) || !IsFinite(v0) || !IsFinite(v1) || !IsFinite(v2)) {
        return; // non-finite facet: dropped
    }

    Vec3 flatNormal = Cross(v1 - v0, v2 - v0);
    float flatLengthSquared = Dot(flatNormal, flatNormal);
    if (flatLengthSquared <= 1e-12f) {
        return; // degenerate (near-zero-area) facet: dropped
    }
    float flatInvLength = 1.0f / std::sqrt(flatLengthSquared);
    Vec3 flatNormalized{ flatNormal.x * flatInvLength, flatNormal.y * flatInvLength,
                          flatNormal.z * flatInvLength };

    float suppliedLengthSquared = Dot(suppliedNormal, suppliedNormal);
    Vec3 normal;
    if (suppliedLengthSquared > 0.81f && suppliedLengthSquared < 1.21f) {
        float invLength = 1.0f / std::sqrt(suppliedLengthSquared);
        normal = { suppliedNormal.x * invLength, suppliedNormal.y * invLength,
                   suppliedNormal.z * invLength };
    } else {
        normal = flatNormalized;
    }

    uint32_t baseIndex = static_cast<uint32_t>(vertices.size());
    for (const Vec3& v : { v0, v1, v2 }) {
        VertexPositionNormalUv0F32 vertex{};
        vertex.px = v.x;
        vertex.py = v.y;
        vertex.pz = v.z;
        vertex.nx = normal.x;
        vertex.ny = normal.y;
        vertex.nz = normal.z;
        vertex.u = 0.0f;
        vertex.v = 0.0f;
        vertices.push_back(vertex);
    }
    indices.push_back(baseIndex);
    indices.push_back(baseIndex + 1);
    indices.push_back(baseIndex + 2);
}

// Shared wire-format write, once facet parsing (binary or ASCII) has
// produced a vertex/index list. Mirrors GltfAdapter.cpp/SyntheticSceneGenerator's
// "compute everything, check once, then write sequentially, header last"
// structure. Always exactly one chunk.
std::variant<StlImportResult, ImportErrorCode> WriteStlChunk(
    const std::vector<VertexPositionNormalUv0F32>& vertices, const std::vector<uint32_t>& indices,
    std::span<std::byte> destination, uint64_t generationId)
{
    if (vertices.empty()) {
        return ImportErrorCode::MalformedData; // every facet was dropped
    }

    uint64_t vertexBytes = static_cast<uint64_t>(vertices.size()) * sizeof(VertexPositionNormalUv0F32);
    uint64_t indexBytes = static_cast<uint64_t>(indices.size()) * sizeof(uint32_t);
    auto payloadSizeOpt = CheckedAdd(vertexBytes, indexBytes);
    if (!payloadSizeOpt) {
        return ImportErrorCode::ResourceLimit;
    }
    uint64_t headerAndTable = static_cast<uint64_t>(kSectionHeaderSize) + kChunkDescriptorSize;
    auto sectionLengthOpt = CheckedAdd(headerAndTable, *payloadSizeOpt);
    if (!sectionLengthOpt) {
        return ImportErrorCode::ResourceLimit;
    }
    uint64_t payloadOffset = headerAndTable;
    uint64_t payloadSize = *payloadSizeOpt;
    uint64_t sectionLength = *sectionLengthOpt;

    if (sectionLength > destination.size()) {
        return ImportErrorCode::ResourceLimit;
    }

    std::memcpy(destination.data() + payloadOffset, vertices.data(), vertexBytes);
    std::memcpy(destination.data() + payloadOffset + vertexBytes, indices.data(), indexBytes);

    ChunkDescriptor descriptor{};
    descriptor.sourceRangeOffset = 0;
    descriptor.sourceRangeLength = 0;
    descriptor.normalizedRangeOffset = payloadOffset;
    descriptor.normalizedRangeLength = payloadSize;
    descriptor.topology = ChunkTopology::TriangleList;
    descriptor.indexCount = static_cast<uint32_t>(indices.size());
    descriptor.vertexCount = static_cast<uint32_t>(vertices.size());
    descriptor.vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionNormalUv0_F32);
    descriptor.lodLevel = 0;
    descriptor.chunkId = 1;
    descriptor.byteSize = payloadSize;
    descriptor.dependencyCount = 0;
    descriptor.chunkChecksum = Fnv1a64(destination.subspan(payloadOffset, payloadSize));

    std::memcpy(destination.data() + kSectionHeaderSize, &descriptor, sizeof(descriptor));

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = 1;
    header.reserved = 0;
    header.sectionChecksum
        = Fnv1a64(destination.subspan(kSectionHeaderSize, sectionLength - kSectionHeaderSize));

    std::memcpy(destination.data(), &header, sizeof(header));

    StlImportResult result;
    result.chunkCount = 1;
    result.sectionBytesWritten = sectionLength;
    return result;
}

std::variant<StlImportResult, ImportErrorCode> ImportStlBinary(std::span<const std::byte> sourceStlBytes,
                                                                  std::span<std::byte> destination,
                                                                  uint64_t generationId)
{
    if (sourceStlBytes.size() < kStlPrefixBytes) {
        return ImportErrorCode::MalformedData;
    }

    uint32_t triangleCount = ReadU32LE(sourceStlBytes.data() + kStlHeaderBytes);
    if (triangleCount > kMaxFacets) {
        return ImportErrorCode::ResourceLimit;
    }

    auto facetsBytesOpt
        = CheckedMultiply(static_cast<uint64_t>(triangleCount), static_cast<uint64_t>(kStlFacetBytes));
    auto expectedMinSizeOpt = facetsBytesOpt
        ? CheckedAdd(static_cast<uint64_t>(kStlPrefixBytes), *facetsBytesOpt)
        : std::nullopt;
    if (!facetsBytesOpt || !expectedMinSizeOpt) {
        return ImportErrorCode::ResourceLimit;
    }
    // Trailing bytes beyond the declared facet table are tolerated, per
    // the design doc -- only a truncated (too-short) file is rejected.
    if (sourceStlBytes.size() < *expectedMinSizeOpt) {
        return ImportErrorCode::MalformedData;
    }

    std::vector<VertexPositionNormalUv0F32> vertices;
    vertices.reserve(static_cast<size_t>(triangleCount) * 3);
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(triangleCount) * 3);

    const std::byte* facetBase = sourceStlBytes.data() + kStlPrefixBytes;
    for (uint32_t i = 0; i < triangleCount; ++i) {
        const std::byte* facet = facetBase + static_cast<size_t>(i) * kStlFacetBytes;

        Vec3 suppliedNormal{ ReadFloatLE(facet + 0), ReadFloatLE(facet + 4), ReadFloatLE(facet + 8) };
        Vec3 v0{ ReadFloatLE(facet + 12), ReadFloatLE(facet + 16), ReadFloatLE(facet + 20) };
        Vec3 v1{ ReadFloatLE(facet + 24), ReadFloatLE(facet + 28), ReadFloatLE(facet + 32) };
        Vec3 v2{ ReadFloatLE(facet + 36), ReadFloatLE(facet + 40), ReadFloatLE(facet + 44) };

        ProcessFacet(suppliedNormal, v0, v1, v2, vertices, indices);
    }

    return WriteStlChunk(vertices, indices, destination, generationId);
}

std::variant<StlImportResult, ImportErrorCode> ImportStlAscii(std::span<const std::byte> sourceStlBytes,
                                                                 std::span<std::byte> destination,
                                                                 uint64_t generationId)
{
    AsciiTokenizer tokenizer(sourceStlBytes);

    auto expectKeyword = [&](std::string_view keyword) -> bool {
        auto token = tokenizer.NextToken();
        return token.has_value() && *token == keyword;
    };

    if (!expectKeyword("solid")) {
        return ImportErrorCode::MalformedData;
    }

    // Skip the free-form solid name (zero or more tokens) up to the first
    // "facet" or "endsolid" keyword.
    std::optional<std::string_view> next;
    for (int skipped = 0;; ++skipped) {
        next = tokenizer.NextToken();
        if (!next) {
            return ImportErrorCode::MalformedData; // ran out before facet/endsolid
        }
        if (*next == "facet" || *next == "endsolid") {
            break;
        }
        if (skipped >= kMaxSolidNameTokens) {
            return ImportErrorCode::MalformedData;
        }
    }

    std::vector<VertexPositionNormalUv0F32> vertices;
    std::vector<uint32_t> indices;
    uint32_t facetCount = 0;

    while (next && *next == "facet") {
        if (facetCount >= kMaxFacets) {
            return ImportErrorCode::ResourceLimit;
        }
        ++facetCount;

        if (!expectKeyword("normal")) {
            return ImportErrorCode::MalformedData;
        }
        auto nx = tokenizer.NextNumber();
        auto ny = tokenizer.NextNumber();
        auto nz = tokenizer.NextNumber();
        if (!nx || !ny || !nz) {
            return ImportErrorCode::MalformedData;
        }
        Vec3 suppliedNormal{ static_cast<float>(*nx), static_cast<float>(*ny), static_cast<float>(*nz) };

        if (!expectKeyword("outer") || !expectKeyword("loop")) {
            return ImportErrorCode::MalformedData;
        }

        Vec3 verts[3];
        for (Vec3& vert : verts) {
            if (!expectKeyword("vertex")) {
                return ImportErrorCode::MalformedData;
            }
            auto x = tokenizer.NextNumber();
            auto y = tokenizer.NextNumber();
            auto z = tokenizer.NextNumber();
            if (!x || !y || !z) {
                return ImportErrorCode::MalformedData;
            }
            vert = Vec3{ static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z) };
        }

        if (!expectKeyword("endloop") || !expectKeyword("endfacet")) {
            return ImportErrorCode::MalformedData;
        }

        ProcessFacet(suppliedNormal, verts[0], verts[1], verts[2], vertices, indices);

        next = tokenizer.NextToken();
        if (!next) {
            return ImportErrorCode::MalformedData; // ran out before endsolid
        }
    }

    if (*next != "endsolid") {
        return ImportErrorCode::MalformedData;
    }
    // Any trailing solid-name tokens after "endsolid" are ignored.

    return WriteStlChunk(vertices, indices, destination, generationId);
}

} // namespace

std::variant<StlImportResult, ImportErrorCode> ImportStl(std::span<const std::byte> sourceStlBytes,
                                                            std::span<std::byte> destination,
                                                            uint64_t generationId,
                                                            uint32_t maxChunkCount)
{
    if (maxChunkCount < 1) {
        return ImportErrorCode::ResourceLimit;
    }

    // A file is treated as binary whenever its declared triangle count and
    // actual length are consistent with the binary layout (trailing bytes
    // tolerated) -- even if it also starts with "solid" -- and as ASCII
    // only when it isn't binary-shaped but does start with "solid".
    // Anything else falls through to ImportStlBinary, which reproduces the
    // exact MalformedData/ResourceLimit outcomes this function already
    // returned before ASCII support existed.
    bool isBinaryShape = false;
    if (sourceStlBytes.size() >= kStlPrefixBytes) {
        uint32_t triangleCount = ReadU32LE(sourceStlBytes.data() + kStlHeaderBytes);
        if (triangleCount <= kMaxFacets) {
            auto facetsBytesOpt = CheckedMultiply(static_cast<uint64_t>(triangleCount),
                                                   static_cast<uint64_t>(kStlFacetBytes));
            auto expectedMinSizeOpt = facetsBytesOpt
                ? CheckedAdd(static_cast<uint64_t>(kStlPrefixBytes), *facetsBytesOpt)
                : std::nullopt;
            if (facetsBytesOpt && expectedMinSizeOpt && sourceStlBytes.size() >= *expectedMinSizeOpt) {
                isBinaryShape = true;
            }
        }
    }

    constexpr std::string_view kAsciiKeyword = "solid";
    bool looksAscii = !isBinaryShape && sourceStlBytes.size() >= kAsciiKeyword.size()
        && std::string_view(reinterpret_cast<const char*>(sourceStlBytes.data()), kAsciiKeyword.size())
            == kAsciiKeyword;

    if (looksAscii) {
        return ImportStlAscii(sourceStlBytes, destination, generationId);
    }
    return ImportStlBinary(sourceStlBytes, destination, generationId);
}

} // namespace import_worker

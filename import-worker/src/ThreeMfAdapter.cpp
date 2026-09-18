#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ThreeMfAdapter.h"

#include "BoundedChunkWriter.h"
#include "ChunkBatchSink.h"
#include "model_core/GeometryBounds.h"
#include "model_core/TierALimits.h"
#include "model_core/VertexLayouts.h"

#include <Bindings/Cpp/lib3mf_implicit.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace import_worker {
namespace {

using namespace model_core;

constexpr uint32_t kChunkTriangles = 65'536; // 13.1 MiB deindexed full vertices + indices.

ThreeMfImportOutcome Fail(ImportErrorCode code)
{
    return ThreeMfImportFailure{code, ImportFailurePhase::Geometry};
}

bool Finite(double value)
{
    return std::isfinite(value) && std::abs(value) <= 1.0e30;
}

void Identity(double result[16])
{
    std::fill(result, result + 16, 0.0);
    result[0] = result[5] = result[10] = result[15] = 1.0;
}

bool ToWire(const Lib3MF::sTransform& source, double result[16])
{
    Identity(result);
    for (uint32_t row = 0; row < 4; ++row)
        for (uint32_t column = 0; column < 3; ++column) {
            result[row * 4 + column] = source.m_Fields[row][column];
            if (!Finite(result[row * 4 + column])) return false;
        }
    return true;
}

bool ValidMatrix(const double matrix[16])
{
    for (uint32_t index = 0; index < 16; ++index)
        if (!Finite(matrix[index])) return false;
    if (matrix[3] != 0.0 || matrix[7] != 0.0 || matrix[11] != 0.0 || matrix[15] != 1.0)
        return false;
    const double determinant =
        matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9])
        - matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8])
        + matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
    return Finite(determinant) && std::abs(determinant) >= 1.0e-18;
}

bool Multiply(const double left[16], const double right[16], double result[16])
{
    double scratch[16]{};
    for (uint32_t row = 0; row < 4; ++row)
        for (uint32_t column = 0; column < 4; ++column)
            for (uint32_t item = 0; item < 4; ++item)
                scratch[row * 4 + column] += left[row * 4 + item] * right[item * 4 + column];
    if (!ValidMatrix(scratch)) return false;
    std::copy(std::begin(scratch), std::end(scratch), result);
    return true;
}

bool TransformBounds(const ChunkDescriptor& geometry, const double world[16],
                     double minimum[3], double maximum[3])
{
    std::fill(minimum, minimum + 3, (std::numeric_limits<double>::max)());
    std::fill(maximum, maximum + 3, -(std::numeric_limits<double>::max)());
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const double point[3]{
            geometry.origin[0] + (corner & 1 ? geometry.localMax[0] : geometry.localMin[0]),
            geometry.origin[1] + (corner & 2 ? geometry.localMax[1] : geometry.localMin[1]),
            geometry.origin[2] + (corner & 4 ? geometry.localMax[2] : geometry.localMin[2]),
        };
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const double value = point[0] * world[axis] + point[1] * world[4 + axis]
                + point[2] * world[8 + axis] + world[12 + axis];
            if (!Finite(value)) return false;
            minimum[axis] = (std::min)(minimum[axis], value);
            maximum[axis] = (std::max)(maximum[axis], value);
        }
    }
    return true;
}

double MetersPerUnit(Lib3MF::eModelUnit unit)
{
    switch (unit) {
    case Lib3MF::eModelUnit::MicroMeter: return 1.0e-6;
    case Lib3MF::eModelUnit::MilliMeter: return 1.0e-3;
    case Lib3MF::eModelUnit::CentiMeter: return 1.0e-2;
    case Lib3MF::eModelUnit::Inch: return 0.0254;
    case Lib3MF::eModelUnit::Foot: return 0.3048;
    case Lib3MF::eModelUnit::Meter: return 1.0;
    default: return 0.0;
    }
}

bool AllowedType(Lib3MF::eObjectType type)
{
    return type == Lib3MF::eObjectType::Model || type == Lib3MF::eObjectType::Support
        || type == Lib3MF::eObjectType::SolidSupport || type == Lib3MF::eObjectType::Surface;
}

struct Occurrence {
    Lib3MF::PMeshObject mesh;
    uint32_t meshKey = 0;
    double transform[16]{};
};

struct Traversal {
    const ThreeMfImportOptions& options;
    std::vector<Occurrence> occurrences;
    std::unordered_map<uint32_t, Lib3MF::PMeshObject> meshes;
    std::vector<uint32_t> meshOrder;
    std::unordered_set<uint32_t> recursion;
    ImportErrorCode error = ImportErrorCode::None;

    bool Visit(const Lib3MF::PObject& object, const double parent[16], uint32_t depth)
    {
        if (options.Cancelled()) { error = ImportErrorCode::Cancelled; return false; }
        if (!object || depth > kMaxSceneHierarchyDepth) { error = ImportErrorCode::ResourceLimit; return false; }
        if (!AllowedType(object->GetType())) { error = ImportErrorCode::UnsupportedRequiredFeature; return false; }
        const uint32_t key = object->GetUniqueResourceID();
        if (!key) { error = ImportErrorCode::MalformedData; return false; }
        if (object->IsMeshObject()) {
            auto mesh = std::dynamic_pointer_cast<Lib3MF::CMeshObject>(object);
            if (!mesh) { error = ImportErrorCode::MalformedData; return false; }
            if (occurrences.size() >= kTierBObjectLimit) { error = ImportErrorCode::ResourceLimit; return false; }
            if (!meshes.contains(key)) meshOrder.push_back(key);
            meshes.emplace(key, mesh);
            Occurrence occurrence{};
            occurrence.mesh = std::move(mesh); occurrence.meshKey = key;
            std::copy(parent, parent + 16, occurrence.transform);
            occurrences.push_back(std::move(occurrence));
            return true;
        }
        if (!object->IsComponentsObject() || !recursion.insert(key).second) {
            error = object->IsComponentsObject() ? ImportErrorCode::MalformedData
                                                 : ImportErrorCode::UnsupportedRequiredFeature;
            return false;
        }
        auto components = std::dynamic_pointer_cast<Lib3MF::CComponentsObject>(object);
        if (!components) { error = ImportErrorCode::MalformedData; return false; }
        const uint32_t count = components->GetComponentCount();
        if (!count || count > kTierBObjectLimit) { error = ImportErrorCode::ResourceLimit; return false; }
        for (uint32_t index = 0; index < count; ++index) {
            if (options.Cancelled()) { error = ImportErrorCode::Cancelled; recursion.erase(key); return false; }
            const auto component = components->GetComponent(index);
            if (!component || !component->GetObjectResource()) { error = ImportErrorCode::MalformedData; recursion.erase(key); return false; }
            double local[16]{};
            if (component->HasTransform()) {
                if (!ToWire(component->GetTransform(), local) || !ValidMatrix(local)) {
                    error = ImportErrorCode::MalformedData; recursion.erase(key); return false;
                }
            } else Identity(local);
            double world[16]{};
            if (!Multiply(parent, local, world)
                || !Visit(component->GetObjectResource(), world, depth + 1)) { recursion.erase(key); return false; }
        }
        recursion.erase(key);
        return true;
    }
};

struct GeometryRecord { uint32_t chunkId = 0; ChunkDescriptor descriptor{}; };

bool EmitMesh(BoundedChunkWriter& writer, const Lib3MF::PMeshObject& mesh, uint32_t meshKey,
              uint32_t meshOrdinal,
              const ThreeMfImportOptions& options, std::vector<GeometryRecord>& output,
              uint64_t& totalTriangles, uint64_t& totalVertices)
{
    if (options.Cancelled()) return false;
    const uint32_t vertexCount = mesh->GetVertexCount();
    const uint32_t triangleCount = mesh->GetTriangleCount();
    if (!vertexCount || !triangleCount || vertexCount > kTierBVertexLimit
        || triangleCount > kTierBTriangleLimit) return false;
    std::vector<Lib3MF::sPosition> positions;
    std::vector<Lib3MF::sTriangle> triangles;
    mesh->GetVertices(positions); mesh->GetTriangleIndices(triangles);
    if (positions.size() != vertexCount || triangles.size() != triangleCount) return false;
    for (const auto& position : positions)
        for (float coordinate : position.m_Coordinates)
            if (!Finite(coordinate)) return false;
    for (const auto& triangle : triangles)
        for (uint32_t index : triangle.m_Indices)
            if (index >= positions.size()) return false;

    for (uint32_t start = 0; start < triangleCount; start += kChunkTriangles) {
        if (options.Cancelled()) return false;
        const uint32_t count = (std::min)(kChunkTriangles, triangleCount - start);
        if (totalTriangles > kTierBTriangleLimit - count || totalVertices > kTierBVertexLimit - uint64_t(count) * 3)
            return false;
        std::vector<VertexPositionNormalUv0F32> vertices(size_t(count) * 3);
        std::vector<uint32_t> indices(size_t(count) * 3);
        for (uint32_t triangleIndex = 0; triangleIndex < count; ++triangleIndex) {
            const auto& triangle = triangles[start + triangleIndex];
            float point[3][3]{};
            for (uint32_t corner = 0; corner < 3; ++corner) {
                const auto& source = positions[triangle.m_Indices[corner]];
                std::copy(std::begin(source.m_Coordinates), std::end(source.m_Coordinates), point[corner]);
            }
            const float ax = point[1][0] - point[0][0], ay = point[1][1] - point[0][1], az = point[1][2] - point[0][2];
            const float bx = point[2][0] - point[0][0], by = point[2][1] - point[0][1], bz = point[2][2] - point[0][2];
            float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
            const double length = std::sqrt(double(nx) * nx + double(ny) * ny + double(nz) * nz);
            if (Finite(length) && length > 1.0e-20) { nx = float(nx / length); ny = float(ny / length); nz = float(nz / length); }
            else { nx = 0.0f; ny = 0.0f; nz = 1.0f; }
            for (uint32_t corner = 0; corner < 3; ++corner) {
                auto& vertex = vertices[size_t(triangleIndex) * 3 + corner];
                vertex.px = point[corner][0]; vertex.py = point[corner][1]; vertex.pz = point[corner][2];
                vertex.nx = nx; vertex.ny = ny; vertex.nz = nz;
                indices[size_t(triangleIndex) * 3 + corner] = triangleIndex * 3 + corner;
            }
        }
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::TriangleList;
        descriptor.chunkId = writer.NextId();
        descriptor.vertexCount = count * 3; descriptor.indexCount = count * 3;
        descriptor.vertexLayoutId = uint32_t(VertexLayoutId::PositionNormalUv0_F32);
        descriptor.meshId = meshOrdinal;
        descriptor.sourceRangeOffset = (uint64_t(meshKey) << 32) | (uint64_t(start) * 3);
        descriptor.sourceRangeLength = uint64_t(count) * 3;
        descriptor.geometryFlags = kGeometryDeindexed | kGeometryReusableInstanceSource;
        if (!SetLocalBounds(descriptor, ChunkBytes(vertices)) || !writer.Add(descriptor, ChunkBytes(vertices), ChunkBytes(indices))) return false;
        output.push_back({descriptor.chunkId, descriptor});
        totalTriangles += count; totalVertices += uint64_t(count) * 3;
    }
    return true;
}

} // namespace

ThreeMfImportOutcome ImportThreeMf(const Lib3MF::PModel& model, std::span<std::byte> destination,
                                   uint64_t generationId, uint32_t maxChunkCount,
                                   ChunkBatchSink* batchSink, const ThreeMfImportOptions& options)
{
    if (!model || destination.size() <= kSectionHeaderSize + kChunkDescriptorSize) return Fail(ImportErrorCode::ResourceLimit);
    if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
    const double meters = MetersPerUnit(model->GetUnit());
    if (!Finite(meters) || meters <= 0.0) return Fail(ImportErrorCode::MalformedData);
    auto build = model->GetBuildItems();
    if (!build || !build->Count() || build->Count() > kTierBObjectLimit) return Fail(ImportErrorCode::EmptyGeometry);

    Traversal traversal{options};
    for (uint64_t index = 0; index < build->Count(); ++index) {
        if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
        if (!build->MoveNext()) return Fail(ImportErrorCode::MalformedData);
        const auto item = build->GetCurrent();
        if (!item || !item->GetObjectResource()) return Fail(ImportErrorCode::MalformedData);
        double placement[16]{};
        if (item->HasObjectTransform()) {
            if (!ToWire(item->GetObjectTransform(), placement) || !ValidMatrix(placement)) return Fail(ImportErrorCode::MalformedData);
        } else Identity(placement);
        if (!traversal.Visit(item->GetObjectResource(), placement, 1)) return Fail(traversal.error);
    }
    if (traversal.occurrences.empty() || traversal.meshes.empty()) return Fail(ImportErrorCode::EmptyGeometry);

    SceneMetadata metadata{};
    metadata.generationId = generationId; metadata.format = SourceFormatId::ThreeMf;
    metadata.upAxis = UpAxisId::Z; metadata.metersPerUnit = meters;
    metadata.meshCount = uint32_t(traversal.meshes.size());
    metadata.nodeCount = uint32_t(traversal.occurrences.size());
    BoundedChunkWriter writer(destination, generationId, maxChunkCount, metadata, batchSink);
    std::unordered_map<uint32_t, std::vector<GeometryRecord>> geometry;
    uint64_t triangles = 0, vertices = 0;
    uint32_t meshOrdinal = 0;
    for (uint32_t key : traversal.meshOrder) {
        const auto mesh = traversal.meshes.at(key);
        auto& records = geometry[key];
        if (!EmitMesh(writer, mesh, key, ++meshOrdinal, options, records, triangles, vertices)) {
            const ImportErrorCode code = options.Cancelled() ? ImportErrorCode::Cancelled
                : writer.Error() == ImportErrorCode::None ? ImportErrorCode::MalformedData : writer.Error();
            return Fail(code);
        }
    }
    if (!triangles) return Fail(ImportErrorCode::EmptyGeometry);

    std::vector<uint32_t> nodeIds; nodeIds.reserve(traversal.occurrences.size());
    for (const auto& occurrence : traversal.occurrences) {
        if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
        NodePayload node{}; node.nodeId = writer.NextId(); node.flags = kSceneRecordVisible;
        std::copy(std::begin(occurrence.transform), std::end(occurrence.transform), node.localTransform);
        if (!ValidMatrix(node.localTransform) || !writer.AddNode(node)) return Fail(options.Cancelled() ? ImportErrorCode::Cancelled : writer.Error());
        nodeIds.push_back(node.nodeId);
    }
    uint64_t instances = 0;
    for (size_t occurrenceIndex = 0; occurrenceIndex < traversal.occurrences.size(); ++occurrenceIndex) {
        const auto& occurrence = traversal.occurrences[occurrenceIndex];
        const auto found = geometry.find(occurrence.meshKey);
        if (found == geometry.end()) return Fail(ImportErrorCode::MalformedData);
        for (const auto& record : found->second) {
            if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
            if (++instances > kTierBObjectLimit) return Fail(ImportErrorCode::ResourceLimit);
            MeshInstancePayload instance{};
            instance.instanceId = writer.NextId(); instance.nodeId = nodeIds[occurrenceIndex];
            instance.geometryChunkId = record.chunkId; instance.flags = kSceneRecordVisible;
            if (!TransformBounds(record.descriptor, occurrence.transform, instance.worldMin, instance.worldMax)
                || !writer.AddInstance(instance)) return Fail(options.Cancelled() ? ImportErrorCode::Cancelled : writer.Error());
        }
    }
    if (!writer.Complete()) return Fail(writer.Error());
    return ThreeMfImportResult{writer.Count(), writer.Length()};
}

} // namespace import_worker

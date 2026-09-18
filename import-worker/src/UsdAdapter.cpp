#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "UsdAdapter.h"

#include "BoundedChunkWriter.h"
#include "model_core/GeometryBounds.h"
#include "model_core/TierALimits.h"
#include "model_core/VertexLayouts.h"

#include "tinyusdz.hh"
#include "tydra/render-data.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace import_worker {
namespace {

using namespace model_core;
using tinyusdz::GeomMesh;
using tinyusdz::GPrim;
using tinyusdz::PointInstancer;
using tinyusdz::Prim;
using tinyusdz::Purpose;
using tinyusdz::Visibility;
using tinyusdz::tydra::Node;
using tinyusdz::tydra::RenderMesh;
using tinyusdz::tydra::RenderScene;
using tinyusdz::tydra::VertexAttribute;
using tinyusdz::tydra::VertexAttributeFormat;

UsdImportOutcome Fail(ImportErrorCode code,
                      ImportFailurePhase phase = ImportFailurePhase::Geometry)
{
    return UsdImportFailure{code, phase};
}

bool Finite(double value) { return std::isfinite(value); }

bool ValidMatrix(const tinyusdz::value::matrix4d& matrix)
{
    for (const auto& row : matrix.m)
        for (double value : row)
            if (!Finite(value)) return false;
    constexpr double epsilon = 1e-12;
    return std::abs(matrix.m[0][3]) <= epsilon && std::abs(matrix.m[1][3]) <= epsilon
        && std::abs(matrix.m[2][3]) <= epsilon && std::abs(matrix.m[3][3] - 1.0) <= epsilon;
}

template<class T>
const T* AsExact(const Prim& prim)
{
    return prim.type_id() == tinyusdz::value::TypeTraits<T>::type_id()
        ? prim.as<T>() : nullptr;
}

void CopyMatrix(const tinyusdz::value::matrix4d& source, double destination[16])
{
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
            destination[row * 4 + column] = source.m[row][column];
}

const GPrim* AsGPrim(const Prim& prim)
{
    if (const auto* value = AsExact<tinyusdz::Xform>(prim)) return value;
    if (const auto* value = AsExact<GeomMesh>(prim)) return value;
    if (const auto* value = AsExact<PointInstancer>(prim)) return value;
    if (const auto* value = AsExact<tinyusdz::GeomBasisCurves>(prim)) return value;
    if (const auto* value = AsExact<tinyusdz::GeomNurbsCurves>(prim)) return value;
    if (const auto* value = AsExact<tinyusdz::GeomSphere>(prim)) return value;
    if (const auto* value = AsExact<tinyusdz::GeomCube>(prim)) return value;
    if (const auto* value = AsExact<tinyusdz::GeomCone>(prim)) return value;
    if (const auto* value = AsExact<tinyusdz::GeomCylinder>(prim)) return value;
    if (const auto* value = AsExact<tinyusdz::GeomCapsule>(prim)) return value;
    if (const auto* value = AsExact<tinyusdz::GeomPoints>(prim)) return value;
    return nullptr;
}

struct PrimPolicy {
    bool visible = true;
    bool includedPurpose = true;
};

struct StagePolicy {
    std::unordered_map<std::string, PrimPolicy> prims;
    std::vector<std::pair<std::string, const PointInstancer*>> pointInstancers;
    std::unordered_set<std::string> prototypeRoots;
    uint32_t primCount = 0;
    uint32_t optionalWarnings = 0;
    bool hasComposition = false;
    bool unsupportedRequired = false;
    bool omittedPurpose = false;
};

void SaturatingWarn(StagePolicy& policy)
{
    policy.optionalWarnings = (std::min)(64u, policy.optionalWarnings + 1);
}

bool HasComposition(const tinyusdz::PrimMetas& metas)
{
    return metas.references.has_value() || metas.payload.has_value()
        || metas.inherits.has_value() || metas.specializes.has_value()
        || metas.variantSets.has_value() || metas.variants.has_value()
        || metas.clips.has_value() || metas.instanceable.value_or(false);
}

void ClassifyPrim(const Prim& prim, double time, bool parentVisible,
                  bool parentPurpose, std::string_view parentPath,
                  uint32_t depth, StagePolicy& policy)
{
    if (depth > kMaxSceneHierarchyDepth || ++policy.primCount > kTierBObjectLimit) {
        policy.unsupportedRequired = true;
        return;
    }
    policy.hasComposition |= HasComposition(prim.metas()) || !prim.variantSets().empty();

    bool visible = parentVisible && prim.metas().active.value_or(true);
    bool includedPurpose = parentPurpose;
    if (const GPrim* gprim = AsGPrim(prim)) {
        if (gprim->visibility.get_value().is_timesamples()) SaturatingWarn(policy);
        Visibility visibility = Visibility::Inherited;
        if (!gprim->visibility.get_value().get(
                time, &visibility, tinyusdz::value::TimeSampleInterpolationType::Linear)
            || visibility == Visibility::Invalid) {
            policy.unsupportedRequired = true;
        } else if (visibility == Visibility::Invisible) {
            visible = false;
        }
        if (gprim->purpose.authored()) {
            const Purpose purpose = gprim->purpose.get_value();
            includedPurpose = purpose == Purpose::Default || purpose == Purpose::Render;
            if (!includedPurpose) {
                policy.omittedPurpose = true;
                SaturatingWarn(policy);
            }
        }
    }

    const std::string path = parentPath.empty()
        ? "/" + prim.element_name()
        : std::string(parentPath) + "/" + prim.element_name();
    policy.prims[path] = PrimPolicy{visible, includedPurpose};

    if (const auto* mesh = AsExact<GeomMesh>(prim)) {
        const bool hasCreases = mesh->cornerIndices.authored() || mesh->cornerSharpnesses.authored()
            || mesh->creaseIndices.authored() || mesh->creaseLengths.authored()
            || mesh->creaseSharpnesses.authored();
        if (hasCreases) policy.unsupportedRequired = true;
        if (mesh->subdivisionScheme.get_value() != GeomMesh::SubdivisionScheme::SubdivisionSchemeNone)
            SaturatingWarn(policy); // bounded control-cage approximation
    } else if (const auto* instancer = AsExact<PointInstancer>(prim)) {
        if (visible && includedPurpose) {
            if (instancer->velocities.authored() || instancer->accelerations.authored()
                || instancer->angularVelocities.authored())
                policy.unsupportedRequired = true;
            policy.pointInstancers.emplace_back(path, instancer);
            if (!instancer->prototypes.has_value()) {
                policy.unsupportedRequired = true;
            } else {
                const auto& relationship = *instancer->prototypes;
                if (relationship.is_path()) {
                    policy.prototypeRoots.insert(relationship.targetPath.full_path_name());
                } else if (relationship.is_pathvector()) {
                    for (const auto& target : relationship.targetPathVector)
                        policy.prototypeRoots.insert(target.full_path_name());
                } else {
                    policy.unsupportedRequired = true;
                }
            }
        }
    } else if ((AsExact<tinyusdz::GeomBasisCurves>(prim) || AsExact<tinyusdz::GeomNurbsCurves>(prim)
                || AsExact<tinyusdz::GeomSphere>(prim) || AsExact<tinyusdz::GeomCube>(prim)
                || AsExact<tinyusdz::GeomCone>(prim) || AsExact<tinyusdz::GeomCylinder>(prim)
                || AsExact<tinyusdz::GeomCapsule>(prim) || AsExact<tinyusdz::GeomPoints>(prim))
               && visible && includedPurpose) {
        policy.unsupportedRequired = true;
    } else if (AsExact<tinyusdz::GeomCamera>(prim)) {
        SaturatingWarn(policy);
    }

    for (const Prim& child : prim.children())
        ClassifyPrim(child, time, visible, includedPurpose, path, depth + 1, policy);
}

StagePolicy ClassifyStage(const tinyusdz::Stage& stage, double time)
{
    StagePolicy policy;
    policy.hasComposition = !stage.metas().subLayers.empty();
    for (const Prim& root : stage.root_prims())
        ClassifyPrim(root, time, true, true, {}, 1, policy);
    const auto& defaultPrim = stage.metas().defaultPrim;
    if (defaultPrim.valid()) {
        const bool found = std::ranges::any_of(stage.root_prims(), [&](const Prim& root) {
            return root.element_name() == defaultPrim.str();
        });
        if (!found) SaturatingWarn(policy);
    }
    return policy;
}

UpAxisId Axis(const tinyusdz::Stage& stage)
{
    switch (stage.metas().upAxis.get_value()) {
    case tinyusdz::Axis::X: return UpAxisId::X;
    case tinyusdz::Axis::Y: return UpAxisId::Y;
    case tinyusdz::Axis::Z: return UpAxisId::Z;
    default: return UpAxisId::Unknown;
    }
}

bool ReadFloatComponents(const VertexAttribute& attribute, size_t index,
                         float* output, size_t components)
{
    if (attribute.empty() || index >= attribute.vertex_count() || attribute.elementSize != 1)
        return false;
    const size_t stride = attribute.stride_bytes();
    const uint8_t* source = attribute.data.data() + index * stride;
    size_t available = 0;
    bool isDouble = false;
    switch (attribute.format) {
    case VertexAttributeFormat::Float: available = 1; break;
    case VertexAttributeFormat::Vec2: available = 2; break;
    case VertexAttributeFormat::Vec3: available = 3; break;
    case VertexAttributeFormat::Vec4: available = 4; break;
    case VertexAttributeFormat::Double: available = 1; isDouble = true; break;
    case VertexAttributeFormat::Dvec2: available = 2; isDouble = true; break;
    case VertexAttributeFormat::Dvec3: available = 3; isDouble = true; break;
    case VertexAttributeFormat::Dvec4: available = 4; isDouble = true; break;
    default: return false;
    }
    if (available < components) return false;
    for (size_t component = 0; component < components; ++component) {
        if (isDouble) {
            double value = 0;
            std::memcpy(&value, source + component * sizeof(double), sizeof(value));
            if (!Finite(value) || value > (std::numeric_limits<float>::max)()
                || value < -(std::numeric_limits<float>::max)()) return false;
            output[component] = static_cast<float>(value);
        } else {
            std::memcpy(&output[component], source + component * sizeof(float), sizeof(float));
            if (!std::isfinite(output[component])) return false;
        }
    }
    return true;
}

size_t AttributeIndex(const VertexAttribute& attribute, size_t vertexIndex)
{
    if (attribute.variability == tinyusdz::tydra::VertexVariability::Constant) return 0;
    if (attribute.is_indexed() && vertexIndex < attribute.indices.size())
        return attribute.indices[vertexIndex];
    return vertexIndex;
}

bool MakeVertex(const RenderMesh& mesh, uint32_t sourceIndex,
                const std::array<double, 3>& origin,
                VertexPositionNormalUv0TangentColorF32& vertex,
                bool& hasUv, bool& hasColor)
{
    if (sourceIndex >= mesh.points.size()) return false;
    const auto& point = mesh.points[sourceIndex];
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(point[axis])) return false;
        const double local = double(point[axis]) - origin[axis];
        if (!Finite(local) || local > (std::numeric_limits<float>::max)()
            || local < -(std::numeric_limits<float>::max)()) return false;
        (&vertex.px)[axis] = static_cast<float>(local);
    }

    float normal[3]{0.0f, 0.0f, 1.0f};
    if (!mesh.normals.empty()
        && !ReadFloatComponents(mesh.normals, AttributeIndex(mesh.normals, sourceIndex), normal, 3))
        return false;
    const double length = std::sqrt(double(normal[0]) * normal[0]
        + double(normal[1]) * normal[1] + double(normal[2]) * normal[2]);
    if (!Finite(length) || length <= 1e-20) return false;
    vertex.nx = float(normal[0] / length);
    vertex.ny = float(normal[1] / length);
    vertex.nz = float(normal[2] / length);

    vertex.u = vertex.v = 0.0f;
    if (const auto uv = mesh.texcoords.find(0); uv != mesh.texcoords.end() && !uv->second.empty()) {
        float values[2]{};
        if (!ReadFloatComponents(uv->second, AttributeIndex(uv->second, sourceIndex), values, 2))
            return false;
        vertex.u = values[0]; vertex.v = values[1]; hasUv = true;
    }

    vertex.tx = vertex.ty = vertex.tz = 0.0f;
    vertex.tw = mesh.is_rightHanded ? 1.0f : -1.0f;
    float color[3]{mesh.displayColor[0], mesh.displayColor[1], mesh.displayColor[2]};
    if (!mesh.vertex_colors.empty()) {
        if (!ReadFloatComponents(mesh.vertex_colors,
                                 AttributeIndex(mesh.vertex_colors, sourceIndex), color, 3))
            return false;
        hasColor = true;
    }
    float opacity = mesh.displayOpacity;
    if (!mesh.vertex_opacities.empty()) {
        if (!ReadFloatComponents(mesh.vertex_opacities,
                                 AttributeIndex(mesh.vertex_opacities, sourceIndex), &opacity, 1))
            return false;
        hasColor = true;
    }
    for (float value : color) if (!std::isfinite(value)) return false;
    if (!std::isfinite(opacity)) return false;
    vertex.r = color[0]; vertex.g = color[1]; vertex.b = color[2]; vertex.a = opacity;
    return true;
}

struct GeometryRecord {
    uint32_t chunkId = 0;
    ChunkDescriptor descriptor{};
};

bool TransformBounds(const ChunkDescriptor& geometry,
                     const tinyusdz::value::matrix4d& world,
                     double minimum[3], double maximum[3])
{
    if (!ValidMatrix(world)) return false;
    std::fill(minimum, minimum + 3, (std::numeric_limits<double>::max)());
    std::fill(maximum, maximum + 3, -(std::numeric_limits<double>::max)());
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const double point[3]{
            geometry.origin[0] + (corner & 1 ? geometry.localMax[0] : geometry.localMin[0]),
            geometry.origin[1] + (corner & 2 ? geometry.localMax[1] : geometry.localMin[1]),
            geometry.origin[2] + (corner & 4 ? geometry.localMax[2] : geometry.localMin[2]),
        };
        for (size_t axis = 0; axis < 3; ++axis) {
            const double value = point[0] * world.m[0][axis]
                + point[1] * world.m[1][axis] + point[2] * world.m[2][axis]
                + world.m[3][axis];
            if (!Finite(value)) return false;
            minimum[axis] = (std::min)(minimum[axis], value);
            maximum[axis] = (std::max)(maximum[axis], value);
        }
    }
    return true;
}

bool EmitMesh(BoundedChunkWriter& writer, const RenderMesh& mesh, uint32_t meshOrdinal,
              uint32_t chunkTriangleLimit, uint64_t& triangleCount,
              uint64_t& vertexCount, std::vector<GeometryRecord>& records,
              const UsdImportOptions& options, ImportErrorCode& error)
{
    const auto& indices = mesh.faceVertexIndices();
    const auto& counts = mesh.faceVertexCounts();
    if (indices.empty() || counts.empty()) return true;
    if (indices.size() % 3 != 0 || counts.size() != indices.size() / 3
        || std::any_of(counts.begin(), counts.end(), [](uint32_t count) { return count != 3; })) {
        error = ImportErrorCode::MalformedData;
        return false;
    }
    const uint64_t meshTriangles = indices.size() / 3;
    if (meshTriangles > kTierBTriangleLimit - triangleCount
        || meshTriangles * 3 > kTierBVertexLimit - vertexCount) {
        error = ImportErrorCode::ResourceLimit;
        return false;
    }

    for (uint64_t firstTriangle = 0; firstTriangle < meshTriangles;
         firstTriangle += chunkTriangleLimit) {
        if (options.Cancelled()) { error = ImportErrorCode::Cancelled; return false; }
        const uint32_t count = static_cast<uint32_t>((std::min<uint64_t>)(
            chunkTriangleLimit, meshTriangles - firstTriangle));
        std::vector<VertexPositionNormalUv0TangentColorF32> vertices(size_t(count) * 3);
        std::vector<uint32_t> normalizedIndices(size_t(count) * 3);
        const uint32_t firstSourceIndex = indices[size_t(firstTriangle) * 3];
        if (firstSourceIndex >= mesh.points.size()) {
            error = ImportErrorCode::MalformedData;
            return false;
        }
        const auto& firstPoint = mesh.points[firstSourceIndex];
        std::array<double, 3> origin{double(firstPoint[0]), double(firstPoint[1]),
                                     double(firstPoint[2])};
        bool hasUv = false, hasColor = false;
        for (uint32_t triangle = 0; triangle < count; ++triangle) {
            for (uint32_t corner = 0; corner < 3; ++corner) {
                const uint32_t destinationCorner = mesh.is_rightHanded ? corner : 2 - corner;
                const size_t destinationIndex = size_t(triangle) * 3 + destinationCorner;
                const uint32_t sourceIndex = indices[(size_t(firstTriangle) + triangle) * 3 + corner];
                if (!MakeVertex(mesh, sourceIndex, origin, vertices[destinationIndex], hasUv, hasColor)) {
                    error = ImportErrorCode::MalformedData;
                    return false;
                }
                normalizedIndices[destinationIndex] = static_cast<uint32_t>(destinationIndex);
            }
        }

        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::TriangleList;
        descriptor.vertexCount = descriptor.indexCount = count * 3;
        descriptor.vertexLayoutId = uint32_t(VertexLayoutId::PositionNormalUv0TangentColor_F32);
        descriptor.chunkId = writer.NextId();
        descriptor.meshId = meshOrdinal + 1;
        descriptor.geometryFlags = kGeometryDeindexed | kGeometryReusableInstanceSource
            | (hasUv ? kGeometryHasUv0 : 0) | (hasColor ? kGeometryHasColors : 0);
        descriptor.sourceRangeOffset = (uint64_t(meshOrdinal) << 32)
            | uint64_t(uint32_t(firstTriangle * 3));
        descriptor.sourceRangeLength = uint64_t(count) * 3;
        std::copy(origin.begin(), origin.end(), descriptor.origin);
        if (!SetLocalBounds(descriptor, ChunkBytes(vertices))) {
            error = ImportErrorCode::MalformedData;
            return false;
        }
        if (!writer.Add(descriptor, ChunkBytes(vertices), ChunkBytes(normalizedIndices))) {
            error = writer.Error();
            return false;
        }
        records.push_back(GeometryRecord{descriptor.chunkId, descriptor});
    }
    triangleCount += meshTriangles;
    vertexCount += meshTriangles * 3;
    return true;
}

struct FlatNode {
    const Node* node = nullptr;
    uint32_t parentIndex = UINT32_MAX;
    bool visible = true;
};

struct PointInstanceRecord {
    uint32_t parentFlatIndex = 0;
    uint32_t meshIndex = 0;
    tinyusdz::value::matrix4d localMatrix;
    tinyusdz::value::matrix4d worldMatrix;
    bool visible = true;
};

void FlattenNodes(const Node& node, uint32_t parentIndex,
                  const StagePolicy& policy, std::vector<FlatNode>& nodes)
{
    bool visible = parentIndex == UINT32_MAX || nodes[parentIndex].visible;
    if (const auto found = policy.prims.find(node.abs_path); found != policy.prims.end())
        visible = visible && found->second.visible && found->second.includedPurpose;
    for (const auto& root : policy.prototypeRoots) {
        if (node.abs_path == root
            || (node.abs_path.size() > root.size() && node.abs_path.starts_with(root)
                && node.abs_path[root.size()] == '/')) {
            visible = false;
            break;
        }
    }
    const uint32_t index = static_cast<uint32_t>(nodes.size());
    nodes.push_back(FlatNode{&node, parentIndex, visible});
    for (const Node& child : node.children) FlattenNodes(child, index, policy, nodes);
}

template<class T>
bool Evaluate(const tinyusdz::TypedAttribute<tinyusdz::Animatable<T>>& attribute,
              double time, T& value)
{
    const auto authored = attribute.get_value();
    return authored.has_value() && authored->get(
        time, &value, tinyusdz::value::TimeSampleInterpolationType::Linear);
}

std::vector<std::string> PrototypePaths(const PointInstancer& instancer)
{
    std::vector<std::string> result;
    if (!instancer.prototypes) return result;
    const auto& relationship = *instancer.prototypes;
    if (relationship.is_path()) result.push_back(relationship.targetPath.full_path_name());
    else if (relationship.is_pathvector()) {
        for (const auto& path : relationship.targetPathVector)
            result.push_back(path.full_path_name());
    }
    return result;
}

struct PrototypeMesh {
    uint32_t meshIndex = 0;
    tinyusdz::value::matrix4d relativeMatrix;
    bool visible = true;
};

std::vector<PrototypeMesh> FindPrototypeMeshes(const std::vector<FlatNode>& nodes,
                                               std::string_view prototypePath,
                                               const StagePolicy& policy)
{
    const auto root = std::find_if(nodes.begin(), nodes.end(), [&](const FlatNode& candidate) {
        return candidate.node->abs_path == prototypePath;
    });
    if (root == nodes.end()) return {};
    const uint32_t rootIndex = static_cast<uint32_t>(root - nodes.begin());
    std::vector<PrototypeMesh> result;
    for (uint32_t index = rootIndex; index < nodes.size(); ++index) {
        const FlatNode& candidate = nodes[index];
        if (candidate.node->abs_path != prototypePath
            && !(candidate.node->abs_path.size() > prototypePath.size()
                 && candidate.node->abs_path.starts_with(prototypePath)
                 && candidate.node->abs_path[prototypePath.size()] == '/'))
            continue;
        if (candidate.node->nodeType != tinyusdz::tydra::NodeType::Mesh) continue;
        if (candidate.node->id < 0) return {};
        auto relative = candidate.node->local_matrix;
        uint32_t parent = candidate.parentIndex;
        while (index != rootIndex && parent != UINT32_MAX && parent != rootIndex) {
            relative = relative * nodes[parent].node->local_matrix;
            parent = nodes[parent].parentIndex;
        }
        if (index != rootIndex) {
            if (parent != rootIndex) return {};
            relative = relative * nodes[rootIndex].node->local_matrix;
        }
        if (!ValidMatrix(relative)) return {};
        bool visible = true;
        if (const auto found = policy.prims.find(candidate.node->abs_path);
            found != policy.prims.end())
            visible = found->second.visible && found->second.includedPurpose;
        result.push_back(PrototypeMesh{
            static_cast<uint32_t>(candidate.node->id), relative, visible});
    }
    return result;
}

tinyusdz::value::matrix4d InstanceMatrix(const tinyusdz::value::point3f& position,
                                         const tinyusdz::value::quath* orientation,
                                         const tinyusdz::value::float3* scale,
                                         bool& valid)
{
    const double sx = scale ? (*scale)[0] : 1.0;
    const double sy = scale ? (*scale)[1] : 1.0;
    const double sz = scale ? (*scale)[2] : 1.0;
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
    if (orientation) {
        x = tinyusdz::value::half_to_float(orientation->imag[0]);
        y = tinyusdz::value::half_to_float(orientation->imag[1]);
        z = tinyusdz::value::half_to_float(orientation->imag[2]);
        w = tinyusdz::value::half_to_float(orientation->real);
        const double length = std::sqrt(x*x + y*y + z*z + w*w);
        if (!Finite(length) || length <= 1e-20) { valid = false; return {}; }
        x /= length; y /= length; z /= length; w /= length;
    }
    valid = Finite(position[0]) && Finite(position[1]) && Finite(position[2])
        && Finite(sx) && Finite(sy) && Finite(sz) && sx != 0.0 && sy != 0.0 && sz != 0.0;
    if (!valid) return {};
    tinyusdz::value::matrix4d result;
    result.m[0][0] = sx * (1.0 - 2.0*y*y - 2.0*z*z);
    result.m[0][1] = sx * (2.0*x*y + 2.0*z*w);
    result.m[0][2] = sx * (2.0*x*z - 2.0*y*w);
    result.m[1][0] = sy * (2.0*x*y - 2.0*z*w);
    result.m[1][1] = sy * (1.0 - 2.0*x*x - 2.0*z*z);
    result.m[1][2] = sy * (2.0*y*z + 2.0*x*w);
    result.m[2][0] = sz * (2.0*x*z + 2.0*y*w);
    result.m[2][1] = sz * (2.0*y*z - 2.0*x*w);
    result.m[2][2] = sz * (1.0 - 2.0*x*x - 2.0*y*y);
    result.m[3][0] = position[0]; result.m[3][1] = position[1]; result.m[3][2] = position[2];
    return result;
}

} // namespace

UsdImportOutcome ImportUsd(std::span<const std::byte> sourceBytes,
                           std::span<std::byte> destination,
                           uint64_t generationId,
                           uint32_t maxChunkCount,
                           ChunkBatchSink* batchSink,
                           const UsdImportOptions& options)
{
    if (sourceBytes.empty()) return Fail(ImportErrorCode::EmptyGeometry);
    if (sourceBytes.size() > kTierBPrimarySourceBytes)
        return Fail(ImportErrorCode::PrimarySourceLimit);
    if (options.format != SourceFormatId::Usda && options.format != SourceFormatId::Usdc)
        return Fail(ImportErrorCode::UnsupportedEncoding);
    if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
    if (!TierBScratchLimit()) return Fail(ImportErrorCode::ScratchLimit);

    tinyusdz::USDLoadOptions loadOptions{};
    loadOptions.num_threads = 1;
    loadOptions.max_memory_limit_in_mb = 1536; // advisory; Job commit is authoritative
    loadOptions.load_assets = false;
    loadOptions.do_composition = false;
    loadOptions.load_sublayers = false;
    loadOptions.load_references = false;
    loadOptions.load_payloads = false;
    loadOptions.strict_allowedToken_check = true;
    loadOptions.strict_apiSchema_check = false;

    tinyusdz::Stage stage;
    std::string warning, parseError;
    const char* syntheticName = options.format == SourceFormatId::Usda
        ? "broker-primary.usda" : "broker-primary.usdc";
    if (!tinyusdz::LoadUSDFromMemory(
            reinterpret_cast<const uint8_t*>(sourceBytes.data()), sourceBytes.size(),
            syntheticName, &stage, &warning, &parseError, loadOptions))
        return Fail(ImportErrorCode::MalformedData, ImportFailurePhase::Geometry);
    if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
    if (stage.root_prims().empty()) return Fail(ImportErrorCode::EmptyGeometry);

    const auto& metas = stage.metas();
    const double time = metas.startTimeCode.authored() ? metas.startTimeCode.get_value() : 0.0;
    const double metersPerUnit = metas.metersPerUnit.get_value();
    if (!Finite(time) || !Finite(metersPerUnit) || metersPerUnit <= 0.0
        || Axis(stage) == UpAxisId::Unknown)
        return Fail(ImportErrorCode::MalformedData, ImportFailurePhase::Geometry);

    // This classification is intentionally before BoundedChunkWriter exists:
    // UnsupportedComposition can never follow candidate publication.
    StagePolicy policy = ClassifyStage(stage, time);
    if (policy.primCount > kTierBObjectLimit) return Fail(ImportErrorCode::ResourceLimit);
    if (policy.hasComposition) return Fail(ImportErrorCode::UnsupportedComposition,
                                           ImportFailurePhase::Geometry);
    if (policy.unsupportedRequired)
        return Fail(ImportErrorCode::UnsupportedRequiredFeature);
    if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);

    tinyusdz::tydra::RenderSceneConverterEnv environment(stage);
    environment.usd_filename = syntheticName;
    environment.timecode = time;
    environment.tinterp = tinyusdz::value::TimeSampleInterpolationType::Linear;
    environment.scene_config.load_texture_assets = false;
    environment.mesh_config.triangulate = true;
    environment.mesh_config.validate_geomsubset = true;
    environment.mesh_config.build_vertex_indices = true;
    environment.mesh_config.compute_normals = true;
    environment.mesh_config.compute_tangents_and_binormals = false;
    environment.material_config.texture_image_loader_function = nullptr;
    environment.material_config.allow_missing_asset = true;
    environment.material_config.allow_texture_load_failure = true;

    RenderScene scene;
    tinyusdz::tydra::RenderSceneConverter converter;
    if (!converter.ConvertToRenderScene(environment, &scene))
        return Fail(ImportErrorCode::MalformedData, ImportFailurePhase::Geometry);
    if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
    if (scene.meshes.empty()) return Fail(ImportErrorCode::EmptyGeometry);
    if (scene.meshes.size() > kTierBObjectLimit)
        return Fail(ImportErrorCode::ResourceLimit);
    if (!scene.materials.empty() || !scene.images.empty()) SaturatingWarn(policy);

    std::vector<FlatNode> flatNodes;
    flatNodes.reserve(policy.primCount);
    for (const Node& root : scene.nodes) FlattenNodes(root, UINT32_MAX, policy, flatNodes);
    if (flatNodes.empty() || flatNodes.size() > kTierBObjectLimit)
        return Fail(flatNodes.empty() ? ImportErrorCode::EmptyGeometry
                                      : ImportErrorCode::ResourceLimit);

    std::vector<PointInstanceRecord> pointInstances;
    for (const auto& [path, instancer] : policy.pointInstancers) {
        if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
        const auto parent = std::find_if(flatNodes.begin(), flatNodes.end(),
            [&](const FlatNode& candidate) { return candidate.node->abs_path == path; });
        if (parent == flatNodes.end()) return Fail(ImportErrorCode::MalformedData);
        const uint32_t parentIndex = static_cast<uint32_t>(parent - flatNodes.begin());
        std::vector<int32_t> protoIndices;
        std::vector<tinyusdz::value::point3f> positions;
        if (!Evaluate(instancer->protoIndices, time, protoIndices)
            || !Evaluate(instancer->positions, time, positions)
            || protoIndices.size() != positions.size())
            return Fail(ImportErrorCode::MalformedData);
        std::vector<int64_t> ids;
        if (instancer->ids.authored() && (!Evaluate(instancer->ids, time, ids)
                                          || ids.size() != positions.size()))
            return Fail(ImportErrorCode::MalformedData);
        if (ids.empty()) {
            ids.resize(positions.size());
            for (size_t index = 0; index < ids.size(); ++index) ids[index] = int64_t(index);
        }
        std::vector<tinyusdz::value::quath> orientations;
        if (instancer->orientations.authored()
            && (!Evaluate(instancer->orientations, time, orientations)
                || orientations.size() != positions.size()))
            return Fail(ImportErrorCode::MalformedData);
        std::vector<tinyusdz::value::float3> scales;
        if (instancer->scales.authored()
            && (!Evaluate(instancer->scales, time, scales) || scales.size() != positions.size()))
            return Fail(ImportErrorCode::MalformedData);
        std::vector<int64_t> invisibleValues;
        if (instancer->invisibleIds.authored()
            && !Evaluate(instancer->invisibleIds, time, invisibleValues))
            return Fail(ImportErrorCode::MalformedData);
        const std::unordered_set<int64_t> invisible(invisibleValues.begin(), invisibleValues.end());
        const auto prototypePaths = PrototypePaths(*instancer);
        if (prototypePaths.empty()) return Fail(ImportErrorCode::UnsupportedRequiredFeature);
        for (size_t index = 0; index < positions.size(); ++index) {
            if (protoIndices[index] < 0 || size_t(protoIndices[index]) >= prototypePaths.size())
                return Fail(ImportErrorCode::MalformedData);
            const auto prototypeMeshes = FindPrototypeMeshes(
                flatNodes, prototypePaths[size_t(protoIndices[index])], policy);
            if (prototypeMeshes.empty())
                return Fail(ImportErrorCode::UnsupportedRequiredFeature);
            if (prototypeMeshes.size() > kTierBObjectLimit - pointInstances.size()
                || prototypeMeshes.size() > kTierBObjectLimit - flatNodes.size()
                                               - pointInstances.size())
                return Fail(ImportErrorCode::ResourceLimit);
            bool valid = false;
            const auto placement = InstanceMatrix(positions[index],
                orientations.empty() ? nullptr : &orientations[index],
                scales.empty() ? nullptr : &scales[index], valid);
            if (!valid || !ValidMatrix(placement)) return Fail(ImportErrorCode::MalformedData);
            for (const PrototypeMesh& prototype : prototypeMeshes) {
                if (prototype.meshIndex >= scene.meshes.size())
                    return Fail(ImportErrorCode::MalformedData);
                const auto local = prototype.relativeMatrix * placement;
                const auto world = local * parent->node->global_matrix;
                if (!ValidMatrix(local) || !ValidMatrix(world))
                    return Fail(ImportErrorCode::MalformedData);
                pointInstances.push_back(PointInstanceRecord{
                    parentIndex, prototype.meshIndex, local, world,
                    parent->visible && prototype.visible
                        && !invisible.contains(ids[index])});
            }
        }
    }

    const bool hasVisibleMesh = std::ranges::any_of(flatNodes, [](const FlatNode& node) {
        return node.visible && node.node->nodeType == tinyusdz::tydra::NodeType::Mesh;
    }) || std::ranges::any_of(pointInstances, [](const PointInstanceRecord& instance) {
        return instance.visible;
    });
    if (!hasVisibleMesh)
        return Fail(policy.omittedPurpose ? ImportErrorCode::UnsupportedRequiredFeature
                                          : ImportErrorCode::EmptyGeometry);

    SceneMetadata metadata{};
    metadata.generationId = generationId;
    metadata.format = options.format;
    metadata.upAxis = Axis(stage);
    metadata.metersPerUnit = metersPerUnit;
    metadata.meshCount = static_cast<uint32_t>(scene.meshes.size());
    metadata.nodeCount = static_cast<uint32_t>(flatNodes.size() + pointInstances.size());
    BoundedChunkWriter writer(destination, generationId, maxChunkCount, metadata, batchSink);

    if (destination.size() <= kSectionHeaderSize + kChunkDescriptorSize)
        return Fail(ImportErrorCode::ResourceLimit);
    constexpr uint64_t triangleBytes = 3 * sizeof(VertexPositionNormalUv0TangentColorF32)
        + 3 * sizeof(uint32_t);
    const uint32_t chunkTriangleLimit = static_cast<uint32_t>((std::min<uint64_t>)(
        kChunkTriangles, (destination.size() - kSectionHeaderSize - kChunkDescriptorSize)
        / triangleBytes));
    if (!chunkTriangleLimit) return Fail(ImportErrorCode::ResourceLimit);

    std::vector<std::vector<GeometryRecord>> geometry(scene.meshes.size());
    uint64_t triangleCount = 0, vertexCount = 0;
    ImportErrorCode emitError = ImportErrorCode::None;
    for (uint32_t mesh = 0; mesh < scene.meshes.size(); ++mesh) {
        if (!EmitMesh(writer, scene.meshes[mesh], mesh, chunkTriangleLimit,
                      triangleCount, vertexCount, geometry[mesh], options, emitError))
            return Fail(emitError);
    }
    if (!triangleCount) return Fail(ImportErrorCode::EmptyGeometry);

    const uint32_t firstNodeId = writer.NextId();
    if (uint64_t(firstNodeId) + flatNodes.size() > UINT32_MAX)
        return Fail(ImportErrorCode::ChunkCatalogLimit);
    std::vector<uint32_t> nodeIds(flatNodes.size());
    for (size_t index = 0; index < flatNodes.size(); ++index)
        nodeIds[index] = firstNodeId + static_cast<uint32_t>(index);
    for (size_t index = 0; index < flatNodes.size(); ++index) {
        if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
        const FlatNode& source = flatNodes[index];
        if (!ValidMatrix(source.node->local_matrix)) return Fail(ImportErrorCode::MalformedData);
        NodePayload payload{};
        payload.nodeId = nodeIds[index];
        if (source.parentIndex != UINT32_MAX) payload.parentNodeId = nodeIds[source.parentIndex];
        payload.flags = source.visible ? kSceneRecordVisible : 0;
        CopyMatrix(source.node->local_matrix, payload.localTransform);
        if (!writer.AddNode(payload)) return Fail(writer.Error());
    }

    const uint32_t firstPointNodeId = writer.NextId();
    if (uint64_t(firstPointNodeId) + pointInstances.size() > UINT32_MAX)
        return Fail(ImportErrorCode::ChunkCatalogLimit);
    std::vector<uint32_t> pointNodeIds(pointInstances.size());
    for (size_t index = 0; index < pointInstances.size(); ++index) {
        pointNodeIds[index] = firstPointNodeId + static_cast<uint32_t>(index);
        NodePayload payload{};
        payload.nodeId = pointNodeIds[index];
        payload.parentNodeId = nodeIds[pointInstances[index].parentFlatIndex];
        payload.flags = pointInstances[index].visible ? kSceneRecordVisible : 0;
        CopyMatrix(pointInstances[index].localMatrix, payload.localTransform);
        if (!writer.AddNode(payload)) return Fail(writer.Error());
    }

    uint64_t instanceCount = 0;
    for (size_t index = 0; index < flatNodes.size(); ++index) {
        const FlatNode& source = flatNodes[index];
        if (source.node->nodeType != tinyusdz::tydra::NodeType::Mesh) continue;
        if (source.node->id < 0 || size_t(source.node->id) >= geometry.size())
            return Fail(ImportErrorCode::MalformedData);
        if (!ValidMatrix(source.node->global_matrix)) return Fail(ImportErrorCode::MalformedData);
        for (const GeometryRecord& record : geometry[size_t(source.node->id)]) {
            if (++instanceCount > kTierBObjectLimit) return Fail(ImportErrorCode::ResourceLimit);
            if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
            MeshInstancePayload payload{};
            payload.instanceId = writer.NextId();
            payload.nodeId = nodeIds[index];
            payload.geometryChunkId = record.chunkId;
            payload.flags = source.visible ? kSceneRecordVisible : 0;
            if (!TransformBounds(record.descriptor, source.node->global_matrix,
                                 payload.worldMin, payload.worldMax))
                return Fail(ImportErrorCode::MalformedData);
            if (!writer.AddInstance(payload)) return Fail(writer.Error());
        }
    }
    for (size_t index = 0; index < pointInstances.size(); ++index) {
        const PointInstanceRecord& source = pointInstances[index];
        for (const GeometryRecord& record : geometry[source.meshIndex]) {
            if (++instanceCount > kTierBObjectLimit) return Fail(ImportErrorCode::ResourceLimit);
            MeshInstancePayload payload{};
            payload.instanceId = writer.NextId();
            payload.nodeId = pointNodeIds[index];
            payload.geometryChunkId = record.chunkId;
            payload.flags = source.visible ? kSceneRecordVisible : 0;
            if (!TransformBounds(record.descriptor, source.worldMatrix,
                                 payload.worldMin, payload.worldMax))
                return Fail(ImportErrorCode::MalformedData);
            if (!writer.AddInstance(payload)) return Fail(writer.Error());
        }
    }
    if (!instanceCount) return Fail(ImportErrorCode::EmptyGeometry);

    if (policy.optionalWarnings || !warning.empty() || !converter.GetWarning().empty()) {
        ImportStatusPayload status{};
        status.optionalFeatureWarnings = (std::min)(64u, policy.optionalWarnings
            + uint32_t(!warning.empty()) + uint32_t(!converter.GetWarning().empty()));
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::ImportStatus;
        descriptor.chunkId = writer.NextId();
        if (!writer.Add(descriptor, ChunkBytes(status))) return Fail(writer.Error());
    }
    if (!writer.Complete()) return Fail(writer.Error());
    return UsdImportResult{writer.Count(), writer.Length()};
}

} // namespace import_worker

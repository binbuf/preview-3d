// T33 USD/USDZ family adapter implementation (see UsdFamilyAdapter.h).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "UsdFamilyAdapter.h"

#include "ContainmentStage.h"
#include "Deadline.h"
#include "ProviderLimits.h"
#include "UsdZipPreflight.h"

#include "tinyusdz.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>

#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace preview3d::provider {

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

namespace {

// --- Provider-local ceilings -------------------------------------------------

// USDZ archive ceilings: the worker uses a 4 GiB/200:1 policy but the thumbnail
// host is capped far lower (stream itself is 256 MiB).
constexpr std::uint64_t kArchiveExpandedMaxBytes = 128ull * 1024 * 1024;
constexpr std::uint32_t kArchiveRatioMax = 100;
constexpr std::uint32_t kArchiveEntriesMax = 4096;
constexpr std::uint32_t kArchivePathDepthMax = 32;

// Static-scene caps. The worker hands scenes above the tinyusdz fast path to
// OpenUSD; the provider has no compatibility host, so the same input is simply
// the generic icon (`UnsupportedComposition`).
constexpr std::uint32_t kMaxHierarchyDepth = 256;
constexpr std::uint32_t kMaxPrimCount = ProviderLimits::kNodesMax;      // 10 000
constexpr std::size_t kFastUsdNodeLimit = 512;

// TinyUSDZ in-process memory advisory. The provider has no allocation callback;
// the AllocatorLedger plus the checked counts remain authoritative.
constexpr std::uint32_t kTinyusdzMemoryLimitMb = 192;
constexpr std::uint32_t kTinyusdzAssetSizeMb = 128;

constexpr std::uint8_t kCrateMagic[8] = {'P', 'X', 'R', '-', 'U', 'S', 'D', 'C'};
constexpr std::uint8_t kZipLocalMagic[4] = {'P', 'K', 0x03, 0x04};

enum class Container { Unknown, Usda, Usdc, Usdz };

bool Finite(double value) noexcept
{
    return std::isfinite(value) && std::abs(value) <= 1.0e30;
}

bool EqualsIgnoreCase(char a, char b) noexcept
{
    return std::tolower(static_cast<unsigned char>(a))
        == std::tolower(static_cast<unsigned char>(b));
}

std::string FoldAscii(std::string_view value)
{
    std::string folded(value);
    std::ranges::transform(folded, folded.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return folded;
}

std::optional<std::string> NormalizeAssetPath(std::string_view input)
{
    if (input.empty() || input.size() > 1024 || input.front() == '/'
        || input.find('\\') != std::string_view::npos
        || input.find(':') != std::string_view::npos) {
        return std::nullopt;
    }
    if (std::ranges::any_of(input, [](unsigned char ch) {
            return ch < 0x20 || ch == 0x7f;
        })) {
        return std::nullopt;
    }
    std::string result;
    std::size_t start = 0;
    std::uint32_t depth = 0;
    while (start <= input.size()) {
        const std::size_t end = input.find('/', start);
        const std::string_view component = input.substr(
            start, end == std::string_view::npos ? input.size() - start : end - start);
        if (component.empty() || component == "..") {
            return std::nullopt;
        }
        if (component != ".") {
            if (++depth > 32) {
                return std::nullopt;
            }
            if (!result.empty()) {
                result.push_back('/');
            }
            result.append(component);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return result.empty() ? std::nullopt : std::optional<std::string>(std::move(result));
}

bool HasComposition(const tinyusdz::PrimMetas& metas) noexcept
{
    return metas.references.has_value() || metas.payload.has_value()
        || metas.inherits.has_value() || metas.specializes.has_value()
        || metas.variantSets.has_value() || metas.variants.has_value()
        || metas.clips.has_value() || metas.instanceable.value_or(false);
}

bool ValidMatrix(const tinyusdz::value::matrix4d& matrix) noexcept
{
    for (const auto& row : matrix.m) {
        for (const double value : row) {
            if (!Finite(value)) {
                return false;
            }
        }
    }
    constexpr double epsilon = 1e-12;
    return std::abs(matrix.m[0][3]) <= epsilon && std::abs(matrix.m[1][3]) <= epsilon
        && std::abs(matrix.m[2][3]) <= epsilon && std::abs(matrix.m[3][3] - 1.0) <= epsilon;
}

void TransformPoint(const tinyusdz::value::matrix4d& world, const float point[3],
                    double out[3]) noexcept
{
    for (std::uint32_t axis = 0; axis < 3; ++axis) {
        out[axis] = static_cast<double>(point[0]) * world.m[0][axis]
            + static_cast<double>(point[1]) * world.m[1][axis]
            + static_cast<double>(point[2]) * world.m[2][axis] + world.m[3][axis];
    }
}

void TransformNormal(const tinyusdz::value::matrix4d& world, const float normal[3],
                     float out[3]) noexcept
{
    double value[3]{};
    for (std::uint32_t axis = 0; axis < 3; ++axis) {
        value[axis] = static_cast<double>(normal[0]) * world.m[0][axis]
            + static_cast<double>(normal[1]) * world.m[1][axis]
            + static_cast<double>(normal[2]) * world.m[2][axis];
    }
    const double length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (!Finite(length) || length <= 1.0e-20) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    for (std::uint32_t axis = 0; axis < 3; ++axis) {
        out[axis] = static_cast<float>(value[axis] / length);
    }
}

template <class T>
const T* AsExact(const Prim& prim) noexcept
{
    return prim.type_id() == tinyusdz::value::TypeTraits<T>::type_id() ? prim.as<T>() : nullptr;
}

const GPrim* AsGPrim(const Prim& prim) noexcept
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
    std::uint32_t primCount = 0;
    std::uint32_t warnings = 0;
    bool hasComposition = false;
    bool unsupportedRequired = false;
    bool omittedPurpose = false;
};

void SaturatingWarn(StagePolicy& policy) noexcept
{
    policy.warnings = (std::min)(64u, policy.warnings + 1);
}

void ClassifyPrim(const Prim& prim, double time, bool parentVisible, bool parentPurpose,
                  std::string_view parentPath, std::uint32_t depth, StagePolicy& policy)
{
    if (depth > kMaxHierarchyDepth || ++policy.primCount > kMaxPrimCount) {
        policy.unsupportedRequired = true;
        return;
    }
    policy.hasComposition |= HasComposition(prim.metas()) || !prim.variantSets().empty();

    bool visible = parentVisible && prim.metas().active.value_or(true);
    bool includedPurpose = parentPurpose;
    if (const GPrim* gprim = AsGPrim(prim)) {
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
        if (hasCreases) {
            policy.unsupportedRequired = true;
        }
        if (mesh->subdivisionScheme.get_value()
            != GeomMesh::SubdivisionScheme::SubdivisionSchemeNone) {
            SaturatingWarn(policy); // bounded control-cage approximation
        }
    } else if (const auto* instancer = AsExact<PointInstancer>(prim)) {
        if (visible && includedPurpose) {
            if (instancer->velocities.authored() || instancer->accelerations.authored()
                || instancer->angularVelocities.authored()) {
                policy.unsupportedRequired = true;
            }
            policy.pointInstancers.emplace_back(path, instancer);
            if (!instancer->prototypes.has_value()) {
                policy.unsupportedRequired = true;
            } else {
                const auto& relationship = *instancer->prototypes;
                if (relationship.is_path()) {
                    policy.prototypeRoots.insert(relationship.targetPath.full_path_name());
                } else if (relationship.is_pathvector()) {
                    for (const auto& target : relationship.targetPathVector) {
                        policy.prototypeRoots.insert(target.full_path_name());
                    }
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
    }

    for (const Prim& child : prim.children()) {
        ClassifyPrim(child, time, visible, includedPurpose, path, depth + 1, policy);
    }
}

StagePolicy ClassifyStage(const tinyusdz::Stage& stage, double time)
{
    StagePolicy policy;
    policy.hasComposition = !stage.metas().subLayers.empty();
    for (const Prim& root : stage.root_prims()) {
        ClassifyPrim(root, time, true, true, {}, 1, policy);
    }
    const auto& defaultPrim = stage.metas().defaultPrim;
    if (defaultPrim.valid()) {
        const bool found = std::ranges::any_of(stage.root_prims(), [&](const Prim& root) {
            return root.element_name() == defaultPrim.str();
        });
        if (!found) {
            SaturatingWarn(policy);
        }
    }
    return policy;
}

// Skeletal deformation is out of scope, but the authored rest/bind pose is still
// meaningful static geometry. Clear each mesh's skeleton binding so TinyUSDZ's
// render-scene converter treats the mesh as static instead of failing on rigs
// it cannot build.
std::uint32_t StripSkeletonBindings(Prim& prim, std::uint32_t depth)
{
    // This walk runs before ClassifyStage enforces kMaxHierarchyDepth, so bound
    // it here too: a deeply nested stage must not overflow the stack on the way
    // to the classification pass that rejects it.
    if (depth > kMaxHierarchyDepth) {
        return 0;
    }
    std::uint32_t cleared = 0;
    if (auto* mesh = prim.get_data().as<GeomMesh>(/*strict_cast=*/true)) {
        if (mesh->skeleton.has_value()) {
            mesh->skeleton.reset();
            ++cleared;
        }
        mesh->props.erase("primvars:skel:jointIndices");
        mesh->props.erase("primvars:skel:jointWeights");
        mesh->props.erase("primvars:skel:joints");
        mesh->props.erase("primvars:skel:geomBindTransform");
        mesh->props.erase("skel:skeleton");
    }
    for (Prim& child : prim.children()) {
        cleared += StripSkeletonBindings(child, depth + 1);
    }
    return cleared;
}

bool ReadFloatComponents(const VertexAttribute& attribute, std::size_t index,
                         float* output, std::size_t components) noexcept
{
    if (attribute.empty() || index >= attribute.vertex_count() || attribute.elementSize != 1) {
        return false;
    }
    const std::size_t stride = attribute.stride_bytes();
    const std::uint8_t* source = attribute.data.data() + index * stride;
    std::size_t available = 0;
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
    if (available < components) {
        return false;
    }
    for (std::size_t component = 0; component < components; ++component) {
        if (isDouble) {
            double value = 0.0;
            std::memcpy(&value, source + component * sizeof(double), sizeof(value));
            if (!Finite(value) || value > (std::numeric_limits<float>::max)()
                || value < -(std::numeric_limits<float>::max)()) {
                return false;
            }
            output[component] = static_cast<float>(value);
        } else {
            std::memcpy(&output[component], source + component * sizeof(float), sizeof(float));
            if (!std::isfinite(output[component])) {
                return false;
            }
        }
    }
    return true;
}

std::size_t AttributeIndex(const VertexAttribute& attribute, std::size_t vertexIndex,
                           std::size_t faceVertexIndex) noexcept
{
    if (attribute.variability == tinyusdz::tydra::VertexVariability::Constant) {
        return 0;
    }
    const std::size_t interpolationIndex =
        attribute.variability == tinyusdz::tydra::VertexVariability::FaceVarying
        ? faceVertexIndex : vertexIndex;
    if (attribute.is_indexed() && interpolationIndex < attribute.indices.size()) {
        return attribute.indices[interpolationIndex];
    }
    return interpolationIndex;
}

// Reads the finite normal and linear RGBA for one mesh corner. `hasBoundMaterial`
// selects the material factor over the mesh display color, matching the viewer's
// normalized policy.
bool ReadAttributes(const RenderMesh& mesh, std::uint32_t sourceIndex,
                    std::size_t faceVertexIndex, bool hasBoundMaterial,
                    float normal[3], float color[4]) noexcept
{
    if (sourceIndex >= mesh.points.size()) {
        return false;
    }
    normal[0] = normal[1] = normal[2] = 0.0f;
    if (!mesh.normals.empty()) {
        float values[3]{0.0f, 0.0f, 1.0f};
        if (!ReadFloatComponents(mesh.normals,
                                 AttributeIndex(mesh.normals, sourceIndex, faceVertexIndex),
                                 values, 3)) {
            return false;
        }
        const double length = std::sqrt(static_cast<double>(values[0]) * values[0]
            + static_cast<double>(values[1]) * values[1]
            + static_cast<double>(values[2]) * values[2]);
        if (Finite(length) && length > 1.0e-20) {
            for (std::uint32_t axis = 0; axis < 3; ++axis) {
                normal[axis] = static_cast<float>(values[axis] / length);
            }
        }
    }

    color[0] = color[1] = color[2] = color[3] = 1.0f;
    if (!hasBoundMaterial) {
        color[0] = mesh.displayColor[0];
        color[1] = mesh.displayColor[1];
        color[2] = mesh.displayColor[2];
        color[3] = mesh.displayOpacity;
    }
    if (!mesh.vertex_colors.empty()) {
        float values[3]{1.0f, 1.0f, 1.0f};
        if (!ReadFloatComponents(mesh.vertex_colors,
                                 AttributeIndex(mesh.vertex_colors, sourceIndex, faceVertexIndex),
                                 values, 3)) {
            return false;
        }
        color[0] = values[0];
        color[1] = values[1];
        color[2] = values[2];
    }
    if (!mesh.vertex_opacities.empty()) {
        float value = 1.0f;
        if (!ReadFloatComponents(mesh.vertex_opacities,
                                 AttributeIndex(mesh.vertex_opacities, sourceIndex, faceVertexIndex),
                                 &value, 1)) {
            return false;
        }
        color[3] = value;
    }
    for (std::uint32_t channel = 0; channel < 4; ++channel) {
        if (!std::isfinite(color[channel])) {
            return false;
        }
    }
    return true;
}

// --- Render-scene holder types ----------------------------------------------

struct MaterialRun {
    std::uint64_t first = 0;
    std::uint64_t count = 0;
    int material = -1; // index into RenderScene::materials, or -1 for unbound
};

struct FlatNode {
    const Node* node = nullptr;
    std::uint32_t parentIndex = UINT32_MAX;
    bool visible = true;
};

struct PointInstanceRecord {
    std::uint32_t meshIndex = 0;
    tinyusdz::value::matrix4d worldMatrix;
    bool visible = true;
};

struct UsdAssetContext {
    std::span<const std::byte> source;
    const import_worker::UsdzArchiveView* archive = nullptr;
    ErrorCode error = ErrorCode::None;

    const import_worker::UsdzEntryView* Find(std::string_view normalized) const
    {
        if (archive == nullptr) {
            return nullptr;
        }
        std::string candidate(normalized);
        const std::size_t slash = archive->rootLayerName.find_last_of('/');
        if (slash != std::string::npos) {
            candidate = archive->rootLayerName.substr(0, slash + 1) + candidate;
        }
        const std::string folded = FoldAscii(candidate);
        const auto found = std::ranges::find_if(archive->entries,
            [&](const import_worker::UsdzEntryView& entry) { return entry.foldedName == folded; });
        return found == archive->entries.end() ? nullptr : &*found;
    }
};

int ResolveAsset(const char* assetName, const std::vector<std::string>&,
                 std::string* resolved, std::string*, void* userdata) noexcept
{
    auto& context = *static_cast<UsdAssetContext*>(userdata);
    try {
        const auto normalized = NormalizeAssetPath(
            assetName ? std::string_view(assetName) : std::string_view{});
        if (!normalized) {
            context.error = ErrorCode::UnsafeReference;
            return -2;
        }
        if (context.Find(*normalized) == nullptr) {
            context.error = ErrorCode::UnsafeReference;
            return -2;
        }
        *resolved = *normalized;
        return 0;
    } catch (const std::bad_alloc&) {
        // TinyUSDZ calls this through a plain function pointer; contain the
        // product-owned allocation here rather than let it unwind through the
        // vendored library.
        context.error = ErrorCode::OutOfMemory;
        return -2;
    } catch (...) {
        context.error = ErrorCode::InternalImporterFailure;
        return -2;
    }
}

int SizeAsset(const char* resolvedName, std::uint64_t* bytes, std::string*,
              void* userdata) noexcept
{
    auto& context = *static_cast<UsdAssetContext*>(userdata);
    try {
        const std::string path = resolvedName ? resolvedName : "";
        const auto* entry = context.Find(path);
        if (entry == nullptr) {
            return -1;
        }
        *bytes = entry->byteSize;
        return entry->byteSize != 0 ? 0 : -1;
    } catch (const std::bad_alloc&) {
        context.error = ErrorCode::OutOfMemory;
        return -2;
    } catch (...) {
        context.error = ErrorCode::InternalImporterFailure;
        return -2;
    }
}

int ReadAsset(const char* resolvedName, std::uint64_t requested, std::uint8_t* output,
              std::uint64_t* bytes, std::string*, void* userdata) noexcept
{
    auto& context = *static_cast<UsdAssetContext*>(userdata);
    try {
        const std::string path = resolvedName ? resolvedName : "";
        const auto* entry = context.Find(path);
        if (entry == nullptr) {
            return -1;
        }
        if (entry->dataOffset > context.source.size()
            || entry->byteSize > context.source.size() - entry->dataOffset) {
            context.error = ErrorCode::ArchiveLimit;
            return -2;
        }
        if (requested < entry->byteSize) {
            context.error = ErrorCode::ResourceLimit;
            return -2;
        }
        if (entry->byteSize != 0) {
            std::memcpy(output, context.source.data() + entry->dataOffset,
                        static_cast<std::size_t>(entry->byteSize));
        }
        *bytes = entry->byteSize;
        return 0;
    } catch (const std::bad_alloc&) {
        context.error = ErrorCode::OutOfMemory;
        return -2;
    } catch (...) {
        context.error = ErrorCode::InternalImporterFailure;
        return -2;
    }
}

template <class T>
bool Evaluate(const tinyusdz::TypedAttribute<tinyusdz::Animatable<T>>& attribute,
              double time, T& value)
{
    const auto authored = attribute.get_value();
    return authored.has_value()
        && authored->get(time, &value, tinyusdz::value::TimeSampleInterpolationType::Linear);
}

std::vector<std::string> PrototypePaths(const PointInstancer& instancer)
{
    std::vector<std::string> result;
    if (!instancer.prototypes) {
        return result;
    }
    const auto& relationship = *instancer.prototypes;
    if (relationship.is_path()) {
        result.push_back(relationship.targetPath.full_path_name());
    } else if (relationship.is_pathvector()) {
        for (const auto& path : relationship.targetPathVector) {
            result.push_back(path.full_path_name());
        }
    }
    return result;
}

struct PrototypeMesh {
    std::uint32_t meshIndex = 0;
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
    if (root == nodes.end()) {
        return {};
    }
    const std::uint32_t rootIndex = static_cast<std::uint32_t>(root - nodes.begin());
    std::vector<PrototypeMesh> result;
    for (std::uint32_t index = rootIndex; index < nodes.size(); ++index) {
        const FlatNode& candidate = nodes[index];
        if (candidate.node->abs_path != prototypePath
            && !(candidate.node->abs_path.size() > prototypePath.size()
                 && candidate.node->abs_path.starts_with(prototypePath)
                 && candidate.node->abs_path[prototypePath.size()] == '/')) {
            continue;
        }
        if (candidate.node->nodeType != tinyusdz::tydra::NodeType::Mesh) {
            continue;
        }
        if (candidate.node->id < 0) {
            return {};
        }
        auto relative = candidate.node->local_matrix;
        std::uint32_t parent = candidate.parentIndex;
        while (index != rootIndex && parent != UINT32_MAX && parent != rootIndex) {
            relative = relative * nodes[parent].node->local_matrix;
            parent = nodes[parent].parentIndex;
        }
        if (index != rootIndex) {
            if (parent != rootIndex) {
                return {};
            }
            relative = relative * nodes[rootIndex].node->local_matrix;
        }
        if (!ValidMatrix(relative)) {
            return {};
        }
        bool visible = true;
        if (const auto found = policy.prims.find(candidate.node->abs_path);
            found != policy.prims.end()) {
            visible = found->second.visible && found->second.includedPurpose;
        }
        result.push_back(PrototypeMesh{static_cast<std::uint32_t>(candidate.node->id),
                                       relative, visible});
    }
    return result;
}

tinyusdz::value::matrix4d InstanceMatrix(const tinyusdz::value::point3f& position,
                                         const tinyusdz::value::quath* orientation,
                                         const tinyusdz::value::float3* scale,
                                         bool& valid) noexcept
{
    const double sx = scale ? (*scale)[0] : 1.0;
    const double sy = scale ? (*scale)[1] : 1.0;
    const double sz = scale ? (*scale)[2] : 1.0;
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
    if (orientation != nullptr) {
        x = tinyusdz::value::half_to_float(orientation->imag[0]);
        y = tinyusdz::value::half_to_float(orientation->imag[1]);
        z = tinyusdz::value::half_to_float(orientation->imag[2]);
        w = tinyusdz::value::half_to_float(orientation->real);
        const double length = std::sqrt(x * x + y * y + z * z + w * w);
        if (!Finite(length) || length <= 1.0e-20) {
            valid = false;
            return {};
        }
        x /= length;
        y /= length;
        z /= length;
        w /= length;
    }
    valid = Finite(position[0]) && Finite(position[1]) && Finite(position[2])
        && Finite(sx) && Finite(sy) && Finite(sz) && sx != 0.0 && sy != 0.0 && sz != 0.0;
    if (!valid) {
        return {};
    }
    tinyusdz::value::matrix4d result;
    result.m[0][0] = sx * (1.0 - 2.0 * y * y - 2.0 * z * z);
    result.m[0][1] = sx * (2.0 * x * y + 2.0 * z * w);
    result.m[0][2] = sx * (2.0 * x * z - 2.0 * y * w);
    result.m[1][0] = sy * (2.0 * x * y - 2.0 * z * w);
    result.m[1][1] = sy * (1.0 - 2.0 * x * x - 2.0 * z * z);
    result.m[1][2] = sy * (2.0 * y * z + 2.0 * x * w);
    result.m[2][0] = sz * (2.0 * x * z + 2.0 * y * w);
    result.m[2][1] = sz * (2.0 * y * z - 2.0 * x * w);
    result.m[2][2] = sz * (1.0 - 2.0 * x * x - 2.0 * y * y);
    result.m[3][0] = position[0];
    result.m[3][1] = position[1];
    result.m[3][2] = position[2];
    return result;
}

void FlattenNodes(const Node& node, std::uint32_t parentIndex, const StagePolicy& policy,
                  std::vector<FlatNode>& nodes)
{
    bool visible = parentIndex == UINT32_MAX || nodes[parentIndex].visible;
    if (const auto found = policy.prims.find(node.abs_path); found != policy.prims.end()) {
        visible = visible && found->second.visible && found->second.includedPurpose;
    }
    for (const auto& root : policy.prototypeRoots) {
        if (node.abs_path == root
            || (node.abs_path.size() > root.size() && node.abs_path.starts_with(root)
                && node.abs_path[root.size()] == '/')) {
            visible = false;
            break;
        }
    }
    const std::uint32_t index = static_cast<std::uint32_t>(nodes.size());
    nodes.push_back(FlatNode{&node, parentIndex, visible});
    for (const Node& child : node.children) {
        FlattenNodes(child, index, policy, nodes);
    }
}

} // namespace

struct UsdModelHolder {
    tinyusdz::Stage stage;
    tinyusdz::tydra::RenderScene scene;
    StagePolicy policy;
    double time = 0.0;
    std::vector<FlatNode> flatNodes;
    std::vector<PointInstanceRecord> pointInstances;
    std::vector<std::vector<MaterialRun>> meshRuns;
    std::vector<model_core::MaterialPayload> materials;
    std::size_t whiteIndex = 0; // 0-based; used only when hasWhiteMaterial is true
    bool hasWhiteMaterial = false;
    import_worker::UsdzArchiveView archive;
    bool archivePresent = false;
};

namespace {

ErrorCode MapArchiveError(import_worker::UsdzPreflightError error) noexcept
{
    using E = import_worker::UsdzPreflightError;
    switch (error) {
        case E::None: return ErrorCode::None;
        case E::Cancelled: return ErrorCode::Cancelled;
        case E::UnsafePath: return ErrorCode::UnsafeReference;
        case E::TooManyEntries:
        case E::EntryTooLarge:
        case E::AggregateTooLarge:
        case E::ExpansionRatio:
        case E::Zip64:
        case E::MultiDisk:
            return ErrorCode::ArchiveLimit;
        case E::NotZip:
        case E::Truncated:
        case E::DuplicatePath:
        case E::Encrypted:
        case E::UnsupportedCompression:
        case E::Misaligned:
        case E::InvalidDirectory:
            return ErrorCode::MalformedData;
    }
    return ErrorCode::MalformedData;
}

} // namespace

UsdAdapter::UsdAdapter() noexcept = default;
UsdAdapter::~UsdAdapter() noexcept = default;

void UsdAdapter::ResetState() noexcept
{
    holder_.reset();
    parsed_ = false;
    detectedComposition_ = false;
    detectedExternalAsset_ = false;
    usedUsdz_ = false;
    meshCount_ = 0;
    nodeCount_ = 0;
    materialCount_ = 0;
    inspectedTriangles_ = 0;
    instanceCount_ = 0;
    bytes_ = {};
    ownedBytes_.clear();
    ownedBytes_.shrink_to_fit();
    bytesReservation_.reset();
    input_ = AdapterInput{};
}

void UsdAdapter::Reset() noexcept { ResetState(); }

ErrorCode UsdAdapter::Initialize(const AdapterInput& input) noexcept
{
    return RunContainedStageMember([this, &input]() { return InitializeImpl(input); },
                                   DiagnosticStage::AdapterInitialize);
}

ErrorCode UsdAdapter::InitializeImpl(const AdapterInput& input)
{
    ResetState();
    if (input.source == nullptr || input.limits == nullptr || input.deadline == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    input_ = input;
    return ErrorCode::None;
}

ErrorCode UsdAdapter::SourceReadFailure() const noexcept
{
    if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    return ErrorCode::MalformedData;
}

ErrorCode UsdAdapter::LoadSourceBytes()
{
    bytes_ = input_.source->ContiguousView();
    if (!bytes_.empty()) {
        return ErrorCode::None;
    }
    const std::uint64_t size = input_.source->Size();
    if (size == 0) {
        return ErrorCode::MalformedData;
    }
    if (size > ProviderLimits::kContiguousBackingMaxBytes) {
        return ErrorCode::ResourceLimit;
    }
    if (input_.ledger != nullptr) {
        auto reservation = input_.ledger->ReserveScoped(size);
        if (!reservation.has_value()) {
            return ErrorCode::ResourceLimit;
        }
        bytesReservation_ = std::move(reservation);
    }
    ownedBytes_.resize(static_cast<std::size_t>(size));
    if (!input_.source->ReadAt(0, ownedBytes_)) {
        ownedBytes_.clear();
        return SourceReadFailure();
    }
    bytes_ = std::span<const std::byte>(ownedBytes_.data(), ownedBytes_.size());
    return ErrorCode::None;
}

ErrorCode UsdAdapter::SniffContainer()
{
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes_.data());
    const std::size_t size = bytes_.size();
    if (size >= sizeof(kZipLocalMagic)
        && std::memcmp(data, kZipLocalMagic, sizeof(kZipLocalMagic)) == 0) {
        usedUsdz_ = true;
        return ErrorCode::None;
    }
    if (size >= sizeof(kCrateMagic) && std::memcmp(data, kCrateMagic, sizeof(kCrateMagic)) == 0) {
        return ErrorCode::None;
    }
    std::size_t cursor = 0;
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        cursor = 3; // UTF-8 BOM
    }
    while (cursor < size && (data[cursor] == ' ' || data[cursor] == '\t'
           || data[cursor] == '\r' || data[cursor] == '\n')) {
        ++cursor;
    }
    static constexpr char kUsdaMagic[] = "#usda";
    if (size - cursor >= sizeof(kUsdaMagic) - 1
        && std::memcmp(data + cursor, kUsdaMagic, sizeof(kUsdaMagic) - 1) == 0) {
        return ErrorCode::None;
    }
    return ErrorCode::MalformedData;
}

ErrorCode UsdAdapter::PreflightArchive()
{
    if (!usedUsdz_) {
        return ErrorCode::None;
    }
    import_worker::UsdzArchiveView archive;
    import_worker::UsdzPreflightLimits limits;
    limits.maxEntries = kArchiveEntriesMax;
    limits.maxPathDepth = kArchivePathDepthMax;
    limits.maxEntryBytes = kArchiveExpandedMaxBytes;
    limits.maxExpandedBytes = kArchiveExpandedMaxBytes;
    limits.maxExpansionRatio = kArchiveRatioMax;
    const Deadline* deadline = input_.deadline;
    const auto cancelled = [deadline] {
        return deadline != nullptr && !deadline->Checkpoint();
    };
    const auto error = import_worker::InspectUsdz(bytes_, &archive, limits, cancelled);
    const ErrorCode mapped = MapArchiveError(error);
    if (mapped != ErrorCode::None) {
        return mapped;
    }
    holder_ = std::make_unique<UsdModelHolder>();
    holder_->archive = std::move(archive);
    holder_->archivePresent = true;
    return ErrorCode::None;
}

ErrorCode UsdAdapter::LoadStage()
{
    try {
        tinyusdz::USDLoadOptions options{};
        options.num_threads = 1;
        options.max_memory_limit_in_mb = kTinyusdzMemoryLimitMb;
        options.max_allowed_asset_size_in_mb = kTinyusdzAssetSizeMb;
        options.load_assets = false;
        options.do_composition = false;
        options.load_sublayers = false;
        options.load_references = false;
        options.load_payloads = false;
        options.strict_allowedToken_check = true;
        options.strict_apiSchema_check = false;

        const char* syntheticName = usedUsdz_ ? "preview3d-primary.usd"
                                              : "preview3d-primary.usda";
        std::string warning, parseError;
        const bool ok = tinyusdz::LoadUSDFromMemory(
                reinterpret_cast<const std::uint8_t*>(bytes_.data()), bytes_.size(),
                syntheticName, &holder_->stage, &warning, &parseError, options);
        if (!ok) {
            return ErrorCode::MalformedData;
        }
        if (holder_->stage.root_prims().empty()) {
            return ErrorCode::EmptyGeometry;
        }
        return ErrorCode::None;
    } catch (const std::bad_alloc&) {
        return ErrorCode::OutOfMemory;
    } catch (...) {
        return ErrorCode::InternalImporterFailure;
    }
}

ErrorCode UsdAdapter::BuildScene()
{
    if (holder_ == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    tinyusdz::Stage& stage = holder_->stage;
    const auto& metas = stage.metas();

    const double time = metas.startTimeCode.authored() ? metas.startTimeCode.get_value() : 0.0;
    const double metersPerUnit = metas.metersPerUnit.get_value();
    if (!Finite(time) || !Finite(metersPerUnit) || metersPerUnit <= 0.0) {
        return ErrorCode::MalformedData;
    }
    switch (metas.upAxis.get_value()) {
        case tinyusdz::Axis::X:
        case tinyusdz::Axis::Y:
        case tinyusdz::Axis::Z:
            break;
        default:
            return ErrorCode::MalformedData;
    }

    if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }

    // Ignore skeletal bindings and preview the authored rest pose; this must
    // precede classification and conversion.
    std::uint32_t skeletonBindings = 0;
    for (Prim& root : stage.root_prims()) {
        skeletonBindings += StripSkeletonBindings(root, 1);
    }

    StagePolicy policy = ClassifyStage(stage, time);
    if (skeletonBindings != 0) {
        SaturatingWarn(policy);
    }
    holder_->policy = policy;
    holder_->time = time;
    detectedComposition_ = policy.hasComposition;
    if (policy.primCount > kMaxPrimCount) {
        return ErrorCode::ResourceLimit;
    }
    if (policy.hasComposition) {
        return ErrorCode::UnsupportedComposition;
    }
    if (policy.unsupportedRequired) {
        return ErrorCode::UnsupportedRequiredFeature;
    }
    if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }

    tinyusdz::tydra::RenderSceneConverterEnv environment(stage);
    environment.usd_filename = holder_->archivePresent
        ? holder_->archive.rootLayerName : "preview3d-primary.usda";
    environment.timecode = holder_->archivePresent
        ? tinyusdz::value::TimeCode::Default() : time;
    environment.tinterp = tinyusdz::value::TimeSampleInterpolationType::Linear;
    environment.scene_config.load_texture_assets = false;
    environment.mesh_config.triangulate = true;
    environment.mesh_config.validate_geomsubset = true;
    environment.mesh_config.build_vertex_indices = false;
    environment.mesh_config.compute_normals = true;
    environment.mesh_config.compute_tangents_and_binormals = false;
    environment.material_config.texture_image_loader_function = nullptr;
    environment.material_config.allow_missing_asset = true;
    environment.material_config.allow_texture_load_failure = true;

    UsdAssetContext assets;
    assets.source = bytes_;
    assets.archive = holder_->archivePresent ? &holder_->archive : nullptr;
    tinyusdz::AssetResolutionHandler handler{};
    handler.resolve_fun = ResolveAsset;
    handler.size_fun = SizeAsset;
    handler.read_fun = ReadAsset;
    handler.userdata = &assets;
    environment.asset_resolver.register_wildcard_asset_resolution_handler(handler);

    tinyusdz::tydra::RenderSceneConverter converter;
    if (!converter.ConvertToRenderScene(environment, &holder_->scene)) {
        if (assets.error != ErrorCode::None) {
            detectedExternalAsset_ = true;
            return assets.error;
        }
        return ErrorCode::MalformedData;
    }
    if (assets.error != ErrorCode::None) {
        detectedExternalAsset_ = true;
        return assets.error;
    }
    if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    return ErrorCode::None;
}

ErrorCode UsdAdapter::ValidateScene()
{
    if (holder_ == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    RenderScene& scene = holder_->scene;
    if (scene.meshes.empty()) {
        return ErrorCode::EmptyGeometry;
    }
    if (scene.meshes.size() > kMaxPrimCount) {
        return ErrorCode::ResourceLimit;
    }
    if (scene.materials.size() + 1 > ProviderLimits::kMaterialsMax
        || scene.images.size() > static_cast<std::size_t>(ProviderLimits::kMaterialsMax) * 4) {
        return ErrorCode::ResourceLimit;
    }

    // A texture asset that is not contained in the stream is an external
    // dependency the provider must never resolve. Catch it even when the
    // resolver was not consulted for a particular path.
    for (const auto& image : scene.images) {
        if (image.asset_identifier.empty()) {
            continue;
        }
        if (holder_->archivePresent) {
            const auto normalized = NormalizeAssetPath(image.asset_identifier);
            if (normalized && UsdAssetContext{bytes_, &holder_->archive}.Find(*normalized) != nullptr) {
                continue;
            }
        }
        detectedExternalAsset_ = true;
        return ErrorCode::UnsafeReference;
    }

    const StagePolicy& policy = holder_->policy;
    const double time = holder_->time;

    std::vector<FlatNode> flatNodes;
    flatNodes.reserve(static_cast<std::size_t>(policy.primCount) + 8);
    for (const Node& root : scene.nodes) {
        FlattenNodes(root, UINT32_MAX, policy, flatNodes);
    }
    if (flatNodes.empty()) {
        return ErrorCode::EmptyGeometry;
    }
    if (flatNodes.size() > kMaxPrimCount) {
        return ErrorCode::ResourceLimit;
    }
    if (flatNodes.size() > kFastUsdNodeLimit) {
        // The worker hands scenes this deep to OpenUSD; the provider has no
        // compatibility host, so they are simply the generic icon.
        return ErrorCode::UnsupportedComposition;
    }

    std::vector<PointInstanceRecord> pointInstances;
    for (const auto& [path, instancer] : policy.pointInstancers) {
        if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
            return ErrorCode::Cancelled;
        }
        const auto parent = std::find_if(flatNodes.begin(), flatNodes.end(),
            [&](const FlatNode& candidate) { return candidate.node->abs_path == path; });
        if (parent == flatNodes.end()) {
            return ErrorCode::MalformedData;
        }
        std::vector<int32_t> protoIndices;
        std::vector<tinyusdz::value::point3f> positions;
        if (!Evaluate(instancer->protoIndices, time, protoIndices)
            || !Evaluate(instancer->positions, time, positions)) {
            return ErrorCode::MalformedData;
        }
        // TinyUSDZ exposes no element-count accessor before Evaluate copies the
        // authored array, so cap the result immediately. A point instancer can
        // contribute at most kMaxPrimCount instances, and each instance needs a
        // matching proto/position/id, so anything larger is ResourceLimit (not
        // OutOfMemory) and must not be absorbed into the ledger.
        if (protoIndices.size() > kMaxPrimCount || positions.size() > kMaxPrimCount) {
            return ErrorCode::ResourceLimit;
        }
        if (protoIndices.size() != positions.size()) {
            return ErrorCode::MalformedData;
        }
        std::vector<int64_t> ids;
        if (instancer->ids.authored()
            && (!Evaluate(instancer->ids, time, ids)
                || ids.size() != positions.size())) {
            return ErrorCode::MalformedData;
        }
        if (ids.empty()) {
            ids.resize(positions.size());
            for (std::size_t index = 0; index < ids.size(); ++index) {
                ids[index] = static_cast<int64_t>(index);
            }
        }
        std::vector<tinyusdz::value::quath> orientations;
        if (instancer->orientations.authored()
            && (!Evaluate(instancer->orientations, time, orientations)
                || orientations.size() != positions.size())) {
            return ErrorCode::MalformedData;
        }
        std::vector<tinyusdz::value::float3> scales;
        if (instancer->scales.authored()
            && (!Evaluate(instancer->scales, time, scales)
                || scales.size() != positions.size())) {
            return ErrorCode::MalformedData;
        }
        std::vector<int64_t> invisibleValues;
        if (instancer->invisibleIds.authored()
            && !Evaluate(instancer->invisibleIds, time, invisibleValues)) {
            return ErrorCode::MalformedData;
        }
        // Bound the id set before it is materialized: it is file-count-driven
        // and unrelated to positions.size().
        if (invisibleValues.size() > kMaxPrimCount) {
            return ErrorCode::ResourceLimit;
        }
        const std::unordered_set<int64_t> invisible(invisibleValues.begin(), invisibleValues.end());
        const auto prototypePaths = PrototypePaths(*instancer);
        if (prototypePaths.empty()) {
            return ErrorCode::UnsupportedRequiredFeature;
        }
        for (std::size_t index = 0; index < positions.size(); ++index) {
            if (protoIndices[index] < 0
                || static_cast<std::size_t>(protoIndices[index]) >= prototypePaths.size()) {
                return ErrorCode::MalformedData;
            }
            const auto prototypeMeshes = FindPrototypeMeshes(flatNodes,
                prototypePaths[static_cast<std::size_t>(protoIndices[index])], policy);
            if (prototypeMeshes.empty()) {
                return ErrorCode::UnsupportedRequiredFeature;
            }
            if (prototypeMeshes.size() > kMaxPrimCount - pointInstances.size()
                || prototypeMeshes.size() > kMaxPrimCount - flatNodes.size()
                                               - pointInstances.size()) {
                return ErrorCode::ResourceLimit;
            }
            bool valid = false;
            const auto placement = InstanceMatrix(positions[index],
                orientations.empty() ? nullptr : &orientations[index],
                scales.empty() ? nullptr : &scales[index], valid);
            if (!valid || !ValidMatrix(placement)) {
                return ErrorCode::MalformedData;
            }
            for (const PrototypeMesh& prototype : prototypeMeshes) {
                if (prototype.meshIndex >= scene.meshes.size()) {
                    return ErrorCode::MalformedData;
                }
                const auto local = prototype.relativeMatrix * placement;
                const auto world = local * parent->node->global_matrix;
                if (!ValidMatrix(local) || !ValidMatrix(world)) {
                    return ErrorCode::MalformedData;
                }
                pointInstances.push_back(PointInstanceRecord{
                    prototype.meshIndex, world,
                    parent->visible && prototype.visible && !invisible.contains(ids[index])});
            }
        }
    }

    const bool hasVisibleMesh = std::ranges::any_of(flatNodes, [](const FlatNode& node) {
        return node.visible && node.node->nodeType == tinyusdz::tydra::NodeType::Mesh;
    }) || std::ranges::any_of(pointInstances, [](const PointInstanceRecord& instance) {
        return instance.visible;
    });
    if (!hasVisibleMesh) {
        return policy.omittedPurpose ? ErrorCode::UnsupportedRequiredFeature
                                     : ErrorCode::EmptyGeometry;
    }

    // Build the per-mesh material runs once; they are reused for every node and
    // point-instancer occurrence of the mesh.
    std::vector<std::vector<MaterialRun>> meshRuns(scene.meshes.size());
    bool needsWhiteMaterial = false;
    std::uint64_t triangleTotal = 0;
    for (std::size_t meshIndex = 0; meshIndex < scene.meshes.size(); ++meshIndex) {
        const RenderMesh& mesh = scene.meshes[meshIndex];
        const auto& indices = mesh.faceVertexIndices();
        const auto& counts = mesh.faceVertexCounts();
        if (indices.empty()) {
            continue;
        }
        if (indices.size() % 3 != 0 || counts.size() != indices.size() / 3
            || std::any_of(counts.begin(), counts.end(),
                           [](uint32_t count) { return count != 3; })) {
            return ErrorCode::MalformedData;
        }
        const std::uint64_t meshTriangles = indices.size() / 3;
        if (meshTriangles > ProviderLimits::kTrianglesInspectedMax - triangleTotal) {
            return ErrorCode::ResourceLimit;
        }
        triangleTotal += meshTriangles;

        std::vector<int> triangleMaterials(static_cast<std::size_t>(meshTriangles),
                                           mesh.material_id);
        std::vector<bool> assigned(static_cast<std::size_t>(meshTriangles), false);
        for (const auto& [name, subset] : mesh.material_subsetMap) {
            (void)name;
            for (const int face : subset.indices()) {
                if (face < 0 || static_cast<std::uint64_t>(face) >= meshTriangles
                    || assigned[static_cast<std::size_t>(face)]) {
                    return ErrorCode::MalformedData;
                }
                assigned[static_cast<std::size_t>(face)] = true;
                triangleMaterials[static_cast<std::size_t>(face)] = subset.material_id;
            }
        }
        std::vector<MaterialRun>& runs = meshRuns[meshIndex];
        std::uint64_t first = 0;
        while (first < meshTriangles) {
            std::uint64_t end = first + 1;
            while (end < meshTriangles
                   && triangleMaterials[static_cast<std::size_t>(end)]
                       == triangleMaterials[static_cast<std::size_t>(first)]) {
                ++end;
            }
            runs.push_back(MaterialRun{first, end - first,
                                       triangleMaterials[static_cast<std::size_t>(first)]});
            if (triangleMaterials[static_cast<std::size_t>(first)] < 0) {
                needsWhiteMaterial = true;
            }
            first = end;
        }
    }
    if (triangleTotal == 0) {
        return ErrorCode::EmptyGeometry;
    }

    std::vector<bool> doubleSided(scene.materials.size(), false);
    for (const RenderMesh& mesh : scene.meshes) {
        if (mesh.doubleSided && mesh.material_id >= 0
            && static_cast<std::size_t>(mesh.material_id) < doubleSided.size()) {
            doubleSided[static_cast<std::size_t>(mesh.material_id)] = true;
        }
        if (mesh.doubleSided) {
            for (const auto& [name, subset] : mesh.material_subsetMap) {
                (void)name;
                if (subset.material_id >= 0
                    && static_cast<std::size_t>(subset.material_id) < doubleSided.size()) {
                    doubleSided[static_cast<std::size_t>(subset.material_id)] = true;
                }
            }
        }
    }

    std::vector<model_core::MaterialPayload> materials;
    materials.reserve(scene.materials.size() + 1);
    for (std::size_t index = 0; index < scene.materials.size(); ++index) {
        const auto& shader = scene.materials[index].surfaceShader;
        model_core::MaterialPayload payload{};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            payload.baseColorFactor[channel] = shader.diffuseColor.is_texture()
                ? 1.0f : shader.diffuseColor.value[channel];
            payload.emissiveFactor[channel] = shader.emissiveColor.is_texture()
                ? 1.0f : shader.emissiveColor.value[channel];
        }
        payload.baseColorFactor[3] = shader.opacity.is_texture() ? 1.0f : shader.opacity.value;
        payload.metallicFactor = shader.metallic.value;
        payload.roughnessFactor = shader.roughness.value;
        payload.uvScale[0] = payload.uvScale[1] = 1.0f;
        payload.alphaCutoff = shader.opacityThreshold.value > 0.0f
            ? shader.opacityThreshold.value : 0.5f;
        payload.alphaMode = static_cast<std::uint32_t>(
            shader.opacityThreshold.value > 0.0f
                ? model_core::AlphaModeId::Mask
                : (shader.opacity.is_texture() || shader.opacity.value < 1.0f)
                    ? model_core::AlphaModeId::Blend
                    : model_core::AlphaModeId::Opaque);
        payload.flags = doubleSided[index] ? model_core::kMaterialFlagDoubleSided : 0u;
        for (const float value : payload.baseColorFactor) {
            if (!std::isfinite(value)) {
                return ErrorCode::MalformedData;
            }
        }
        for (const float value : payload.emissiveFactor) {
            if (!std::isfinite(value)) {
                return ErrorCode::MalformedData;
            }
        }
        if (!std::isfinite(payload.metallicFactor) || !std::isfinite(payload.roughnessFactor)
            || !std::isfinite(payload.alphaCutoff)) {
            return ErrorCode::MalformedData;
        }
        materials.push_back(payload);
    }
    if (needsWhiteMaterial) {
        model_core::MaterialPayload white = NeutralMaterial();
        white.baseColorFactor[0] = 1.0f;
        white.baseColorFactor[1] = 1.0f;
        white.baseColorFactor[2] = 1.0f;
        white.baseColorFactor[3] = 1.0f;
        materials.push_back(white);
        holder_->hasWhiteMaterial = true;
        holder_->whiteIndex = materials.size() - 1;
    }

    holder_->flatNodes = std::move(flatNodes);
    holder_->pointInstances = std::move(pointInstances);
    holder_->meshRuns = std::move(meshRuns);
    holder_->materials = std::move(materials);

    meshCount_ = scene.meshes.size();
    nodeCount_ = holder_->flatNodes.size() + holder_->pointInstances.size();
    materialCount_ = holder_->materials.size();
    detectedExternalAsset_ = false;
    return ErrorCode::None;
}

ErrorCode UsdAdapter::Parse() noexcept
{
    return RunContainedStageMember([this]() { return ParseImpl(); }, DiagnosticStage::Parse);
}

ErrorCode UsdAdapter::ParseImpl()
{
    try {
        if (input_.deadline == nullptr || !input_.deadline->Checkpoint()) {
            return ErrorCode::Cancelled;
        }
        const ErrorCode load = LoadSourceBytes();
        if (load != ErrorCode::None) {
            return load;
        }
        if (bytes_.size() > ProviderLimits::kStreamMaxBytes) {
            return ErrorCode::ResourceLimit;
        }
        const ErrorCode container = SniffContainer();
        if (container != ErrorCode::None) {
            return container;
        }
        const ErrorCode archive = PreflightArchive();
        if (archive != ErrorCode::None) {
            return archive;
        }
        if (holder_ == nullptr) {
            holder_ = std::make_unique<UsdModelHolder>();
        }
        const ErrorCode stage = LoadStage();
        if (stage != ErrorCode::None) {
            holder_.reset();
            return stage;
        }
        const ErrorCode scene = BuildScene();
        if (scene != ErrorCode::None) {
            holder_.reset();
            return scene;
        }
        const ErrorCode validate = ValidateScene();
        if (validate != ErrorCode::None) {
            holder_.reset();
            return validate;
        }
        parsed_ = true;
        return ErrorCode::None;
    } catch (const std::bad_alloc&) {
        holder_.reset();
        return ErrorCode::OutOfMemory;
    } catch (...) {
        holder_.reset();
        return ErrorCode::InternalImporterFailure;
    }
}

ErrorCode UsdAdapter::EnumerateMaterials(IMaterialSink& sink) noexcept
{
    return RunContainedStageMember([this, &sink]() { return EnumerateMaterialsImpl(sink); },
                                   DiagnosticStage::Materials);
}

ErrorCode UsdAdapter::EnumerateMaterialsImpl(IMaterialSink& sink)
{
    if (!parsed_ || holder_ == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    for (std::size_t index = 0; index < holder_->materials.size(); ++index) {
        if (!sink.OnMaterial(static_cast<std::uint32_t>(index + 1), holder_->materials[index])) {
            return ErrorCode::None;
        }
    }
    return ErrorCode::None;
}

ErrorCode UsdAdapter::EnumerateGeometry(IGeometrySink& sink) noexcept
{
    return RunContainedStageMember([this, &sink]() { return EnumerateGeometryImpl(sink); },
                                   DiagnosticStage::Geometry);
}

ErrorCode UsdAdapter::EnumerateGeometryImpl(IGeometrySink& sink)
{
    if (!parsed_ || holder_ == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    try {
        auto emitMesh = [&](std::size_t meshIndex,
                            const tinyusdz::value::matrix4d& world) -> ErrorCode {
            if (meshIndex >= holder_->scene.meshes.size()
                || meshIndex >= holder_->meshRuns.size()) {
                return ErrorCode::MalformedData;
            }
            const RenderMesh& mesh = holder_->scene.meshes[meshIndex];
            const auto& indices = mesh.faceVertexIndices();
            for (const MaterialRun& run : holder_->meshRuns[meshIndex]) {
                const bool hasBoundMaterial = run.material >= 0;
                const std::uint32_t materialIndex = hasBoundMaterial
                    ? static_cast<std::uint32_t>(run.material + 1)
                    : (holder_->hasWhiteMaterial
                        ? static_cast<std::uint32_t>(holder_->whiteIndex + 1) : 0u);
                for (std::uint64_t triangle = run.first; triangle < run.first + run.count;
                     ++triangle) {
                    if ((triangle & 0x3FFu) == 0 && input_.deadline != nullptr
                        && !input_.deadline->Checkpoint()) {
                        return ErrorCode::Cancelled;
                    }
                    if (++inspectedTriangles_ > ProviderLimits::kTrianglesInspectedMax) {
                        return ErrorCode::ResourceLimit;
                    }
                    struct Corner {
                        double transformed[3];
                        float normal[3];
                        float color[4];
                    };
                    Corner corners[3]{};
                    for (std::uint32_t corner = 0; corner < 3; ++corner) {
                        const std::size_t sourceCorner =
                            static_cast<std::size_t>(triangle) * 3 + corner;
                        if (sourceCorner >= indices.size()) {
                            return ErrorCode::MalformedData;
                        }
                        const std::uint32_t sourceIndex = indices[sourceCorner];
                        if (sourceIndex >= mesh.points.size()) {
                            return ErrorCode::MalformedData;
                        }
                        const auto& point = mesh.points[sourceIndex];
                        const float local[3] = {point[0], point[1], point[2]};
                        TransformPoint(world, local, corners[corner].transformed);
                        for (const double value : corners[corner].transformed) {
                            if (!Finite(value)) {
                                return ErrorCode::MalformedData;
                            }
                        }
                        if (!ReadAttributes(mesh, sourceIndex, sourceCorner, hasBoundMaterial,
                                            corners[corner].normal, corners[corner].color)) {
                            return ErrorCode::MalformedData;
                        }
                    }

                    TriangleSample sample{};
                    sample.origin[0] = corners[0].transformed[0];
                    sample.origin[1] = corners[0].transformed[1];
                    sample.origin[2] = corners[0].transformed[2];
                    sample.materialIndex = materialIndex;
                    for (std::uint32_t corner = 0; corner < 3; ++corner) {
                        const std::uint32_t destination =
                            mesh.is_rightHanded ? corner : 2 - corner;
                        VertexSample& vertex = sample.vertices[destination];
                        for (std::uint32_t axis = 0; axis < 3; ++axis) {
                            vertex.position[axis] = static_cast<float>(
                                corners[corner].transformed[axis] - sample.origin[axis]);
                        }
                        TransformNormal(world, corners[corner].normal, vertex.normal);
                        std::copy(std::begin(corners[corner].color),
                                  std::end(corners[corner].color), std::begin(vertex.color));
                    }
                    if (!sink.OnTriangle(sample)) {
                        return ErrorCode::None;
                    }
                }
            }
            return ErrorCode::None;
        };

        for (const FlatNode& source : holder_->flatNodes) {
            if (!source.visible || source.node == nullptr
                || source.node->nodeType != tinyusdz::tydra::NodeType::Mesh) {
                continue;
            }
            if (source.node->id < 0) {
                return ErrorCode::MalformedData;
            }
            if (!ValidMatrix(source.node->global_matrix)) {
                return ErrorCode::MalformedData;
            }
            ++instanceCount_;
            const ErrorCode result = emitMesh(
                static_cast<std::size_t>(source.node->id), source.node->global_matrix);
            if (result != ErrorCode::None) {
                return result;
            }
        }
        for (const PointInstanceRecord& instance : holder_->pointInstances) {
            if (!instance.visible) {
                continue;
            }
            if (!ValidMatrix(instance.worldMatrix)) {
                return ErrorCode::MalformedData;
            }
            ++instanceCount_;
            const ErrorCode result = emitMesh(instance.meshIndex, instance.worldMatrix);
            if (result != ErrorCode::None) {
                return result;
            }
        }
        return ErrorCode::None;
    } catch (const std::bad_alloc&) {
        return ErrorCode::OutOfMemory;
    } catch (...) {
        return ErrorCode::InternalImporterFailure;
    }
}

} // namespace preview3d::provider

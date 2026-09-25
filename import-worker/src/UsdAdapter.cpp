#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "UsdAdapter.h"

#include "BoundedChunkWriter.h"
#include "ImageFormatSniff.h"
#include "SidecarFileClient.h"
#include "TextureDecodePolicy.h"
#include "TextureTranscodeAdapter.h"
#include "UsdZipPreflight.h"
#include "WebpDecodeAdapter.h"
#include "WicImageDecodeAdapter.h"
#include "model_core/GeometryBounds.h"
#include "model_core/ControlProtocol.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/TierALimits.h"
#include "model_core/VertexLayouts.h"

#include "tinyusdz.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
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

constexpr uint64_t kUsdMaxEncodedTextureBytes = 256ull * 1024 * 1024;

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
    if (input.empty() || input.size() > model_core::kMaxSidecarRelativePathBytes
        || input.front() == '/' || input.find('\\') != std::string_view::npos
        || input.find(':') != std::string_view::npos) return std::nullopt;
    if (std::ranges::any_of(input, [](unsigned char ch) {
            return ch < 0x20 || ch == 0x7f;
        })) return std::nullopt;
    std::string result;
    size_t start = 0;
    uint32_t depth = 0;
    while (start <= input.size()) {
        const size_t end = input.find('/', start);
        const std::string_view component = input.substr(
            start, end == std::string_view::npos ? input.size() - start : end - start);
        if (component.empty() || component == "..") return std::nullopt;
        if (component != ".") {
            if (++depth > 32) return std::nullopt;
            if (!result.empty()) result.push_back('/');
            result.append(component);
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (result.empty()) return std::nullopt;
    // Path safety only. Container type is checked separately in ResolveAsset so
    // an unsupported image type (for example an EXR normal map) degrades to an
    // optional-texture miss instead of a fatal UnsafeReference.
    return result;
}

// Extensions this adapter can actually consume: image containers the shared
// decoders handle, plus the USD layer encodings. Anything else (EXR, TGA, ...)
// is an unsupported optional asset: TinyUSDZ should treat it as missing rather
// than as a containment failure.
bool SupportedAssetExtension(std::string_view path)
{
    if (HasDecodableImageExtension(path)) return true;
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return false;
    const std::string extension = FoldAscii(path.substr(dot));
    return extension == ".usd" || extension == ".usda" || extension == ".usdc";
}

struct AssetContext {
    std::span<const std::byte> source;
    const UsdzArchiveView* archive = nullptr;
    SidecarFileClient* sidecars = nullptr;
    uint64_t maxBytes = 0;
    uint32_t maxCount = 0;
    uint64_t retainedBytes = 0;
    uint32_t retainedCount = 0;
    uint32_t warnings = 0;
    ImportErrorCode error = ImportErrorCode::None;
    std::unordered_map<std::string, std::vector<std::byte>> local;
    std::unordered_set<std::string> missing;
    std::unordered_set<std::string> chargedArchive;

    const UsdzEntryView* FindArchive(std::string_view normalized) const
    {
        if (!archive) return nullptr;
        std::string candidate(normalized);
        const size_t slash = archive->rootLayerName.find_last_of('/');
        if (slash != std::string::npos) candidate = archive->rootLayerName.substr(0, slash + 1) + candidate;
        const std::string folded = FoldAscii(candidate);
        const auto found = std::ranges::find_if(archive->entries, [&](const UsdzEntryView& entry) {
            return entry.foldedName == folded;
        });
        return found == archive->entries.end() ? nullptr : &*found;
    }

    bool Charge(std::string_view key, uint64_t bytes, bool archiveAsset)
    {
        if (archiveAsset && chargedArchive.contains(std::string(key))) return true;
        if (retainedCount >= maxCount || bytes > maxBytes - retainedBytes) {
            error = ImportErrorCode::AggregateSourceLimit;
            return false;
        }
        ++retainedCount;
        retainedBytes += bytes;
        if (archiveAsset) chargedArchive.emplace(key);
        return true;
    }
};

int ResolveAsset(const char* assetName, const std::vector<std::string>&,
                 std::string* resolved, std::string*, void* userdata)
{
    auto& context = *static_cast<AssetContext*>(userdata);
    const auto normalized = NormalizeAssetPath(assetName ? std::string_view(assetName) : std::string_view{});
    if (!normalized) {
        context.error = ImportErrorCode::UnsafeReference;
        return -2;
    }
    // A path-safe reference to a container no decoder handles (for example an
    // EXR normal map) is an unsupported optional asset, not a containment
    // failure. Report it as missing after a bounded warning so the rest of the
    // stage still imports.
    if (!SupportedAssetExtension(*normalized)) {
        context.warnings = (std::min)(64u, context.warnings + 1);
        return -2;
    }
    *resolved = *normalized;
    return 0;
}

bool EnsureLocalAsset(AssetContext& context, const std::string& path)
{
    if (context.local.contains(path)) return true;
    if (context.missing.contains(path)) return false;
    if (!context.sidecars) {
        context.missing.insert(path);
        context.warnings = (std::min)(64u, context.warnings + 1);
        return false;
    }
    const uint64_t remaining = context.retainedBytes < context.maxBytes
        ? context.maxBytes - context.retainedBytes : 0;
    auto result = context.sidecars->RequestSidecarBytes(path,
        (std::min)(remaining, kUsdMaxEncodedTextureBytes));
    if (!result.bytes) {
        if (result.errorCode == ImportErrorCode::UnsafeReference
            || result.errorCode == ImportErrorCode::FileChanged
            || result.errorCode == ImportErrorCode::ImportProtocolViolation
            || result.errorCode == ImportErrorCode::ResourceLimit
            || result.errorCode == ImportErrorCode::AggregateSourceLimit) {
            context.error = result.errorCode;
        } else {
            context.warnings = (std::min)(64u, context.warnings + 1);
        }
        context.missing.insert(path);
        return false;
    }
    if (!context.Charge(path, result.bytes->size(), false)) return false;
    context.local.emplace(path, std::move(*result.bytes));
    return true;
}

int SizeAsset(const char* resolvedName, uint64_t* bytes, std::string*, void* userdata)
{
    auto& context = *static_cast<AssetContext*>(userdata);
    const std::string path = resolvedName ? resolvedName : "";
    if (const UsdzEntryView* entry = context.FindArchive(path)) {
        if (!context.Charge(path, entry->byteSize, true)) return -2;
        *bytes = entry->byteSize;
        return entry->byteSize ? 0 : -1;
    }
    if (!EnsureLocalAsset(context, path)) return -1;
    *bytes = context.local.at(path).size();
    return *bytes ? 0 : -1;
}

int ReadAsset(const char* resolvedName, uint64_t requested, uint8_t* output,
              uint64_t* bytes, std::string*, void* userdata)
{
    auto& context = *static_cast<AssetContext*>(userdata);
    const std::string path = resolvedName ? resolvedName : "";
    std::span<const std::byte> source;
    if (const UsdzEntryView* entry = context.FindArchive(path)) {
        if (entry->dataOffset > context.source.size()
            || entry->byteSize > context.source.size() - entry->dataOffset) {
            context.error = ImportErrorCode::ArchiveLimit;
            return -2;
        }
        source = context.source.subspan(size_t(entry->dataOffset), size_t(entry->byteSize));
    } else {
        if (!EnsureLocalAsset(context, path)) return -1;
        source = context.local.at(path);
    }
    if (requested < source.size()) {
        context.error = ImportErrorCode::ResourceLimit;
        return -2;
    }
    if (!source.empty()) std::memcpy(output, source.data(), source.size());
    *bytes = source.size();
    return 0;
}

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

// Skeletal deformation is out of scope, but the authored rest/bind pose is
// still meaningful static geometry (the FBX path already previews a baked start
// pose). Clear each mesh's skel:skeleton binding so TinyUSDZ's render-scene
// converter treats the mesh as static instead of failing on rigs it cannot
// build (for example a multi-root armature). Returns the number of bindings
// cleared so the caller can surface one bounded optional warning.
uint32_t StripSkeletonBindings(Prim& prim)
{
    uint32_t cleared = 0;
    if (auto* mesh = prim.get_data().as<GeomMesh>(/*strict_cast=*/true)) {
        if (mesh->skeleton.has_value()) {
            mesh->skeleton.reset();
            ++cleared;
        }
        // Remove the skinning inputs too. TinyUSDZ's converter indexes its
        // skeleton table from a mesh's joint indices even when the skeleton
        // binding was absent, so leaving them turns the rest-pose preview into
        // an out-of-bounds crash. With no skinning primvars the mesh is emitted
        // as plain static geometry.
        mesh->props.erase("primvars:skel:jointIndices");
        mesh->props.erase("primvars:skel:jointWeights");
        mesh->props.erase("primvars:skel:joints");
        mesh->props.erase("primvars:skel:geomBindTransform");
        mesh->props.erase("skel:skeleton");
    }
    for (Prim& child : prim.children()) cleared += StripSkeletonBindings(child);
    return cleared;
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

size_t AttributeIndex(const VertexAttribute& attribute, size_t vertexIndex,
                      size_t faceVertexIndex)
{
    if (attribute.variability == tinyusdz::tydra::VertexVariability::Constant) return 0;
    const size_t interpolationIndex =
        attribute.variability == tinyusdz::tydra::VertexVariability::FaceVarying
        ? faceVertexIndex : vertexIndex;
    if (attribute.is_indexed() && interpolationIndex < attribute.indices.size())
        return attribute.indices[interpolationIndex];
    return interpolationIndex;
}

bool MakeVertex(const RenderMesh& mesh, uint32_t sourceIndex, size_t faceVertexIndex,
                bool hasBoundMaterial,
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
        && !ReadFloatComponents(mesh.normals,
                                AttributeIndex(mesh.normals, sourceIndex, faceVertexIndex),
                                normal, 3))
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
        if (!ReadFloatComponents(uv->second,
                                 AttributeIndex(uv->second, sourceIndex, faceVertexIndex),
                                 values, 2))
            return false;
        vertex.u = values[0]; vertex.v = values[1]; hasUv = true;
    }

    vertex.tx = vertex.ty = vertex.tz = 0.0f;
    vertex.tw = mesh.is_rightHanded ? 1.0f : -1.0f;
    float color[3]{1.0f, 1.0f, 1.0f};
    if (!hasBoundMaterial) {
        color[0] = mesh.displayColor[0];
        color[1] = mesh.displayColor[1];
        color[2] = mesh.displayColor[2];
    }
    if (!mesh.vertex_colors.empty()) {
        if (!ReadFloatComponents(mesh.vertex_colors,
                                 AttributeIndex(mesh.vertex_colors, sourceIndex, faceVertexIndex),
                                 color, 3))
            return false;
        hasColor = true;
    }
    float opacity = hasBoundMaterial ? 1.0f : mesh.displayOpacity;
    if (!mesh.vertex_opacities.empty()) {
        if (!ReadFloatComponents(mesh.vertex_opacities,
                                 AttributeIndex(mesh.vertex_opacities, sourceIndex,
                                                faceVertexIndex), &opacity, 1))
            return false;
        hasColor = true;
    }
    for (float value : color) if (!std::isfinite(value)) return false;
    if (!std::isfinite(opacity)) return false;
    vertex.r = color[0]; vertex.g = color[1]; vertex.b = color[2]; vertex.a = opacity;
    return true;
}

std::optional<std::string> MeshTexcoordName(const RenderScene& scene,
                                            const RenderMesh& mesh)
{
    std::vector<int> materialIndices;
    if (mesh.material_id >= 0) materialIndices.push_back(mesh.material_id);
    for (const auto& [name, subset] : mesh.material_subsetMap) {
        (void)name;
        if (subset.material_id >= 0) materialIndices.push_back(subset.material_id);
    }
    for (const int materialIndex : materialIndices) {
        if (size_t(materialIndex) >= scene.materials.size()) continue;
        const auto& shader = scene.materials[size_t(materialIndex)].surfaceShader;
        const int textureIndices[]{
            shader.diffuseColor.texture_id, shader.emissiveColor.texture_id,
            shader.normal.texture_id, shader.metallic.texture_id,
            shader.roughness.texture_id, shader.opacity.texture_id};
        for (const int textureIndex : textureIndices) {
            if (textureIndex < 0 || size_t(textureIndex) >= scene.textures.size()) continue;
            const std::string& name = scene.textures[size_t(textureIndex)].varname_uv;
            if (!name.empty()) return name;
        }
    }
    return std::nullopt;
}

bool ExpandPrimvarForTriangulatedMesh(const RenderMesh& mesh,
                                      VertexAttribute& attribute)
{
    const auto& triangleIndices = mesh.faceVertexIndices();
    if (triangleIndices.empty()) return false;
    std::vector<size_t> sourceFaces(mesh.usdFaceVertexIndices.size());
    size_t offset = 0;
    for (size_t face = 0; face < mesh.usdFaceVertexCounts.size(); ++face) {
        const size_t count = mesh.usdFaceVertexCounts[face];
        if (count > sourceFaces.size() - offset) return false;
        std::fill_n(sourceFaces.begin() + offset, count, face);
        offset += count;
    }
    if (offset != sourceFaces.size()) return false;

    const size_t stride = attribute.stride_bytes();
    if (!stride || attribute.data.size() % stride) return false;
    std::vector<uint8_t> expanded;
    expanded.reserve(triangleIndices.size() * stride);
    for (size_t corner = 0; corner < triangleIndices.size(); ++corner) {
        const size_t originalCorner = mesh.is_triangulated()
            ? (corner < mesh.triangulatedToOrigFaceVertexIndexMap.size()
                ? mesh.triangulatedToOrigFaceVertexIndexMap[corner] : SIZE_MAX)
            : corner;
        if (originalCorner >= mesh.usdFaceVertexIndices.size()) return false;
        size_t item = 0;
        switch (attribute.variability) {
        case tinyusdz::tydra::VertexVariability::Constant:
            break;
        case tinyusdz::tydra::VertexVariability::Uniform:
            item = sourceFaces[originalCorner];
            break;
        case tinyusdz::tydra::VertexVariability::FaceVarying:
            item = originalCorner;
            break;
        case tinyusdz::tydra::VertexVariability::Vertex:
        case tinyusdz::tydra::VertexVariability::Varying:
            item = mesh.usdFaceVertexIndices[originalCorner];
            break;
        default:
            return false;
        }
        if (item >= attribute.vertex_count()) return false;
        const uint8_t* value = attribute.data.data() + item * stride;
        expanded.insert(expanded.end(), value, value + stride);
    }
    attribute.data = std::move(expanded);
    attribute.indices.clear();
    attribute.variability = tinyusdz::tydra::VertexVariability::FaceVarying;
    return true;
}

bool ReadTexcoordPrimvar(const tinyusdz::GeomPrimvar& primvar, double time,
                         VertexAttribute& attribute, std::string& error)
{
    std::vector<tinyusdz::value::float2> values;
    if (!primvar.flatten_with_indices(
            time, &values, tinyusdz::value::TimeSampleInterpolationType::Linear, &error))
        return false;
    attribute.name = primvar.name();
    attribute.format = VertexAttributeFormat::Vec2;
    attribute.elementSize = 1;
    attribute.data.resize(values.size() * sizeof(values.front()));
    if (!values.empty())
        std::memcpy(attribute.data.data(), values.data(), attribute.data.size());
    switch (primvar.get_interpolation()) {
    case tinyusdz::Interpolation::Constant:
        attribute.variability = tinyusdz::tydra::VertexVariability::Constant; break;
    case tinyusdz::Interpolation::Uniform:
        attribute.variability = tinyusdz::tydra::VertexVariability::Uniform; break;
    case tinyusdz::Interpolation::Varying:
        attribute.variability = tinyusdz::tydra::VertexVariability::Varying; break;
    case tinyusdz::Interpolation::Vertex:
        attribute.variability = tinyusdz::tydra::VertexVariability::Vertex; break;
    case tinyusdz::Interpolation::FaceVarying:
        attribute.variability = tinyusdz::tydra::VertexVariability::FaceVarying; break;
    default:
        return false;
    }
    return true;
}

bool RecoverMissingTexcoords(const tinyusdz::Stage& stage, double time,
                             RenderScene& scene, uint32_t& warnings)
{
    for (RenderMesh& mesh : scene.meshes) {
        if (mesh.texcoords.contains(0)) continue;
        const auto name = MeshTexcoordName(scene, mesh);
        if (!name) continue;
        const Prim* prim = nullptr;
        std::string error;
        if (!stage.find_prim_at_path(tinyusdz::Path(mesh.abs_path, ""), prim, &error)
            || !prim) return false;
        const GeomMesh* sourceMesh = AsExact<GeomMesh>(*prim);
        if (!sourceMesh) return false;
        tinyusdz::GeomPrimvar primvar;
        if (!tinyusdz::tydra::GetGeomPrimvar(stage, sourceMesh, *name, &primvar, &error)) {
            warnings = (std::min)(64u, warnings + 1);
            continue;
        }
        VertexAttribute attribute;
        if (!ReadTexcoordPrimvar(primvar, time, attribute, error)
            || !ExpandPrimvarForTriangulatedMesh(mesh, attribute)) return false;
        mesh.texcoords.emplace(0, std::move(attribute));
        mesh.texcoordSlotIdMap.add(*name, 0);
    }
    return true;
}

struct GeometryRecord {
    uint32_t chunkId = 0;
    ChunkDescriptor descriptor{};
    int materialIndex = -1;
};

struct DecodedUsdImage {
    PixelFormatId format = PixelFormatId::Unknown;
    ColorSpaceId space = ColorSpaceId::Linear;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 0;
    std::vector<std::byte> pixels;
};

struct MaterialContext {
    const RenderScene* scene = nullptr;
    const TextureDecodeOptions* options = nullptr;
    ImportErrorCode error = ImportErrorCode::None;
    uint32_t optionalWarnings = 0;
    uint32_t textureWarnings = 0;
    uint64_t decodedBytes = 0;
    uint64_t decodedPixels = 0;
    std::unordered_map<uint64_t, uint32_t> imageIds;
};

void Warn(uint32_t& warnings) { warnings = (std::min)(64u, warnings + 1); }

bool ImageExtensionMatches(std::string_view path, SniffedImageFormat format)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return false;
    const std::string extension = FoldAscii(path.substr(dot));
    switch (format) {
    case SniffedImageFormat::Png: return extension == ".png";
    case SniffedImageFormat::Jpeg: return extension == ".jpg" || extension == ".jpeg";
    case SniffedImageFormat::Bmp: return extension == ".bmp";
    case SniffedImageFormat::Tiff: return extension == ".tif" || extension == ".tiff";
    case SniffedImageFormat::WebP: return extension == ".webp";
    case SniffedImageFormat::Ktx2: return extension == ".ktx2";
    // An unrecognized/corrupt payload is an optional-texture decode failure.
    // Only a positively identified format that disagrees with the authored
    // extension is a terminal type-mismatch classification.
    case SniffedImageFormat::Unknown: return true;
    default: return false;
    }
}

std::optional<DecodedUsdImage> DecodeImage(MaterialContext& context, int64_t imageIndex,
                                           ColorSpaceId space, TextureSemantic semantic)
{
    if (!context.scene || imageIndex < 0 || size_t(imageIndex) >= context.scene->images.size()) {
        Warn(context.textureWarnings);
        return std::nullopt;
    }
    const auto& source = context.scene->images[size_t(imageIndex)];
    if (source.buffer_id < 0 || size_t(source.buffer_id) >= context.scene->buffers.size()) {
        Warn(context.textureWarnings);
        return std::nullopt;
    }
    const auto& raw = context.scene->buffers[size_t(source.buffer_id)].data;
    const std::span encoded(reinterpret_cast<const std::byte*>(raw.data()), raw.size());
    if (encoded.empty() || encoded.size() > kUsdMaxEncodedTextureBytes) {
        Warn(context.textureWarnings);
        return std::nullopt;
    }
    const SniffedImageFormat sniffed = SniffImageFormat(encoded);
    if (!ImageExtensionMatches(source.asset_identifier, sniffed)) {
        context.error = ImportErrorCode::UnsafeReference;
        return std::nullopt;
    }
    TextureDecodeOptions options = context.options ? *context.options : TextureDecodeOptions{};
    options.semantic = semantic;
    const uint64_t remainingBytes = context.decodedBytes < kMaxAggregateTextureBytes
        ? kMaxAggregateTextureBytes - context.decodedBytes : 0;
    const uint64_t remainingPixels = context.decodedPixels < kMaxAggregateTexturePixels
        ? kMaxAggregateTexturePixels - context.decodedPixels : 0;
    options.maxDecodedBytes = (std::min)(options.maxDecodedBytes, remainingBytes);
    options.maxPixels = (std::min)(options.maxPixels, remainingPixels);

    DecodedUsdImage image;
    image.space = space;
    switch (sniffed) {
    case SniffedImageFormat::Ktx2:
        if (auto decoded = TranscodeKtx2BasisImage(encoded, options)) {
            image.format = decoded->pixelFormat;
            image.width = decoded->width; image.height = decoded->height;
            image.levels = decoded->mipLevels; image.pixels = std::move(decoded->pixelBytes);
        }
        break;
    case SniffedImageFormat::WebP:
        if (auto decoded = DecodeWebpImage(encoded, space, options)) {
            image.format = decoded->pixelFormat; image.space = decoded->colorSpace;
            image.width = decoded->width; image.height = decoded->height;
            image.levels = decoded->mipLevels; image.pixels = std::move(decoded->pixelBytes);
        }
        break;
    case SniffedImageFormat::Png:
    case SniffedImageFormat::Jpeg:
    case SniffedImageFormat::Bmp:
    case SniffedImageFormat::Tiff:
        if (auto decoded = DecodeRasterImageWic(encoded, space, options)) {
            image.format = decoded->pixelFormat; image.space = decoded->colorSpace;
            image.width = decoded->width; image.height = decoded->height;
            image.levels = decoded->mipLevels; image.pixels = std::move(decoded->pixelBytes);
        }
        break;
    default: break;
    }
    if (options.Cancelled()) {
        context.error = ImportErrorCode::Cancelled;
        return std::nullopt;
    }
    if (image.pixels.empty()) {
        Warn(context.textureWarnings);
        return std::nullopt;
    }
    const uint64_t pixels = uint64_t(image.width) * image.height;
    if (image.pixels.size() > remainingBytes || pixels > remainingPixels) {
        context.error = ImportErrorCode::ResourceLimit;
        return std::nullopt;
    }
    context.decodedBytes += image.pixels.size();
    context.decodedPixels += pixels;
    return image;
}

uint32_t EmitImage(BoundedChunkWriter& writer, const DecodedUsdImage& image,
                   uint32_t firstMip, uint32_t refines)
{
    if (firstMip >= image.levels) return 0;
    const auto prefix = firstMip
        ? ComputeImagePixelBytes(image.format, image.width, image.height, firstMip)
        : std::optional<uint64_t>{0};
    if (!prefix || *prefix > image.pixels.size()) return 0;
    ImagePayloadHeader header{};
    header.pixelFormat = uint32_t(image.format);
    header.width = (std::max)(1u, image.width >> firstMip);
    header.height = (std::max)(1u, image.height >> firstMip);
    header.mipLevels = image.levels - firstMip;
    header.colorSpace = uint32_t(image.space);
    header.reserved0 = refines;
    header.pixelDataByteSize = image.pixels.size() - *prefix;
    ChunkDescriptor descriptor{};
    descriptor.topology = ChunkTopology::Image;
    descriptor.chunkId = writer.NextId();
    if (!writer.Add(descriptor, ChunkBytes(header),
                    std::span(image.pixels).subspan(size_t(*prefix)))) return 0;
    return descriptor.chunkId;
}

uint32_t ResolveTexture(BoundedChunkWriter& writer, MaterialContext& context,
                        int textureIndex, ColorSpaceId space, TextureSemantic semantic)
{
    if (textureIndex < 0 || !context.scene || size_t(textureIndex) >= context.scene->textures.size())
        return 0;
    const auto& texture = context.scene->textures[size_t(textureIndex)];
    const uint64_t key = uint64_t(uint32_t(texture.texture_image_id))
        | (uint64_t(space) << 32) | (uint64_t(semantic) << 40);
    if (const auto found = context.imageIds.find(key); found != context.imageIds.end())
        return found->second;
    auto image = DecodeImage(context, texture.texture_image_id, space, semantic);
    if (!image) {
        if (context.error != ImportErrorCode::None) return 0;
        DecodedUsdImage fallback;
        fallback.format = PixelFormatId::RGBA8_UNORM;
        fallback.space = space;
        fallback.width = fallback.height = semantic == TextureSemantic::Color ? 2u : 1u;
        fallback.levels = 1;
        fallback.pixels.resize(size_t(fallback.width) * fallback.height * 4, std::byte{255});
        if (semantic == TextureSemantic::Color) {
            for (unsigned pixel = 0; pixel < 4; ++pixel)
                for (unsigned channel = 0; channel < 3; ++channel)
                    fallback.pixels[pixel * 4 + channel]
                        = std::byte((pixel == 0 || pixel == 3) ? 64 : 192);
        } else if (semantic == TextureSemantic::Normal) {
            fallback.pixels[0] = fallback.pixels[1] = std::byte{128};
        } else if (semantic == TextureSemantic::Emissive) {
            fallback.pixels[0] = fallback.pixels[1] = fallback.pixels[2] = std::byte{0};
        }
        image = std::move(fallback);
    }

    uint32_t firstMip = 0;
    while (firstMip + 1 < image->levels) {
        const auto prefix = ComputeImagePixelBytes(
            image->format, image->width, image->height, firstMip + 1);
        if (!prefix || image->pixels.size() - *prefix <= 64 * 1024) {
            ++firstMip;
            break;
        }
        ++firstMip;
    }
    const uint32_t initial = EmitImage(writer, *image, firstMip, 0);
    if (!initial) { context.error = writer.Error(); return 0; }
    if (firstMip && !EmitImage(writer, *image, 0, initial)) {
        context.error = writer.Error();
        return 0;
    }
    context.imageIds.emplace(key, initial);
    return initial;
}

bool EmitMaterials(BoundedChunkWriter& writer, const RenderScene& scene,
                   MaterialContext& context, std::vector<uint32_t>& materialIds)
{
    if (scene.materials.size() > kTierBMaterialLimit) {
        context.error = ImportErrorCode::ResourceLimit;
        return false;
    }
    materialIds.resize(scene.materials.size());
    std::vector<bool> doubleSided(scene.materials.size());
    for (const RenderMesh& mesh : scene.meshes) {
        if (mesh.doubleSided && mesh.material_id >= 0
            && size_t(mesh.material_id) < doubleSided.size()) doubleSided[size_t(mesh.material_id)] = true;
        if (mesh.doubleSided) for (const auto& [name, subset] : mesh.material_subsetMap) {
            (void)name;
            if (subset.material_id >= 0 && size_t(subset.material_id) < doubleSided.size())
                doubleSided[size_t(subset.material_id)] = true;
        }
    }
    for (size_t index = 0; index < scene.materials.size(); ++index) {
        if (context.options && context.options->Cancelled()) {
            context.error = ImportErrorCode::Cancelled;
            return false;
        }
        const auto& material = scene.materials[index];
        const auto& shader = material.surfaceShader;
        if (shader.useSpecularWorkflow || shader.specularColor.is_texture()
            || shader.clearcoat.is_texture() || shader.clearcoat.value != 0.0f
            || shader.clearcoatRoughness.is_texture()
            || shader.displacement.is_texture() || shader.displacement.value != 0.0f
            || shader.occlusion.is_texture() || shader.occlusion.value != 0.0f) {
            Warn(context.optionalWarnings);
        }
        MaterialPayload payload{};
        for (size_t channel = 0; channel < 3; ++channel) {
            payload.baseColorFactor[channel] = shader.diffuseColor.is_texture()
                ? 1.0f : shader.diffuseColor.value[channel];
            payload.emissiveFactor[channel] = shader.emissiveColor.is_texture()
                ? 1.0f : shader.emissiveColor.value[channel];
        }
        payload.baseColorFactor[3] = shader.opacity.is_texture()
            ? 1.0f : shader.opacity.value;
        payload.metallicFactor = shader.metallic.is_texture() ? 1.0f : shader.metallic.value;
        payload.roughnessFactor = shader.roughness.is_texture() ? 1.0f : shader.roughness.value;
        payload.uvScale[0] = payload.uvScale[1] = 1.0f;
        payload.alphaCutoff = shader.opacityThreshold.value > 0.0f
            ? shader.opacityThreshold.value : 0.5f;
        payload.alphaMode = uint32_t(shader.opacityThreshold.value > 0.0f
            ? AlphaModeId::Mask : shader.opacity.is_texture() || shader.opacity.value < 1.0f
                ? AlphaModeId::Blend : AlphaModeId::Opaque);
        payload.flags = kMaterialFlagFlipV
            | (doubleSided[index] ? kMaterialFlagDoubleSided : 0);
        for (float value : payload.baseColorFactor) if (!std::isfinite(value)) {
            context.error = ImportErrorCode::MalformedData; return false;
        }
        if (!std::isfinite(payload.metallicFactor) || !std::isfinite(payload.roughnessFactor)
            || !std::isfinite(payload.alphaCutoff)) {
            context.error = ImportErrorCode::MalformedData; return false;
        }
        for (float value : payload.emissiveFactor) if (!std::isfinite(value)) {
            context.error = ImportErrorCode::MalformedData; return false;
        }

        uint32_t images[4]{};
        images[0] = ResolveTexture(writer, context, shader.diffuseColor.texture_id,
                                   ColorSpaceId::Srgb, TextureSemantic::Color);
        if (context.error != ImportErrorCode::None) return false;
        if (shader.opacity.is_texture()
            && shader.opacity.texture_id != shader.diffuseColor.texture_id) {
            Warn(context.optionalWarnings);
        }
        if (shader.metallic.is_texture() && shader.roughness.is_texture()
            && shader.metallic.texture_id == shader.roughness.texture_id) {
            images[1] = ResolveTexture(writer, context, shader.roughness.texture_id,
                                       ColorSpaceId::Linear, TextureSemantic::Data);
        } else if (shader.metallic.is_texture() || shader.roughness.is_texture()) {
            Warn(context.optionalWarnings);
        }
        if (context.error != ImportErrorCode::None) return false;
        images[2] = ResolveTexture(writer, context, shader.normal.texture_id,
                                   ColorSpaceId::Linear, TextureSemantic::Normal);
        if (context.error != ImportErrorCode::None) return false;
        images[3] = ResolveTexture(writer, context, shader.emissiveColor.texture_id,
                                   ColorSpaceId::Srgb, TextureSemantic::Emissive);
        if (context.error != ImportErrorCode::None) return false;

        if (shader.diffuseColor.is_texture()
            && size_t(shader.diffuseColor.texture_id) < scene.textures.size()) {
            const auto& texture = scene.textures[size_t(shader.diffuseColor.texture_id)];
            if (texture.has_transform2d) {
                payload.uvOffset[0] = texture.tx_translation[0];
                payload.uvOffset[1] = texture.tx_translation[1];
                payload.uvScale[0] = texture.tx_scale[0];
                payload.uvScale[1] = texture.tx_scale[1];
                payload.uvRotation = texture.tx_rotation * (std::numbers::pi_v<float> / 180.0f);
                if (!std::isfinite(payload.uvOffset[0]) || !std::isfinite(payload.uvOffset[1])
                    || !std::isfinite(payload.uvScale[0]) || !std::isfinite(payload.uvScale[1])
                    || !std::isfinite(payload.uvRotation)) {
                    context.error = ImportErrorCode::MalformedData; return false;
                }
            }
            if (!texture.varname_uv.empty() && texture.varname_uv != "st")
                Warn(context.optionalWarnings);
        }
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::Material;
        descriptor.chunkId = writer.NextId();
        for (size_t slot = 0; slot < 4; ++slot) descriptor.dependencyIds[slot] = images[slot];
        descriptor.dependencyCount = uint32_t(std::ranges::count_if(images,
            [](uint32_t value) { return value != 0; }));
        if (!writer.Add(descriptor, ChunkBytes(payload))) {
            context.error = writer.Error(); return false;
        }
        materialIds[index] = descriptor.chunkId;
    }
    return true;
}

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

    std::vector<int> triangleMaterials(size_t(meshTriangles), mesh.material_id);
    std::vector<bool> assigned(static_cast<size_t>(meshTriangles), false);
    for (const auto& [name, subset] : mesh.material_subsetMap) {
        (void)name;
        for (const int face : subset.indices()) {
            if (face < 0 || uint64_t(face) >= meshTriangles || assigned[size_t(face)]) {
                error = ImportErrorCode::MalformedData;
                return false;
            }
            assigned[size_t(face)] = true;
            triangleMaterials[size_t(face)] = subset.material_id;
        }
    }

    for (uint64_t firstTriangle = 0; firstTriangle < meshTriangles;) {
        if (options.Cancelled()) { error = ImportErrorCode::Cancelled; return false; }
        uint64_t runEnd = firstTriangle + 1;
        while (runEnd < meshTriangles
               && triangleMaterials[size_t(runEnd)] == triangleMaterials[size_t(firstTriangle)])
            ++runEnd;
        const uint32_t count = static_cast<uint32_t>((std::min<uint64_t>)(
            chunkTriangleLimit, runEnd - firstTriangle));
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
        const bool hasBoundMaterial = triangleMaterials[size_t(firstTriangle)] >= 0;
        for (uint32_t triangle = 0; triangle < count; ++triangle) {
            for (uint32_t corner = 0; corner < 3; ++corner) {
                const uint32_t destinationCorner = mesh.is_rightHanded ? corner : 2 - corner;
                const size_t destinationIndex = size_t(triangle) * 3 + destinationCorner;
                const uint32_t sourceIndex = indices[(size_t(firstTriangle) + triangle) * 3 + corner];
                const size_t sourceCorner = (size_t(firstTriangle) + triangle) * 3 + corner;
                if (!MakeVertex(mesh, sourceIndex, sourceCorner, hasBoundMaterial, origin,
                                vertices[destinationIndex], hasUv, hasColor)) {
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
        records.push_back(GeometryRecord{
            descriptor.chunkId, descriptor, triangleMaterials[size_t(firstTriangle)]});
        firstTriangle += count;
    }
    triangleCount += meshTriangles;
    vertexCount += meshTriangles * 3;
    return true;
}

struct FlatNode {
    const Node* node = nullptr;
    uint32_t parentIndex = UINT32_MAX;
    uint32_t transformParentIndex = UINT32_MAX;
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
    nodes.push_back(FlatNode{
        &node, parentIndex, node.has_resetXform ? UINT32_MAX : parentIndex, visible});
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
    if (options.format != SourceFormatId::Usda && options.format != SourceFormatId::Usdc
        && options.format != SourceFormatId::Usdz)
        return Fail(ImportErrorCode::UnsupportedEncoding);
    if (options.format == SourceFormatId::Usdz && !options.archive)
        return Fail(ImportErrorCode::ArchiveLimit);
    if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
    if (!TierBScratchLimit()) return Fail(ImportErrorCode::ScratchLimit);

    tinyusdz::USDLoadOptions loadOptions{};
    loadOptions.num_threads = 1;
    loadOptions.max_memory_limit_in_mb = 1536; // advisory; Job commit is authoritative
    loadOptions.max_allowed_asset_size_in_mb = 256;
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
        ? "broker-primary.usda" : options.format == SourceFormatId::Usdc
            ? "broker-primary.usdc" : "broker-primary.usd";
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

    // Ignore skeletal bindings and preview the authored rest pose; see
    // StripSkeletonBindings. This must precede the classification and the
    // render-scene conversion.
    uint32_t skeletonBindings = 0;
    for (Prim& root : stage.root_prims()) skeletonBindings += StripSkeletonBindings(root);

    // This classification is intentionally before BoundedChunkWriter exists:
    // UnsupportedComposition can never follow candidate publication.
    StagePolicy policy = ClassifyStage(stage, time);
    if (skeletonBindings) SaturatingWarn(policy);
    if (policy.primCount > kTierBObjectLimit) return Fail(ImportErrorCode::ResourceLimit);
    if (policy.hasComposition) return Fail(ImportErrorCode::UnsupportedComposition,
                                           ImportFailurePhase::Geometry);
    if (policy.unsupportedRequired)
        return Fail(ImportErrorCode::UnsupportedRequiredFeature);
    if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);

    tinyusdz::tydra::RenderSceneConverterEnv environment(stage);
    environment.usd_filename = options.format == SourceFormatId::Usdz
        ? options.archive->rootLayerName : syntheticName;
    environment.timecode = options.format == SourceFormatId::Usdz
        ? tinyusdz::value::TimeCode::Default() : time;
    environment.tinterp = tinyusdz::value::TimeSampleInterpolationType::Linear;
    environment.scene_config.load_texture_assets = false;
    environment.mesh_config.triangulate = true;
    environment.mesh_config.validate_geomsubset = true;
    // The wire format is deindexed, so rebuilding a synthetic single index is
    // unnecessary and loses interpolation information needed by some USD UV sets.
    environment.mesh_config.build_vertex_indices = false;
    environment.mesh_config.compute_normals = true;
    environment.mesh_config.compute_tangents_and_binormals = false;
    environment.material_config.texture_image_loader_function = nullptr;
    environment.material_config.allow_missing_asset = true;
    environment.material_config.allow_texture_load_failure = true;
    AssetContext assets;
    assets.source = sourceBytes;
    assets.archive = options.archive;
    assets.sidecars = options.sidecars;
    assets.maxBytes = options.maxAggregateDependencyBytes;
    assets.maxCount = options.maxDependencyCount;
    tinyusdz::AssetResolutionHandler assetHandler{};
    assetHandler.resolve_fun = ResolveAsset;
    assetHandler.size_fun = SizeAsset;
    assetHandler.read_fun = ReadAsset;
    assetHandler.userdata = &assets;
    environment.asset_resolver.register_wildcard_asset_resolution_handler(assetHandler);

    RenderScene scene;
    tinyusdz::tydra::RenderSceneConverter converter;
    if (!converter.ConvertToRenderScene(environment, &scene))
        return Fail(assets.error != ImportErrorCode::None ? assets.error
                                                          : ImportErrorCode::MalformedData,
                    assets.error != ImportErrorCode::None
                        ? ImportFailurePhase::Sidecars : ImportFailurePhase::Geometry);
    if (assets.error != ImportErrorCode::None)
        return Fail(assets.error, ImportFailurePhase::Sidecars);
    if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
    if (scene.meshes.empty()) return Fail(ImportErrorCode::EmptyGeometry);
    if (!RecoverMissingTexcoords(stage, time, scene, policy.optionalWarnings))
        return Fail(ImportErrorCode::MalformedData, ImportFailurePhase::Geometry);
    if (scene.meshes.size() > kTierBObjectLimit)
        return Fail(ImportErrorCode::ResourceLimit);
    if (scene.materials.size() > kTierBMaterialLimit
        || scene.images.size() > uint64_t(kTierBMaterialLimit) * 4
        || scene.textures.size() > uint64_t(kTierBMaterialLimit) * 4)
        return Fail(ImportErrorCode::ResourceLimit, ImportFailurePhase::Textures);
    // Resolver/path/type classification is complete before a writer exists,
    // so an unsafe late texture cannot follow candidate publication.
    for (const auto& image : scene.images) {
        if (image.buffer_id < 0) continue; // optional missing asset; deterministic fallback later
        if (size_t(image.buffer_id) >= scene.buffers.size())
            return Fail(ImportErrorCode::MalformedData, ImportFailurePhase::Textures);
        const auto& raw = scene.buffers[size_t(image.buffer_id)].data;
        const std::span encoded(reinterpret_cast<const std::byte*>(raw.data()), raw.size());
        if (encoded.empty() || encoded.size() > kUsdMaxEncodedTextureBytes)
            return Fail(ImportErrorCode::ResourceLimit, ImportFailurePhase::Textures);
        if (!ImageExtensionMatches(image.asset_identifier, SniffImageFormat(encoded)))
            return Fail(ImportErrorCode::UnsafeReference, ImportFailurePhase::Sidecars);
    }

    std::vector<FlatNode> flatNodes;
    flatNodes.reserve(policy.primCount);
    for (const Node& root : scene.nodes) FlattenNodes(root, UINT32_MAX, policy, flatNodes);
    if (flatNodes.empty() || flatNodes.size() > kTierBObjectLimit)
        return Fail(flatNodes.empty() ? ImportErrorCode::EmptyGeometry
                                      : ImportErrorCode::ResourceLimit);
    // TinyUSDZ's render-scene normalization is the bounded fast path. Deep,
    // highly articulated exports can require OpenUSD's full xform evaluator;
    // hand those scenes to the compatibility host before publishing chunks.
    constexpr size_t kFastUsdNodeLimit = 512;
    if (flatNodes.size() > kFastUsdNodeLimit)
        return Fail(ImportErrorCode::UnsupportedComposition,
                    ImportFailurePhase::Geometry);

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

    MaterialContext materialContext;
    materialContext.scene = &scene;
    materialContext.options = options.textureOptions;
    std::vector<uint32_t> materialIds;
    if (!EmitMaterials(writer, scene, materialContext, materialIds))
        return Fail(materialContext.error == ImportErrorCode::None
                        ? writer.Error() : materialContext.error,
                    ImportFailurePhase::Textures);

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
        if (source.transformParentIndex != UINT32_MAX)
            payload.parentNodeId = nodeIds[source.transformParentIndex];
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
            if (record.materialIndex >= 0) {
                if (size_t(record.materialIndex) >= materialIds.size())
                    return Fail(ImportErrorCode::MalformedData);
                payload.materialChunkId = materialIds[size_t(record.materialIndex)];
            }
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
            if (record.materialIndex >= 0) {
                if (size_t(record.materialIndex) >= materialIds.size())
                    return Fail(ImportErrorCode::MalformedData);
                payload.materialChunkId = materialIds[size_t(record.materialIndex)];
            }
            payload.flags = source.visible ? kSceneRecordVisible : 0;
            if (!TransformBounds(record.descriptor, source.worldMatrix,
                                 payload.worldMin, payload.worldMax))
                return Fail(ImportErrorCode::MalformedData);
            if (!writer.AddInstance(payload)) return Fail(writer.Error());
        }
    }
    if (!instanceCount) return Fail(ImportErrorCode::EmptyGeometry);

    if (policy.optionalWarnings || materialContext.optionalWarnings || assets.warnings
        || materialContext.textureWarnings || !warning.empty() || !converter.GetWarning().empty()) {
        ImportStatusPayload status{};
        status.optionalFeatureWarnings = (std::min)(64u, policy.optionalWarnings
            + materialContext.optionalWarnings + assets.warnings
            + uint32_t(!warning.empty()) + uint32_t(!converter.GetWarning().empty()));
        status.textureWarnings = materialContext.textureWarnings;
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::ImportStatus;
        descriptor.chunkId = writer.NextId();
        if (!writer.Add(descriptor, ChunkBytes(status))) return Fail(writer.Error());
    }
    if (!writer.Complete()) return Fail(writer.Error());
    return UsdImportResult{writer.Count(), writer.Length()};
}

} // namespace import_worker

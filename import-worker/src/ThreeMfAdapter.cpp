#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ThreeMfAdapter.h"

#include "BoundedChunkWriter.h"
#include "ChunkBatchSink.h"
#include "ImageFormatSniff.h"
#include "TextureDecodePolicy.h"
#include "ThreeMfDisplayProperties.h"
#include "WicImageDecodeAdapter.h"
#include "model_core/GeometryBounds.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/TierALimits.h"
#include "model_core/VertexLayouts.h"

#include <Bindings/Cpp/lib3mf_implicit.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
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
        || type == Lib3MF::eObjectType::SolidSupport || type == Lib3MF::eObjectType::Surface
        // Some slicers reference a mesh marked "other" from a model component.
        // Core forbids that in a build, but the mesh is still valid for preview.
        || type == Lib3MF::eObjectType::Other;
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
            // 3MF uses row vectors: a component is transformed into its
            // containing object before the build item's placement is applied.
            if (!Multiply(local, parent, world)
                || !Visit(component->GetObjectResource(), world, depth + 1)) { recursion.erase(key); return false; }
        }
        recursion.erase(key);
        return true;
    }
};

struct GeometryRecord {
    uint32_t chunkId = 0;
    ChunkDescriptor descriptor{};
    uint32_t materialId = 0;
};

float SrgbToLinear(uint8_t value)
{
    const float c = float(value) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

struct PropertySample {
    float color[4]{1, 1, 1, 1};
    float uv[2]{};
    uint32_t textureId = 0;
    float metallic = 0.0f;
    float roughness = 1.0f;
    uint32_t samplerFlags = 0;
    bool hasUv = false;
    bool hasPbr = false;
    bool textureLayer = false;
    bool textureMix = false;
    bool colorSrgb = false;
};

struct MaterialKey {
    uint32_t textureId = 0;
    uint32_t metallic = 0;
    uint32_t roughness = 0;
    uint32_t samplerFlags = 0;
    bool blend = false;
    bool textureLayer = false;
    bool textureMix = false;
    bool vertexSrgb = false;
    bool operator==(const MaterialKey&) const = default;
};

struct MaterialKeyHash {
    size_t operator()(const MaterialKey& key) const noexcept
    {
        uint64_t hash = 1469598103934665603ull;
        const uint32_t values[]{key.textureId, key.metallic, key.roughness,
                                key.samplerFlags, key.blend ? 1u : 0u,
                                key.textureLayer ? 1u : 0u, key.textureMix ? 1u : 0u,
                                key.vertexSrgb ? 1u : 0u};
        for (uint32_t value : values) { hash ^= value; hash *= 1099511628211ull; }
        return size_t(hash);
    }
};

struct DecodedTexture {
    PixelFormatId format = PixelFormatId::Unknown;
    uint32_t width = 0, height = 0, levels = 0;
    std::vector<std::byte> pixels;
};

void Warn(uint32_t& count) { count = (std::min)(64u, count + 1); }

TextureAddressId AddressMode(Lib3MF::eTextureTileStyle style)
{
    switch (style) {
    case Lib3MF::eTextureTileStyle::Wrap: return TextureAddressId::Wrap;
    case Lib3MF::eTextureTileStyle::Mirror: return TextureAddressId::Mirror;
    case Lib3MF::eTextureTileStyle::Clamp: return TextureAddressId::Clamp;
    case Lib3MF::eTextureTileStyle::NoTileStyle: return TextureAddressId::None;
    default: return TextureAddressId::None;
    }
}

class PropertyCatalog {
public:
    PropertyCatalog(const Lib3MF::PModel& model, BoundedChunkWriter& writer,
                    const ThreeMfImportOptions& options)
        : model_(model), writer_(writer), options_(options) {}

    ImportErrorCode Error() const { return error_; }
    void SetError(ImportErrorCode error) { if (error_ == ImportErrorCode::None) error_ = error; }
    uint32_t WarningCount() const { return warnings_; }

    bool ValidateDisplayProperties()
    {
        std::unordered_set<std::string> seen;
        auto resources = model_->GetResources();
        if (!resources) { error_ = ImportErrorCode::MalformedData; return false; }
        while (resources->MoveNext()) {
            if (options_.Cancelled()) { error_ = ImportErrorCode::Cancelled; return false; }
            const auto resource = resources->GetCurrent();
            std::string key;
            if (!ResourceKey(resource, key)) continue;
            if (!resourcesByKey_.emplace(key, resource).second) {
                error_ = ImportErrorCode::MalformedData; return false;
            }
            if (!options_.displayCatalog) continue;
            const auto association = options_.displayCatalog->associations.find(key);
            if (association == options_.displayCatalog->associations.end()) continue;
            seen.insert(key);
            const auto group = options_.displayCatalog->groups.find(association->second);
            if (group == options_.displayCatalog->groups.end()) {
                error_ = ImportErrorCode::MalformedData; return false;
            }
            if (group->second.kind == ThreeMfDisplayKind::Unsupported) continue;
            std::vector<uint32_t> ids;
            if (!PropertyIds(resource, ids)) return false;
            if (ids.size() != group->second.properties.size()) {
                error_ = ImportErrorCode::MalformedData; return false;
            }
        }
        if (options_.displayCatalog
            && seen.size() != options_.displayCatalog->associations.size()) {
            error_ = ImportErrorCode::MalformedData; return false;
        }
        return true;
    }

    bool Resolve(uint32_t resourceId, uint32_t propertyId, PropertySample& sample)
    {
        std::unordered_set<uint64_t> recursion;
        return ResolveInner(resourceId, propertyId, sample, recursion, 0);
    }

    bool ResolveLatticeProperty(std::string_view packagePart,
                                const ThreeMfLatticePropertyRef& latticeDefault,
                                const ThreeMfLatticePropertyRef& element,
                                uint32_t objectResource, uint32_t objectProperty,
                                PropertySample& sample)
    {
        uint32_t uniqueResource = objectResource;
        uint32_t propertyIndex = 0;
        bool hasProperty = objectResource != 0;
        if (hasProperty) {
            const auto resource = model_->GetResourceByID(objectResource);
            if (!PropertyIndex(resource, objectProperty, propertyIndex)) return false;
        }

        auto apply = [&](const ThreeMfLatticePropertyRef& reference) -> bool {
            if (reference.hasResource) {
                const auto found = resourcesByKey_.find(
                    ThreeMfResourceKey(packagePart, reference.resourceId));
                if (found == resourcesByKey_.end()) {
                    error_ = ImportErrorCode::MalformedData; return false;
                }
                uniqueResource = found->second->GetUniqueResourceID();
                hasProperty = true;
            }
            if (reference.hasProperty) {
                propertyIndex = reference.propertyIndex;
                hasProperty = true;
            }
            return true;
        };
        if (!apply(latticeDefault) || !apply(element)) return false;
        if (!hasProperty) {
            sample.color[0] = sample.color[1] = sample.color[2] = 0.8f;
            return true;
        }
        if (!uniqueResource) { error_ = ImportErrorCode::MalformedData; return false; }
        const auto resource = model_->GetResourceByID(uniqueResource);
        std::vector<uint32_t> ids;
        if (!PropertyIds(resource, ids)) return false;
        if (propertyIndex >= ids.size()) { error_ = ImportErrorCode::MalformedData; return false; }
        return Resolve(uniqueResource, ids[propertyIndex], sample);
    }

    uint32_t MaterialFor(PropertySample samples[3])
    {
        if (error_ != ImportErrorCode::None) return 0;
        const uint32_t texture = samples[0].textureId;
        float metallic = 0.0f, roughness = 0.0f;
        for (unsigned corner = 0; corner < 3; ++corner) {
            metallic += samples[corner].metallic / 3.0f;
            roughness += samples[corner].roughness / 3.0f;
        }
        uint32_t sampler = samples[0].samplerFlags;
        const bool textureLayer = samples[0].textureLayer;
        const bool textureMix = samples[0].textureMix;
        bool vertexSrgb = samples[0].colorSrgb;
        for (unsigned corner = 1; corner < 3; ++corner)
            if (samples[corner].colorSrgb != vertexSrgb) {
                for (auto& sample : std::span<PropertySample, 3>(samples, 3)) Linearize(sample);
                vertexSrgb = false;
                break;
            }
        bool blend = false;
        for (unsigned corner = 0; corner < 3; ++corner) {
            blend |= samples[corner].color[3] < 0.99999f || samples[corner].textureLayer;
            if (samples[corner].textureId != texture
                || samples[corner].samplerFlags != sampler
                || samples[corner].textureLayer != textureLayer
                || samples[corner].textureMix != textureMix) {
                error_ = ImportErrorCode::UnsupportedRequiredFeature;
                return 0;
            }
        }
        MaterialKey key{texture, std::bit_cast<uint32_t>(metallic),
                        std::bit_cast<uint32_t>(roughness), sampler, blend,
                        textureLayer, textureMix, vertexSrgb};
        if (const auto found = materials_.find(key); found != materials_.end()) return found->second;
        if (materials_.size() >= kTierBMaterialLimit) {
            error_ = ImportErrorCode::ResourceLimit; return 0;
        }
        uint32_t imageId = 0;
        if (texture) {
            imageId = ResolveImage(texture);
            if (!imageId) return 0;
        }
        MaterialPayload payload{};
        payload.baseColorFactor[0] = payload.baseColorFactor[1]
            = payload.baseColorFactor[2] = payload.baseColorFactor[3] = 1.0f;
        payload.metallicFactor = metallic;
        payload.roughnessFactor = roughness;
        payload.uvScale[0] = payload.uvScale[1] = 1.0f;
        payload.alphaMode = uint32_t(blend ? AlphaModeId::Blend : AlphaModeId::Opaque);
        payload.alphaCutoff = 0.5f;
        payload.flags = sampler
            | (textureLayer ? kMaterialFlagTextureLayer : 0)
            | (textureMix ? kMaterialFlagTextureMix : 0)
            | (vertexSrgb ? kMaterialFlagVertexSrgb : 0);
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::Material;
        descriptor.chunkId = writer_.NextId();
        if (imageId) { descriptor.dependencyIds[0] = imageId; descriptor.dependencyCount = 1; }
        if (!writer_.Add(descriptor, ChunkBytes(payload))) { error_ = writer_.Error(); return 0; }
        materials_.emplace(key, descriptor.chunkId);
        return descriptor.chunkId;
    }

private:
    bool ResourceKey(const Lib3MF::PResource& resource, std::string& key)
    {
        if (!resource) return false;
        const auto part = resource->PackagePart();
        if (!part || !resource->GetModelResourceID()) return false;
        key = ThreeMfResourceKey(part->GetPath(), resource->GetModelResourceID());
        return true;
    }

    bool PropertyIds(const Lib3MF::PResource& resource, std::vector<uint32_t>& ids)
    {
        ids.clear();
        if (!resource) { error_ = ImportErrorCode::MalformedData; return false; }
        const uint32_t resourceId = resource->GetUniqueResourceID();
        switch (model_->GetPropertyTypeByID(resourceId)) {
        case Lib3MF::ePropertyType::BaseMaterial: {
            const auto group = model_->GetBaseMaterialGroupByID(resourceId);
            if (!group) { error_ = ImportErrorCode::MalformedData; return false; }
            group->GetAllPropertyIDs(ids); break;
        }
        case Lib3MF::ePropertyType::Colors: {
            const auto group = model_->GetColorGroupByID(resourceId);
            if (!group) { error_ = ImportErrorCode::MalformedData; return false; }
            group->GetAllPropertyIDs(ids); break;
        }
        case Lib3MF::ePropertyType::TexCoord: {
            const auto group = model_->GetTexture2DGroupByID(resourceId);
            if (!group) { error_ = ImportErrorCode::MalformedData; return false; }
            group->GetAllPropertyIDs(ids); break;
        }
        case Lib3MF::ePropertyType::Composite: {
            const auto group = model_->GetCompositeMaterialsByID(resourceId);
            if (!group) { error_ = ImportErrorCode::MalformedData; return false; }
            group->GetAllPropertyIDs(ids); break;
        }
        case Lib3MF::ePropertyType::Multi: {
            const auto group = model_->GetMultiPropertyGroupByID(resourceId);
            if (!group) { error_ = ImportErrorCode::MalformedData; return false; }
            group->GetAllPropertyIDs(ids); break;
        }
        default: error_ = ImportErrorCode::MalformedData; return false;
        }
        if (ids.size() > kTierBMaterialLimit) {
            error_ = ImportErrorCode::ResourceLimit; return false;
        }
        return true;
    }

    bool PropertyIndex(const Lib3MF::PResource& resource, uint32_t propertyId,
                       uint32_t& index)
    {
        if (!resource) { error_ = ImportErrorCode::MalformedData; return false; }
        const uint32_t resourceId = resource->GetUniqueResourceID();
        auto found = propertyIndices_.find(resourceId);
        if (found == propertyIndices_.end()) {
            std::vector<uint32_t> ids;
            if (!PropertyIds(resource, ids)) return false;
            std::unordered_map<uint32_t, uint32_t> indices;
            indices.reserve(ids.size());
            for (uint32_t ordinal = 0; ordinal < ids.size(); ++ordinal)
                if (!indices.emplace(ids[ordinal], ordinal).second) {
                    error_ = ImportErrorCode::MalformedData; return false;
                }
            found = propertyIndices_.emplace(resourceId, std::move(indices)).first;
        }
        const auto property = found->second.find(propertyId);
        if (property == found->second.end()) {
            error_ = ImportErrorCode::MalformedData; return false;
        }
        index = property->second;
        return true;
    }

    void Linearize(PropertySample& sample)
    {
        if (!sample.colorSrgb) return;
        for (unsigned channel = 0; channel < 3; ++channel)
            sample.color[channel] = SrgbToLinear(uint8_t(std::lround(sample.color[channel] * 255.0f)));
        sample.colorSrgb = false;
    }

    bool ApplyDisplay(const Lib3MF::PResource& resource, uint32_t propertyId,
                      PropertySample& sample)
    {
        if (!options_.displayCatalog) return true;
        std::string key;
        if (!ResourceKey(resource, key)) { error_ = ImportErrorCode::MalformedData; return false; }
        const auto association = options_.displayCatalog->associations.find(key);
        if (association == options_.displayCatalog->associations.end()) return true;
        const auto group = options_.displayCatalog->groups.find(association->second);
        if (group == options_.displayCatalog->groups.end()) {
            error_ = ImportErrorCode::MalformedData; return false;
        }
        if (group->second.kind == ThreeMfDisplayKind::Unsupported) {
            if (warnedDisplayGroups_.insert(association->second).second) Warn(warnings_);
            return true;
        }
        uint32_t propertyIndex = 0;
        if (!PropertyIndex(resource, propertyId, propertyIndex)) return false;
        if (propertyIndex >= group->second.properties.size()) {
            error_ = ImportErrorCode::MalformedData; return false;
        }
        const auto& display = group->second.properties[propertyIndex];
        Linearize(sample);
        if (group->second.kind == ThreeMfDisplayKind::Metallic) {
            sample.metallic = display.metallic;
            sample.roughness = display.roughness;
        } else {
            constexpr float dielectric = 0.04f;
            const float specularStrength = (std::max)({display.specular[0], display.specular[1],
                                                       display.specular[2]});
            const float metallic = std::clamp((specularStrength - dielectric)
                                               / (1.0f - dielectric), 0.0f, 1.0f);
            float diffuseBase[3]{};
            float specularBase[3]{};
            for (unsigned channel = 0; channel < 3; ++channel) {
                diffuseBase[channel] = sample.color[channel]
                    / (std::max)((1.0f - metallic) * (1.0f - dielectric), 1.0e-6f);
                specularBase[channel] = display.specular[channel] / (std::max)(metallic, 1.0e-6f);
                sample.color[channel] = std::clamp(
                    diffuseBase[channel] * (1.0f - metallic)
                    + specularBase[channel] * metallic, 0.0f, 1.0f);
            }
            sample.metallic = metallic;
            sample.roughness = display.roughness;
        }
        sample.hasPbr = true;
        return true;
    }

    bool Color(const Lib3MF::sColor& source, PropertySample& sample)
    {
        sample.color[0] = float(source.m_Red) / 255.0f;
        sample.color[1] = float(source.m_Green) / 255.0f;
        sample.color[2] = float(source.m_Blue) / 255.0f;
        sample.color[3] = float(source.m_Alpha) / 255.0f;
        sample.colorSrgb = true;
        return true;
    }

    bool ResolveInner(uint32_t resourceId, uint32_t propertyId, PropertySample& sample,
                      std::unordered_set<uint64_t>& recursion, uint32_t depth)
    {
        if (options_.Cancelled()) { error_ = ImportErrorCode::Cancelled; return false; }
        if (!resourceId || depth > 32) { error_ = ImportErrorCode::MalformedData; return false; }
        const uint64_t recursionKey = (uint64_t(resourceId) << 32) | propertyId;
        if (!recursion.insert(recursionKey).second) { error_ = ImportErrorCode::MalformedData; return false; }
        const auto finish = [&](bool result) { recursion.erase(recursionKey); return result; };
        switch (model_->GetPropertyTypeByID(resourceId)) {
        case Lib3MF::ePropertyType::BaseMaterial: {
            const auto group = model_->GetBaseMaterialGroupByID(resourceId);
            if (!group) return finish(false);
            if (!Color(group->GetDisplayColor(propertyId), sample)) return finish(false);
            return finish(ApplyDisplay(group, propertyId, sample));
        }
        case Lib3MF::ePropertyType::Colors: {
            const auto group = model_->GetColorGroupByID(resourceId);
            if (!group) return finish(false);
            if (!Color(group->GetColor(propertyId), sample)) return finish(false);
            return finish(ApplyDisplay(group, propertyId, sample));
        }
        case Lib3MF::ePropertyType::TexCoord: {
            const auto group = model_->GetTexture2DGroupByID(resourceId);
            if (!group || !group->GetTexture2D()) return finish(false);
            const auto coordinate = group->GetTex2Coord(propertyId);
            if (!Finite(coordinate.m_U) || !Finite(coordinate.m_V)) return finish(false);
            sample.uv[0] = float(coordinate.m_U); sample.uv[1] = float(coordinate.m_V);
            sample.hasUv = true;
            const auto texture = group->GetTexture2D();
            sample.textureId = texture->GetUniqueResourceID();
            Lib3MF::eTextureTileStyle u{}, v{};
            texture->GetTileStyleUV(u, v);
            sample.samplerFlags = MaterialAddressFlags(AddressMode(u), AddressMode(v))
                | kMaterialFlagFlipV;
            if (texture->GetFilter() == Lib3MF::eTextureFilter::Nearest)
                sample.samplerFlags |= kMaterialFlagNearest;
            return finish(sample.textureId != 0 && ApplyDisplay(group, propertyId, sample));
        }
        case Lib3MF::ePropertyType::Composite: {
            const auto composite = model_->GetCompositeMaterialsByID(resourceId);
            if (!composite || !composite->GetBaseMaterialGroup()) return finish(false);
            std::vector<Lib3MF::sCompositeConstituent> values;
            composite->GetComposite(propertyId, values);
            if (values.empty() || values.size() > kTierBMaterialLimit) return finish(false);
            double sum = 0.0;
            for (const auto& value : values) {
                if (!Finite(value.m_MixingRatio) || value.m_MixingRatio < 0.0) return finish(false);
                sum += value.m_MixingRatio;
            }
            if (!Finite(sum)) return finish(false);
            const bool equalWeights = sum == 0.0;
            if (equalWeights) sum = double(values.size());
            float mixed[4]{};
            for (const auto& value : values) {
                PropertySample constituent;
                Color(composite->GetBaseMaterialGroup()->GetDisplayColor(value.m_PropertyID), constituent);
                const float weight = equalWeights ? 1.0f : float(value.m_MixingRatio);
                for (unsigned channel = 0; channel < 3; ++channel)
                    mixed[channel] += SrgbToLinear(uint8_t(std::lround(constituent.color[channel] * 255.0f)))
                        * weight;
                mixed[3] += constituent.color[3] * weight;
            }
            for (unsigned channel = 0; channel < 4; ++channel) sample.color[channel] = mixed[channel] / float(sum);
            sample.colorSrgb = false;
            return finish(ApplyDisplay(composite, propertyId, sample));
        }
        case Lib3MF::ePropertyType::Multi: {
            const auto group = model_->GetMultiPropertyGroupByID(resourceId);
            if (!group) return finish(false);
            const uint32_t layers = group->GetLayerCount();
            std::vector<uint32_t> indices;
            group->GetMultiProperty(propertyId, indices);
            if (!layers || layers != indices.size() || layers > 16) return finish(false);
            PropertySample accumulated;
            bool initialized = false;
            for (uint32_t layerIndex = 0; layerIndex < layers; ++layerIndex) {
                const auto layer = group->GetLayer(layerIndex);
                PropertySample current;
                if (!ResolveInner(layer.m_ResourceID, indices[layerIndex], current,
                                  recursion, depth + 1)) return finish(false);
                if (!initialized) { accumulated = current; initialized = true; continue; }
                if (accumulated.colorSrgb) {
                    for (unsigned channel = 0; channel < 3; ++channel)
                        accumulated.color[channel] = SrgbToLinear(uint8_t(std::lround(accumulated.color[channel] * 255.0f)));
                    accumulated.colorSrgb = false;
                }
                if (current.colorSrgb) {
                    for (unsigned channel = 0; channel < 3; ++channel)
                        current.color[channel] = SrgbToLinear(uint8_t(std::lround(current.color[channel] * 255.0f)));
                    current.colorSrgb = false;
                }
                if (current.textureId) {
                    if (accumulated.textureId) {
                        error_ = ImportErrorCode::UnsupportedRequiredFeature;
                        return finish(false);
                    }
                    accumulated.textureId = current.textureId;
                    accumulated.uv[0] = current.uv[0]; accumulated.uv[1] = current.uv[1];
                    accumulated.hasUv = current.hasUv;
                    accumulated.samplerFlags = current.samplerFlags;
                    accumulated.textureLayer = true;
                    accumulated.textureMix = layer.m_TheBlendMethod == Lib3MF::eBlendMethod::Mix;
                    continue;
                }
                if (accumulated.textureId) {
                    error_ = ImportErrorCode::UnsupportedRequiredFeature;
                    return finish(false);
                }
                if (layer.m_TheBlendMethod == Lib3MF::eBlendMethod::Multiply) {
                    for (unsigned channel = 0; channel < 4; ++channel)
                        accumulated.color[channel] *= current.color[channel];
                } else if (layer.m_TheBlendMethod == Lib3MF::eBlendMethod::Mix) {
                    const float alpha = current.color[3];
                    for (unsigned channel = 0; channel < 3; ++channel)
                        accumulated.color[channel] = accumulated.color[channel] * (1.0f - alpha)
                            + current.color[channel] * alpha;
                    accumulated.color[3] = alpha + accumulated.color[3] * (1.0f - alpha);
                } else return finish(false);
                if (current.hasPbr && !accumulated.hasPbr) {
                    accumulated.metallic = current.metallic;
                    accumulated.roughness = current.roughness;
                    accumulated.hasPbr = true;
                } else if (current.hasPbr) Warn(warnings_);
            }
            sample = accumulated;
            return finish(initialized && ApplyDisplay(group, propertyId, sample));
        }
        default: error_ = ImportErrorCode::UnsupportedRequiredFeature; return finish(false);
        }
    }

    uint32_t ResolveImage(uint32_t textureId)
    {
        if (const auto found = images_.find(textureId); found != images_.end()) return found->second;
        const auto texture = model_->GetTexture2DByID(textureId);
        if (!texture || !texture->GetAttachment()) { error_ = ImportErrorCode::MalformedData; return 0; }
        const auto attachment = texture->GetAttachment();
        const uint64_t encodedSize = attachment->GetStreamSize();
        constexpr uint64_t kMaxEncodedTextureBytes = 256ull * 1024 * 1024;
        if (!encodedSize || encodedSize > kMaxEncodedTextureBytes) {
            error_ = ImportErrorCode::ResourceLimit; return 0;
        }
        std::vector<Lib3MF_uint8> encodedRaw;
        attachment->WriteToBuffer(encodedRaw);
        if (encodedRaw.size() != encodedSize) { error_ = ImportErrorCode::MalformedData; return 0; }
        const std::span<const std::byte> encoded(
            reinterpret_cast<const std::byte*>(encodedRaw.data()), encodedRaw.size());
        const auto sniffed = SniffImageFormat(encoded);
        const bool mimeMatches = (texture->GetContentType() == Lib3MF::eTextureType::PNG
                                  && sniffed == SniffedImageFormat::Png)
            || (texture->GetContentType() == Lib3MF::eTextureType::JPEG
                && sniffed == SniffedImageFormat::Jpeg);
        if (!mimeMatches && sniffed != SniffedImageFormat::Unknown) {
            error_ = ImportErrorCode::MalformedData;
            return 0;
        }
        std::optional<DecodedRasterImage> decoded;
        const uint64_t remainingBytes = decodedBytes_ < options_.maxAggregateTextureBytes
            ? options_.maxAggregateTextureBytes - decodedBytes_ : 0;
        const uint64_t remainingPixels = decodedPixels_ < options_.maxAggregateTexturePixels
            ? options_.maxAggregateTexturePixels - decodedPixels_ : 0;
        TextureDecodeOptions decodeOptions;
        decodeOptions.isCancelled = options_.isCancelled;
        decodeOptions.semantic = TextureSemantic::Color;
        decodeOptions.maxEncodedBytes = kMaxEncodedTextureBytes;
        decodeOptions.maxDecodedBytes = (std::min)(decodeOptions.maxDecodedBytes, remainingBytes);
        decodeOptions.maxPixels = (std::min)(decodeOptions.maxPixels, remainingPixels);
        if (mimeMatches) decoded = DecodeRasterImageWic(encoded, ColorSpaceId::Srgb, decodeOptions);
        if (options_.Cancelled()) { error_ = ImportErrorCode::Cancelled; return 0; }
        DecodedTexture image;
        if (decoded) {
            image.format = decoded->pixelFormat; image.width = decoded->width;
            image.height = decoded->height; image.levels = decoded->mipLevels;
            image.pixels = std::move(decoded->pixelBytes);
        } else {
            if (remainingBytes < 16 || remainingPixels < 4) {
                error_ = ImportErrorCode::ResourceLimit; return 0;
            }
            Warn(warnings_);
            image.format = PixelFormatId::RGBA8_UNORM; image.width = image.height = 2;
            image.levels = 1; image.pixels.resize(16, std::byte{255});
            for (unsigned pixel = 0; pixel < 4; ++pixel)
                for (unsigned channel = 0; channel < 3; ++channel)
                    image.pixels[pixel * 4 + channel]
                        = std::byte((pixel == 0 || pixel == 3) ? 64 : 192);
        }
        const uint64_t pixels = uint64_t(image.width) * image.height;
        if (image.pixels.size() > remainingBytes || pixels > remainingPixels) {
            error_ = ImportErrorCode::ResourceLimit; return 0;
        }
        decodedBytes_ += image.pixels.size(); decodedPixels_ += pixels;
        ImagePayloadHeader header{};
        header.pixelFormat = uint32_t(image.format); header.width = image.width;
        header.height = image.height; header.mipLevels = image.levels;
        header.colorSpace = uint32_t(ColorSpaceId::Srgb);
        header.pixelDataByteSize = image.pixels.size();
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::Image; descriptor.chunkId = writer_.NextId();
        if (!writer_.Add(descriptor, ChunkBytes(header), image.pixels)) {
            error_ = writer_.Error(); return 0;
        }
        images_.emplace(textureId, descriptor.chunkId);
        return descriptor.chunkId;
    }

    Lib3MF::PModel model_;
    BoundedChunkWriter& writer_;
    const ThreeMfImportOptions& options_;
    std::unordered_map<MaterialKey, uint32_t, MaterialKeyHash> materials_;
    std::unordered_map<uint32_t, uint32_t> images_;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> propertyIndices_;
    std::unordered_map<std::string, Lib3MF::PResource> resourcesByKey_;
    std::unordered_set<std::string> warnedDisplayGroups_;
    ImportErrorCode error_ = ImportErrorCode::None;
    uint32_t warnings_ = 0;
    uint64_t decodedBytes_ = 0, decodedPixels_ = 0;
};

struct TriangleAppearance {
    PropertySample corners[3];
    uint32_t materialId = 0;
};

struct Vec3d { double x = 0.0, y = 0.0, z = 0.0; };

Vec3d operator+(Vec3d a, Vec3d b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3d operator-(Vec3d a, Vec3d b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3d operator*(Vec3d a, double value) { return {a.x * value, a.y * value, a.z * value}; }
double Dot(Vec3d a, Vec3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3d Cross(Vec3d a, Vec3d b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
double Length(Vec3d value) { return std::sqrt(Dot(value, value)); }
Vec3d Normalize(Vec3d value) {
    const double length = Length(value);
    return length > 1.0e-20 && Finite(length) ? value * (1.0 / length) : Vec3d{0, 0, 1};
}

struct GeneratedVertex {
    Vec3d position;
    Vec3d normal;
    PropertySample property;
};

struct GeneratedTriangle { GeneratedVertex vertex[3]; };

PropertySample Interpolate(const PropertySample& a, const PropertySample& b, double t)
{
    PropertySample result = a;
    for (uint32_t channel = 0; channel < 4; ++channel)
        result.color[channel] = float(double(a.color[channel])
            + (double(b.color[channel]) - a.color[channel]) * t);
    for (uint32_t axis = 0; axis < 2; ++axis)
        result.uv[axis] = float(double(a.uv[axis]) + (double(b.uv[axis]) - a.uv[axis]) * t);
    result.metallic = float(double(a.metallic) + (double(b.metallic) - a.metallic) * t);
    result.roughness = float(double(a.roughness) + (double(b.roughness) - a.roughness) * t);
    result.hasUv = a.hasUv || b.hasUv;
    result.hasPbr = a.hasPbr || b.hasPbr;
    result.colorSrgb = a.colorSrgb && b.colorSrgb;
    return result;
}

GeneratedVertex Interpolate(const GeneratedVertex& a, const GeneratedVertex& b, double t)
{
    return {a.position + (b.position - a.position) * t,
            Normalize(a.normal + (b.normal - a.normal) * t),
            Interpolate(a.property, b.property, t)};
}

void AddTriangle(std::vector<GeneratedTriangle>& output, const GeneratedVertex& a,
                 const GeneratedVertex& b, const GeneratedVertex& c)
{
    output.push_back({a, b, c});
}

void Basis(Vec3d axis, Vec3d& first, Vec3d& second)
{
    const Vec3d reference = std::abs(axis.z) < 0.8 ? Vec3d{0, 0, 1} : Vec3d{0, 1, 0};
    first = Normalize(Cross(reference, axis));
    second = Normalize(Cross(axis, first));
}

void AddSphere(std::vector<GeneratedTriangle>& output, Vec3d center, double radius,
               Vec3d axis, uint32_t radial, uint32_t latitude,
               const PropertySample& property, double angularLimit)
{
    Vec3d u{}, v{};
    axis = Normalize(axis); Basis(axis, u, v);
    const bool fullSphere = angularLimit >= std::numbers::pi_v<double> - 1.0e-12;
    std::vector<std::vector<GeneratedVertex>> rings;
    for (uint32_t row = 1; row < latitude + (fullSphere ? 0u : 1u); ++row) {
        const double theta = angularLimit * double(row) / double(latitude);
        std::vector<GeneratedVertex> ring;
        ring.reserve(radial);
        for (uint32_t column = 0; column < radial; ++column) {
            const double angle = 2.0 * std::numbers::pi_v<double> * column / radial;
            const Vec3d normal = axis * std::cos(theta)
                + (u * std::cos(angle) + v * std::sin(angle)) * std::sin(theta);
            ring.push_back({center + normal * radius, normal, property});
        }
        rings.push_back(std::move(ring));
    }
    GeneratedVertex top{center + axis * radius, axis, property};
    if (rings.empty()) return;
    for (uint32_t column = 0; column < radial; ++column)
        AddTriangle(output, top, rings.front()[column], rings.front()[(column + 1) % radial]);
    for (size_t row = 1; row < rings.size(); ++row)
        for (uint32_t column = 0; column < radial; ++column) {
            const uint32_t next = (column + 1) % radial;
            AddTriangle(output, rings[row - 1][column], rings[row][column], rings[row][next]);
            AddTriangle(output, rings[row - 1][column], rings[row][next], rings[row - 1][next]);
        }
    if (fullSphere) {
        GeneratedVertex bottom{center - axis * radius, axis * -1.0, property};
        for (uint32_t column = 0; column < radial; ++column)
            AddTriangle(output, rings.back()[column], bottom,
                        rings.back()[(column + 1) % radial]);
    }
}

void AddBeam(std::vector<GeneratedTriangle>& output, Vec3d begin, Vec3d end,
             double radius0, double radius1, ThreeMfLatticeCapMode cap0,
             ThreeMfLatticeCapMode cap1, const PropertySample& property0,
             const PropertySample& property1, uint32_t radial)
{
    const Vec3d delta = end - begin;
    const double length = Length(delta);
    const Vec3d axis = delta * (1.0 / length);
    const double slope = (radius1 - radius0) / length;
    double trim0 = 0.0, trim1 = 0.0;
    double sphereAngle0 = std::numbers::pi_v<double> * 0.5;
    double sphereAngle1 = std::numbers::pi_v<double> * 0.5;
    if (cap0 == ThreeMfLatticeCapMode::Sphere && slope < 0.0) {
        trim0 = -2.0 * radius0 * slope / (1.0 + slope * slope);
        sphereAngle0 = std::acos((std::clamp)(2.0 * slope / (1.0 + slope * slope), -1.0, 1.0));
    }
    const double reverseSlope = -slope;
    if (cap1 == ThreeMfLatticeCapMode::Sphere && reverseSlope < 0.0) {
        trim1 = -2.0 * radius1 * reverseSlope / (1.0 + reverseSlope * reverseSlope);
        sphereAngle1 = std::acos((std::clamp)(2.0 * reverseSlope
            / (1.0 + reverseSlope * reverseSlope), -1.0, 1.0));
    }
    if (trim0 + trim1 >= length) { trim0 = trim1 = 0.0; sphereAngle0 = sphereAngle1 = std::numbers::pi_v<double>; }
    const Vec3d sideBegin = begin + axis * trim0;
    const Vec3d sideEnd = end - axis * trim1;
    const double sideRadius0 = radius0 + slope * trim0;
    const double sideRadius1 = radius1 - slope * trim1;
    const PropertySample sideProperty0 = Interpolate(property0, property1, trim0 / length);
    const PropertySample sideProperty1 = Interpolate(property0, property1, 1.0 - trim1 / length);
    Vec3d u{}, v{}; Basis(axis, u, v);
    std::vector<GeneratedVertex> ring0, ring1;
    ring0.reserve(radial); ring1.reserve(radial);
    for (uint32_t column = 0; column < radial; ++column) {
        const double angle = 2.0 * std::numbers::pi_v<double> * column / radial;
        const Vec3d radialNormal = u * std::cos(angle) + v * std::sin(angle);
        const Vec3d normal = Normalize(radialNormal - axis * slope);
        ring0.push_back({sideBegin + radialNormal * sideRadius0, normal, sideProperty0});
        ring1.push_back({sideEnd + radialNormal * sideRadius1, normal, sideProperty1});
    }
    for (uint32_t column = 0; column < radial; ++column) {
        const uint32_t next = (column + 1) % radial;
        AddTriangle(output, ring0[column], ring0[next], ring1[next]);
        AddTriangle(output, ring0[column], ring1[next], ring1[column]);
    }
    const auto addCap = [&](bool endCap, ThreeMfLatticeCapMode cap, double radius,
                            const PropertySample& property) {
        const Vec3d center = endCap ? end : begin;
        const Vec3d direction = endCap ? axis : axis * -1.0;
        const auto& ring = endCap ? ring1 : ring0;
        if (cap == ThreeMfLatticeCapMode::Butt) {
            GeneratedVertex middle{center, direction, property};
            for (uint32_t column = 0; column < radial; ++column) {
                const uint32_t next = (column + 1) % radial;
                if (endCap) AddTriangle(output, middle, ring[column], ring[next]);
                else AddTriangle(output, middle, ring[next], ring[column]);
            }
        } else {
            const double angle = cap == ThreeMfLatticeCapMode::Hemisphere
                ? std::numbers::pi_v<double> * 0.5 : (endCap ? sphereAngle1 : sphereAngle0);
            const uint32_t rows = (std::max)(1u, uint32_t(std::ceil(
                double(radial) * angle / (2.0 * std::numbers::pi_v<double>))));
            AddSphere(output, center, radius, direction, radial, rows, property, angle);
        }
    };
    addCap(false, cap0, radius0, property0);
    addCap(true, cap1, radius1, property1);
}

uint64_t SphereTriangles(uint32_t radial, bool hemisphere)
{
    const uint32_t latitude = hemisphere ? (std::max)(1u, radial / 4)
                                         : (std::max)(2u, radial / 2);
    return hemisphere ? uint64_t(radial) * (2 * latitude - 1)
                      : uint64_t(2) * radial * (latitude - 1);
}

uint64_t BeamTriangles(const ThreeMfLatticeBeam& beam, uint32_t radial)
{
    uint64_t count = uint64_t(2) * radial;
    for (const auto cap : beam.cap)
        count += cap == ThreeMfLatticeCapMode::Butt ? radial
            : SphereTriangles(radial, cap == ThreeMfLatticeCapMode::Hemisphere);
    return count;
}

struct Aabb { Vec3d minimum, maximum; };

bool Inside(const Vec3d& point, uint32_t plane, const Aabb& box)
{
    const uint32_t axis = plane / 2;
    const double value = axis == 0 ? point.x : axis == 1 ? point.y : point.z;
    const double limit = (plane & 1) == 0
        ? (axis == 0 ? box.minimum.x : axis == 1 ? box.minimum.y : box.minimum.z)
        : (axis == 0 ? box.maximum.x : axis == 1 ? box.maximum.y : box.maximum.z);
    return (plane & 1) == 0 ? value >= limit : value <= limit;
}

double Coordinate(const Vec3d& point, uint32_t axis)
{
    return axis == 0 ? point.x : axis == 1 ? point.y : point.z;
}

struct PointKey {
    int64_t value[3]{};
    bool operator==(const PointKey&) const = default;
};

struct PointKeyHash {
    size_t operator()(const PointKey& key) const noexcept {
        size_t hash = 1469598103934665603ull;
        for (const int64_t value : key.value) {
            hash ^= size_t(value); hash *= 1099511628211ull;
        }
        return hash;
    }
};

struct EdgeKey {
    PointKey first, second;
    bool operator==(const EdgeKey&) const = default;
};

struct EdgeKeyHash {
    size_t operator()(const EdgeKey& key) const noexcept {
        PointKeyHash hash;
        return hash(key.first) ^ (hash(key.second) * 0x9e3779b97f4a7c15ull);
    }
};

bool Less(const PointKey& a, const PointKey& b)
{
    for (uint32_t axis = 0; axis < 3; ++axis) {
        if (a.value[axis] < b.value[axis]) return true;
        if (a.value[axis] > b.value[axis]) return false;
    }
    return false;
}

PointKey Quantize(Vec3d point, double scale)
{
    return {{int64_t(std::llround(point.x / scale)), int64_t(std::llround(point.y / scale)),
             int64_t(std::llround(point.z / scale))}};
}

void AddClippingCaps(std::vector<GeneratedTriangle>& triangles, const Aabb& box,
                     const PropertySample& property)
{
    const double extent = (std::max)({box.maximum.x - box.minimum.x,
                                      box.maximum.y - box.minimum.y,
                                      box.maximum.z - box.minimum.z});
    const double magnitude = (std::max)({std::abs(box.minimum.x), std::abs(box.minimum.y),
                                         std::abs(box.minimum.z), std::abs(box.maximum.x),
                                         std::abs(box.maximum.y), std::abs(box.maximum.z)});
    const double epsilon = (std::max)(1.0e-9, (std::max)(extent, magnitude) * 1.0e-8);
    for (uint32_t plane = 0; plane < 6; ++plane) {
        const uint32_t axis = plane / 2;
        const double limit = (plane & 1) == 0 ? Coordinate(box.minimum, axis)
                                              : Coordinate(box.maximum, axis);
        std::unordered_map<EdgeKey, uint32_t, EdgeKeyHash> counts;
        std::unordered_map<PointKey, Vec3d, PointKeyHash> points;
        for (const auto& triangle : triangles)
            for (uint32_t edge = 0; edge < 3; ++edge) {
                const Vec3d a = triangle.vertex[edge].position;
                const Vec3d b = triangle.vertex[(edge + 1) % 3].position;
                if (std::abs(Coordinate(a, axis) - limit) > epsilon
                    || std::abs(Coordinate(b, axis) - limit) > epsilon) continue;
                PointKey ka = Quantize(a, epsilon), kb = Quantize(b, epsilon);
                if (ka == kb) continue;
                points.emplace(ka, a); points.emplace(kb, b);
                if (Less(kb, ka)) std::swap(ka, kb);
                ++counts[{ka, kb}];
            }
        std::unordered_map<PointKey, std::vector<PointKey>, PointKeyHash> adjacent;
        std::unordered_set<EdgeKey, EdgeKeyHash> remaining;
        for (const auto& [edge, count] : counts) {
            if ((count & 1u) == 0) continue;
            adjacent[edge.first].push_back(edge.second);
            adjacent[edge.second].push_back(edge.first);
            remaining.insert(edge);
        }
        while (!remaining.empty()) {
            const EdgeKey seed = *remaining.begin();
            std::vector<PointKey> loop{seed.first, seed.second};
            remaining.erase(seed);
            PointKey previous = seed.first, current = seed.second;
            while (!(current == loop.front())) {
                const auto found = adjacent.find(current);
                if (found == adjacent.end() || found->second.size() < 2) { loop.clear(); break; }
                PointKey next = found->second[0] == previous && found->second.size() > 1
                    ? found->second[1] : found->second[0];
                PointKey a = current, b = next;
                if (Less(b, a)) std::swap(a, b);
                if (!remaining.erase({a, b})) { loop.clear(); break; }
                previous = current; current = next;
                if (!(current == loop.front())) loop.push_back(current);
                if (loop.size() > counts.size() + 1) { loop.clear(); break; }
            }
            if (loop.size() < 3) continue;
            std::vector<Vec3d> polygon;
            polygon.reserve(loop.size());
            for (const auto& key : loop) polygon.push_back(points.at(key));
            Vec3d normal{};
            for (size_t index = 0; index < polygon.size(); ++index)
                normal = normal + Cross(polygon[index], polygon[(index + 1) % polygon.size()]);
            Vec3d desired{};
            if (axis == 0) desired.x = (plane & 1) ? 1.0 : -1.0;
            else if (axis == 1) desired.y = (plane & 1) ? 1.0 : -1.0;
            else desired.z = (plane & 1) ? 1.0 : -1.0;
            if (Dot(normal, desired) < 0.0) std::reverse(polygon.begin(), polygon.end());
            Vec3d center{};
            for (const auto point : polygon) center = center + point;
            center = center * (1.0 / polygon.size());
            GeneratedVertex middle{center, desired, property};
            for (size_t index = 0; index < polygon.size(); ++index) {
                GeneratedVertex a{polygon[index], desired, property};
                GeneratedVertex b{polygon[(index + 1) % polygon.size()], desired, property};
                AddTriangle(triangles, middle, a, b);
            }
        }
    }
}

void ClipInside(std::vector<GeneratedTriangle>& triangles, const Aabb& box,
                const PropertySample& clippingProperty)
{
    for (uint32_t plane = 0; plane < 6; ++plane) {
        std::vector<GeneratedTriangle> clipped;
        for (const auto& triangle : triangles) {
            std::vector<GeneratedVertex> polygon(std::begin(triangle.vertex),
                                                 std::end(triangle.vertex));
            std::vector<GeneratedVertex> next;
            for (size_t index = 0; index < polygon.size(); ++index) {
                const auto& a = polygon[index];
                const auto& b = polygon[(index + 1) % polygon.size()];
                const bool inA = Inside(a.position, plane, box);
                const bool inB = Inside(b.position, plane, box);
                if (inA) next.push_back(a);
                if (inA != inB) {
                    const uint32_t axis = plane / 2;
                    const double limit = (plane & 1) == 0
                        ? Coordinate(box.minimum, axis) : Coordinate(box.maximum, axis);
                    const double denominator = Coordinate(b.position, axis)
                        - Coordinate(a.position, axis);
                    if (std::abs(denominator) > 1.0e-30) {
                        const double t = (limit - Coordinate(a.position, axis)) / denominator;
                        next.push_back(Interpolate(a, b, (std::clamp)(t, 0.0, 1.0)));
                    }
                }
            }
            for (size_t index = 1; index + 1 < next.size(); ++index)
                AddTriangle(clipped, next[0], next[index], next[index + 1]);
        }
        triangles.swap(clipped);
        if (triangles.empty()) return;
    }
    AddClippingCaps(triangles, box, clippingProperty);
}

bool ResolveTriangleAppearance(const Lib3MF::PMeshObject& mesh, uint32_t triangleIndex,
                               PropertyCatalog& catalog, TriangleAppearance& appearance)
{
    Lib3MF_uint32 defaultResource = 0, defaultProperty = 0;
    const bool hasDefault = mesh->GetObjectLevelProperty(defaultResource, defaultProperty);
    Lib3MF::sTriangleProperties properties{};
    mesh->GetTriangleProperties(triangleIndex, properties);
    uint32_t resource = properties.m_ResourceID;
    if (!resource && hasDefault) resource = defaultResource;
    for (unsigned corner = 0; corner < 3; ++corner) {
        uint32_t property = properties.m_ResourceID ? properties.m_PropertyIDs[corner]
                                                   : defaultProperty;
        if (resource) {
            if (!catalog.Resolve(resource, property, appearance.corners[corner])) return false;
        } else {
            appearance.corners[corner].color[0] = appearance.corners[corner].color[1]
                = appearance.corners[corner].color[2] = 0.8f;
        }
    }
    appearance.materialId = catalog.MaterialFor(appearance.corners);
    return appearance.materialId != 0;
}

bool SourceKey(const Lib3MF::PResource& resource, std::string& key)
{
    if (!resource || !resource->GetModelResourceID()) return false;
    const auto part = resource->PackagePart();
    if (!part) return false;
    key = ThreeMfResourceKey(part->GetPath(), resource->GetModelResourceID());
    return true;
}

bool ClippingBox(const Lib3MF::PMeshObject& mesh, const ThreeMfDisplayCatalog* catalog,
                 Aabb& box)
{
    if (!mesh || mesh->GetType() != Lib3MF::eObjectType::Model
        || mesh->GetVertexCount() != 8 || mesh->GetTriangleCount() != 12)
        return false;
    std::string key;
    if (!SourceKey(mesh, key) || (catalog && catalog->lattices.contains(key))) return false;
    std::vector<Lib3MF::sPosition> positions;
    std::vector<Lib3MF::sTriangle> triangles;
    mesh->GetVertices(positions); mesh->GetTriangleIndices(triangles);
    if (positions.size() != 8 || triangles.size() != 12) return false;
    box.minimum = {(std::numeric_limits<double>::max)(), (std::numeric_limits<double>::max)(),
                   (std::numeric_limits<double>::max)()};
    box.maximum = {-box.minimum.x, -box.minimum.y, -box.minimum.z};
    for (const auto& position : positions) {
        const Vec3d point{position.m_Coordinates[0], position.m_Coordinates[1],
                          position.m_Coordinates[2]};
        if (!Finite(point.x) || !Finite(point.y) || !Finite(point.z)) return false;
        box.minimum.x = (std::min)(box.minimum.x, point.x);
        box.minimum.y = (std::min)(box.minimum.y, point.y);
        box.minimum.z = (std::min)(box.minimum.z, point.z);
        box.maximum.x = (std::max)(box.maximum.x, point.x);
        box.maximum.y = (std::max)(box.maximum.y, point.y);
        box.maximum.z = (std::max)(box.maximum.z, point.z);
    }
    if (!(box.maximum.x > box.minimum.x && box.maximum.y > box.minimum.y
          && box.maximum.z > box.minimum.z)) return false;
    for (const auto& position : positions) {
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const double value = position.m_Coordinates[axis];
            if (value != Coordinate(box.minimum, axis) && value != Coordinate(box.maximum, axis))
                return false;
        }
    }
    for (const auto& triangle : triangles) {
        for (uint32_t index : triangle.m_Indices) if (index >= positions.size()) return false;
        bool face = false;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const double a = positions[triangle.m_Indices[0]].m_Coordinates[axis];
            const double b = positions[triangle.m_Indices[1]].m_Coordinates[axis];
            const double c = positions[triangle.m_Indices[2]].m_Coordinates[axis];
            face |= a == b && b == c
                && (a == Coordinate(box.minimum, axis) || a == Coordinate(box.maximum, axis));
        }
        if (!face) return false;
    }
    return true;
}

bool EmitGenerated(BoundedChunkWriter& writer, PropertyCatalog& catalog,
                   std::span<const GeneratedTriangle> triangles,
                   uint32_t meshKey, uint32_t meshOrdinal,
                   std::vector<GeometryRecord>& output,
                   uint64_t& totalTriangles, uint64_t& totalVertices)
{
    if (triangles.empty()) return true;
    if (triangles.size() > kTierBTriangleLimit - totalTriangles
        || triangles.size() * 3 > kTierBVertexLimit - totalVertices)
        return false;
    std::vector<uint32_t> materials;
    materials.reserve(triangles.size());
    for (const auto& triangle : triangles) {
        PropertySample samples[3]{triangle.vertex[0].property, triangle.vertex[1].property,
                                  triangle.vertex[2].property};
        const uint32_t material = catalog.MaterialFor(samples);
        if (!material) return false;
        materials.push_back(material);
    }
    size_t start = 0;
    while (start < triangles.size()) {
        size_t count = 1;
        while (count < kChunkTriangles && start + count < triangles.size()
               && materials[start + count] == materials[start]) ++count;
        std::vector<VertexPositionNormalUv0TangentColorF32> vertices(count * 3);
        std::vector<uint32_t> indices(count * 3);
        bool hasUv = false;
        for (size_t triangleIndex = 0; triangleIndex < count; ++triangleIndex)
            for (uint32_t corner = 0; corner < 3; ++corner) {
                const auto& source = triangles[start + triangleIndex].vertex[corner];
                auto& target = vertices[triangleIndex * 3 + corner];
                target.px = float(source.position.x); target.py = float(source.position.y);
                target.pz = float(source.position.z); target.nx = float(source.normal.x);
                target.ny = float(source.normal.y); target.nz = float(source.normal.z);
                target.u = source.property.uv[0]; target.v = source.property.uv[1];
                target.tx = 1.0f; target.tw = 1.0f;
                target.r = source.property.color[0]; target.g = source.property.color[1];
                target.b = source.property.color[2]; target.a = source.property.color[3];
                indices[triangleIndex * 3 + corner] = uint32_t(triangleIndex * 3 + corner);
                hasUv |= source.property.textureId != 0;
            }
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::TriangleList;
        descriptor.chunkId = writer.NextId(); descriptor.vertexCount = uint32_t(count * 3);
        descriptor.indexCount = uint32_t(count * 3);
        descriptor.vertexLayoutId = uint32_t(VertexLayoutId::PositionNormalUv0TangentColor_F32);
        descriptor.meshId = meshOrdinal;
        descriptor.sourceRangeOffset = (uint64_t(meshKey) << 32) | (uint64_t(start) * 3);
        descriptor.sourceRangeLength = uint64_t(count) * 3;
        descriptor.geometryFlags = kGeometryDeindexed | kGeometryReusableInstanceSource
            | kGeometryHasColors | (hasUv ? kGeometryHasUv0 : 0);
        if (!SetLocalBounds(descriptor, ChunkBytes(vertices))
            || !writer.Add(descriptor, ChunkBytes(vertices), ChunkBytes(indices))) return false;
        output.push_back({descriptor.chunkId, descriptor, materials[start]});
        start += count;
    }
    totalTriangles += triangles.size(); totalVertices += triangles.size() * 3;
    return true;
}

bool EmitMesh(BoundedChunkWriter& writer, PropertyCatalog& catalog,
              const Lib3MF::PMeshObject& mesh, uint32_t meshKey,
              uint32_t meshOrdinal,
              const ThreeMfImportOptions& options, std::vector<GeometryRecord>& output,
              uint64_t& totalTriangles, uint64_t& totalVertices)
{
    if (options.Cancelled()) return false;
    const uint32_t vertexCount = mesh->GetVertexCount();
    const uint32_t triangleCount = mesh->GetTriangleCount();
    if (!vertexCount || vertexCount > kTierBVertexLimit
        || triangleCount > kTierBTriangleLimit) return false;
    if (!triangleCount) return true;
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

    // Resolve once without emitting geometry so every dependency precedes the
    // first chunk that can reference it. The second pass re-resolves bounded
    // values instead of retaining source-proportional appearance state.
    for (uint32_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex) {
        if (options.Cancelled()) return false;
        TriangleAppearance appearance;
        if (!ResolveTriangleAppearance(mesh, triangleIndex, catalog, appearance)) return false;
    }

    uint32_t start = 0;
    while (start < triangleCount) {
        if (options.Cancelled()) return false;
        TriangleAppearance firstAppearance;
        if (!ResolveTriangleAppearance(mesh, start, catalog, firstAppearance)) return false;
        uint32_t count = 1;
        while (count < kChunkTriangles && start + count < triangleCount) {
            TriangleAppearance next;
            if (!ResolveTriangleAppearance(mesh, start + count, catalog, next)) return false;
            if (next.materialId != firstAppearance.materialId) break;
            ++count;
        }
        if (totalTriangles > kTierBTriangleLimit - count || totalVertices > kTierBVertexLimit - uint64_t(count) * 3)
            return false;
        std::vector<VertexPositionNormalUv0TangentColorF32> vertices(size_t(count) * 3);
        std::vector<uint32_t> indices(size_t(count) * 3);
        for (uint32_t triangleIndex = 0; triangleIndex < count; ++triangleIndex) {
            const auto& triangle = triangles[start + triangleIndex];
            TriangleAppearance appearance;
            if (!ResolveTriangleAppearance(mesh, start + triangleIndex, catalog, appearance)
                || appearance.materialId != firstAppearance.materialId) return false;
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
                vertex.u = appearance.corners[corner].uv[0];
                vertex.v = appearance.corners[corner].uv[1];
                vertex.tx = 1.0f; vertex.tw = 1.0f;
                vertex.r = appearance.corners[corner].color[0];
                vertex.g = appearance.corners[corner].color[1];
                vertex.b = appearance.corners[corner].color[2];
                vertex.a = appearance.corners[corner].color[3];
                indices[size_t(triangleIndex) * 3 + corner] = triangleIndex * 3 + corner;
            }
        }
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::TriangleList;
        descriptor.chunkId = writer.NextId();
        descriptor.vertexCount = count * 3; descriptor.indexCount = count * 3;
        descriptor.vertexLayoutId = uint32_t(VertexLayoutId::PositionNormalUv0TangentColor_F32);
        descriptor.meshId = meshOrdinal;
        descriptor.sourceRangeOffset = (uint64_t(meshKey) << 32) | (uint64_t(start) * 3);
        descriptor.sourceRangeLength = uint64_t(count) * 3;
        descriptor.geometryFlags = kGeometryDeindexed | kGeometryReusableInstanceSource
            | kGeometryHasColors;
        if (firstAppearance.corners[0].textureId) descriptor.geometryFlags |= kGeometryHasUv0;
        if (!SetLocalBounds(descriptor, ChunkBytes(vertices)) || !writer.Add(descriptor, ChunkBytes(vertices), ChunkBytes(indices))) return false;
        output.push_back({descriptor.chunkId, descriptor, firstAppearance.materialId});
        totalTriangles += count; totalVertices += uint64_t(count) * 3;
        start += count;
    }
    return true;
}

bool SameDisplayProperty(const PropertySample& a, const PropertySample& b)
{
    if (a.hasPbr != b.hasPbr) return false;
    if (!a.hasPbr && !b.hasPbr) return true;
    constexpr float epsilon = 1.0e-6f;
    if (std::abs(a.metallic - b.metallic) > epsilon
        || std::abs(a.roughness - b.roughness) > epsilon) return false;
    for (uint32_t channel = 0; channel < 4; ++channel)
        if (std::abs(a.color[channel] - b.color[channel]) > epsilon) return false;
    return true;
}

bool SameProperty(const PropertySample& a, const PropertySample& b)
{
    if (a.textureId != b.textureId || a.samplerFlags != b.samplerFlags
        || a.hasUv != b.hasUv || a.hasPbr != b.hasPbr
        || a.textureLayer != b.textureLayer || a.textureMix != b.textureMix
        || a.colorSrgb != b.colorSrgb) return false;
    constexpr float epsilon = 1.0e-6f;
    for (uint32_t channel = 0; channel < 4; ++channel)
        if (std::abs(a.color[channel] - b.color[channel]) > epsilon) return false;
    for (uint32_t axis = 0; axis < 2; ++axis)
        if (std::abs(a.uv[axis] - b.uv[axis]) > epsilon) return false;
    return std::abs(a.metallic - b.metallic) <= epsilon
        && std::abs(a.roughness - b.roughness) <= epsilon;
}

bool EmitLattice(BoundedChunkWriter& writer, PropertyCatalog& catalog,
                 const Lib3MF::PMeshObject& mesh,
                 uint32_t meshKey, uint32_t meshOrdinal,
                 const ThreeMfLatticeDefinition& lattice,
                 const std::unordered_map<std::string, Lib3MF::PMeshObject>& sourceMeshes,
                 const ThreeMfDisplayCatalog* xmlCatalog,
                 const ThreeMfImportOptions& options,
                 std::vector<GeometryRecord>& output,
                 uint64_t& totalTriangles, uint64_t& totalVertices)
{
    if (options.Cancelled()) return false;
    std::string sourceKey;
    if (!SourceKey(mesh, sourceKey) || sourceKey != ThreeMfResourceKey(
            lattice.packagePart, lattice.objectId)) return false;
    const auto findReferencedMesh = [&](uint32_t localId) -> Lib3MF::PMeshObject {
        if (!localId) return {};
        const auto found = sourceMeshes.find(ThreeMfResourceKey(lattice.packagePart, localId));
        return found == sourceMeshes.end() ? Lib3MF::PMeshObject{} : found->second;
    };
    const auto representation = findReferencedMesh(lattice.representationMeshId);
    const auto clipping = findReferencedMesh(lattice.clippingMeshId);
    if (lattice.representationMeshId) {
        std::string key;
        if (!representation || representation == mesh
            || representation->GetType() != Lib3MF::eObjectType::Model
            || !representation->GetTriangleCount() || !SourceKey(representation, key)
            || (xmlCatalog && xmlCatalog->lattices.contains(key))) return false;
    }
    if (lattice.clipMode != ThreeMfLatticeClipMode::None) {
        std::string key;
        if (!clipping || clipping == mesh || clipping->GetType() != Lib3MF::eObjectType::Model
            || !SourceKey(clipping, key) || (xmlCatalog && xmlCatalog->lattices.contains(key)))
            return false;
    }
    if (!representation && lattice.clipMode == ThreeMfLatticeClipMode::Outside) {
        catalog.SetError(ImportErrorCode::UnsupportedRequiredFeature);
        return false;
    }

    Aabb clippingBounds{};
    const bool clipped = !representation && lattice.clipMode == ThreeMfLatticeClipMode::Inside;
    if (clipped && !ClippingBox(clipping, xmlCatalog, clippingBounds)) {
        catalog.SetError(ImportErrorCode::UnsupportedRequiredFeature);
        return false;
    }
    PropertySample clippingProperty;
    if (clipped) {
        bool initialized = false;
        for (uint32_t triangle = 0; triangle < clipping->GetTriangleCount(); ++triangle) {
            TriangleAppearance appearance;
            if (!ResolveTriangleAppearance(clipping, triangle, catalog, appearance)) return false;
            for (const auto& sample : appearance.corners) {
                if (!initialized) { clippingProperty = sample; initialized = true; }
                else if (!SameProperty(clippingProperty, sample)) {
                    catalog.SetError(ImportErrorCode::UnsupportedRequiredFeature);
                    return false;
                }
            }
        }
        if (!initialized) return false;
    }

    const uint32_t vertexCount = mesh->GetVertexCount();
    if (!vertexCount || vertexCount > kTierBVertexLimit) return false;
    std::vector<Lib3MF::sPosition> rawPositions;
    mesh->GetVertices(rawPositions);
    if (rawPositions.size() != vertexCount) return false;
    std::vector<Vec3d> positions;
    positions.reserve(rawPositions.size());
    for (const auto& position : rawPositions) {
        Vec3d value{position.m_Coordinates[0], position.m_Coordinates[1],
                    position.m_Coordinates[2]};
        if (!Finite(value.x) || !Finite(value.y) || !Finite(value.z)) return false;
        positions.push_back(value);
    }

    uint32_t objectResource = 0, objectProperty = 0;
    const bool objectHasProperty = mesh->GetObjectLevelProperty(objectResource, objectProperty);
    bool hasElementProperty = false;
    for (const auto& beam : lattice.beams)
        for (const auto& property : beam.property)
            hasElementProperty |= property.hasResource || property.hasProperty;
    for (const auto& ball : lattice.balls)
        hasElementProperty |= ball.property.hasResource || ball.property.hasProperty;
    if (hasElementProperty && !lattice.defaultProperty.hasResource && !objectHasProperty)
        return false;

    struct ActiveBeam {
        const ThreeMfLatticeBeam* source = nullptr;
        Vec3d begin, end;
        double radius[2]{};
        PropertySample property[2];
    };
    std::vector<ActiveBeam> beams;
    beams.reserve(lattice.beams.size());
    for (const auto& beam : lattice.beams) {
        if (options.Cancelled()) return false;
        if (beam.vertex[0] >= positions.size() || beam.vertex[1] >= positions.size()) return false;
        const double radius0 = beam.hasRadius[0] ? beam.radius[0] : lattice.defaultRadius;
        const double radius1 = beam.hasRadius[1] ? beam.radius[1] : radius0;
        if (!Finite(radius0) || !Finite(radius1) || radius0 <= 0.0 || radius1 <= 0.0)
            return false;
        const double length = Length(positions[beam.vertex[1]] - positions[beam.vertex[0]]);
        if (!Finite(length)) return false;
        ActiveBeam active;
        active.source = &beam; active.begin = positions[beam.vertex[0]];
        active.end = positions[beam.vertex[1]]; active.radius[0] = radius0;
        active.radius[1] = radius1;
        for (uint32_t endpoint = 0; endpoint < 2; ++endpoint)
            if (!catalog.ResolveLatticeProperty(lattice.packagePart, lattice.defaultProperty,
                    beam.property[endpoint], objectHasProperty ? objectResource : 0,
                    objectProperty, active.property[endpoint])) return false;
        if (!SameDisplayProperty(active.property[0], active.property[1])) {
            catalog.SetError(ImportErrorCode::UnsupportedRequiredFeature);
            return false;
        }
        if (length < lattice.minimumLength) continue;
        beams.push_back(std::move(active));
    }

    std::unordered_map<uint32_t, const ThreeMfLatticeBall*> explicitBalls;
    for (const auto& ball : lattice.balls) {
        if (ball.vertex >= positions.size() || !explicitBalls.emplace(ball.vertex, &ball).second)
            return false;
        const double radius = ball.hasRadius ? ball.radius : lattice.defaultBallRadius;
        if (!Finite(radius) || radius <= 0.0) return false;
        PropertySample ignored;
        if (!catalog.ResolveLatticeProperty(lattice.packagePart, lattice.defaultProperty,
                ball.property, objectHasProperty ? objectResource : 0, objectProperty, ignored))
            return false;
    }
    if (representation) {
        return EmitMesh(writer, catalog, representation, representation->GetUniqueResourceID(),
                        meshOrdinal, options, output, totalTriangles, totalVertices);
    }
    std::vector<uint32_t> ballVertices;
    if (lattice.ballMode == ThreeMfLatticeBallMode::Mixed) {
        ballVertices.reserve(explicitBalls.size());
        for (const auto& ball : lattice.balls) ballVertices.push_back(ball.vertex);
    } else if (lattice.ballMode == ThreeMfLatticeBallMode::All) {
        std::unordered_set<uint32_t> unique;
        for (const auto& beam : lattice.beams) {
            unique.insert(beam.vertex[0]); unique.insert(beam.vertex[1]);
        }
        ballVertices.assign(unique.begin(), unique.end());
        std::sort(ballVertices.begin(), ballVertices.end());
    }

    constexpr std::array<uint32_t, 6> radialCandidates{16, 12, 8, 6, 4, 3};
    uint32_t radial = 0;
    for (const uint32_t candidate : radialCandidates) {
        uint64_t estimate = uint64_t(ballVertices.size()) * SphereTriangles(candidate, false);
        for (const auto& beam : beams) {
            if (estimate > UINT64_MAX - BeamTriangles(*beam.source, candidate)) {
                estimate = UINT64_MAX; break;
            }
            estimate += BeamTriangles(*beam.source, candidate);
        }
        // Plane clipping can split each source triangle into at most a bounded
        // fan. Reserve a conservative factor rather than allowing a clipped
        // lattice to exceed the same product-owned preview ceiling.
        const uint64_t budgeted = clipped && estimate <= UINT64_MAX / 2 ? estimate * 2 : estimate;
        const uint64_t remaining = totalTriangles < kTierBTriangleLimit
            ? kTierBTriangleLimit - totalTriangles : 0;
        if (budgeted <= options.maxLatticeTriangles && budgeted <= remaining) {
            radial = candidate; break;
        }
    }
    if (!radial) {
        catalog.SetError(ImportErrorCode::ResourceLimit);
        return false;
    }

    std::vector<GeneratedTriangle> generated;
    generated.reserve(options.maxLatticeTriangles);
    for (const auto& beam : beams) {
        if (options.Cancelled()) return false;
        std::vector<GeneratedTriangle> primitive;
        AddBeam(primitive, beam.begin, beam.end, beam.radius[0], beam.radius[1],
                beam.source->cap[0], beam.source->cap[1], beam.property[0],
                beam.property[1], radial);
        if (clipped) ClipInside(primitive, clippingBounds, clippingProperty);
        if (primitive.size() > options.maxLatticeTriangles
            || generated.size() > options.maxLatticeTriangles - primitive.size()) {
            catalog.SetError(ImportErrorCode::ResourceLimit); return false;
        }
        generated.insert(generated.end(), primitive.begin(), primitive.end());
    }
    for (const uint32_t vertex : ballVertices) {
        if (options.Cancelled()) return false;
        const auto explicitBall = explicitBalls.find(vertex);
        const ThreeMfLatticeBall* source = explicitBall == explicitBalls.end()
            ? nullptr : explicitBall->second;
        const double radius = source && source->hasRadius ? source->radius
                                                          : lattice.defaultBallRadius;
        if (!Finite(radius) || radius <= 0.0) return false;
        PropertySample property;
        const ThreeMfLatticePropertyRef empty;
        if (!catalog.ResolveLatticeProperty(lattice.packagePart, lattice.defaultProperty,
                source ? source->property : empty, objectHasProperty ? objectResource : 0,
                objectProperty, property)) return false;
        std::vector<GeneratedTriangle> primitive;
        AddSphere(primitive, positions[vertex], radius, Vec3d{0, 0, 1}, radial,
                  (std::max)(2u, radial / 2), property, std::numbers::pi_v<double>);
        if (clipped) ClipInside(primitive, clippingBounds, clippingProperty);
        if (primitive.size() > options.maxLatticeTriangles
            || generated.size() > options.maxLatticeTriangles - primitive.size()) {
            catalog.SetError(ImportErrorCode::ResourceLimit); return false;
        }
        generated.insert(generated.end(), primitive.begin(), primitive.end());
    }
    if (generated.empty() && mesh->GetTriangleCount() == 0) return false;
    return EmitGenerated(writer, catalog, generated, meshKey, meshOrdinal,
                         output, totalTriangles, totalVertices);
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
    PropertyCatalog properties(model, writer, options);
    if (!properties.ValidateDisplayProperties()) return Fail(properties.Error());
    std::unordered_map<std::string, Lib3MF::PMeshObject> sourceMeshes;
    auto resources = model->GetResources();
    if (!resources) return Fail(ImportErrorCode::MalformedData);
    while (resources->MoveNext()) {
        if (options.Cancelled()) return Fail(ImportErrorCode::Cancelled);
        const auto resource = resources->GetCurrent();
        const auto mesh = std::dynamic_pointer_cast<Lib3MF::CMeshObject>(resource);
        if (!mesh) continue;
        std::string key;
        if (!SourceKey(mesh, key) || !sourceMeshes.emplace(key, mesh).second)
            return Fail(ImportErrorCode::MalformedData);
    }
    std::unordered_map<uint32_t, std::vector<GeometryRecord>> geometry;
    uint64_t triangles = 0, vertices = 0;
    uint32_t meshOrdinal = 0;
    for (uint32_t key : traversal.meshOrder) {
        const auto mesh = traversal.meshes.at(key);
        auto& records = geometry[key];
        if (!EmitMesh(writer, properties, mesh, key, ++meshOrdinal, options,
                      records, triangles, vertices)) {
            const ImportErrorCode code = options.Cancelled() ? ImportErrorCode::Cancelled
                : properties.Error() != ImportErrorCode::None ? properties.Error()
                : writer.Error() == ImportErrorCode::None ? ImportErrorCode::MalformedData
                                                          : writer.Error();
            return Fail(code);
        }
        std::string sourceKey;
        if (!SourceKey(mesh, sourceKey)) return Fail(ImportErrorCode::MalformedData);
        const ThreeMfLatticeDefinition* lattice = nullptr;
        if (options.displayCatalog) {
            const auto found = options.displayCatalog->lattices.find(sourceKey);
            if (found != options.displayCatalog->lattices.end()) lattice = &found->second;
        }
        const auto libLattice = mesh->BeamLattice();
        const bool libHasLattice = libLattice
            && (libLattice->GetBeamCount() || libLattice->GetBallCount());
        if (libHasLattice != (lattice != nullptr)) return Fail(ImportErrorCode::MalformedData);
        if (lattice && !EmitLattice(writer, properties, mesh, key, meshOrdinal,
                *lattice, sourceMeshes, options.displayCatalog, options,
                records, triangles, vertices)) {
            const ImportErrorCode code = options.Cancelled() ? ImportErrorCode::Cancelled
                : properties.Error() != ImportErrorCode::None ? properties.Error()
                : writer.Error() == ImportErrorCode::None ? ImportErrorCode::MalformedData
                                                          : writer.Error();
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
            instance.geometryChunkId = record.chunkId;
            instance.materialChunkId = record.materialId;
            instance.flags = kSceneRecordVisible;
            if (!TransformBounds(record.descriptor, occurrence.transform, instance.worldMin, instance.worldMax)
                || !writer.AddInstance(instance)) return Fail(options.Cancelled() ? ImportErrorCode::Cancelled : writer.Error());
        }
    }
    if (properties.WarningCount()) {
        ChunkDescriptor warning{};
        warning.topology = ChunkTopology::TextureWarning;
        warning.chunkId = writer.NextId();
        const uint32_t count = properties.WarningCount();
        if (!writer.Add(warning, ChunkBytes(count))) return Fail(writer.Error());
    }
    if (!writer.Complete()) return Fail(writer.Error());
    return ThreeMfImportResult{writer.Count(), writer.Length()};
}

} // namespace import_worker

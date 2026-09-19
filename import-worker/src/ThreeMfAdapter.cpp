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
    uint32_t WarningCount() const { return warnings_; }

    bool ValidateDisplayProperties()
    {
        if (!options_.displayCatalog) return true;
        std::unordered_set<std::string> seen;
        auto resources = model_->GetResources();
        if (!resources) { error_ = ImportErrorCode::MalformedData; return false; }
        while (resources->MoveNext()) {
            if (options_.Cancelled()) { error_ = ImportErrorCode::Cancelled; return false; }
            const auto resource = resources->GetCurrent();
            std::string key;
            if (!ResourceKey(resource, key)) continue;
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
        if (seen.size() != options_.displayCatalog->associations.size()) {
            error_ = ImportErrorCode::MalformedData; return false;
        }
        return true;
    }

    bool Resolve(uint32_t resourceId, uint32_t propertyId, PropertySample& sample)
    {
        std::unordered_set<uint64_t> recursion;
        return ResolveInner(resourceId, propertyId, sample, recursion, 0);
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
    std::unordered_set<std::string> warnedDisplayGroups_;
    ImportErrorCode error_ = ImportErrorCode::None;
    uint32_t warnings_ = 0;
    uint64_t decodedBytes_ = 0, decodedPixels_ = 0;
};

struct TriangleAppearance {
    PropertySample corners[3];
    uint32_t materialId = 0;
};

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

bool EmitMesh(BoundedChunkWriter& writer, PropertyCatalog& catalog,
              const Lib3MF::PMeshObject& mesh, uint32_t meshKey,
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

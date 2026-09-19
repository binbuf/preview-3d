#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ThreeMfDisplayProperties.h"

#include "model_core/TierALimits.h"

#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <limits>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

namespace import_worker {
namespace {

constexpr wchar_t kCoreNamespace[] = L"http://schemas.microsoft.com/3dmanufacturing/core/2015/02";
constexpr wchar_t kMaterialNamespace[] = L"http://schemas.microsoft.com/3dmanufacturing/material/2015/02";
constexpr wchar_t kBeamNamespace[] = L"http://schemas.microsoft.com/3dmanufacturing/beamlattice/2017/02";
constexpr wchar_t kBallNamespace[] = L"http://schemas.microsoft.com/3dmanufacturing/beamlattice/balls/2020/07";
constexpr uint64_t kMaxModelXmlBytes = 256ull * 1024 * 1024;
constexpr uint64_t kMaxAggregateModelXmlBytes = 512ull * 1024 * 1024;

bool Equals(const wchar_t* value, const wchar_t* expected)
{
    return value && std::wcscmp(value, expected) == 0;
}

bool Attribute(IXmlReader* reader, const wchar_t* name, std::wstring& value)
{
    value.clear();
    if (reader->MoveToAttributeByName(name, nullptr) != S_OK) {
        reader->MoveToElement();
        return false;
    }
    const wchar_t* raw = nullptr;
    UINT length = 0;
    const bool ok = reader->GetValue(&raw, &length) == S_OK && raw;
    if (ok) value.assign(raw, length);
    reader->MoveToElement();
    return ok;
}

bool AttributeNamespace(IXmlReader* reader, const wchar_t* name, const wchar_t* uri,
                        std::wstring& value)
{
    value.clear();
    if (reader->MoveToAttributeByName(name, uri) != S_OK) {
        reader->MoveToElement();
        return false;
    }
    const wchar_t* raw = nullptr;
    UINT length = 0;
    const bool ok = reader->GetValue(&raw, &length) == S_OK && raw;
    if (ok) value.assign(raw, length);
    reader->MoveToElement();
    return ok;
}

bool Unsigned(const std::wstring& value, uint32_t& result)
{
    if (value.empty()) return false;
    uint64_t parsed = 0;
    for (const wchar_t ch : value) {
        if (ch < L'0' || ch > L'9') return false;
        const uint32_t digit = uint32_t(ch - L'0');
        if (parsed > (UINT32_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    if (!parsed) return false;
    result = uint32_t(parsed);
    return true;
}

bool Index(const std::wstring& value, uint32_t& result)
{
    if (value.empty()) return false;
    uint64_t parsed = 0;
    for (const wchar_t ch : value) {
        if (ch < L'0' || ch > L'9') return false;
        const uint32_t digit = uint32_t(ch - L'0');
        if (parsed > (uint64_t(INT32_MAX) - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    result = uint32_t(parsed);
    return true;
}

bool PositiveDouble(const std::wstring& value, double& result)
{
    if (value.empty()) return false;
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(value.c_str(), &end);
    if (end != value.c_str() + value.size() || !std::isfinite(parsed)
        || parsed <= 0.0 || parsed > 1.0e30) return false;
    result = parsed;
    return true;
}

bool CapMode(const std::wstring& value, ThreeMfLatticeCapMode& result)
{
    if (value == L"sphere") result = ThreeMfLatticeCapMode::Sphere;
    else if (value == L"hemisphere" || value == L"round")
        result = ThreeMfLatticeCapMode::Hemisphere;
    else if (value == L"butt") result = ThreeMfLatticeCapMode::Butt;
    else return false;
    return true;
}

bool PropertyRef(IXmlReader* reader, const wchar_t* resourceName,
                 const wchar_t* propertyName, ThreeMfLatticePropertyRef& result)
{
    std::wstring value;
    if (Attribute(reader, resourceName, value)) {
        if (!Unsigned(value, result.resourceId)) return false;
        result.hasResource = true;
    }
    if (Attribute(reader, propertyName, value)) {
        if (!Index(value, result.propertyIndex)) return false;
        result.hasProperty = true;
    }
    return true;
}

bool UnitFloat(const std::wstring& value, float& result)
{
    if (value.empty()) return false;
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(value.c_str(), &end);
    if (end != value.c_str() + value.size() || !std::isfinite(parsed)
        || parsed < 0.0 || parsed > 1.0) return false;
    result = float(parsed);
    return true;
}

int Hex(wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9') return int(ch - L'0');
    if (ch >= L'a' && ch <= L'f') return int(ch - L'a') + 10;
    if (ch >= L'A' && ch <= L'F') return int(ch - L'A') + 10;
    return -1;
}

float SrgbToLinear(uint8_t value)
{
    const float c = float(value) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

bool Color(const std::wstring& value, std::array<float, 3>& result)
{
    if ((value.size() != 7 && value.size() != 9) || value[0] != L'#') return false;
    for (size_t channel = 0; channel < 3; ++channel) {
        const int high = Hex(value[1 + channel * 2]);
        const int low = Hex(value[2 + channel * 2]);
        if (high < 0 || low < 0) return false;
        result[channel] = SrgbToLinear(uint8_t((high << 4) | low));
    }
    if (value.size() == 9 && (Hex(value[7]) < 0 || Hex(value[8]) < 0)) return false;
    return true;
}

bool IsAssociatedResource(const wchar_t* local, const wchar_t* uri)
{
    if (!local || !uri) return false;
    if (Equals(uri, kCoreNamespace) && Equals(local, L"basematerials")) return true;
    if (!Equals(uri, kMaterialNamespace)) return false;
    return Equals(local, L"colorgroup") || Equals(local, L"texture2dgroup")
        || Equals(local, L"compositematerials") || Equals(local, L"multiproperties");
}

model_core::ImportErrorCode ParseModelPart(std::span<const std::byte> bytes,
                                           const std::string& partName,
                                           ThreeMfDisplayCatalog& catalog,
                                           uint64_t& propertyCount,
                                           const std::function<bool()>& cancelled)
{
    if (bytes.empty() || bytes.size() > UINT_MAX) return model_core::ImportErrorCode::ResourceLimit;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), UINT(bytes.size())));
    if (!stream) return model_core::ImportErrorCode::OutOfMemory;
    ComPtr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr))
        || FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit))
        || FAILED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 64))
        || FAILED(reader->SetInput(stream.Get()))) return model_core::ImportErrorCode::MalformedData;

    std::string activeKey;
    ThreeMfDisplayKind activeKind = ThreeMfDisplayKind::Unsupported;
    UINT activeDepth = UINT_MAX;
    uint32_t objectId = 0;
    UINT objectDepth = UINT_MAX;
    ThreeMfLatticeDefinition* lattice = nullptr;
    UINT latticeDepth = UINT_MAX, beamsDepth = UINT_MAX, ballsDepth = UINT_MAX;
    UINT beamSetsDepth = UINT_MAX, beamSetDepth = UINT_MAX;
    bool sawBeams = false, sawBalls = false, sawBeamSets = false;
    std::unordered_set<std::wstring> beamSetIdentifiers;
    std::unordered_set<uint32_t> seenObjectIds;
    uint64_t nodes = 0;
    XmlNodeType type{};
    HRESULT hr = S_OK;
    while ((hr = reader->Read(&type)) == S_OK) {
        if ((++nodes & 1023u) == 0 && cancelled && cancelled())
            return model_core::ImportErrorCode::Cancelled;
        UINT depth = 0;
        if (FAILED(reader->GetDepth(&depth))) return model_core::ImportErrorCode::MalformedData;
        if (type == XmlNodeType_EndElement) {
            if (depth == activeDepth) { activeKey.clear(); activeDepth = UINT_MAX; }
            if (depth == beamSetDepth) beamSetDepth = UINT_MAX;
            if (depth == beamSetsDepth) beamSetsDepth = UINT_MAX;
            if (depth == ballsDepth) ballsDepth = UINT_MAX;
            if (depth == beamsDepth) beamsDepth = UINT_MAX;
            if (depth == latticeDepth) {
                if (!sawBeams) return model_core::ImportErrorCode::MalformedData;
                lattice = nullptr; latticeDepth = UINT_MAX;
            }
            if (depth == objectDepth) { objectId = 0; objectDepth = UINT_MAX; }
            continue;
        }
        if (type != XmlNodeType_Element) continue;
        const wchar_t* local = nullptr;
        const wchar_t* uri = nullptr;
        if (FAILED(reader->GetLocalName(&local, nullptr)) || FAILED(reader->GetNamespaceUri(&uri, nullptr)))
            return model_core::ImportErrorCode::MalformedData;

        if (Equals(uri, kCoreNamespace) && Equals(local, L"object")) {
            std::wstring idText;
            if (!Attribute(reader.Get(), L"id", idText) || !Unsigned(idText, objectId))
                return model_core::ImportErrorCode::MalformedData;
            if (!seenObjectIds.insert(objectId).second)
                return model_core::ImportErrorCode::MalformedData;
            objectDepth = depth;
        }

        const bool beamNamespace = Equals(uri, kBeamNamespace) || Equals(uri, kBallNamespace);
        if (beamNamespace && Equals(local, L"beamlattice")) {
            if (!objectId || lattice) return model_core::ImportErrorCode::MalformedData;
            ThreeMfLatticeDefinition definition;
            definition.packagePart = partName;
            definition.objectId = objectId;
            std::wstring value;
            if (!Attribute(reader.Get(), L"minlength", value)
                || !PositiveDouble(value, definition.minimumLength)
                || !Attribute(reader.Get(), L"radius", value)
                || !PositiveDouble(value, definition.defaultRadius))
                return model_core::ImportErrorCode::MalformedData;
            if (Attribute(reader.Get(), L"cap", value)
                && !CapMode(value, definition.defaultCap))
                return model_core::ImportErrorCode::MalformedData;
            bool hasBallMode = Attribute(reader.Get(), L"ballmode", value);
            if (!hasBallMode)
                hasBallMode = AttributeNamespace(reader.Get(), L"ballmode", kBallNamespace, value);
            if (hasBallMode) {
                if (value == L"none") definition.ballMode = ThreeMfLatticeBallMode::None;
                else if (value == L"mixed") definition.ballMode = ThreeMfLatticeBallMode::Mixed;
                else if (value == L"all") definition.ballMode = ThreeMfLatticeBallMode::All;
                else return model_core::ImportErrorCode::MalformedData;
            }
            bool hasBallRadius = Attribute(reader.Get(), L"ballradius", value);
            if (!hasBallRadius)
                hasBallRadius = AttributeNamespace(reader.Get(), L"ballradius", kBallNamespace, value);
            if (hasBallRadius) {
                if (!PositiveDouble(value, definition.defaultBallRadius))
                    return model_core::ImportErrorCode::MalformedData;
                definition.hasDefaultBallRadius = true;
            }
            if (definition.ballMode != ThreeMfLatticeBallMode::None
                && !definition.hasDefaultBallRadius)
                return model_core::ImportErrorCode::MalformedData;
            bool hasClipMode = Attribute(reader.Get(), L"clippingmode", value);
            if (!hasClipMode) hasClipMode = Attribute(reader.Get(), L"clipping", value);
            if (hasClipMode) {
                if (value == L"none") definition.clipMode = ThreeMfLatticeClipMode::None;
                else if (value == L"inside") definition.clipMode = ThreeMfLatticeClipMode::Inside;
                else if (value == L"outside") definition.clipMode = ThreeMfLatticeClipMode::Outside;
                else return model_core::ImportErrorCode::MalformedData;
            }
            if (Attribute(reader.Get(), L"clippingmesh", value)
                && !Unsigned(value, definition.clippingMeshId))
                return model_core::ImportErrorCode::MalformedData;
            if (Attribute(reader.Get(), L"representationmesh", value)
                && !Unsigned(value, definition.representationMeshId))
                return model_core::ImportErrorCode::MalformedData;
            if (definition.clipMode != ThreeMfLatticeClipMode::None
                && !definition.clippingMeshId)
                return model_core::ImportErrorCode::MalformedData;
            if ((definition.clippingMeshId
                    && (definition.clippingMeshId == objectId
                        || !seenObjectIds.contains(definition.clippingMeshId)))
                || (definition.representationMeshId
                    && (definition.representationMeshId == objectId
                        || !seenObjectIds.contains(definition.representationMeshId))))
                return model_core::ImportErrorCode::MalformedData;
            if (!PropertyRef(reader.Get(), L"pid", L"pindex", definition.defaultProperty)
                || definition.defaultProperty.hasResource != definition.defaultProperty.hasProperty)
                return model_core::ImportErrorCode::MalformedData;
            const auto key = ThreeMfResourceKey(partName, objectId);
            auto inserted = catalog.lattices.emplace(key, std::move(definition));
            if (!inserted.second || catalog.lattices.size() > model_core::kTierBObjectLimit)
                return inserted.second ? model_core::ImportErrorCode::ResourceLimit
                                       : model_core::ImportErrorCode::MalformedData;
            lattice = &inserted.first->second;
            latticeDepth = depth;
            sawBeams = sawBalls = sawBeamSets = false;
            beamSetIdentifiers.clear();
            if (reader->IsEmptyElement())
                return model_core::ImportErrorCode::MalformedData;
            continue;
        }

        if (lattice && beamNamespace) {
            if (Equals(local, L"beams") && depth == latticeDepth + 1) {
                if (sawBeams) return model_core::ImportErrorCode::MalformedData;
                sawBeams = true;
                beamsDepth = depth;
                continue;
            }
            if (Equals(local, L"balls") && depth == latticeDepth + 1) {
                if (sawBalls) return model_core::ImportErrorCode::MalformedData;
                sawBalls = true;
                ballsDepth = depth;
                continue;
            }
            if (Equals(local, L"beamsets") && depth == latticeDepth + 1) {
                if (sawBeamSets) return model_core::ImportErrorCode::MalformedData;
                sawBeamSets = true;
                beamSetsDepth = depth;
                continue;
            }
            if (Equals(local, L"beam") && depth == beamsDepth + 1) {
                if (lattice->beams.size() >= model_core::kTierBPointLimit)
                    return model_core::ImportErrorCode::ResourceLimit;
                ThreeMfLatticeBeam beam;
                beam.cap[0] = beam.cap[1] = lattice->defaultCap;
                std::wstring value;
                if (!Attribute(reader.Get(), L"v1", value) || !Index(value, beam.vertex[0])
                    || !Attribute(reader.Get(), L"v2", value) || !Index(value, beam.vertex[1])
                    || beam.vertex[0] == beam.vertex[1])
                    return model_core::ImportErrorCode::MalformedData;
                for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
                    const wchar_t* radiusName = endpoint ? L"r2" : L"r1";
                    if (Attribute(reader.Get(), radiusName, value)) {
                        if (!PositiveDouble(value, beam.radius[endpoint]))
                            return model_core::ImportErrorCode::MalformedData;
                        beam.hasRadius[endpoint] = true;
                    }
                    const wchar_t* capName = endpoint ? L"cap2" : L"cap1";
                    if (Attribute(reader.Get(), capName, value)
                        && !CapMode(value, beam.cap[endpoint]))
                        return model_core::ImportErrorCode::MalformedData;
                }
                if (beam.hasRadius[1] && !beam.hasRadius[0])
                    return model_core::ImportErrorCode::MalformedData;
                ThreeMfLatticePropertyRef shared;
                if (!PropertyRef(reader.Get(), L"pid", L"__unused", shared))
                    return model_core::ImportErrorCode::MalformedData;
                for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
                    beam.property[endpoint].resourceId = shared.resourceId;
                    beam.property[endpoint].hasResource = shared.hasResource;
                    if (Attribute(reader.Get(), endpoint ? L"p2" : L"p1", value)) {
                        if (!Index(value, beam.property[endpoint].propertyIndex))
                            return model_core::ImportErrorCode::MalformedData;
                        beam.property[endpoint].hasProperty = true;
                    }
                }
                lattice->beams.push_back(beam);
                continue;
            }
            if (Equals(local, L"ball") && depth == ballsDepth + 1) {
                if (lattice->balls.size() >= model_core::kTierBPointLimit)
                    return model_core::ImportErrorCode::ResourceLimit;
                ThreeMfLatticeBall ball;
                std::wstring value;
                if (!Attribute(reader.Get(), L"vindex", value) || !Index(value, ball.vertex))
                    return model_core::ImportErrorCode::MalformedData;
                if (Attribute(reader.Get(), L"r", value)) {
                    if (!PositiveDouble(value, ball.radius))
                        return model_core::ImportErrorCode::MalformedData;
                    ball.hasRadius = true;
                }
                if (!PropertyRef(reader.Get(), L"pid", L"p", ball.property))
                    return model_core::ImportErrorCode::MalformedData;
                lattice->balls.push_back(ball);
                continue;
            }
            if (Equals(local, L"beamset") && depth == beamSetsDepth + 1) {
                if (++lattice->beamSetCount > model_core::kTierBObjectLimit)
                    return model_core::ImportErrorCode::ResourceLimit;
                std::wstring identifier;
                if (Attribute(reader.Get(), L"identifier", identifier) && !identifier.empty()) {
                    if (identifier.size() > 1024 || !beamSetIdentifiers.insert(identifier).second)
                        return model_core::ImportErrorCode::MalformedData;
                }
                beamSetDepth = depth;
                continue;
            }
            if ((Equals(local, L"ref") || Equals(local, L"ballref"))
                && depth == beamSetDepth + 1) {
                std::wstring value;
                uint32_t index = 0;
                if (!Attribute(reader.Get(), L"index", value) || !Index(value, index))
                    return model_core::ImportErrorCode::MalformedData;
                const size_t count = Equals(local, L"ref") ? lattice->beams.size()
                                                            : lattice->balls.size();
                if (index >= count) return model_core::ImportErrorCode::MalformedData;
                continue;
            }
        }

        if (IsAssociatedResource(local, uri)) {
            std::wstring idText, displayText;
            uint32_t id = 0, displayId = 0;
            if (!Attribute(reader.Get(), L"displaypropertiesid", displayText)) continue;
            if (!Attribute(reader.Get(), L"id", idText) || !Unsigned(idText, id)
                || !Unsigned(displayText, displayId)) return model_core::ImportErrorCode::MalformedData;
            const auto key = ThreeMfResourceKey(partName, id);
            const auto displayKey = ThreeMfResourceKey(partName, displayId);
            if (!catalog.associations.emplace(key, displayKey).second)
                return model_core::ImportErrorCode::MalformedData;
            if (catalog.associations.size() > model_core::kTierBMaterialLimit)
                return model_core::ImportErrorCode::ResourceLimit;
            continue;
        }

        ThreeMfDisplayKind kind{};
        const bool displayGroup = Equals(uri, kMaterialNamespace)
            && (Equals(local, L"pbmetallicdisplayproperties")
                || Equals(local, L"pbspeculardisplayproperties")
                || Equals(local, L"pbmetallictexturedisplayproperties")
                || Equals(local, L"pbspeculartexturedisplayproperties")
                || Equals(local, L"translucentdisplayproperties"));
        if (displayGroup) {
            if (Equals(local, L"pbmetallicdisplayproperties")) kind = ThreeMfDisplayKind::Metallic;
            else if (Equals(local, L"pbspeculardisplayproperties")) kind = ThreeMfDisplayKind::Specular;
            else kind = ThreeMfDisplayKind::Unsupported;
            std::wstring idText;
            uint32_t id = 0;
            if (!Attribute(reader.Get(), L"id", idText) || !Unsigned(idText, id))
                return model_core::ImportErrorCode::MalformedData;
            activeKey = ThreeMfResourceKey(partName, id);
            activeKind = kind;
            activeDepth = depth;
            ThreeMfDisplayPropertySet set;
            set.kind = kind;
            if (!catalog.groups.emplace(activeKey, std::move(set)).second)
                return model_core::ImportErrorCode::MalformedData;
            if (catalog.groups.size() > model_core::kTierBMaterialLimit)
                return model_core::ImportErrorCode::ResourceLimit;
            continue;
        }

        if (activeKey.empty() || depth != activeDepth + 1 || !Equals(uri, kMaterialNamespace)) continue;
        if (activeKind == ThreeMfDisplayKind::Unsupported) continue;
        ThreeMfDisplayProperty property;
        std::wstring name;
        if (!Attribute(reader.Get(), L"name", name) || name.empty())
            return model_core::ImportErrorCode::MalformedData;
        if (activeKind == ThreeMfDisplayKind::Metallic && Equals(local, L"pbmetallic")) {
            std::wstring value;
            if (Attribute(reader.Get(), L"metallicness", value) && !UnitFloat(value, property.metallic))
                return model_core::ImportErrorCode::MalformedData;
            if (Attribute(reader.Get(), L"roughness", value) && !UnitFloat(value, property.roughness))
                return model_core::ImportErrorCode::MalformedData;
        } else if (activeKind == ThreeMfDisplayKind::Specular && Equals(local, L"pbspecular")) {
            std::wstring value;
            if (Attribute(reader.Get(), L"specularcolor", value) && !Color(value, property.specular))
                return model_core::ImportErrorCode::MalformedData;
            float glossiness = 0.0f;
            if (Attribute(reader.Get(), L"glossiness", value) && !UnitFloat(value, glossiness))
                return model_core::ImportErrorCode::MalformedData;
            property.roughness = 1.0f - glossiness;
        } else {
            return model_core::ImportErrorCode::MalformedData;
        }
        if (++propertyCount > model_core::kTierBMaterialLimit)
            return model_core::ImportErrorCode::ResourceLimit;
        catalog.groups.at(activeKey).properties.push_back(property);
    }
    if (hr != S_FALSE) return model_core::ImportErrorCode::MalformedData;
    return model_core::ImportErrorCode::None;
}

} // namespace

std::string ThreeMfResourceKey(std::string_view packagePart, uint32_t localResourceId)
{
    while (!packagePart.empty() && packagePart.front() == '/') packagePart.remove_prefix(1);
    std::string result;
    result.reserve(packagePart.size() + 12);
    for (const char ch : packagePart)
        result.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
    result.push_back('#');
    result += std::to_string(localResourceId);
    return result;
}

model_core::ImportErrorCode ScanThreeMfDisplayProperties(
    std::span<const std::byte> packageBytes,
    const ThreeMfOpcPackage& package,
    ThreeMfDisplayCatalog& catalog,
    const std::function<bool()>& cancelled)
{
    catalog = {};
    uint64_t aggregate = 0, propertyCount = 0;
    for (const auto& part : package.parts) {
        if (!part.name.ends_with(".model")) continue;
        if (cancelled && cancelled()) return model_core::ImportErrorCode::Cancelled;
        if (part.expandedBytes > kMaxModelXmlBytes
            || aggregate > kMaxAggregateModelXmlBytes - part.expandedBytes)
            return model_core::ImportErrorCode::ResourceLimit;
        aggregate += part.expandedBytes;
        std::vector<std::byte> xml;
        const auto extracted = ExtractThreeMfOpcPart(packageBytes, part, xml,
                                                      kMaxModelXmlBytes, cancelled);
        if (extracted == ThreeMfOpcError::Cancelled) return model_core::ImportErrorCode::Cancelled;
        if (extracted == ThreeMfOpcError::EntryTooLarge) return model_core::ImportErrorCode::ResourceLimit;
        if (extracted != ThreeMfOpcError::None) return model_core::ImportErrorCode::MalformedData;
        const auto parsed = ParseModelPart(xml, part.name, catalog, propertyCount, cancelled);
        if (parsed != model_core::ImportErrorCode::None) return parsed;
    }
    for (const auto& [resource, display] : catalog.associations)
        if (!catalog.groups.contains(display)) return model_core::ImportErrorCode::MalformedData;
    return model_core::ImportErrorCode::None;
}

} // namespace import_worker

#pragma once

// Bounded, string-free description of optional source features that an import
// process degraded or ignored. A producer sends only stable numeric feature
// IDs, a closed severity, and a saturated occurrence count; the trusted host
// owns every user-visible string and localizes it from its own table.
//
// Wire layout mirrors WireFormat.h: fixed-size, packed, static_assert-pinned,
// and free of pointers or variable-length text. The CompatibilityReport chunk
// is covered by the ordinary section/chunk checksums, not a side channel.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace model_core {

// Critical normally means the import failed; a record at this severity is
// diagnostic only and never changes the typed error code. Degraded means the
// model loaded with an approximation. Ignored means the feature had no effect
// on the preview.
enum class CompatibilitySeverity : uint8_t {
    Critical = 0,
    Degraded = 1,
    Ignored = 2,
};
constexpr uint8_t kCompatibilitySeverityLast = static_cast<uint8_t>(CompatibilitySeverity::Ignored);

// Feature-ID ranges are reserved per producer family and are append-only.
// Later format tasks add named constants inside their own range; they never
// renumber an existing ID. Keep these values independent of SourceFormatId.
//
//   1..999      glTF extensions and glTF-level features
//   1000..1999  3MF extensions/features
//   2000..2999  USD schemas/primitives
//   3000..3999  STEP header/unit features
//   4000..4999  texture features (any format)
//   5000..5999  container/archive features (any format)
constexpr uint32_t kFeatureIdGltfMin = 1;
constexpr uint32_t kFeatureIdGltfMax = 999;
constexpr uint32_t kFeatureIdThreeMfMin = 1000;
constexpr uint32_t kFeatureIdThreeMfMax = 1999;
constexpr uint32_t kFeatureIdUsdMin = 2000;
constexpr uint32_t kFeatureIdUsdMax = 2999;
constexpr uint32_t kFeatureIdStepMin = 3000;
constexpr uint32_t kFeatureIdStepMax = 3999;
constexpr uint32_t kFeatureIdTextureMin = 4000;
constexpr uint32_t kFeatureIdTextureMax = 4999;
constexpr uint32_t kFeatureIdContainerMin = 5000;
constexpr uint32_t kFeatureIdContainerMax = 5999;

constexpr bool IsKnownFeatureId(uint32_t featureId)
{
    return (featureId >= kFeatureIdGltfMin && featureId <= kFeatureIdGltfMax)
        || (featureId >= kFeatureIdThreeMfMin && featureId <= kFeatureIdThreeMfMax)
        || (featureId >= kFeatureIdUsdMin && featureId <= kFeatureIdUsdMax)
        || (featureId >= kFeatureIdStepMin && featureId <= kFeatureIdStepMax)
        || (featureId >= kFeatureIdTextureMin && featureId <= kFeatureIdTextureMax)
        || (featureId >= kFeatureIdContainerMin && featureId <= kFeatureIdContainerMax);
}

// Named glTF IDs used by the current adapter. Unknown glTF extensions share a
// single generic bucket because the worker may not send the extension string;
// the host still shows a specific "unsupported glTF extension" message.
constexpr uint32_t kGltfFeatureClearcoat = 1;
constexpr uint32_t kGltfFeatureSheen = 2;
constexpr uint32_t kGltfFeatureSpecular = 3;
constexpr uint32_t kGltfFeatureIridescence = 4;
constexpr uint32_t kGltfFeatureAnisotropy = 5;
constexpr uint32_t kGltfFeatureVolume = 6;
constexpr uint32_t kGltfFeatureEmissiveStrength = 7;
constexpr uint32_t kGltfFeatureDispersion = 8;
constexpr uint32_t kGltfFeatureUnspecifiedExtension = 900;

// 3MF extension/feature IDs (1000..1999, append-only). A required extension is
// only terminal when the visible geometry depends on it; unknown required
// namespaces are ignored with a record because the Core mesh/build data stays
// authoritative.
constexpr uint32_t kThreeMfFeatureUnknownRequiredExtension = 1000;
// These extensions replace or define the visible geometry with data this
// adapter does not consume: slice stacks, volumetric/implicit geometry and
// encrypted Secure Content packages. A required use stays terminal.
constexpr uint32_t kThreeMfFeatureSliceExtension = 1001;
constexpr uint32_t kThreeMfFeatureVolumetricExtension = 1002;
constexpr uint32_t kThreeMfFeatureImplicitExtension = 1003;
constexpr uint32_t kThreeMfFeatureSecureContentExtension = 1004;
// A triangle whose corners disagree about texture/sampler/layer/mix is split
// into per-material triangles when the budget allows (Degraded), otherwise the
// dominant corner is chosen (Degraded).
constexpr uint32_t kThreeMfFeaturePerCornerSplit = 1010;
constexpr uint32_t kThreeMfFeaturePerCornerDominant = 1011;
// Slicer-private object types (notably "other" referenced by the build) and
// an unsupported object type that was skipped without losing core geometry.
constexpr uint32_t kThreeMfFeatureSlicerPrivateObjectType = 1012;
constexpr uint32_t kThreeMfFeatureUnsupportedObjectType = 1013;
// Optional display/material surfaces this adapter cannot render but whose
// absence does not change the mesh.
constexpr uint32_t kThreeMfFeatureTranslucentDisplayProperty = 1014;
constexpr uint32_t kThreeMfFeatureUnsupportedDisplayProperty = 1015;
constexpr uint32_t kThreeMfFeatureMultipleTextureLayers = 1016;
constexpr uint32_t kThreeMfFeatureTextureDecodeFallback = 1017;
constexpr uint32_t kThreeMfFeatureMultiplePbrLayers = 1018;

// Named STEP header/unit features (3000..3999). These describe a degraded but
// usable main document: the file opened, and the named authored information was
// skipped or assumed. They never make a malformed file acceptable.
constexpr uint32_t kStepFeatureExternalDocumentUnresolved = 3000;
constexpr uint32_t kStepFeatureLengthUnitAssumedMillimetre = 3001;
constexpr uint32_t kStepFeatureLengthUnitContradictory = 3002;
constexpr uint32_t kStepFeatureBoundedRepair = 3003;
// A definition that could not be tessellated was skipped (per-definition
// isolation), so the rest of the assembly still opens.
constexpr uint32_t kStepFeatureUnmeshedDefinition = 3004;
// T08 CAD interchange features (same 3000-range, append-only). IGES has an
// optional global unit flag; absence is a documented millimetre assumption.
// BREP carries no authored unit at all, so its scale is explicitly unknown.
constexpr uint32_t kCadFeatureIgesUnitAssumedMillimetre = 3010;
constexpr uint32_t kCadFeatureIgesUnitNonMillimetre = 3011;
constexpr uint32_t kCadFeatureBrepUnitsUnspecified = 3012;
// T09 OCCT-native formats.
constexpr uint32_t kCadFeatureXbfUnitAssumedMillimetre = 3013;
// FreeCAD has no authored unit: the document's Part payload is millimetres by
// format convention. The warning names the assumption instead of implying it
// was authored data.
constexpr uint32_t kCadFeatureFcstdMillimetresAssumed = 3014;
// A FreeCAD link points at another document/package the host cannot open.
constexpr uint32_t kCadFeatureFcstdExternalLinkIgnored = 3015;
// Unsupported FreeCAD objects are skipped without failing the document.
constexpr uint32_t kCadFeatureFcstdUnsupportedObject = 3016;

// Maps a glTF extension name to its stable numeric feature ID. This is a pure
// lookup; it does not change whether the adapter supports the extension.
inline uint32_t GltfExtensionFeatureId(std::string_view extension)
{
    if (extension == "KHR_materials_clearcoat")
        return kGltfFeatureClearcoat;
    if (extension == "KHR_materials_sheen")
        return kGltfFeatureSheen;
    if (extension == "KHR_materials_specular")
        return kGltfFeatureSpecular;
    if (extension == "KHR_materials_iridescence")
        return kGltfFeatureIridescence;
    if (extension == "KHR_materials_anisotropy")
        return kGltfFeatureAnisotropy;
    if (extension == "KHR_materials_volume")
        return kGltfFeatureVolume;
    if (extension == "KHR_materials_emissive_strength")
        return kGltfFeatureEmissiveStrength;
    if (extension == "KHR_materials_dispersion")
        return kGltfFeatureDispersion;
    return kGltfFeatureUnspecifiedExtension;
}

constexpr uint32_t kMaxCompatibilityRecords = 64;
// A record's count is saturated at this documented cap, not UINT32_MAX, so a
// hostile worker cannot force unbounded host arithmetic or UI formatting.
constexpr uint32_t kCompatibilityCountCap = 1'000'000;
constexpr uint32_t kCompatibilityReportChunkId = 0xf0000003u;

#pragma pack(push, 1)
struct CompatibilityRecord {
    uint32_t featureId;
    uint8_t severity; // CompatibilitySeverity
    uint8_t reserved[3]; // zero
    uint32_t count; // [1, kCompatibilityCountCap]
};
static_assert(sizeof(CompatibilityRecord) == 12, "CompatibilityRecord wire layout changed");

struct CompatibilityReportPayload {
    uint32_t recordCount; // [0, kMaxCompatibilityRecords]
    uint32_t reserved;    // zero
    CompatibilityRecord records[kMaxCompatibilityRecords];
};
static_assert(sizeof(CompatibilityReportPayload) == 776,
              "CompatibilityReportPayload wire layout changed");
#pragma pack(pop)

// Producer-side builder shared by adapters. Add() deduplicates by
// (featureId, severity), saturates the count, and refuses an unknown feature
// ID or an empty/over-cap batch rather than emitting something the host would
// reject.
struct CompatibilityReportBuilder {
    bool Add(uint32_t featureId, CompatibilitySeverity severity, uint32_t count = 1)
    {
        if (!IsKnownFeatureId(featureId)
            || static_cast<uint8_t>(severity) > kCompatibilitySeverityLast || count == 0)
            return false;
        const auto recordCount = payload_.recordCount;
        for (uint32_t i = 0; i < recordCount; ++i) {
            auto& record = payload_.records[i];
            if (record.featureId == featureId && record.severity == static_cast<uint8_t>(severity)) {
                const uint64_t total = uint64_t(record.count) + count;
                record.count = total > kCompatibilityCountCap ? kCompatibilityCountCap
                                                              : static_cast<uint32_t>(total);
                return true;
            }
        }
        if (recordCount >= kMaxCompatibilityRecords)
            return false;
        auto& record = payload_.records[recordCount];
        record.featureId = featureId;
        record.severity = static_cast<uint8_t>(severity);
        record.reserved[0] = record.reserved[1] = record.reserved[2] = 0;
        record.count = count > kCompatibilityCountCap ? kCompatibilityCountCap : count;
        ++payload_.recordCount;
        return true;
    }

    bool Empty() const
    {
        return payload_.recordCount == 0;
    }

    const CompatibilityReportPayload& Payload() const
    {
        return payload_;
    }

  private:
    CompatibilityReportPayload payload_{};
};

} // namespace model_core

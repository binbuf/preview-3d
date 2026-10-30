// Pure layout and policy tests for the compatibility severity model. No
// process launch: proves the fixed wire shape, the producer-side dedup /
// saturation rules, the reserved feature-ID ranges, and that the trusted host
// owns specific localized text for known IDs.

#include "CompatibilityText.h"
#include "model_core/Compatibility.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace model_core;

TEST_CASE("Compatibility records and report have a fixed string-free wire layout",
          "[compatibility]")
{
    CHECK(sizeof(CompatibilityRecord) == 12);
    CHECK(offsetof(CompatibilityRecord, featureId) == 0);
    CHECK(offsetof(CompatibilityRecord, severity) == 4);
    CHECK(offsetof(CompatibilityRecord, reserved) == 5);
    CHECK(offsetof(CompatibilityRecord, count) == 8);

    CHECK(sizeof(CompatibilityReportPayload) == 776);
    CHECK(offsetof(CompatibilityReportPayload, recordCount) == 0);
    CHECK(offsetof(CompatibilityReportPayload, reserved) == 4);
    CHECK(offsetof(CompatibilityReportPayload, records) == 8);
    CHECK(kMaxCompatibilityRecords == 64);
    CHECK(kCompatibilityReportChunkId != 0);

    // Only the closed severity set is accepted by the wire contract.
    CHECK(static_cast<uint8_t>(CompatibilitySeverity::Critical) == 0);
    CHECK(static_cast<uint8_t>(CompatibilitySeverity::Degraded) == 1);
    CHECK(static_cast<uint8_t>(CompatibilitySeverity::Ignored) == 2);
}

TEST_CASE("Compatibility feature ID ranges are reserved per producer family",
          "[compatibility]")
{
    CHECK(IsKnownFeatureId(kFeatureIdGltfMin));
    CHECK(IsKnownFeatureId(kFeatureIdGltfMax));
    CHECK_FALSE(IsKnownFeatureId(0));
    CHECK(IsKnownFeatureId(kFeatureIdThreeMfMin));
    CHECK(IsKnownFeatureId(kFeatureIdThreeMfMax));
    CHECK(IsKnownFeatureId(kFeatureIdUsdMin));
    CHECK(IsKnownFeatureId(kFeatureIdUsdMax));
    CHECK(IsKnownFeatureId(kFeatureIdStepMin));
    CHECK(IsKnownFeatureId(kFeatureIdStepMax));
    CHECK(IsKnownFeatureId(kFeatureIdTextureMin));
    CHECK(IsKnownFeatureId(kFeatureIdTextureMax));
    CHECK(IsKnownFeatureId(kFeatureIdContainerMin));
    CHECK(IsKnownFeatureId(kFeatureIdContainerMax));
    CHECK_FALSE(IsKnownFeatureId(kFeatureIdContainerMax + 1));
}

TEST_CASE("glTF extensions map to stable compatibility feature IDs", "[compatibility]")
{
    CHECK(GltfExtensionFeatureId("KHR_materials_clearcoat") == kGltfFeatureClearcoat);
    CHECK(GltfExtensionFeatureId("KHR_materials_sheen") == kGltfFeatureSheen);
    CHECK(GltfExtensionFeatureId("KHR_materials_specular") == kGltfFeatureSpecular);
    CHECK(GltfExtensionFeatureId("KHR_materials_iridescence") == kGltfFeatureIridescence);
    CHECK(GltfExtensionFeatureId("KHR_materials_anisotropy") == kGltfFeatureAnisotropy);
    CHECK(GltfExtensionFeatureId("KHR_materials_volume") == kGltfFeatureVolume);
    CHECK(GltfExtensionFeatureId("KHR_materials_emissive_strength")
          == kGltfFeatureEmissiveStrength);
    CHECK(GltfExtensionFeatureId("KHR_materials_dispersion") == kGltfFeatureDispersion);
    // Unknown extension names share one bounded bucket; the worker never sends
    // the string itself.
    CHECK(GltfExtensionFeatureId("VENDOR_private_extension") == kGltfFeatureUnspecifiedExtension);
    // Every mapped ID stays inside the reserved glTF range.
    CHECK(IsKnownFeatureId(GltfExtensionFeatureId("KHR_materials_clearcoat")));
    CHECK(IsKnownFeatureId(GltfExtensionFeatureId("VENDOR_private_extension")));
}

TEST_CASE("Compatibility builder deduplicates by ID and severity and saturates counts",
          "[compatibility]")
{
    CompatibilityReportBuilder builder;
    CHECK(builder.Empty());
    REQUIRE(builder.Add(kGltfFeatureClearcoat, CompatibilitySeverity::Degraded, 2));
    REQUIRE(builder.Add(kGltfFeatureClearcoat, CompatibilitySeverity::Degraded, 3));
    REQUIRE(builder.Add(kGltfFeatureClearcoat, CompatibilitySeverity::Ignored, 1));
    REQUIRE(builder.Add(kGltfFeatureSheen, CompatibilitySeverity::Degraded));
    CHECK_FALSE(builder.Empty());
    REQUIRE(builder.Payload().recordCount == 3);
    CHECK(builder.Payload().records[0].featureId == kGltfFeatureClearcoat);
    CHECK(builder.Payload().records[0].severity
          == static_cast<uint8_t>(CompatibilitySeverity::Degraded));
    CHECK(builder.Payload().records[0].count == 5);
    CHECK(builder.Payload().records[1].severity
          == static_cast<uint8_t>(CompatibilitySeverity::Ignored));
    CHECK(builder.Payload().records[2].featureId == kGltfFeatureSheen);

    REQUIRE(builder.Add(kGltfFeatureSheen, CompatibilitySeverity::Degraded, 999'999));
    REQUIRE(builder.Add(kGltfFeatureSheen, CompatibilitySeverity::Degraded, 999'999));
    CHECK(builder.Payload().records[2].count == kCompatibilityCountCap);

    // Every reserved byte is zero.
    for (uint32_t i = 0; i < builder.Payload().recordCount; ++i) {
        const auto& record = builder.Payload().records[i];
        CHECK(record.reserved[0] == 0);
        CHECK(record.reserved[1] == 0);
        CHECK(record.reserved[2] == 0);
    }
}

TEST_CASE("Compatibility builder rejects unknown IDs, empty counts and bad severity",
          "[compatibility]")
{
    CompatibilityReportBuilder builder;
    CHECK_FALSE(builder.Add(0, CompatibilitySeverity::Degraded));
    CHECK_FALSE(builder.Add(kFeatureIdContainerMax + 1, CompatibilitySeverity::Degraded));
    CHECK_FALSE(builder.Add(kGltfFeatureClearcoat, CompatibilitySeverity::Degraded, 0));
    CHECK_FALSE(builder.Add(kGltfFeatureClearcoat, static_cast<CompatibilitySeverity>(3)));
    CHECK(builder.Empty());
}

TEST_CASE("Compatibility builder cannot grow beyond the fixed report cap", "[compatibility]")
{
    CompatibilityReportBuilder builder;
    for (uint32_t i = 0; i < kMaxCompatibilityRecords; ++i)
        REQUIRE(builder.Add(kFeatureIdGltfMin + i, CompatibilitySeverity::Degraded));
    CHECK(builder.Payload().recordCount == kMaxCompatibilityRecords);
    CHECK_FALSE(builder.Add(kFeatureIdGltfMin + kMaxCompatibilityRecords,
                            CompatibilitySeverity::Degraded));
    CHECK(builder.Payload().recordCount == kMaxCompatibilityRecords);
}

TEST_CASE("The trusted host owns specific localized text for known feature IDs",
          "[compatibility]")
{
    const auto clearcoat = CompatibilityFeatureText(kGltfFeatureClearcoat);
    CHECK_FALSE(clearcoat.empty());
    CHECK(clearcoat.find(L"Clearcoat") != std::wstring::npos);
    CHECK_FALSE(CompatibilityFeatureText(kGltfFeatureUnspecifiedExtension).empty());
    // An ID this build has no table entry for falls back to the generic host
    // warning rather than inventing text.
    CHECK(CompatibilityFeatureText(kFeatureIdStepMin).empty());
    CHECK(CompatibilitySeverityLabel(static_cast<uint8_t>(CompatibilitySeverity::Degraded))
          == L"degraded");
    CHECK(CompatibilitySeverityLabel(static_cast<uint8_t>(CompatibilitySeverity::Ignored))
          == L"ignored");

    // A clearcoat report must select its specific message, not the generic one.
    std::vector<CompatibilityRecord> records{{kGltfFeatureClearcoat,
        static_cast<uint8_t>(CompatibilitySeverity::Degraded), {0, 0, 0}, 1}};
    const auto specific = CompatibilityWarningText(records, 1);
    CHECK(specific.find(L"Clearcoat") != std::wstring::npos);
    CHECK(specific.find(L"Some optional glTF features") == std::wstring::npos);

    // A legacy count with no known record keeps the old generic warning.
    const auto generic = CompatibilityWarningText({}, 1);
    CHECK(generic.find(L"Some optional glTF features") != std::wstring::npos);

    // Critical records never become the user-facing compatibility warning.
    records[0].severity = static_cast<uint8_t>(CompatibilitySeverity::Critical);
    CHECK(CompatibilityWarningText(records, 1).find(L"Some optional glTF features")
          != std::wstring::npos);
}

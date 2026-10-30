#pragma once

// Trusted-host localization table for model_core::CompatibilityRecord feature
// IDs. The worker never supplies text: it sends only a stable numeric ID, a
// closed severity, and a saturated count, and this table turns the ID into a
// localized message keyed in the shipped language packs.

#include "Localization.h"
#include "model_core/Compatibility.h"

#include <string>
#include <vector>

// One short, specific message for a known feature ID, or an empty string when
// this build has no host-side text for it (the caller then keeps the existing
// generic optional-feature warning).
inline std::wstring CompatibilityFeatureText(uint32_t featureId)
{
    switch (featureId) {
    case model_core::kGltfFeatureClearcoat:
        return Loc("compat.gltf.clearcoat",
                   L"Clearcoat material layers are approximated; the surface may look less glossy.");
    case model_core::kGltfFeatureSheen:
        return Loc("compat.gltf.sheen",
                   L"Fabric sheen is approximated with the standard material model.");
    case model_core::kGltfFeatureSpecular:
        return Loc("compat.gltf.specular",
                   L"Custom specular color or strength is approximated.");
    case model_core::kGltfFeatureIridescence:
        return Loc("compat.gltf.iridescence",
                   L"Iridescent thin-film coloring is not shown.");
    case model_core::kGltfFeatureAnisotropy:
        return Loc("compat.gltf.anisotropy",
                   L"Anisotropic highlights are approximated as isotropic.");
    case model_core::kGltfFeatureVolume:
        return Loc("compat.gltf.volume",
                   L"Material volume absorption is not shown; the surface uses its base color and alpha.");
    case model_core::kGltfFeatureEmissiveStrength:
        return Loc("compat.gltf.emissiveStrength",
                   L"An emissive-strength multiplier is approximated without high-dynamic-range bloom.");
    case model_core::kGltfFeatureDispersion:
        return Loc("compat.gltf.dispersion",
                   L"Material dispersion is not shown.");
    case model_core::kGltfFeatureUnspecifiedExtension:
        return Loc("compat.gltf.unspecifiedExtension",
                   L"An unsupported glTF extension was ignored; the preview may differ from the source.");
    case model_core::kStepFeatureExternalDocumentUnresolved:
        return Loc("compat.step.externalDocument",
                   L"External STEP references were skipped; the main document is shown.");
    case model_core::kStepFeatureLengthUnitAssumedMillimetre:
        return Loc("compat.step.unitAssumedMillimetres",
                   L"The STEP file declares no length unit; millimetres were assumed.");
    case model_core::kStepFeatureLengthUnitContradictory:
        return Loc("compat.step.unitContradictory",
                   L"The STEP file declares conflicting length units; the first unit was used.");
    case model_core::kStepFeatureBoundedRepair:
        return Loc("compat.step.repaired",
                   L"Some STEP geometry needed bounded repair and is shown degraded.");
    case model_core::kStepFeatureUnmeshedDefinition:
        return Loc("compat.step.unmeshedDefinition",
                   L"Some STEP shapes could not be tessellated and were skipped.");
    default:
        return {};
    }
}

// Composes the user-visible compatibility warning. One specific message per
// known degraded/ignored feature (capped), or the legacy generic text when the
// worker sent only a count. Critical records are diagnostic only.
inline std::wstring CompatibilityWarningText(
    const std::vector<model_core::CompatibilityRecord>& records,
    uint32_t optionalFeatureWarnings, size_t maxMessages = 4)
{
    std::wstring text;
    size_t shown = 0;
    for (const auto& record : records) {
        if (record.severity
                != static_cast<uint8_t>(model_core::CompatibilitySeverity::Degraded)
            && record.severity
                != static_cast<uint8_t>(model_core::CompatibilitySeverity::Ignored))
            continue;
        const auto message = CompatibilityFeatureText(record.featureId);
        if (message.empty())
            continue;
        if (shown++ >= maxMessages)
            break;
        if (!text.empty())
            text += L"\n";
        text += message;
    }
    if (text.empty() && optionalFeatureWarnings)
        text = Loc("warning.optionalFeatures",
                   L"Some optional glTF features are not supported. Their fallback representation is shown.");
    return text;
}

// Closed severity name for diagnostics; never derived from worker text.
inline std::wstring CompatibilitySeverityLabel(uint8_t severity)
{
    switch (static_cast<model_core::CompatibilitySeverity>(severity)) {
    case model_core::CompatibilitySeverity::Critical:
        return Loc("compat.severity.critical", L"critical");
    case model_core::CompatibilitySeverity::Degraded:
        return Loc("compat.severity.degraded", L"degraded");
    case model_core::CompatibilitySeverity::Ignored:
        return Loc("compat.severity.ignored", L"ignored");
    default:
        return Loc("compat.severity.unknown", L"unknown");
    }
}

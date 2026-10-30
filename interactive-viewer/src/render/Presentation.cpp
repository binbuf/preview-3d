#include "Presentation.h"

#include "StudioEnvironment.h"

#include <algorithm>
#include <cmath>

namespace viewer_presentation {
namespace {

constexpr float kSrgbLow = 0.0031308f;

float Sanitize(float value) noexcept
{
    if (std::isnan(value)) return 0.0f;
    // 65504 is the largest finite half-float; keeping the containment value
    // curve-safe avoids x*x overflow in ACES/Filmic before the roll-off.
    if (std::isinf(value)) return value > 0.0f ? 65504.0f : 0.0f;
    return value;
}

std::array<float, 3> Sanitize(const std::array<float, 3>& value) noexcept
{
    return { Sanitize(value[0]), Sanitize(value[1]), Sanitize(value[2]) };
}

float SrgbEncode(float value) noexcept
{
    if (!std::isfinite(value)) return 0.0f;
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= kSrgbLow ? value * 12.92f
                             : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

// Narkowicz's ACES fit. Bounded, monotonic on the non-negative domain and it
// reaches one only asymptotically, exactly like the reference curve.
std::array<float, 3> AcesToneMap(const std::array<float, 3>& linear) noexcept
{
    constexpr float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    std::array<float, 3> result{};
    for (int i = 0; i < 3; ++i) {
        const float x = std::max(0.0f, linear[static_cast<size_t>(i)]);
        const float mapped = (x * (a * x + b)) / (x * (c * x + d) + e);
        result[static_cast<size_t>(i)] = std::clamp(mapped, 0.0f, 1.0f);
    }
    return result;
}

float HableCurve(float x) noexcept
{
    constexpr float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

std::array<float, 3> FilmicToneMap(const std::array<float, 3>& linear) noexcept
{
    constexpr float whitePoint = 11.2f;
    const float whiteScale = HableCurve(whitePoint);
    std::array<float, 3> result{};
    for (int i = 0; i < 3; ++i) {
        const float mapped = HableCurve(std::max(0.0f, linear[static_cast<size_t>(i)])) / whiteScale;
        result[static_cast<size_t>(i)] = std::clamp(mapped, 0.0f, 1.0f);
    }
    return result;
}

} // namespace

MaterialSummary SummarizeMaterials(std::span<const model_core::MaterialPayload> materials,
                                   uint64_t pointCount, uint64_t triangleCount,
                                   bool cadFormat,
                                   std::span<const uint32_t> baseColorImageChunkIds) noexcept
{
    MaterialSummary summary;
    summary.pointCount = pointCount;
    summary.triangleCount = triangleCount;
    summary.cadFormat = cadFormat;

    const size_t count = std::min<size_t>(materials.size(), kMaterialScanCap);
    for (size_t i = 0; i < count; ++i) {
        const model_core::MaterialPayload& material = materials[i];
        ++summary.materialCount;
        if (i < baseColorImageChunkIds.size() && baseColorImageChunkIds[i] != 0)
            ++summary.baseColorTextureCount;
        if ((material.flags & model_core::kMaterialFlagUnlit) != 0) ++summary.unlitMaterialCount;
        if ((material.flags & model_core::kMaterialFlagTransmissive) != 0
            && material.transmissionFactor > 0.0f)
            ++summary.transmissionMaterialCount;
        if (material.alphaMode != uint32_t(model_core::AlphaModeId::Opaque)
            || material.baseColorFactor[3] < 1.0f)
            ++summary.alphaMaterialCount;

        const float metallic = std::clamp(material.metallicFactor, 0.0f, 1.0f);
        const float roughness = std::clamp(material.roughnessFactor, 0.0f, 1.0f);
        summary.metallicMin = std::min(summary.metallicMin, metallic);
        summary.metallicMax = std::max(summary.metallicMax, metallic);
        summary.roughnessMin = std::min(summary.roughnessMin, roughness);
        summary.roughnessMax = std::max(summary.roughnessMax, roughness);
    }
    // Base-color texture presence is not on MaterialPayload; the caller fills
    // it from the image dependency slots. Keep a deterministic default here.
    return summary;
}

PresentationPreset SelectPresentationPreset(const MaterialSummary& summary,
                                            model_core::PresentationProfile catalogProfile) noexcept
{
    if (summary.PointOnly() || catalogProfile == model_core::PresentationProfile::PointCloud)
        return PresentationPreset::PointCloud;

    if (summary.cadFormat || catalogProfile == model_core::PresentationProfile::StudioCad)
        return PresentationPreset::StudioCad;

    if (summary.materialCount > 0) {
        // A majority alpha/transmission asset is reviewed against the
        // transparency checker instead of the dark studio backdrop.
        if (summary.transmissionMaterialCount * 2 >= summary.materialCount
            || summary.alphaMaterialCount * 2 >= summary.materialCount)
            return PresentationPreset::Transparent;
    }

    if (summary.materialCount > 0 && summary.baseColorTextureCount == 0
        && summary.triangleCount >= kScanTriangleThreshold
        && summary.unlitMaterialCount == 0)
        return PresentationPreset::ScanSculpt;

    return PresentationPreset::StudioPbr;
}

PresetSettings SettingsForPreset(PresentationPreset preset) noexcept
{
    PresetSettings settings;
    settings.preset = preset;
    switch (preset) {
    case PresentationPreset::StudioCad:
        settings.environmentIntensity = 0.95f;
        settings.keyScale = 0.95f;
        settings.fillScale = 0.85f;
        settings.rimScale = 0.90f;
        settings.background = BackgroundMode::Dark;
        settings.gridVisible = true;
        settings.shadowEnabled = true;
        break;
    case PresentationPreset::ScanSculpt:
        settings.environmentIntensity = 0.85f;
        settings.keyScale = 1.15f;
        settings.fillScale = 0.70f;
        settings.rimScale = 0.55f;
        settings.background = BackgroundMode::MidGrey;
        settings.gridVisible = false;
        settings.shadowEnabled = true;
        break;
    case PresentationPreset::Transparent:
        settings.environmentIntensity = 1.0f;
        settings.background = BackgroundMode::Checkerboard;
        settings.gridVisible = false;
        settings.shadowEnabled = false;
        break;
    case PresentationPreset::PointCloud:
        settings.environmentIntensity = 0.80f;
        settings.fillScale = 0.80f;
        settings.rimScale = 0.80f;
        settings.background = BackgroundMode::Dark;
        settings.gridVisible = false;
        settings.shadowEnabled = false;
        break;
    case PresentationPreset::StudioPbr:
    default:
        settings.background = BackgroundMode::Dark;
        settings.gridVisible = true;
        settings.shadowEnabled = true;
        break;
    }
    return settings;
}

PresetSettings ResolvePresetSettings(const MaterialSummary& summary,
                                     model_core::PresentationProfile catalogProfile,
                                     bool manualOverride,
                                     PresentationPreset manualPreset) noexcept
{
    const PresentationPreset preset = manualOverride
        ? manualPreset
        : SelectPresentationPreset(summary, catalogProfile);
    return SettingsForPreset(preset);
}

bool ResolveShadowVisible(bool userOverridden, bool currentVisible,
                          bool presetDefault) noexcept
{
    return userOverridden ? currentVisible : presetDefault;
}

float ClampExposure(float exposure) noexcept
{
    if (!std::isfinite(exposure)) return 1.0f;
    return std::clamp(exposure, kMinExposure, kMaxExposure);
}

std::array<float, 3> ToneMapLinear(const std::array<float, 3>& linear, float exposure,
                                   ToneMapMode mode) noexcept
{
    const std::array<float, 3> scaled = {
        Sanitize(linear[0]) * ClampExposure(exposure),
        Sanitize(linear[1]) * ClampExposure(exposure),
        Sanitize(linear[2]) * ClampExposure(exposure),
    };
    switch (mode) {
    case ToneMapMode::Aces:
        return AcesToneMap(scaled);
    case ToneMapMode::Filmic:
        return FilmicToneMap(scaled);
    case ToneMapMode::None: {
        std::array<float, 3> clamped{};
        for (int i = 0; i < 3; ++i)
            clamped[static_cast<size_t>(i)] = std::clamp(scaled[static_cast<size_t>(i)], 0.0f, 1.0f);
        return clamped;
    }
    case ToneMapMode::Neutral:
    default:
        return studio_ibl::NeutralToneMapLinear(scaled, 1.0f);
    }
}

std::array<float, 3> ToneMapToSrgb(const std::array<float, 3>& linear, float exposure,
                                   ToneMapMode mode) noexcept
{
    const auto mapped = ToneMapLinear(linear, exposure, mode);
    return { SrgbEncode(mapped[0]), SrgbEncode(mapped[1]), SrgbEncode(mapped[2]) };
}

std::array<float, 4> BackgroundLinear(BackgroundMode mode) noexcept
{
    switch (mode) {
    case BackgroundMode::MidGrey:
        return { 0.21f, 0.21f, 0.22f, 1.0f };
    case BackgroundMode::Transparent:
    case BackgroundMode::Checkerboard:
        return { 0.0f, 0.0f, 0.0f, 0.0f };
    case BackgroundMode::Dark:
    default:
        return { 0x1C / 255.0f, 0x1C / 255.0f, 0x1E / 255.0f, 1.0f };
    }
}

std::array<float, 3> BackgroundDisplay(BackgroundMode mode, float exposure,
                                       ToneMapMode toneMap) noexcept
{
    std::array<float, 3> color{};
    if (mode == BackgroundMode::Checkerboard) {
        const float mean = (kCheckerDark + kCheckerLight) * 0.5f;
        color = { mean, mean, mean };
    } else {
        const auto linear = BackgroundLinear(mode);
        color = { linear[0], linear[1], linear[2] };
    }
    return ToneMapToSrgb(color, exposure, toneMap);
}

const wchar_t* PresetName(PresentationPreset preset) noexcept
{
    switch (preset) {
    case PresentationPreset::StudioCad: return L"Studio CAD";
    case PresentationPreset::ScanSculpt: return L"Scan / Sculpt";
    case PresentationPreset::Transparent: return L"Transparent";
    case PresentationPreset::PointCloud: return L"Point cloud";
    case PresentationPreset::StudioPbr:
    default: return L"Studio PBR";
    }
}

const wchar_t* ToneMapName(ToneMapMode mode) noexcept
{
    switch (mode) {
    case ToneMapMode::Aces: return L"ACES";
    case ToneMapMode::Filmic: return L"Filmic";
    case ToneMapMode::None: return L"None";
    case ToneMapMode::Neutral:
    default: return L"Neutral";
    }
}

const wchar_t* BackgroundName(BackgroundMode mode) noexcept
{
    switch (mode) {
    case BackgroundMode::MidGrey: return L"Mid grey";
    case BackgroundMode::Transparent: return L"Transparent";
    case BackgroundMode::Checkerboard: return L"Checkerboard";
    case BackgroundMode::Dark:
    default: return L"Dark";
    }
}

} // namespace viewer_presentation

#pragma once

// T15 presentation state: the HDR intermediate, the tone-map options and the
// material-aware Studio presets.
//
// Everything in this header is a pure function over bounded data. The renderer
// uses the same code the unit tests exercise, so the GPU resolve pass and the
// preset decision cannot drift from their CPU reference. Direct3D types are
// deliberately absent; only the material wire payload and the catalog's
// presentation enum are visible.

#include <array>
#include <cstdint>
#include <span>

#include "model_core/FormatCatalog.h"
#include "model_core/MaterialPayload.h"

namespace viewer_presentation {

inline constexpr float kMinExposure = 0.05f;
inline constexpr float kMaxExposure = 8.0f;
// Preset selection and summary computation never inspect more than this many
// material records, regardless of what a producer claims.
inline constexpr uint32_t kMaterialScanCap = 4096;
// A dense, untextured mesh with at least this many triangles is a scan or a
// sculpt rather than a hand-authored PBR asset.
inline constexpr uint64_t kScanTriangleThreshold = 50000;

enum class ToneMapMode : uint8_t {
    Neutral = 0, // Khronos PBR Neutral (default)
    Aces = 1,
    Filmic = 2,
    None = 3,
};

enum class BackgroundMode : uint8_t {
    Dark = 0,         // Windows Photos dark #1C1C1E (default)
    MidGrey = 1,      // studio gray #808080
    Transparent = 2,  // alpha is preserved for capture/shell use
    Checkerboard = 3, // visible transparency checker behind the asset
    // Appended so the four saved values above keep their original meaning.
    Custom = 4,       // user-selected display color (see customColor)
};

// Default custom background display color, packed 0xRRGGBB. The value is only
// used while BackgroundMode::Custom is selected; it is persisted independently
// so switching modes never loses the user's choice.
inline constexpr uint32_t kDefaultCustomBackground = 0x336699u;

// Packed 0xRRGGBB helpers. Validation clamps each channel to 0..255.
uint32_t NormalizeBackgroundColor(uint32_t packed) noexcept;
uint32_t PackBackgroundColor(float red, float green, float blue) noexcept;

enum class PresentationPreset : uint8_t {
    StudioPbr = 0,
    StudioCad = 1,
    ScanSculpt = 2,
    Transparent = 3,
    PointCloud = 4,
};

// Bounded facts derived from the imported material/geometry catalog. Counts
// saturate at kMaterialScanCap and every range is normalized to [0,1].
struct MaterialSummary
{
    uint32_t materialCount = 0;
    uint32_t baseColorTextureCount = 0;
    uint32_t alphaMaterialCount = 0;
    uint32_t transmissionMaterialCount = 0;
    uint32_t unlitMaterialCount = 0;
    float metallicMin = 1.0f;
    float metallicMax = 0.0f;
    float roughnessMin = 1.0f;
    float roughnessMax = 0.0f;
    uint64_t pointCount = 0;
    uint64_t triangleCount = 0;
    bool cadFormat = false;

    bool PointOnly() const noexcept { return pointCount > 0 && triangleCount == 0; }
    bool HasBaseColorTexture() const noexcept { return baseColorTextureCount > 0; }
};

// The view state a preset applies. It never mutates imported materials.
struct PresetSettings
{
    PresentationPreset preset = PresentationPreset::StudioPbr;
    float environmentIntensity = 1.0f;
    // Multipliers on the studio rig's direct key/fill/rim. The renderer scales
    // the whole rig (direct + IBL) by environmentIntensity as well, matching
    // F3D's paired render.light.intensity / IBLIntensity behavior.
    float keyScale = 1.0f;
    float fillScale = 1.0f;
    float rimScale = 1.0f;
    float exposure = 1.0f;
    // Presets keep their historical appearance default for callers that still
    // consume a complete preset record, but the shipping viewer never applies
    // this field: background selection is a global user preference (T21a).
    BackgroundMode background = BackgroundMode::Dark;
    ToneMapMode toneMap = ToneMapMode::Neutral;
    bool gridVisible = true;
    // T16 ground contact shadow. CAD/PBR/Scan opt in by default; point clouds
    // and transparent/checkerboard assets do not. The grid is independent.
    bool shadowEnabled = true;
    // T21a illuminated ground area. Presets never override the user preference.
    bool groundHighlightEnabled = true;
};

// Computes the bounded summary. Only the first kMaterialScanCap records are
// read; the counts reflect what was scanned, never an unbounded claim.
// `baseColorImageChunkIds`, when supplied, is index-aligned with `materials`
// and records each material's base-color image dependency (0 = none), which
// MaterialPayload itself does not carry.
MaterialSummary SummarizeMaterials(std::span<const model_core::MaterialPayload> materials,
                                   uint64_t pointCount, uint64_t triangleCount,
                                   bool cadFormat,
                                   std::span<const uint32_t> baseColorImageChunkIds = {}) noexcept;

// Pure catalog/summary -> preset mapping. An explicit point-only scene always
// wins, then a CAD family, then a mostly transparent asset, then a dense
// untextured scan/sculpt; everything else is Studio PBR.
PresentationPreset SelectPresentationPreset(const MaterialSummary& summary,
                                            model_core::PresentationProfile catalogProfile) noexcept;

PresetSettings SettingsForPreset(PresentationPreset preset) noexcept;

// Manual override wins, but still returns a complete settings record so the
// caller can reapply exposure/background while the preset stays pinned.
PresetSettings ResolvePresetSettings(const MaterialSummary& summary,
                                     model_core::PresentationProfile catalogProfile,
                                     bool manualOverride,
                                     PresentationPreset manualPreset) noexcept;

// T16 toggle/preset arbitration: an explicit user toggle (H / menu) survives a
// later preset application; otherwise the preset's default wins.
bool ResolveShadowVisible(bool userOverridden, bool currentVisible,
                          bool presetDefault) noexcept;

float ClampExposure(float exposure) noexcept;

// Tone maps linear HDR to display-linear [0,1]. Exposure is applied first.
// Non-finite channels are contained: NaN becomes 0, positive infinity is
// treated as a very large finite value so the curve still rolls off.
std::array<float, 3> ToneMapLinear(const std::array<float, 3>& linear, float exposure,
                                   ToneMapMode mode) noexcept;

// ToneMapLinear followed by the sRGB transfer. This is what the resolve pass
// writes into the UNORM back buffer.
std::array<float, 3> ToneMapToSrgb(const std::array<float, 3>& linear, float exposure,
                                   ToneMapMode mode) noexcept;

// Display-encoded (sRGB) backdrop color. This is written directly to the
// display-space backdrop target, so it is independent of exposure and the
// selected tone curve. #1C1C1E therefore reaches the screen as #1C1C1E.
std::array<float, 3> BackgroundDisplayColor(BackgroundMode mode, uint32_t customColor) noexcept;

// The display-space illuminated ground tint for a backdrop. It is a subtle
// local lift: the backdrop plus a small fixed increment, never a bright sweep.
std::array<float, 3> GroundHighlightColor(const std::array<float, 3>& backdropDisplay) noexcept;

// Display-space compositing reference used by the resolve pass and tests.
// `scene` is the straight-alpha HDR scene color; the CPU mirrors
// tone-map-then-composite-over-display-backdrop. For coverage 0 the backdrop is
// returned unchanged, and for coverage 1 only the tone-mapped model remains.
std::array<float, 3> CompositeOverBackground(const std::array<float, 3>& sceneLinear,
                                             float coverage, float exposure,
                                             ToneMapMode mode,
                                             const std::array<float, 3>& backdropDisplay) noexcept;

// Checkerboard shades in display space (resolve pass and tests share them).
constexpr float kCheckerDark = 0.10f;
constexpr float kCheckerLight = 0.18f;

const wchar_t* PresetName(PresentationPreset preset) noexcept;
const wchar_t* ToneMapName(ToneMapMode mode) noexcept;
const wchar_t* BackgroundName(BackgroundMode mode) noexcept;

} // namespace viewer_presentation

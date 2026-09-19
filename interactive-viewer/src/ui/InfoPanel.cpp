#include "framework.h"
#include "InfoPanel.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace
{
constexpr float kPanelWidthLogical = 300.0f;

std::wstring FormatCount(std::uint64_t value)
{
    std::wstring text = std::to_wstring(value);
    for (std::ptrdiff_t index = static_cast<std::ptrdiff_t>(text.size()) - 3; index > 0; index -= 3)
    {
        text.insert(static_cast<std::size_t>(index), 1, L',');
    }
    return text;
}

std::wstring YesNo(bool value)
{
    return value ? L"Yes" : L"No";
}

// A texture-slot row reads as a count when any material references it, and
// falls back to "Constant"/"No" when it's factor-only or entirely unset —
// matching the (?) fields the user's spec couldn't pin an exact int/bool
// shape for ahead of time.
std::wstring TextureSlotValue(int textureCount, bool hasConstantFactor)
{
    if (textureCount > 0) return L"Textured (" + std::to_wstring(textureCount) + L")";
    if (hasConstantFactor) return L"Constant";
    return L"No";
}

std::wstring FormatDimension(double value, double metersPerUnit)
{
    std::wostringstream text;
    if (metersPerUnit > 0) value *= metersPerUnit;
    if (value && std::abs(value) < 0.001) text << std::scientific << std::setprecision(3);
    else text << std::fixed << std::setprecision(3);
    text << value << (metersPerUnit > 0 ? L" m" : L" units");
    return text.str();
}
}

std::vector<InfoPanelSection> BuildInfoPanelSections(
    const ModelStats& stats, std::uint64_t triangleCount, std::uint64_t vertexCount,
    const DirectX::XMFLOAT3& boundsMin, const DirectX::XMFLOAT3& boundsMax,
    double metersPerUnit, std::uint64_t pointCount, bool boundsVerified, model_core::SourceFormatId format, const double* dimensions)
{
    std::vector<InfoPanelSection> sections;

    sections.push_back({ L"Dimensions",
        {
            { L"Width (X)", FormatDimension(dimensions ? dimensions[0] : double(boundsMax.x) - double(boundsMin.x), metersPerUnit) },
            { L"Depth (Y)", FormatDimension(dimensions ? dimensions[1] : double(boundsMax.y) - double(boundsMin.y), metersPerUnit) },
            { L"Height (Z)", FormatDimension(dimensions ? dimensions[2] : double(boundsMax.z) - double(boundsMin.z), metersPerUnit) },
            { L"Bounds", boundsVerified ? L"Verified" : L"Provisional (loading)" },
        } });

    sections.push_back({ L"Mesh Data",
        {
            { L"Triangles", FormatCount(triangleCount) },
            { L"Vertices", FormatCount(vertexCount) },
            { L"Points", FormatCount(pointCount) },
            { L"UV Set 0", YesNo(stats.hasUv0) },
            { L"UV Set 1", YesNo(stats.hasUv1) },
            { L"Vertex Colors", YesNo(stats.hasVertexColors) },
            { L"Material IDs", std::to_wstring(stats.materialCount) },
        } });

    sections.push_back({ L"Texture Data",
        {
            { L"Albedo", TextureSlotValue(stats.albedoTextureCount, stats.hasConstantBaseColor) },
            { L"Normal", TextureSlotValue(stats.normalTextureCount, false) },
            { L"Specular / Metallic", TextureSlotValue(stats.specularMetallicTextureCount, false) },
            { L"Gloss / Roughness", stats.specularMetallicTextureCount > 0 ? L"Packed with Specular/Metallic" : L"No" },
            { L"Occlusion", TextureSlotValue(stats.occlusionTextureCount, false) },
            { L"Emissive", TextureSlotValue(stats.emissiveTextureCount, false) },
            { L"Opacity", YesNo(stats.hasTransparency) },
            { L"Base Color", stats.hasConstantBaseColor ? L"Constant" : (stats.albedoTextureCount > 0 ? L"Textured" : L"No") },
            { L"Specular Color", TextureSlotValue(0, stats.hasConstantSpecularColor) },
            { L"Emissive Color", TextureSlotValue(stats.emissiveTextureCount, stats.hasConstantEmissiveColor) },
        } });

    sections.push_back({ L"Animation Data",
        {
            { L"Bones", std::to_wstring(stats.boneCount) },
            { L"Skins", std::to_wstring(stats.skinCount) },
            { L"Animation Takes", std::to_wstring(stats.animationCount) },
        } });

    sections.push_back({ L"Performance Data",
        {
            { L"Draw Calls", std::to_wstring(stats.drawCallCount) },
        } });

    sections.push_back({ L"Scene Data",
        {
            { L"Nodes", std::to_wstring(stats.nodeCount) },
            { L"Meshes", std::to_wstring(stats.meshCount) },
            { L"Format", format == model_core::SourceFormatId::Gltf ? L"glTF" : format == model_core::SourceFormatId::Glb ? L"GLB"
                : format == model_core::SourceFormatId::Stl ? L"STL (binary)"
                : format == model_core::SourceFormatId::AsciiStl ? L"STL (ASCII)"
                : format == model_core::SourceFormatId::Ply ? L"PLY (binary)"
                : format == model_core::SourceFormatId::AsciiPly ? L"PLY (ASCII)"
                : format == model_core::SourceFormatId::Obj ? L"OBJ"
                : format == model_core::SourceFormatId::Fbx ? L"FBX"
                : format == model_core::SourceFormatId::Usda ? L"USD (ASCII)"
                : format == model_core::SourceFormatId::Usdc ? L"USD (crate)"
                : format == model_core::SourceFormatId::Usdz ? L"USDZ"
                : format == model_core::SourceFormatId::ThreeMf ? L"3MF"
                : format == model_core::SourceFormatId::Step ? L"STEP" : L"Unknown" },
            { L"Units", metersPerUnit > 0 ? L"Metres" : L"Unspecified" },
        } });

    return sections;
}

std::vector<InfoPanelSection> BuildInfoPanelSections(
    const ModelData& metadata, bool showNativeOrientation, GroundAxis groundAxis)
{
    double dimensions[3] = {metadata.relativeMax[0]-metadata.relativeMin[0],
        metadata.relativeMax[1]-metadata.relativeMin[1], metadata.relativeMax[2]-metadata.relativeMin[2]};
    // Exact axis permutations avoid float residue on planar/tiny models.
    PermuteGroundedDimensions(dimensions, groundAxis, metadata.source.upAxis, showNativeOrientation);
    return BuildInfoPanelSections(metadata.stats,metadata.triangleCount,metadata.vertexCount,
        metadata.boundsMin,metadata.boundsMax,metadata.source.metersPerUnit,metadata.pointCount,
        metadata.boundsVerified,metadata.source.format,dimensions);
}

InfoPanelLayout ComputeInfoPanelLayout(int viewportWidth, int viewportHeight,
    int titleBarHeight, int bottomBarHeight, float dpiScale)
{
    InfoPanelLayout layout;
    const float width = std::min(kPanelWidthLogical * std::max(0.75f, dpiScale),
        std::max(0.0f, static_cast<float>(viewportWidth) * 0.9f));
    layout.right = static_cast<float>(viewportWidth);
    layout.left = layout.right - width;
    layout.top = static_cast<float>(titleBarHeight);
    layout.bottom = static_cast<float>(viewportHeight - bottomBarHeight);
    return layout;
}

namespace
{
// Kept numerically identical to the header/row/section-gap math in
// D3D11On12Overlay::DrawInfoPanel (D3D11On12Overlay.cpp) — see that function before changing
// any of these.
constexpr float kHeaderTopPad = 18.0f;
constexpr float kHeaderAdvance = 34.0f;
constexpr float kRowHeight = 24.0f;
constexpr float kSectionGap = 18.0f;
// Extra breathing room after the last row, so scrolling to the end doesn't
// leave the final label flush against the panel's bottom edge.
constexpr float kBottomPadding = 16.0f;

float ScaleRound(float value, float dpiScale)
{
    return std::round(value * dpiScale);
}
}

InfoPanelScrollMetrics ComputeInfoPanelScrollMetrics(
    const std::vector<InfoPanelSection>& sections, float dpiScale)
{
    InfoPanelScrollMetrics metrics;
    metrics.headerHeight = ScaleRound(kHeaderTopPad, dpiScale) + ScaleRound(kHeaderAdvance, dpiScale);
    const float rowHeight = ScaleRound(kRowHeight, dpiScale);
    const float sectionGap = ScaleRound(kSectionGap, dpiScale);
    for (const InfoPanelSection& section : sections)
    {
        metrics.contentHeight += static_cast<float>(section.rows.size()) * rowHeight + sectionGap;
    }
    if (!sections.empty()) metrics.contentHeight += ScaleRound(kBottomPadding, dpiScale);
    return metrics;
}

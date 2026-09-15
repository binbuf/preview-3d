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

std::wstring FormatMeters(float value)
{
    std::wostringstream text;
    text << std::fixed << std::setprecision(3) << value << L" m";
    return text.str();
}
}

std::vector<InfoPanelSection> BuildInfoPanelSections(
    const ModelStats& stats, std::uint64_t triangleCount, std::uint64_t vertexCount,
    const DirectX::XMFLOAT3& boundsMin, const DirectX::XMFLOAT3& boundsMax)
{
    std::vector<InfoPanelSection> sections;

    sections.push_back({ L"Dimensions",
        {
            { L"Width (X)", FormatMeters(boundsMax.x - boundsMin.x) },
            { L"Depth (Y)", FormatMeters(boundsMax.y - boundsMin.y) },
            { L"Height (Z)", FormatMeters(boundsMax.z - boundsMin.z) },
        } });

    sections.push_back({ L"Mesh Data",
        {
            { L"Triangles", FormatCount(triangleCount) },
            { L"Vertices", FormatCount(vertexCount) },
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
        } });

    return sections;
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

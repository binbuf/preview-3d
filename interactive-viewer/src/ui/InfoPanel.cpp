#include "framework.h"
#include "InfoPanel.h"
#include "Localization.h"

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
    return value ? Loc("infopanel.yes", L"Yes") : Loc("infopanel.no", L"No");
}

// A texture-slot row reads as a count when any material references it, and
// falls back to "Constant"/"No" when it's factor-only or entirely unset —
// matching the (?) fields the user's spec couldn't pin an exact int/bool
// shape for ahead of time.
std::wstring TextureSlotValue(int textureCount, bool hasConstantFactor)
{
    if (textureCount > 0) return LocFormat("infopanel.texturedCount", L"Textured ({0})", { std::to_wstring(textureCount) });
    if (hasConstantFactor) return Loc("infopanel.constant", L"Constant");
    return Loc("infopanel.no", L"No");
}

std::wstring FormatDimension(double value, double metersPerUnit)
{
    std::wostringstream text;
    if (metersPerUnit > 0) value *= metersPerUnit;
    if (value && std::abs(value) < 0.001) text << std::scientific << std::setprecision(3) << value;
    else text << std::fixed << std::setprecision(3) << value;
    const std::wstring unit = metersPerUnit > 0
        ? Loc("infopanel.metreSuffix", L"m")
        : Loc("infopanel.unitSuffix", L"units");
    // The unit is a localized fragment; LocalizedJoin owns the separating space
    // so a translation that drops it can never jam the number against the unit.
    return LocalizedJoin(text.str(), unit);
}
}

std::vector<InfoPanelSection> BuildInfoPanelSections(
    const ModelStats& stats, std::uint64_t triangleCount, std::uint64_t vertexCount,
    const DirectX::XMFLOAT3& boundsMin, const DirectX::XMFLOAT3& boundsMax,
    double metersPerUnit, std::uint64_t pointCount, bool boundsVerified, model_core::SourceFormatId format, const double* dimensions)
{
    std::vector<InfoPanelSection> sections;

    sections.push_back({ Loc("infopanel.dimensions", L"Dimensions"),
        {
            { Loc("infopanel.width.x", L"Width (X)"), FormatDimension(dimensions ? dimensions[0] : double(boundsMax.x) - double(boundsMin.x), metersPerUnit) },
            { Loc("infopanel.depth.y", L"Depth (Y)"), FormatDimension(dimensions ? dimensions[1] : double(boundsMax.y) - double(boundsMin.y), metersPerUnit) },
            { Loc("infopanel.height.z", L"Height (Z)"), FormatDimension(dimensions ? dimensions[2] : double(boundsMax.z) - double(boundsMin.z), metersPerUnit) },
            { Loc("infopanel.bounds", L"Bounds"), boundsVerified ? Loc("infopanel.verified", L"Verified") : Loc("infopanel.provisional.loading", L"Provisional (loading)") },
        } });

    sections.push_back({ Loc("infopanel.mesh.data", L"Mesh Data"),
        {
            { Loc("infopanel.triangles", L"Triangles"), FormatCount(triangleCount) },
            { Loc("infopanel.vertices", L"Vertices"), FormatCount(vertexCount) },
            { Loc("infopanel.points", L"Points"), FormatCount(pointCount) },
            { Loc("infopanel.uv.set.0", L"UV Set 0"), YesNo(stats.hasUv0) },
            { Loc("infopanel.uv.set.1", L"UV Set 1"), YesNo(stats.hasUv1) },
            { Loc("infopanel.vertex.colors", L"Vertex Colors"), YesNo(stats.hasVertexColors) },
            { Loc("infopanel.material.ids", L"Material IDs"), std::to_wstring(stats.materialCount) },
        } });

    sections.push_back({ Loc("infopanel.texture.data", L"Texture Data"),
        {
            { Loc("infopanel.albedo", L"Albedo"), TextureSlotValue(stats.albedoTextureCount, stats.hasConstantBaseColor) },
            { Loc("infopanel.normal", L"Normal"), TextureSlotValue(stats.normalTextureCount, false) },
            { Loc("infopanel.specular.metallic", L"Specular / Metallic"), TextureSlotValue(stats.specularMetallicTextureCount, false) },
            { Loc("infopanel.gloss.roughness", L"Gloss / Roughness"), stats.specularMetallicTextureCount > 0 ? Loc("infopanel.packed.with.specular.metallic", L"Packed with Specular/Metallic") : Loc("infopanel.no", L"No") },
            { Loc("infopanel.occlusion", L"Occlusion"), TextureSlotValue(stats.occlusionTextureCount, false) },
            { Loc("infopanel.emissive", L"Emissive"), TextureSlotValue(stats.emissiveTextureCount, false) },
            { Loc("infopanel.opacity", L"Opacity"), YesNo(stats.hasTransparency) },
            { Loc("infopanel.base.color", L"Base Color"), stats.hasConstantBaseColor ? Loc("infopanel.constant", L"Constant") : (stats.albedoTextureCount > 0 ? Loc("infopanel.textured", L"Textured") : Loc("infopanel.no", L"No")) },
            { Loc("infopanel.specular.color", L"Specular Color"), TextureSlotValue(0, stats.hasConstantSpecularColor) },
            { Loc("infopanel.emissive.color", L"Emissive Color"), TextureSlotValue(stats.emissiveTextureCount, stats.hasConstantEmissiveColor) },
        } });

    sections.push_back({ Loc("infopanel.animation.data", L"Animation Data"),
        {
            { Loc("infopanel.bones", L"Bones"), std::to_wstring(stats.boneCount) },
            { Loc("infopanel.skins", L"Skins"), std::to_wstring(stats.skinCount) },
            { Loc("infopanel.animation.takes", L"Animation Takes"), std::to_wstring(stats.animationCount) },
        } });

    sections.push_back({ Loc("infopanel.performance.data", L"Performance Data"),
        {
            { Loc("infopanel.draw.calls", L"Draw Calls"), std::to_wstring(stats.drawCallCount) },
        } });

    sections.push_back({ Loc("infopanel.scene.data", L"Scene Data"),
        {
            { Loc("infopanel.nodes", L"Nodes"), std::to_wstring(stats.nodeCount) },
            { Loc("infopanel.meshes", L"Meshes"), std::to_wstring(stats.meshCount) },
            { Loc("infopanel.format", L"Format"), format == model_core::SourceFormatId::Gltf ? Loc("infopanel.gltf", L"glTF") : format == model_core::SourceFormatId::Glb ? Loc("infopanel.glb", L"GLB")
                : format == model_core::SourceFormatId::Stl ? Loc("infopanel.stl.binary", L"STL (binary)")
                : format == model_core::SourceFormatId::AsciiStl ? Loc("infopanel.stl.ascii", L"STL (ASCII)")
                : format == model_core::SourceFormatId::Ply ? Loc("infopanel.ply.binary", L"PLY (binary)")
                : format == model_core::SourceFormatId::AsciiPly ? Loc("infopanel.ply.ascii", L"PLY (ASCII)")
                : format == model_core::SourceFormatId::Obj ? Loc("infopanel.obj", L"OBJ")
                : format == model_core::SourceFormatId::Fbx ? Loc("infopanel.fbx", L"FBX")
                : format == model_core::SourceFormatId::Usda ? Loc("infopanel.usd.ascii", L"USD (ASCII)")
                : format == model_core::SourceFormatId::Usdc ? Loc("infopanel.usd.crate", L"USD (crate)")
                : format == model_core::SourceFormatId::Usdz ? Loc("infopanel.usdz", L"USDZ")
                : format == model_core::SourceFormatId::ThreeMf ? Loc("infopanel.3mf", L"3MF")
                : format == model_core::SourceFormatId::Step ? Loc("infopanel.step", L"STEP") : Loc("infopanel.unknown", L"Unknown") },
            { Loc("infopanel.units", L"Units"), metersPerUnit > 0 ? Loc("infopanel.metres", L"Metres") : Loc("infopanel.unspecified", L"Unspecified") },
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

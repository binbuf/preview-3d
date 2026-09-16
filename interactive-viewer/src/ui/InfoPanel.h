#pragma once

#include "Model.h"
#include "GroundAxis.h"

#include <cstdint>
#include <string>
#include <vector>

// Read-only "Stats & Shading" side panel content, adapted from the fields the
// (now defunct) Windows 11 3D Viewer showed. Pure data/layout — no D2D or
// window dependency, the same split as NavGizmo vs. D3D11On12Overlay::DrawGizmo:
// D3D11On12Overlay::DrawInfoPanel does the actual text drawing row-by-row within the
// rect ComputeInfoPanelLayout returns.
struct InfoPanelRow
{
    std::wstring label;
    std::wstring value;
};

struct InfoPanelSection
{
    std::wstring title;
    std::vector<InfoPanelRow> rows;
};

// Builds existing section/row text from scanned source facts and accepted
// geometry counts. Unspecified source units are explicit. The compact model
// overload keeps double dimensions and exact source-axis permutations.
std::vector<InfoPanelSection> BuildInfoPanelSections(
    const ModelStats& stats, std::uint64_t triangleCount, std::uint64_t vertexCount,
    const DirectX::XMFLOAT3& boundsMin, const DirectX::XMFLOAT3& boundsMax,
    double metersPerUnit = 1.0, std::uint64_t pointCount = 0, bool boundsVerified = true,
    model_core::SourceFormatId format = model_core::SourceFormatId::Gltf, const double* dimensions = nullptr);

std::vector<InfoPanelSection> BuildInfoPanelSections(
    const ModelData& metadata, bool showNativeOrientation, GroundAxis groundAxis = GroundAxis::Automatic);

// Fixed-width panel docked to the right edge of the viewport, below the title
// bar and above the bottom bar.
struct InfoPanelLayout
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float Width() const { return right - left; }
};

InfoPanelLayout ComputeInfoPanelLayout(int viewportWidth, int viewportHeight,
    int titleBarHeight, int bottomBarHeight, float dpiScale);

// Vertical geometry of the scrollable section list D3D11On12Overlay::DrawInfoPanel
// draws below its fixed "Stats & Shading" header: `headerHeight` is the
// header's own height (the scrollable area starts right below it),
// `contentHeight` the full height of every section/row at this DPI. Mirrors
// DrawInfoPanel's row/section-gap metrics (Renderer.cpp) so the two can't
// drift apart; used both to draw the scroll-clipped content and to clamp the
// scroll offset from WM_MOUSEWHEEL (Preview3D.cpp).
struct InfoPanelScrollMetrics
{
    float headerHeight = 0.0f;
    float contentHeight = 0.0f;
};

InfoPanelScrollMetrics ComputeInfoPanelScrollMetrics(
    const std::vector<InfoPanelSection>& sections, float dpiScale);

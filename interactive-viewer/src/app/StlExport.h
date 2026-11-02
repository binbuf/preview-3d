#pragma once

// Trusted-process binary STL export for an already-validated import result.
//
// The rendered GPU scene is deliberately not the source: transforms, cluster
// origins and up-axis correction are applied per draw, instances are not
// expanded, and the progressive path may only hold coarse geometry. The
// caller re-imports the current source without a batch sink, which yields the
// complete fine-detail CPU mesh set, and this module turns that into a plain
// geometry-only binary STL.

#include "D3D12ImportBridge.h"

#include <cstdint>
#include <string>

namespace viewer_stl_export {

struct Options
{
    // STL is unitless. The viewer's export convention is millimetres, so a
    // declared metres-per-unit value is converted; zero means the source did
    // not declare a unit and coordinates are written 1:1.
    double metersPerUnit = 0.0;
    // The source's authored up axis. The viewer's normal display correction is
    // baked so the exported file is oriented the way the user sees it.
    model_core::UpAxisId sourceUpAxis = model_core::UpAxisId::Unknown;
};

struct Result
{
    bool ok = false;
    std::uint64_t facetCount = 0;
    std::uint64_t droppedFacets = 0;
    std::uint64_t omittedPoints = 0;
    double appliedScale = 1.0;
    std::wstring error;
};

// Writes a binary STL to `path` (created/truncated). Geometry comes from the
// fine-detail TriangleList chunks in `model`; instances are expanded and
// mirrored instances have their winding reversed. Point/line-only content is
// omitted and reported. On failure no partial file is left behind.
Result ExportBinaryStl(const d3d12_import_bridge::ImportResult& model,
                       const std::wstring& path,
                       const Options& options);

} // namespace viewer_stl_export

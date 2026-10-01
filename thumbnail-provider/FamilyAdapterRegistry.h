#pragma once

// T13 family-adapter registry seam.
//
// GetThumbnail selects the adapter from the object's CLSID family and nothing
// else (design/05, FamilyRouting.h). This is the single place a linked family
// adapter is constructed from that routed family:
//
//   std::unique_ptr<IFamilyAdapter> CreateFamilyAdapter(Family family) noexcept;
//
// There is deliberately no mutable runtime registry: the set of linked
// adapters is a build-time property. Until T21-T34 land, every family returns
// nullptr and GetThumbnail maps that to the tabulated "unsupported"
// (ERROR_NOT_SUPPORTED) fallback rather than fabricating a bitmap. Each family
// task adds its case here; it never sniffs content or recovers a path.

#include "FamilyAdapter.h"

#include <memory>

namespace preview3d::provider {

std::unique_ptr<IFamilyAdapter> CreateFamilyAdapter(Family family) noexcept;

} // namespace preview3d::provider
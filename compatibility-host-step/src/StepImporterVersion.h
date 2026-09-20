#pragma once

// STEP importer/cache identity. Any semantic change to acceptance, unit
// handling, scene normalization, or tessellation must bump this so a derived
// cache entry produced by a different policy is never reused. The profile
// version is folded in so the two cannot drift apart silently.
//
// Design authority: .docs/stp.md (STEP-004 work item 1).

#include "StepTessellationProfile.h"

#include <cstdint>

namespace step_host {

constexpr std::uint32_t kStepImporterVersion = 1 + kStepTessellationProfileVersion;

} // namespace step_host
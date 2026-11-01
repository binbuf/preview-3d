#pragma once

// STEP importer/cache identity. Any semantic change to acceptance, unit
// handling, scene normalization, or tessellation must bump this so a derived
// cache entry produced by a different policy is never reused. The profile
// version is folded in so the two cannot drift apart silently.
//
// Design authority: .docs/stp.md (STEP-004 work item 1).

#include "StepTessellationProfile.h"

#include "model_core/ControlProtocol.h"

#include <cstdint>

namespace step_host {

constexpr std::uint32_t kStepImporterVersion = 1 + kStepTessellationProfileVersion;

// Per-request cache identity. The build identity names the release policy; the
// quality is folded into the low two bits so Draft/Balanced/Full can never
// share a derived-cache entry even if a cache key forgets the request flags.
constexpr std::uint32_t StepImporterCacheIdentity(
    model_core::StepImportQuality quality) noexcept
{
    return (kStepImporterVersion << 2)
        | static_cast<std::uint32_t>(quality);
}

} // namespace step_host
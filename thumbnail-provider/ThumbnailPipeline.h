#pragma once

// T13 routed thumbnail orchestration behind IThumbnailProvider::GetThumbnail.
//
// design/05-thumbnail-provider.md ("Call contract", "Threading and unload") and
// ADR-0015 fix the shape:
//
//   1. the family is taken from the object's CLSID (never sniffed);
//   2. the adapter is created for that family only and receives the bounded
//      source, the frozen provider limits, the per-call deadline and the
//      process-wide ledger through the frozen `AdapterInput` (T04/T06/T12);
//   3. the adapter's typed failure maps through the single T06 HRESULT table;
//   4. nothing is rendered and no bitmap is produced on a failure;
//   5. the sampler (T14) and rasterizer (T15) are consumed through their frozen
//      interfaces, never implemented here.
//
// This translation unit stays free of the provider precompiled header and of
// GDI/COM so the exact orchestration can be compiled into Tests.Unit.exe and
// exercised with deterministic doubles. The DIB/HBITMAP boundary lives in
// RasterBitmap.h; the COM object in ComCore.cpp calls both.

#include "CpuRasterizer.h"
#include "FamilyAdapter.h"
#include "GeometrySampler.h"
#include "ProviderErrors.h"
#include "ProviderTypes.h"

#include <cstdint>
#include <memory>

namespace preview3d::provider {

// Supplies the per-call adapter and the sampler/rasterizer. The production
// composition (FamilyAdapterRegistry + T14/T15) is `DefaultThumbnailDependencies()`;
// tests provide deterministic doubles through this seam so routing and error
// mapping can be proven without a real family parser. Implementations may
// return nullptr to mean "not linked in this build".
class IThumbnailDependencies {
public:
    IThumbnailDependencies() = default;
    IThumbnailDependencies(const IThumbnailDependencies&) = delete;
    IThumbnailDependencies& operator=(const IThumbnailDependencies&) = delete;
    virtual ~IThumbnailDependencies() noexcept = default;

    // Creates the adapter for the routed family only. nullptr means this build
    // has no adapter for that family yet.
    virtual std::unique_ptr<IFamilyAdapter> CreateAdapter(Family family) noexcept = 0;

    // Creates one fresh deterministic sampler for the call, or nullptr while
    // T14 is not linked.
    virtual std::unique_ptr<IGeometrySampler> CreateSampler() noexcept = 0;

    // Renders the sampled geometry through the frozen rasterizer contract.
    virtual ErrorCode Render(const RasterRequest& request, RasterImage& out) noexcept = 0;
};

// One bounded GetThumbnail call. `cx` is the caller's maximum physical-pixel
// hint; the rasterizer clamps the actual resolution independently. All pointers
// are owned by the caller of RunThumbnailPipeline and outlive the call. A null
// `ledger` is rejected (production always supplies the process-wide ledger).
struct ThumbnailRequest {
    Family family = Family::Unknown;
    BoundedSource* source = nullptr;
    const ProviderLimits* limits = nullptr;
    Deadline* deadline = nullptr;
    AllocationLedger* ledger = nullptr;
    std::uint32_t cx = 0;
};

// Runs the routed adapter -> sampler -> rasterizer path. Returns the tabulated
// outcome and fills `out` only on Success; every other result leaves `out` empty
// (never a fabricated or partial image). The adapter is always Reset, even on
// failure, per the frozen lifecycle.
ProviderOutcome RunThumbnailPipeline(const ThumbnailRequest& request,
                                     IThumbnailDependencies& dependencies,
                                     RasterImage& out) noexcept;

// The production dependency set linked into Preview3DThumbnailProvider.dll: the
// CLSID-routed family adapter registry plus T14/T15 once they land. Tests do not
// use this and link their own IThumbnailDependencies.
IThumbnailDependencies& DefaultThumbnailDependencies() noexcept;

// Stable per-source sample seed handed to IGeometrySampler::Begin. Derived from
// the routed family and the validated source size so Explorer's cache is
// reproducible; T14 owns the final selection policy and may widen the input.
std::uint64_t SourceSeed(Family family, const BoundedSource& source) noexcept;

} // namespace preview3d::provider
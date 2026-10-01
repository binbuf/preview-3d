#pragma once

// T14 deterministic geometry sampler (design/05-thumbnail-provider.md, "Geometry
// sampling"; ADR-0016). Implements the frozen `IGeometrySampler` contract
// without widening it: the adapter streams inspected triangles/points in, and
// the sampler reduces them to at most the T06 rasterized-sample cap.
//
// Policy, mirrored in miniature from ADR-015's coarse proxy:
//   - bounds accumulate from finite positions only;
//   - non-finite or degenerate triangles/points are discarded;
//   - the inspect caps (2M triangles / 6M points) bound how much is read, but
//     never cause a source prefix: enumeration continues to the cap so the
//     selection stays source-order independent;
//   - retained representatives are chosen by a deterministic min-hash
//     (priority reservoir) seeded from the source, so a reordered enumeration
//     yields byte-identical output and Explorer's cache stays consistent;
//   - spatial strata (a bounded fixed-resolution grid) keep separated
//     components represented and material strata keep every nonempty material
//     represented, so the result carries the mandatory coverage floor of one
//     primitive per nonempty stratum when the caps permit;
//   - retained storage is charged against the T06 allocation ledger before it
//     is allocated.
//
// The class is PCH/COM/GDI-free so the same source compiles into
// Preview3DThumbnailProvider.dll and Tests.Unit.exe.

#include "AllocationLedger.h"
#include "GeometrySampler.h"
#include "ProviderLimits.h"

#include <cstdint>
#include <vector>

namespace preview3d::provider {

class DeterministicGeometrySampler final : public IGeometrySampler {
public:
    // A bounded fixed-resolution spatial grid. Cell size is in model units; the
    // slot count bounds retained coverage memory (one slot per cell bucket).
    static constexpr std::uint32_t kSpatialSlots = 65'536;
    static constexpr double kSpatialCellSize = 1.0;

    // The retained rasterized-sample cap (shared by triangles and points).
    static constexpr std::uint64_t kMaxRetained = ProviderLimits::kRasterizedSamplesMax;

    // A null ledger disables accounting (unit tests only); production passes the
    // process-wide ledger (the default).
    explicit DeterministicGeometrySampler(
        AllocationLedger* ledger = &AllocationLedger::ProcessWide()) noexcept;
    ~DeterministicGeometrySampler() noexcept override;

    DeterministicGeometrySampler(const DeterministicGeometrySampler&) = delete;
    DeterministicGeometrySampler& operator=(const DeterministicGeometrySampler&) =
        delete;

    void Begin(std::uint64_t sourceSeed) noexcept override;
    bool AddTriangle(const TriangleSample& triangle) noexcept override;
    bool AddPoint(const PointSample& point) noexcept override;
    const SampledGeometry& Result() const noexcept override;

    // Diagnostics for tests and telemetry; not part of the frozen contract.
    std::uint64_t InspectedTriangles() const noexcept { return inspectedTriangles_; }
    std::uint64_t InspectedPoints() const noexcept { return inspectedPoints_; }
    std::uint64_t RetainedSamples() const noexcept { return retained_; }
    std::uint64_t DroppedNonFinite() const noexcept { return droppedNonFinite_; }
    std::uint64_t DroppedDegenerate() const noexcept { return droppedDegenerate_; }
    bool TrianglesCapReached() const noexcept { return triangleCapReached_; }
    bool PointsCapReached() const noexcept { return pointCapReached_; }
    bool AccountingFailed() const noexcept { return accountingFailed_; }

private:
    struct Candidate {
        std::uint64_t key = 0;
        bool isTriangle = false;
        TriangleSample triangle{};
        PointSample point{};
    };

    struct CellKey {
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t z = 0;
    };

    struct SpatialSlot {
        CellKey cell{};
        Candidate candidate{};
        bool used = false;
    };

    struct MaterialSlot {
        Candidate candidate{};
        bool used = false;
    };

    static bool CandidateLess(const Candidate& a, const Candidate& b) noexcept;

    void ResetState() noexcept;
    void BuildResult() const noexcept;
    CellKey CellForTriangle(const TriangleSample& triangle) const noexcept;
    CellKey CellForPoint(const PointSample& point) const noexcept;
    void OfferCell(CellKey cell, Candidate&& candidate) noexcept;
    void OfferMaterial(std::uint32_t materialIndex, Candidate&& candidate) noexcept;

    AllocationLedger* ledger_ = nullptr;
    std::uint64_t seed_ = 0;
    std::uint64_t inspectedTriangles_ = 0;
    std::uint64_t inspectedPoints_ = 0;
    std::uint64_t droppedNonFinite_ = 0;
    std::uint64_t droppedDegenerate_ = 0;
    mutable std::uint64_t retained_ = 0;
    bool triangleCapReached_ = false;
    bool pointCapReached_ = false;
    bool accountingFailed_ = false;
    bool reservoirActive_ = false;
    Bounds bounds_{};

    mutable std::vector<Candidate> reservoir_;
    std::vector<SpatialSlot> spatial_;
    std::vector<MaterialSlot> material_;
    AllocationReservation reservoirReservation_{};
    AllocationReservation coverageReservation_{};

    mutable SampledGeometry result_{};
    mutable std::vector<TriangleSample> resultTriangles_;
    mutable std::vector<PointSample> resultPoints_;
    mutable bool resultBuilt_ = false;
};

} // namespace preview3d::provider
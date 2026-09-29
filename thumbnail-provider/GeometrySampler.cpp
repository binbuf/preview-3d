// T14 deterministic geometry sampler implementation (see
// DeterministicGeometrySampler.h and ADR-0016). Free of the provider
// precompiled header, COM and GDI so the same source compiles into the DLL and
// into Tests.Unit.exe, exactly like StreamSource.cpp and ThumbnailPipeline.cpp.

#include "DeterministicGeometrySampler.h"

#include "GeometrySamplingPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_set>
#include <utility>

namespace preview3d::provider {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

// Cells are clamped so a finite-but-huge coordinate can never overflow the
// int64 grid index (positions are validated finite before they reach here).
constexpr std::int64_t kCellMin = -(std::int64_t{1} << 52);
constexpr std::int64_t kCellMax = std::int64_t{1} << 52;

std::uint64_t SplitMix64(std::uint64_t value) noexcept
{
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

void HashByte(std::uint64_t& hash, std::uint8_t byte) noexcept
{
    hash ^= byte;
    hash *= kFnvPrime;
}

void HashU64(std::uint64_t& hash, std::uint64_t value) noexcept
{
    for (int i = 0; i < 8; ++i) {
        HashByte(hash, static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu));
    }
}

void HashU32(std::uint64_t& hash, std::uint32_t value) noexcept
{
    for (int i = 0; i < 4; ++i) {
        HashByte(hash, static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu));
    }
}

void HashF32(std::uint64_t& hash, float value) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    HashU32(hash, bits);
}

void HashF64(std::uint64_t& hash, double value) noexcept
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    HashU64(hash, bits);
}

void HashVertex(std::uint64_t& hash, const VertexSample& vertex) noexcept
{
    for (int i = 0; i < 3; ++i) {
        HashF32(hash, vertex.position[i]);
    }
    for (int i = 0; i < 3; ++i) {
        HashF32(hash, vertex.normal[i]);
    }
    for (int i = 0; i < 4; ++i) {
        HashF32(hash, vertex.color[i]);
    }
}

std::uint64_t HashTriangle(std::uint64_t seed, const TriangleSample& triangle) noexcept
{
    std::uint64_t hash = kFnvOffset;
    HashU64(hash, seed);
    HashByte(hash, 0x54u); // 'T'
    for (int i = 0; i < 3; ++i) {
        HashVertex(hash, triangle.vertices[i]);
    }
    for (int i = 0; i < 3; ++i) {
        HashF64(hash, triangle.origin[i]);
    }
    HashU32(hash, triangle.materialIndex);
    HashU32(hash, triangle.reserved);
    return SplitMix64(hash);
}

std::uint64_t HashPoint(std::uint64_t seed, const PointSample& point) noexcept
{
    std::uint64_t hash = kFnvOffset;
    HashU64(hash, seed);
    HashByte(hash, 0x50u); // 'P'
    HashVertex(hash, point.vertex);
    for (int i = 0; i < 3; ++i) {
        HashF64(hash, point.origin[i]);
    }
    HashF32(hash, point.radius);
    HashU32(hash, point.materialIndex);
    HashU32(hash, point.reserved);
    return SplitMix64(hash);
}

bool IsFiniteVertex(const VertexSample& vertex) noexcept
{
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(vertex.position[i]) || !std::isfinite(vertex.normal[i])) {
            return false;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(vertex.color[i])) {
            return false;
        }
    }
    return true;
}

bool IsFiniteTriangle(const TriangleSample& triangle) noexcept
{
    for (int i = 0; i < 3; ++i) {
        if (!IsFiniteVertex(triangle.vertices[i]) ||
            !std::isfinite(triangle.origin[i])) {
            return false;
        }
    }
    return true;
}

bool IsFinitePoint(const PointSample& point) noexcept
{
    if (!IsFiniteVertex(point.vertex)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(point.origin[i])) {
            return false;
        }
    }
    // A radius of 0 (or any finite value, including negatives meaning "derive
    // from bounds") is valid; only NaN/Inf is not.
    return std::isfinite(point.radius);
}

double Absolute(double origin, float local) noexcept
{
    return origin + static_cast<double>(local);
}

double Coordinate(const TriangleSample& triangle, int vertex, int axis) noexcept
{
    return Absolute(triangle.origin[axis], triangle.vertices[vertex].position[axis]);
}

bool IsDegenerateTriangle(const TriangleSample& triangle) noexcept
{
    double edge0[3];
    double edge1[3];
    for (int axis = 0; axis < 3; ++axis) {
        const double a = Coordinate(triangle, 0, axis);
        edge0[axis] = Coordinate(triangle, 1, axis) - a;
        edge1[axis] = Coordinate(triangle, 2, axis) - a;
    }
    const double cross[3] = {
        edge0[1] * edge1[2] - edge0[2] * edge1[1],
        edge0[2] * edge1[0] - edge0[0] * edge1[2],
        edge0[0] * edge1[1] - edge0[1] * edge1[0],
    };
    const double lengthSquared =
        cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2];
    return !(lengthSquared > 0.0);
}

int CompareFloat(float a, float b) noexcept
{
    if (a < b) {
        return -1;
    }
    return a > b ? 1 : 0;
}

int CompareDouble(double a, double b) noexcept
{
    if (a < b) {
        return -1;
    }
    return a > b ? 1 : 0;
}

int CompareVertex(const VertexSample& a, const VertexSample& b) noexcept
{
    for (int i = 0; i < 3; ++i) {
        const int order = CompareFloat(a.position[i], b.position[i]);
        if (order != 0) {
            return order;
        }
    }
    for (int i = 0; i < 3; ++i) {
        const int order = CompareFloat(a.normal[i], b.normal[i]);
        if (order != 0) {
            return order;
        }
    }
    for (int i = 0; i < 4; ++i) {
        const int order = CompareFloat(a.color[i], b.color[i]);
        if (order != 0) {
            return order;
        }
    }
    return 0;
}

int CompareTriangle(const TriangleSample& a, const TriangleSample& b) noexcept
{
    if (a.materialIndex != b.materialIndex) {
        return a.materialIndex < b.materialIndex ? -1 : 1;
    }
    if (a.reserved != b.reserved) {
        return a.reserved < b.reserved ? -1 : 1;
    }
    for (int vertex = 0; vertex < 3; ++vertex) {
        const int order = CompareVertex(a.vertices[vertex], b.vertices[vertex]);
        if (order != 0) {
            return order;
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        const int order = CompareDouble(a.origin[axis], b.origin[axis]);
        if (order != 0) {
            return order;
        }
    }
    return 0;
}

int ComparePoint(const PointSample& a, const PointSample& b) noexcept
{
    if (a.materialIndex != b.materialIndex) {
        return a.materialIndex < b.materialIndex ? -1 : 1;
    }
    if (a.reserved != b.reserved) {
        return a.reserved < b.reserved ? -1 : 1;
    }
    const int vertexOrder = CompareVertex(a.vertex, b.vertex);
    if (vertexOrder != 0) {
        return vertexOrder;
    }
    for (int axis = 0; axis < 3; ++axis) {
        const int order = CompareDouble(a.origin[axis], b.origin[axis]);
        if (order != 0) {
            return order;
        }
    }
    return CompareFloat(a.radius, b.radius);
}

} // namespace

bool DeterministicGeometrySampler::CandidateLess(const Candidate& a,
                                                 const Candidate& b) noexcept
{
    if (a.key != b.key) {
        return a.key < b.key;
    }
    if (a.isTriangle != b.isTriangle) {
        return a.isTriangle;
    }
    return a.isTriangle ? CompareTriangle(a.triangle, b.triangle) < 0
                        : ComparePoint(a.point, b.point) < 0;
}

DeterministicGeometrySampler::DeterministicGeometrySampler(
    AllocationLedger* ledger) noexcept
    : ledger_(ledger)
{
    ResetState();
}

DeterministicGeometrySampler::~DeterministicGeometrySampler() noexcept = default;

void DeterministicGeometrySampler::ResetState() noexcept
{
    seed_ = 0;
    inspectedTriangles_ = 0;
    inspectedPoints_ = 0;
    droppedNonFinite_ = 0;
    droppedDegenerate_ = 0;
    retained_ = 0;
    triangleCapReached_ = false;
    pointCapReached_ = false;
    accountingFailed_ = false;
    reservoirActive_ = false;
    bounds_ = Bounds{};
    result_ = SampledGeometry{};
    resultTriangles_.clear();
    resultPoints_.clear();
    resultBuilt_ = false;
}

void DeterministicGeometrySampler::Begin(std::uint64_t sourceSeed) noexcept
{
    reservoirReservation_.Release();
    coverageReservation_.Release();
    reservoir_.clear();
    spatial_.clear();
    material_.clear();

    seed_ = sourceSeed;
    ResetState();
    resultBuilt_ = false;

    // Charge retained storage against the T06 ledger before allocating it. A
    // null ledger means unaccounted (unit tests). The reservoir and the coverage
    // tables are reserved independently so a ledger with room for only one still
    // leaves a usable (coverage-only) result.
    const std::uint64_t reservoirBytes = kMaxRetained * sizeof(Candidate);
    const std::uint64_t coverageBytes =
        static_cast<std::uint64_t>(kSpatialSlots) * sizeof(SpatialSlot) +
        static_cast<std::uint64_t>(ProviderLimits::kMaterialsMax + 1) *
            sizeof(MaterialSlot);

    const auto reserve = [this](std::uint64_t bytes) noexcept
        -> std::optional<AllocationReservation> {
        if (ledger_ == nullptr) {
            return AllocationReservation{};
        }
        return ledger_->ReserveScoped(bytes);
    };

    try {
        std::optional<AllocationReservation> reservoir = reserve(reservoirBytes);
        if (!reservoir.has_value()) {
            accountingFailed_ = true;
        } else {
            reservoirReservation_ = std::move(*reservoir);
            reservoir_.reserve(static_cast<std::size_t>(kMaxRetained));
            reservoirActive_ = true;
        }
    } catch (...) {
        reservoirReservation_.Release();
        reservoir_.clear();
        reservoirActive_ = false;
        accountingFailed_ = true;
    }

    try {
        std::optional<AllocationReservation> coverage = reserve(coverageBytes);
        if (!coverage.has_value()) {
            accountingFailed_ = true;
        } else {
            coverageReservation_ = std::move(*coverage);
            spatial_.resize(kSpatialSlots);
            material_.resize(
                static_cast<std::size_t>(ProviderLimits::kMaterialsMax + 1));
        }
    } catch (...) {
        coverageReservation_.Release();
        spatial_.clear();
        material_.clear();
        accountingFailed_ = true;
    }
}

void DeterministicGeometrySampler::OfferCell(CellKey cell,
                                             Candidate&& candidate) noexcept
{
    if (!spatial_.empty()) {
        const std::uint64_t mixed =
            SplitMix64(static_cast<std::uint64_t>(cell.x) * 0x9E3779B97F4A7C15ull ^
                       static_cast<std::uint64_t>(cell.y) * 0xC2B2AE3D27D4EB4Full ^
                       static_cast<std::uint64_t>(cell.z) * 0x165667B19E3779F9ull);
        SpatialSlot& existing = spatial_[static_cast<std::size_t>(mixed % kSpatialSlots)];
        if (!existing.used) {
            existing.used = true;
            existing.cell = cell;
            existing.candidate = candidate;
        } else if (existing.cell.x == cell.x && existing.cell.y == cell.y &&
                   existing.cell.z == cell.z) {
            if (CandidateLess(candidate, existing.candidate)) {
                existing.candidate = candidate;
            }
        } else {
            // One slot per bucket keeps the smallest cell key, so which cell
            // owns a colliding slot is independent of arrival order.
            const bool incomingSmaller =
                cell.x != existing.cell.x ? cell.x < existing.cell.x
                : cell.y != existing.cell.y ? cell.y < existing.cell.y
                                            : cell.z < existing.cell.z;
            if (incomingSmaller) {
                existing.cell = cell;
                existing.candidate = candidate;
            }
        }
    }

    if (reservoirActive_) {
        if (reservoir_.size() < static_cast<std::size_t>(kMaxRetained)) {
            reservoir_.push_back(candidate);
            std::push_heap(reservoir_.begin(), reservoir_.end(), CandidateLess);
        } else if (CandidateLess(candidate, reservoir_.front())) {
            std::pop_heap(reservoir_.begin(), reservoir_.end(), CandidateLess);
            reservoir_.back() = candidate;
            std::push_heap(reservoir_.begin(), reservoir_.end(), CandidateLess);
        }
    }
}

void DeterministicGeometrySampler::OfferMaterial(std::uint32_t materialIndex,
                                                 Candidate&& candidate) noexcept
{
    if (material_.empty()) {
        return;
    }
    const std::size_t slot =
        materialIndex <= ProviderLimits::kMaterialsMax ? materialIndex : 0u;
    MaterialSlot& existing = material_[slot];
    if (!existing.used) {
        existing.used = true;
        existing.candidate = candidate;
    } else if (CandidateLess(candidate, existing.candidate)) {
        existing.candidate = candidate;
    }
}

DeterministicGeometrySampler::CellKey
DeterministicGeometrySampler::CellForTriangle(
    const TriangleSample& triangle) const noexcept
{
    CellKey cell{};
    const double coordinates[3] = {
        (Coordinate(triangle, 0, 0) + Coordinate(triangle, 1, 0) +
         Coordinate(triangle, 2, 0)) /
            3.0,
        (Coordinate(triangle, 0, 1) + Coordinate(triangle, 1, 1) +
         Coordinate(triangle, 2, 1)) /
            3.0,
        (Coordinate(triangle, 0, 2) + Coordinate(triangle, 1, 2) +
         Coordinate(triangle, 2, 2)) /
            3.0,
    };
    std::int64_t* out[3] = {&cell.x, &cell.y, &cell.z};
    for (int axis = 0; axis < 3; ++axis) {
        const double q = std::floor(coordinates[axis] / kSpatialCellSize);
        if (q <= static_cast<double>(kCellMin)) {
            *out[axis] = kCellMin;
        } else if (q >= static_cast<double>(kCellMax)) {
            *out[axis] = kCellMax;
        } else {
            *out[axis] = static_cast<std::int64_t>(q);
        }
    }
    return cell;
}

DeterministicGeometrySampler::CellKey DeterministicGeometrySampler::CellForPoint(
    const PointSample& point) const noexcept
{
    CellKey cell{};
    std::int64_t* out[3] = {&cell.x, &cell.y, &cell.z};
    for (int axis = 0; axis < 3; ++axis) {
        const double q =
            std::floor(Absolute(point.origin[axis], point.vertex.position[axis]) /
                       kSpatialCellSize);
        if (q <= static_cast<double>(kCellMin)) {
            *out[axis] = kCellMin;
        } else if (q >= static_cast<double>(kCellMax)) {
            *out[axis] = kCellMax;
        } else {
            *out[axis] = static_cast<std::int64_t>(q);
        }
    }
    return cell;
}

bool DeterministicGeometrySampler::AddTriangle(const TriangleSample& triangle) noexcept
{
    if (triangleCapReached_) {
        return false;
    }
    if (inspectedTriangles_ >= ProviderLimits::kTrianglesInspectedMax) {
        triangleCapReached_ = true;
        return false;
    }
    ++inspectedTriangles_;

    if (!IsFiniteTriangle(triangle)) {
        ++droppedNonFinite_;
        return true;
    }
    if (IsDegenerateTriangle(triangle)) {
        ++droppedDegenerate_;
        return true;
    }

    for (int vertex = 0; vertex < 3; ++vertex) {
        for (int axis = 0; axis < 3; ++axis) {
            const double value = Coordinate(triangle, vertex, axis);
            if (!bounds_.valid) {
                bounds_.min[axis] = value;
                bounds_.max[axis] = value;
            } else {
                bounds_.min[axis] = (std::min)(bounds_.min[axis], value);
                bounds_.max[axis] = (std::max)(bounds_.max[axis], value);
            }
        }
        if (!bounds_.valid) {
            bounds_.valid = true;
        }
    }

    Candidate candidate{};
    candidate.key = HashTriangle(seed_, triangle);
    candidate.isTriangle = true;
    candidate.triangle = triangle;

    OfferCell(CellForTriangle(triangle), Candidate(candidate));
    OfferMaterial(triangle.materialIndex, Candidate(candidate));
    return true;
}

bool DeterministicGeometrySampler::AddPoint(const PointSample& point) noexcept
{
    if (pointCapReached_) {
        return false;
    }
    if (inspectedPoints_ >= ProviderLimits::kPointsInspectedMax) {
        pointCapReached_ = true;
        return false;
    }
    ++inspectedPoints_;

    if (!IsFinitePoint(point)) {
        ++droppedNonFinite_;
        return true;
    }

    for (int axis = 0; axis < 3; ++axis) {
        const double value = Absolute(point.origin[axis], point.vertex.position[axis]);
        if (!bounds_.valid) {
            bounds_.min[axis] = value;
            bounds_.max[axis] = value;
        } else {
            bounds_.min[axis] = (std::min)(bounds_.min[axis], value);
            bounds_.max[axis] = (std::max)(bounds_.max[axis], value);
        }
    }
    bounds_.valid = true;

    Candidate candidate{};
    candidate.key = HashPoint(seed_, point);
    candidate.isTriangle = false;
    candidate.point = point;

    OfferCell(CellForPoint(point), Candidate(candidate));
    OfferMaterial(point.materialIndex, Candidate(candidate));
    return true;
}

void DeterministicGeometrySampler::BuildResult() const noexcept
{
    try {
        std::size_t coverageCount = 0;
        for (const SpatialSlot& slot : spatial_) {
            coverageCount += slot.used ? 1u : 0u;
        }
        for (const MaterialSlot& slot : material_) {
            coverageCount += slot.used ? 1u : 0u;
        }

        std::vector<Candidate> chosen;
        chosen.reserve((std::min)(static_cast<std::size_t>(kMaxRetained),
                                  coverageCount + reservoir_.size()));
        std::unordered_set<std::uint64_t> chosenKeys;

        const auto addCoverage = [&chosen, &chosenKeys](const Candidate& candidate) {
            if (chosenKeys.insert(candidate.key).second) {
                chosen.push_back(candidate);
            }
        };
        for (const SpatialSlot& slot : spatial_) {
            if (slot.used) {
                addCoverage(slot.candidate);
            }
        }
        for (const MaterialSlot& slot : material_) {
            if (slot.used) {
                addCoverage(slot.candidate);
            }
        }

        std::sort(reservoir_.begin(), reservoir_.end(), CandidateLess);
        for (const Candidate& candidate : reservoir_) {
            if (chosen.size() >= static_cast<std::size_t>(kMaxRetained)) {
                break;
            }
            if (chosenKeys.insert(candidate.key).second) {
                chosen.push_back(candidate);
            }
        }

        std::sort(chosen.begin(), chosen.end(), CandidateLess);

        resultTriangles_.clear();
        resultPoints_.clear();
        for (const Candidate& candidate : chosen) {
            if (candidate.isTriangle) {
                resultTriangles_.push_back(candidate.triangle);
            } else {
                resultPoints_.push_back(candidate.point);
            }
        }
        retained_ = chosen.size();
        result_.triangles = resultTriangles_;
        result_.points = resultPoints_;
        result_.bounds = bounds_;
    } catch (...) {
        resultTriangles_.clear();
        resultPoints_.clear();
        retained_ = 0;
        result_ = SampledGeometry{};
    }
    resultBuilt_ = true;
}

const SampledGeometry& DeterministicGeometrySampler::Result() const noexcept
{
    if (!resultBuilt_) {
        BuildResult();
    }
    return result_;
}

std::vector<std::uint64_t> StratifiedOffsets(std::uint64_t count,
                                             std::uint32_t strata) noexcept
{
    std::vector<std::uint64_t> offsets;
    if (count == 0 || strata == 0) {
        return offsets;
    }
    if (static_cast<std::uint64_t>(strata) > count) {
        strata = static_cast<std::uint32_t>(count);
    }
    offsets.reserve(strata);
    for (std::uint32_t stratum = 0; stratum < strata; ++stratum) {
        // Centre of each stratum, so the sample is spread across the extent and
        // never weighted to the source prefix.
        const std::uint64_t offset =
            (static_cast<std::uint64_t>(stratum) * count + count / 2) / strata;
        offsets.push_back(offset < count ? offset : count - 1);
    }
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    return offsets;
}

} // namespace preview3d::provider
// T13 routed thumbnail orchestration implementation (see ThumbnailPipeline.h).
//
// Free of the provider precompiled header and of GDI/COM, like StreamSource.cpp
// and the shared parser subset, so Tests.Unit.exe compiles the exact
// orchestration and proves routing and error mapping with doubles (ADR-0015).

#include "ThumbnailPipeline.h"

#include "AllocationLedger.h"
#include "Deadline.h"
#include "DeterministicGeometrySampler.h"
#include "FamilyAdapterRegistry.h"
#include "ProviderLimits.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace preview3d::provider {
namespace {

constexpr std::uint64_t kMaterialTableBytes =
    ProviderLimits::kMaterialsMax * sizeof(model_core::MaterialPayload);

// Resets the adapter on every exit, including an early return after a failed
// Initialize (the frozen contract requires no partially trusted state).
struct AdapterResetGuard {
    IFamilyAdapter* adapter = nullptr;
    ~AdapterResetGuard() noexcept
    {
        if (adapter != nullptr) {
            adapter->Reset();
        }
    }
};

// Forwards an adapter's material enumeration into the bounded table. `0` is the
// reserved neutral fallback; adapters emit 1-based indices (FamilyAdapter.h).
class MaterialTable final : public IMaterialSink {
public:
    explicit MaterialTable(std::vector<model_core::MaterialPayload>& materials) noexcept
        : materials_(materials)
    {
    }

    bool OnMaterial(std::uint32_t materialIndex,
                    const model_core::MaterialPayload& material) noexcept override
    {
        if (materialIndex == 0 || materialIndex > ProviderLimits::kMaterialsMax) {
            return false;
        }
        if (materials_.size() >= ProviderLimits::kMaterialsMax) {
            return false;
        }
        materials_.push_back(material);
        return true;
    }

private:
    std::vector<model_core::MaterialPayload>& materials_;
};

// Forwards an adapter's geometry enumeration into the T14 sampler. A false
// return is the sampler's cap stop, not an error (FamilyAdapter.h).
class SamplerGeometrySink final : public IGeometrySink {
public:
    explicit SamplerGeometrySink(IGeometrySampler& sampler) noexcept : sampler_(sampler) {}

    bool OnTriangle(const TriangleSample& triangle) noexcept override
    {
        return sampler_.AddTriangle(triangle);
    }

    bool OnPoint(const PointSample& point) noexcept override
    {
        return sampler_.AddPoint(point);
    }

private:
    IGeometrySampler& sampler_;
};

ProviderOutcome FromError(ErrorCode code) noexcept
{
    return code == ErrorCode::None ? ProviderOutcome::Success : ClassifyError(code);
}

// The production dependency set. Family adapters come from the T13 registry;
// the sampler and rasterizer are T14/T15 and are not linked yet, so an adapter
// that does exist cannot complete a call until those tasks land. This is the
// single composition point they replace.
class DefaultDependencies final : public IThumbnailDependencies {
public:
    std::unique_ptr<IFamilyAdapter> CreateAdapter(Family family) noexcept override
    {
        return CreateFamilyAdapter(family);
    }

    std::unique_ptr<IGeometrySampler> CreateSampler() noexcept override
    {
        // T14: the deterministic spatial/material/reservoir sampler, charged to
        // the process-wide ledger for the duration of the call.
        try {
            return std::make_unique<DeterministicGeometrySampler>();
        } catch (...) {
            return nullptr;
        }
    }

    ErrorCode Render(const RasterRequest&, RasterImage& out) noexcept override
    {
        // T15 replaces this with the product-owned tile rasterizer.
        out = RasterImage{};
        return ErrorCode::UnsupportedRequiredFeature;
    }
};

} // namespace

std::uint64_t SourceSeed(Family family, const BoundedSource& source) noexcept
{
    // FNV-1a over the routed family and the validated size. Stable across calls
    // for the same source so Explorer's cache is consistent. T14 owns the final
    // policy and may widen this to a content hash.
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint64_t value) noexcept {
        for (int i = 0; i < 8; ++i) {
            hash ^= (value & 0xFFu);
            hash *= 1099511628211ull;
            value >>= 8;
        }
    };
    mix(static_cast<std::uint64_t>(family));
    mix(source.Size());
    return hash;
}

ProviderOutcome RunThumbnailPipeline(const ThumbnailRequest& request,
                                     IThumbnailDependencies& dependencies,
                                     RasterImage& out) noexcept
{
    out = RasterImage{};

    if (request.source == nullptr || request.limits == nullptr ||
        request.deadline == nullptr || request.ledger == nullptr) {
        return ProviderOutcome::BadPointer;
    }
    if (!request.deadline->Checkpoint()) {
        return ProviderOutcome::Deadline;
    }

    std::unique_ptr<IFamilyAdapter> adapter = dependencies.CreateAdapter(request.family);
    if (!adapter) {
        // No adapter linked for this routed family in this build; the Shell
        // falls back to its generic icon (design/05, HRESULT mapping).
        return ProviderOutcome::Unsupported;
    }
    AdapterResetGuard reset{adapter.get()};

    std::unique_ptr<IGeometrySampler> sampler = dependencies.CreateSampler();
    if (!sampler) {
        return ProviderOutcome::Unsupported;
    }

    // The material table is bounded by the frozen kMaterialsMax cap; reserve its
    // worst case against the T06 ledger before allocating (ADR-0011).
    std::optional<AllocationReservation> materialReservation =
        request.ledger->ReserveScoped(kMaterialTableBytes);
    if (!materialReservation.has_value()) {
        return ProviderOutcome::LimitExceeded;
    }
    std::vector<model_core::MaterialPayload> materials;
    materials.reserve(ProviderLimits::kMaterialsMax);

    AdapterInput input{};
    input.source = request.source;
    input.limits = request.limits;
    input.deadline = request.deadline;
    input.ledger = request.ledger;
    input.family = request.family;

    ProviderOutcome outcome = FromError(adapter->Initialize(input));
    if (outcome != ProviderOutcome::Success) {
        return outcome;
    }
    outcome = FromError(adapter->Parse());
    if (outcome != ProviderOutcome::Success) {
        return outcome;
    }

    sampler->Begin(SourceSeed(request.family, *request.source));

    MaterialTable materialSink(materials);
    outcome = FromError(adapter->EnumerateMaterials(materialSink));
    if (outcome != ProviderOutcome::Success) {
        return outcome;
    }

    SamplerGeometrySink geometrySink(*sampler);
    outcome = FromError(adapter->EnumerateGeometry(geometrySink));
    if (outcome != ProviderOutcome::Success) {
        return outcome;
    }

    const SampledGeometry& sampled = sampler->Result();
    if (sampled.triangles.empty() && sampled.points.empty()) {
        // Malformed or empty geometry is the tabulated bad-format fallback, not
        // a fabricated success bitmap (design/05).
        return ProviderOutcome::BadFormat;
    }

    RasterRequest raster{};
    raster.geometry = &sampled;
    raster.materials = std::span<const model_core::MaterialPayload>(materials);
    raster.requestedSize = request.cx;
    raster.allowSupersample = true;
    raster.deadline = request.deadline;
    raster.ledger = request.ledger;

    outcome = FromError(dependencies.Render(raster, out));
    if (outcome != ProviderOutcome::Success) {
        out = RasterImage{};
        return outcome;
    }

    if (out.width == 0 || out.height == 0 || out.bgraPremultiplied.empty()) {
        out = RasterImage{};
        return ProviderOutcome::DecoderFailure;
    }
    return ProviderOutcome::Success;
}

IThumbnailDependencies& DefaultThumbnailDependencies() noexcept
{
    static DefaultDependencies dependencies;
    return dependencies;
}

} // namespace preview3d::provider
#pragma once

// Frozen family-adapter contract (T04). One adapter per family is chosen by
// CLSID before any bytes are read, so an adapter never sniffs another grammar.
//
// The lifecycle is deliberately small so each family task (T21-T34) is an
// isolated implementation:
//
//   1. Initialize(input)  - bounded setup, independent of the stream's
//                           eventual position; returns a typed error;
//   2. Parse()            - decode into a bounded per-call intermediate,
//                           checking the deadline at bounded intervals;
//   3. EnumerateMaterials - emit 1-based material records;
//   4. EnumerateGeometry  - emit triangles and/or points to the sampler sink;
//   5. Reset()            - release every per-call resource.
//
// An adapter owns no lasting threads and no process-global mutable state. All
// output uses preview3d::provider or model_core product-owned types only.

#include "ProviderTypes.h"

#include <cstdint>

namespace preview3d::provider {

// Bounded per-call context. `limits`, `deadline` and `ledger` are owned by the
// caller (T12/T13) and outlive the adapter call; `source` is the only input the
// adapter may read. A null `ledger` means allocations are not charged (used by
// unit tests only; production always supplies one).
struct AdapterInput {
    BoundedSource* source = nullptr;
    const ProviderLimits* limits = nullptr;
    Deadline* deadline = nullptr;
    AllocationLedger* ledger = nullptr;
    Family family = Family::Unknown;
};

// Geometry enumeration sink. The sampler (T14) implements it. Returning false
// asks the adapter to stop enumerating promptly; the adapter still returns
// None from EnumerateGeometry because a cap stop is not a failure.
class IGeometrySink {
public:
    IGeometrySink() = default;
    IGeometrySink(const IGeometrySink&) = delete;
    IGeometrySink& operator=(const IGeometrySink&) = delete;
    virtual ~IGeometrySink() noexcept = default;

    virtual bool OnTriangle(const TriangleSample& triangle) = 0;
    virtual bool OnPoint(const PointSample& point) = 0;
};

// Material enumeration sink. materialIndex values are 1-based and must match
// the indices an adapter writes into TriangleSample/PointSample; 0 is reserved
// for the neutral fallback. Returning false stops enumeration.
class IMaterialSink {
public:
    IMaterialSink() = default;
    IMaterialSink(const IMaterialSink&) = delete;
    IMaterialSink& operator=(const IMaterialSink&) = delete;
    virtual ~IMaterialSink() noexcept = default;

    virtual bool OnMaterial(std::uint32_t materialIndex,
                            const model_core::MaterialPayload& material) = 0;
};

class IFamilyAdapter {
public:
    IFamilyAdapter() = default;
    IFamilyAdapter(const IFamilyAdapter&) = delete;
    IFamilyAdapter& operator=(const IFamilyAdapter&) = delete;
    virtual ~IFamilyAdapter() noexcept = default;

    // Bounded one-shot initialization. Takes no ownership of the context and
    // may be called again only after Reset(). Returns None on success; any
    // other code is a typed failure that must leave no partially trusted state.
    virtual ErrorCode Initialize(const AdapterInput& input) noexcept = 0;

    // Decodes into a bounded per-call intermediate. Returns None when the
    // intermediate is ready for enumeration, or a typed failure.
    virtual ErrorCode Parse() noexcept = 0;

    // Enumerates materials before geometry so triangle/point indices resolve.
    virtual ErrorCode EnumerateMaterials(IMaterialSink& sink) noexcept = 0;

    // Enumerates triangles and points. A false return from the sink is a
    // cap/stop request, not a failure.
    virtual ErrorCode EnumerateGeometry(IGeometrySink& sink) noexcept = 0;

    // Releases every per-call resource. Destructors are noexcept.
    virtual void Reset() noexcept = 0;
};

} // namespace preview3d::provider
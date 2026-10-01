#pragma once

// Frozen provider value types shared by every family adapter, the geometry
// sampler and the CPU rasterizer (T04). This header is the authority the later
// foundation tasks (T11-T17) and family tasks (T21-T34) build against.
//
// Rules fixed here:
//  - only product-owned types appear in any signature; no third-party parser
//    type (fastgltf, ufbx, lib3mf, Draco, OCCT, ...) crosses this boundary;
//  - every adapter consumes a bounded, seekable-aware source and a per-call
//    deadline; there are no process-global mutable caches;
//  - allocations the provider can account for are charged against a shared
//    ledger, not against a lock-free global buffer.

#include "model_core/ImportError.h"
#include "model_core/MaterialPayload.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace preview3d::provider {

// Typed failure shared with the rest of the product. The provider maps each
// code to the HRESULT table in design/05-thumbnail-provider.md (T06/T13);
// adapters never build user-visible strings.
using ErrorCode = model_core::ImportErrorCode;

// Closed family enumeration. The numeric values are stable: registration
// (T41) and the CLSID routing table reference them, and tests compare them.
enum class Family : std::uint32_t {
    Unknown = 0,
    Gltf = 1,
    Stl = 2,
    Ply = 3,
    Obj = 4,
    Fbx = 5,
    ThreeMf = 6,
    Usd = 7,
    Step = 8,
};

// Defined in the T06 foundation header. The provider budget caps, the
// monotonic deadline and the process-wide product-owned allocation ledger are
// owned by T06; T04 freezes only the names and the way the contracts consume
// them, so no adapter can grow its own private limit type.
struct ProviderLimits;
class Deadline;
class AllocationLedger;

// Bounded, deadline-aware input handed to an adapter. Implemented once by T12
// over IInitializeWithStream; an adapter never opens a path or resolves a
// sidecar. Seekable streams are read through checked range reads; an adapter
// that needs one contiguous buffer uses ContiguousView() and fails the generic
// fallback if the bounded backing cap is exceeded.
class BoundedSource {
public:
    BoundedSource() = default;
    BoundedSource(const BoundedSource&) = delete;
    BoundedSource& operator=(const BoundedSource&) = delete;
    virtual ~BoundedSource() noexcept = default;

    // Validated source size in bytes, or 0 when the size is unknown (a
    // non-seekable stream with no trustworthy STATSTG). Never a trust decision.
    virtual std::uint64_t Size() const noexcept = 0;

    // True only when ReadAt() may be called at arbitrary offsets.
    virtual bool Seekable() const noexcept = 0;

    // Copies exactly dest.size() bytes from absolute offset. Returns false on a
    // short read, a range outside the validated size, an exceeded limit or an
    // expired deadline. No partial result is left trusted on failure.
    virtual bool ReadAt(std::uint64_t offset, std::span<std::byte> dest) = 0;

    // A const view of the whole bounded source when it fits the contiguous
    // backing cap, otherwise an empty span. Valid only for the lifetime of this
    // call; never assumed for a non-seekable or oversize source.
    virtual std::span<const std::byte> ContiguousView() = 0;
};

// A sampled vertex attribute bundle. Positions are local floats relative to a
// per-sample double origin so large coordinates keep usable precision; normals
// are unit-length when supplied and zero when the adapter wants a flat normal;
// color is linear RGBA in [0,1].
struct VertexSample {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 0.0f, 0.0f};
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

// One triangle emitted by an adapter. materialIndex is 1-based into the
// material table (0 selects the neutral fallback).
struct TriangleSample {
    VertexSample vertices[3];
    double origin[3] = {0.0, 0.0, 0.0};
    std::uint32_t materialIndex = 0;
    std::uint32_t reserved = 0;
};

// One point emitted by an adapter. radius <= 0 selects the bounds-derived
// default; materialIndex follows the same 1-based convention as triangles.
struct PointSample {
    VertexSample vertex;
    double origin[3] = {0.0, 0.0, 0.0};
    float radius = 0.0f;
    std::uint32_t materialIndex = 0;
    std::uint32_t reserved = 0;
};

// Double-precision verified bounds accumulated from finite positions.
struct Bounds {
    double min[3] = {0.0, 0.0, 0.0};
    double max[3] = {0.0, 0.0, 0.0};
    bool valid = false;
};

// The neutral fallback material (design/05, "material fallbacks"). A family
// with no usable material must use this rather than fail valid geometry; it is
// also what the rasterizer substitutes for materialIndex 0 and for an
// out-of-range index.
inline constexpr model_core::MaterialPayload NeutralMaterial() noexcept
{
    model_core::MaterialPayload material{};
    material.baseColorFactor[0] = 0.72f;
    material.baseColorFactor[1] = 0.72f;
    material.baseColorFactor[2] = 0.72f;
    material.baseColorFactor[3] = 1.0f;
    material.metallicFactor = 0.0f;
    material.roughnessFactor = 0.65f;
    material.emissiveFactor[0] = 0.0f;
    material.emissiveFactor[1] = 0.0f;
    material.emissiveFactor[2] = 0.0f;
    material.uvOffset[0] = 0.0f;
    material.uvOffset[1] = 0.0f;
    material.uvScale[0] = 1.0f;
    material.uvScale[1] = 1.0f;
    material.uvRotation = 0.0f;
    material.alphaMode = static_cast<std::uint32_t>(model_core::AlphaModeId::Opaque);
    material.alphaCutoff = 0.5f;
    material.flags = 0u;
    material.transmissionFactor = 0.0f;
    return material;
}

} // namespace preview3d::provider
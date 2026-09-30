#pragma once

// T25 glTF/GLB family adapter (ADR-0004: product adapter over fastgltf).
//
// Renders stream-contained `.glb`/`.gltf` geometry selected only by the routed
// `Family::Gltf` CLSID -- never by content or extension -- through the frozen
// `IFamilyAdapter` lifecycle (FamilyAdapter.h). Parsing uses fastgltf with
// `Options::None`, so an external buffer/image URI is never opened: a `.gltf`
// that depends on a local `.bin`/image sidecar is rejected as an unsafe
// reference and receives no thumbnail. Only the GLB BIN chunk and embedded
// `data:` URIs are resolved.
//
// Decoder scope follows ADR-0003 and is bounded by ProviderLimits: embedded
// uncompressed accessors, bounded **Draco** (`KHR_draco_mesh_compression`) and
// **meshopt** (`EXT_meshopt_compression`) geometry, and bounded **KTX2/Basis**
// and **WebP** images. The frozen `model_core::MaterialPayload` carries no
// texture slots and the T15 rasterizer does not sample textures, so a decoded
// embedded image is budgeted and validated but cannot change the thumbnail's
// product-owned material values; a missing, corrupt or over-budget *optional*
// image therefore uses the default material and never fails valid geometry.
//
// Geometry shape:
//   * nodes are traversed from the default scene (or the implicit roots) and
//     each node instance's world transform, in double precision, is applied to
//     the sampled vertices; a mesh referenced by several nodes emits several
//     instances;
//   * positions/normals are decoded through a meshopt-aware buffer adapter;
//     normals are transformed by the inverse-transpose when present and left
//     zero (the rasterizer derives a geometric normal) otherwise;
//   * vertex colors (`COLOR_0`) are carried; UVs are intentionally dropped
//     because the frozen `VertexSample` has no UV channel and no texture is
//     sampled;
//   * material index is `glTF material index + 1` (0 selects the neutral
//     fallback).
//
// The translation unit stays PCH-free and free of GDI/COM so `Tests.Unit.exe`
// and `Tests.ProviderHost.exe` compile the exact source the DLL links.

#include "AllocationLedger.h"
#include "FamilyAdapter.h"
#include "ProviderTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace preview3d::provider {

// Opaque parsed-asset owner (defined in the .cpp): keeps the fastgltf Asset,
// the non-owning input buffer and the meshopt decode cache together and out of
// third-party headers.
struct GltfAssetHolder;

class GltfAdapter final : public IFamilyAdapter {
public:
    GltfAdapter() noexcept;
    ~GltfAdapter() noexcept override;

    GltfAdapter(const GltfAdapter&) = delete;
    GltfAdapter& operator=(const GltfAdapter&) = delete;

    ErrorCode Initialize(const AdapterInput& input) noexcept override;
    ErrorCode Parse() noexcept override;
    ErrorCode EnumerateMaterials(IMaterialSink& sink) noexcept override;
    ErrorCode EnumerateGeometry(IGeometrySink& sink) noexcept override;
    void Reset() noexcept override;

    // Test/diagnostic hook: true when the parsed asset referenced an external
    // (non-`data:`) buffer or image URI. The provider never opens such a path;
    // Parse fails with an unsafe-reference typed error when this is set.
    bool ExternalReferenceDetected() const noexcept { return externalReferenceDetected_; }

    // Number of embedded images whose KTX2/Basis or WebP decode succeeded, and
    // the aggregate decoded pixels they contributed.
    std::uint64_t DecodedImageCount() const noexcept { return decodedImageCount_; }
    std::uint64_t DecodedImagePixels() const noexcept { return decodedImagePixels_; }

    bool UsedDraco() const noexcept { return usedDraco_; }
    bool UsedMeshopt() const noexcept;

private:
    ErrorCode SourceReadFailure() const noexcept;
    ErrorCode LoadSourceBytes() noexcept;
    ErrorCode DecodeImages() noexcept;
    bool BuildVertex(VertexSample& vertex, const double world[16],
                     const float* positions, const float* normals, const float* colors,
                     std::size_t index) noexcept;

    AdapterInput input_{};
    std::unique_ptr<GltfAssetHolder> holder_;
    bool parsed_ = false;
    bool externalReferenceDetected_ = false;
    bool usedDraco_ = false;
    std::uint64_t decodedImageCount_ = 0;
    std::uint64_t decodedImagePixels_ = 0;

    // Bounded backing buffer when the Shell stream exposes no contiguous view.
    std::span<const std::byte> bytes_{};
    std::vector<std::byte> ownedBytes_;
    std::optional<AllocationReservation> bytesReservation_;

    struct Instance {
        std::size_t meshIndex = 0;
        double world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };
    std::vector<Instance> instances_;
    std::vector<model_core::MaterialPayload> materials_;
    bool haveOrigin_ = false;
    double origin_[3] = {0.0, 0.0, 0.0};
};

} // namespace preview3d::provider
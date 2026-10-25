#pragma once

// T32 3MF family adapter (ADR-0025: product adapter over the provider-local
// pinned lib3mf reader).
//
// Renders the supported static `.3mf` preview subset selected only by the routed
// `Family::ThreeMf` CLSID -- never by extension -- through the frozen
// `IFamilyAdapter` lifecycle (FamilyAdapter.h):
//
//   * the bounded product OPC/ZIP preflight (`import-worker` `InspectThreeMfOpc`,
//     compiled from the same source the worker uses) rejects unsafe paths,
//     duplicate parts, encryption, multi-disk archives, unsupported
//     compression, expansion bombs, malformed headers, and missing OPC parts
//     before lib3mf reads a byte;
//   * a bounded product XML scan rejects a root model whose `requiredextensions`
//     names an extension outside the Core/Materials/Production/Beam-Lattice/Ball
//     allowlist (lib3mf compatible mode does not enforce this);
//   * lib3mf is fed the bounded in-memory stream through read/seek callbacks and
//     never its filename API, so the provider performs no filesystem, sidecar,
//     network or persistent access and launches no worker;
//   * the standard root build is traversed deterministically across build items
//     and component graphs with checked transforms; every occurrence emits
//     triangles (or a bounded beam/ball lattice tessellation) with the object,
//     triangle or per-corner 3MF color resolved to vertex colors;
//   * contained PNG/JPEG texture attachments are structurally validated against
//     the aggregate pixel budget but not decoded: the frozen
//     `model_core::MaterialPayload` has no texture slot and the T15 rasterizer
//     samples no texture, so a decoded image could not change the thumbnail;
//   * an unsupported required extension, an over-budget scene or an unclipped
//     parametric lattice fails closed to the generic icon; a supported scene is
//     never rendered only in part.
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

// Opaque owner of the lib3mf wrapper/model and the flattened root-build
// occurrences; kept in the .cpp so no lib3mf header crosses this boundary.
struct ThreeMfModelHolder;

class ThreeMfAdapter final : public IFamilyAdapter {
public:
    ThreeMfAdapter() noexcept;
    ~ThreeMfAdapter() noexcept override;

    ThreeMfAdapter(const ThreeMfAdapter&) = delete;
    ThreeMfAdapter& operator=(const ThreeMfAdapter&) = delete;

    ErrorCode Initialize(const AdapterInput& input) noexcept override;
    ErrorCode Parse() noexcept override;
    ErrorCode EnumerateMaterials(IMaterialSink& sink) noexcept override;
    ErrorCode EnumerateGeometry(IGeometrySink& sink) noexcept override;
    void Reset() noexcept override;

    // Diagnostics/test hooks. None changes the product decision.
    std::uint64_t BuildItemCount() const noexcept { return buildItemCount_; }
    std::uint64_t OccurrenceCount() const noexcept { return occurrenceCount_; }
    std::uint64_t InspectedTriangleCount() const noexcept { return inspectedTriangles_; }
    std::uint64_t EmbeddedImageCount() const noexcept { return embeddedImageCount_; }
    std::uint64_t EmbeddedImagePixels() const noexcept { return embeddedImagePixels_; }
    std::uint64_t LatticeTriangleCount() const noexcept { return latticeTriangles_; }
    bool UsedLattice() const noexcept { return usedLattice_; }
    bool OmittedUnsupportedTexture() const noexcept { return omittedUnsupportedTexture_; }

private:
    // Per-stage bodies, run under the T06 stage-containment shim
    // (ContainmentStage.h). Non-noexcept so a product-owned `std::bad_alloc`
    // unwinds into the stage boundary (or this adapter's own LoadModel/
    // BuildScene/EnumerateGeometry try-catch) instead of terminating at a frozen
    // `noexcept` public method.
    ErrorCode InitializeImpl(const AdapterInput& input);
    ErrorCode ParseImpl();
    ErrorCode EnumerateMaterialsImpl(IMaterialSink& sink);
    ErrorCode EnumerateGeometryImpl(IGeometrySink& sink);

    ErrorCode SourceReadFailure() const noexcept;
    ErrorCode LoadSourceBytes();
    ErrorCode PreflightPackage();
    ErrorCode ScanRequiredExtensions();
    ErrorCode LoadModel();
    ErrorCode BuildScene();
    void ResetState() noexcept;

    AdapterInput input_{};
    std::unique_ptr<ThreeMfModelHolder> holder_;
    bool parsed_ = false;
    bool usedLattice_ = false;
    bool omittedUnsupportedTexture_ = false;
    std::uint64_t buildItemCount_ = 0;
    std::uint64_t occurrenceCount_ = 0;
    std::uint64_t inspectedTriangles_ = 0;
    std::uint64_t latticeTriangles_ = 0;
    std::uint64_t embeddedImageCount_ = 0;
    std::uint64_t embeddedImagePixels_ = 0;

    // Bounded backing buffer when the Shell stream exposes no contiguous view.
    std::span<const std::byte> bytes_{};
    std::vector<std::byte> ownedBytes_;
    std::optional<AllocationReservation> bytesReservation_;
};

} // namespace preview3d::provider
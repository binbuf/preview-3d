#pragma once

// T33 USD/USDZ family adapter (ADR-0026: product adapter over the provider-local
// pinned TinyUSDZ reader; no OpenUSD, no composition, no external resolution).
//
// Renders the supported static `.usd`/`.usda`/`.usdc`/`.usdz` preview subset
// selected only by the routed `Family::Usd` CLSID -- never by extension --
// through the frozen `IFamilyAdapter` lifecycle (FamilyAdapter.h):
//
//   * the container is byte-sniffed independently of any suffix: a ZIP local
//     header is USDZ, the crate magic `PXR-USDC` is USDC, a leading `#usda` is
//     USDA, anything else is malformed (`UnsupportedFormat` for a mismatch is a
//     viewer concept the provider does not have -- it simply fails closed);
//   * a USDZ stream is validated by the product-owned `import_worker`
//     `InspectUsdz` preflight (compiled from the same source the worker uses)
//     with provider ceilings: stored-only entries, checked offsets/sizes,
//     normalized paths, bounded aggregate expansion and entry count. Archive
//     contents stay in the brokered in-memory stream -- nothing is extracted;
//   * TinyUSDZ is fed the bounded in-memory stream and never a path. Asset and
//     composition loading stay disabled (`load_assets`, `do_composition`,
//     `load_sublayers`, `load_references`, `load_payloads`), so the provider
//     opens no file, sidecar, cache entry or network endpoint and launches no
//     process;
//   * composition arcs (sublayers, references, payloads, inherits, specializes,
//     variants, clips, instanceable) and any external asset reference fail
//     closed to the generic icon (`UnsupportedComposition`/`UnsafeReference`);
//     only stream-contained USDA/USDC bytes and stream-contained USDZ layers or
//     textures are admitted. Contained textures are recorded but not decoded --
//     the frozen `VertexSample`/`MaterialPayload` contracts carry no texture
//     slot, so an absent or external image never fabricates geometry;
//   * the static `UsdPreviewSurface`/display-color policy is normalized into the
//     shared material payload and finite sampled triangles/points within the
//     provider caps, then handed to the T14 sampler and T15 rasterizer.
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

// Opaque owner of the TinyUSDZ stage, the converted render scene, the flattened
// node list and the resolved material table; kept in the .cpp so no TinyUSDZ/
// tydra header crosses this boundary.
struct UsdModelHolder;

class UsdAdapter final : public IFamilyAdapter {
public:
    UsdAdapter() noexcept;
    ~UsdAdapter() noexcept override;

    UsdAdapter(const UsdAdapter&) = delete;
    UsdAdapter& operator=(const UsdAdapter&) = delete;

    ErrorCode Initialize(const AdapterInput& input) noexcept override;
    ErrorCode Parse() noexcept override;
    ErrorCode EnumerateMaterials(IMaterialSink& sink) noexcept override;
    ErrorCode EnumerateGeometry(IGeometrySink& sink) noexcept override;
    void Reset() noexcept override;

    // Diagnostics/test hooks. None changes the product decision.
    std::uint64_t MeshCount() const noexcept { return meshCount_; }
    std::uint64_t NodeCount() const noexcept { return nodeCount_; }
    std::uint64_t MaterialCount() const noexcept { return materialCount_; }
    std::uint64_t InspectedTriangleCount() const noexcept { return inspectedTriangles_; }
    std::uint64_t InstanceCount() const noexcept { return instanceCount_; }
    bool DetectedComposition() const noexcept { return detectedComposition_; }
    bool DetectedExternalAsset() const noexcept { return detectedExternalAsset_; }
    bool UsedUsdzArchive() const noexcept { return usedUsdz_; }

private:
    ErrorCode SourceReadFailure() const noexcept;
    ErrorCode LoadSourceBytes() noexcept;
    ErrorCode SniffContainer() noexcept;
    ErrorCode PreflightArchive() noexcept;
    ErrorCode LoadStage() noexcept;
    ErrorCode BuildScene() noexcept;
    ErrorCode ValidateScene() noexcept;
    void ResetState() noexcept;

    AdapterInput input_{};
    std::unique_ptr<UsdModelHolder> holder_;
    bool parsed_ = false;
    bool detectedComposition_ = false;
    bool detectedExternalAsset_ = false;
    bool usedUsdz_ = false;
    std::uint64_t meshCount_ = 0;
    std::uint64_t nodeCount_ = 0;
    std::uint64_t materialCount_ = 0;
    std::uint64_t inspectedTriangles_ = 0;
    std::uint64_t instanceCount_ = 0;

    // Bounded backing buffer when the Shell stream exposes no contiguous view.
    std::span<const std::byte> bytes_{};
    std::vector<std::byte> ownedBytes_;
    std::optional<AllocationReservation> bytesReservation_;
};

} // namespace preview3d::provider
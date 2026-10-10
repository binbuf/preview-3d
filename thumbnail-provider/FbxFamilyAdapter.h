#pragma once

// T31 FBX family adapter (ADR-0024: product adapter over the provider-local
// pinned ufbx copy).
//
// Renders binary and ASCII `.fbx` geometry selected only by the routed
// `Family::Fbx` CLSID -- never by extension -- through the frozen
// `IFamilyAdapter` lifecycle (FamilyAdapter.h). Parsing uses the
// provider-local pinned ufbx copy configured so path, external-file,
// geometry-cache, plug-in, script and environment-codec access are all
// disabled:
//
//   * `load_external_files = false` plus `ignore_missing_external_files` and a
//     deny `open_file_cb` mean external geometry caches and texture sidecars are
//     never opened; only bytes already inside the bounded Shell stream are read;
//   * `file_format` is left unknown with extension detection disabled and
//     content detection enabled, so ASCII and binary FBX both parse without a
//     path while a non-FBX stream is rejected by `metadata.file_format`.
//
// Deterministic static pose (FBX-001, FBX-008):
//   * if a first animation stack exists, evaluate at its `time_begin`; otherwise
//     evaluate the default `scene->anim` at time zero;
//   * `evaluate_skinning = true`, `evaluate_caches = false`; the authoritative
//     deformed attributes are `mesh.skinned_position` / `mesh.skinned_normal`
//     (identical to the static attributes for non-skinned meshes). When
//     `mesh.skinned_is_local` is true the position is transformed by
//     `node.geometry_to_world` and the normal by its inverse-transpose; when it
//     is false the evaluated attribute is already in world space;
//   * `ufbx_evaluate_scene` has no progress callback (FBX-001), so this adapter
//     bounds the call by preflighting element/triangle/vertex counts *before*
//     evaluation and by explicit temp/result allocator memory and allocation
//     limits. A call never runs unbounded merely because Shell uses a surrogate.
//
// Geometry/materials:
//   * polygons are triangulated with `ufbx_triangulate_face`; reversed winding is
//     honored; vertex colors are carried; UVs are parsed but dropped because the
//     frozen `VertexSample` has no UV channel and no texture is sampled;
//   * each face resolves its material through `node.materials` (per-instance)
//     then `mesh.materials`; a face with no material uses materialIndex 0 except
//     when the mesh carries vertex colors, in which case a white fallback is
//     registered so `vertexColor * baseColor` preserves the source;
//   * NURBS-only, subdivision-only, procedural-only or geometry-cache-only
//     content (no supported polygon remains) returns `UnsupportedRequiredFeature`
//     so Explorer shows its generic icon. When supported polygons remain, the
//     unsupported features are omitted and `OmittedUnsupportedGeometry()` is set.
//
// Embedded images are validated only: the allowlisted container (PNG/JPEG/GIF/
// BMP/WebP) is sniffed, bounded by encoded bytes and decoded pixels against the
// aggregate texture budget, then discarded. The frozen `MaterialPayload` has no
// texture slot and the T15 rasterizer samples no texture, so a decoded image
// could not change the product-owned thumbnail; an external texture therefore
// uses the neutral fallback and never fails valid geometry.
//
// The translation unit stays PCH-free and free of GDI/COM so `Tests.Unit.exe`
// and `Tests.ProviderHost.exe` compile the exact source the DLL links.

#include "AllocationLedger.h"
#include "FamilyAdapter.h"
#include "ProviderTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// Opaque: ufbx.h is not pulled into adapters' clients.
struct ufbx_scene;
struct ufbx_material;

namespace preview3d::provider {

class FbxAdapter final : public IFamilyAdapter {
public:
    FbxAdapter() = default;
    ~FbxAdapter() noexcept override;

    FbxAdapter(const FbxAdapter&) = delete;
    FbxAdapter& operator=(const FbxAdapter&) = delete;

    ErrorCode Initialize(const AdapterInput& input) noexcept override;
    ErrorCode Parse() noexcept override;
    ErrorCode EnumerateMaterials(IMaterialSink& sink) noexcept override;
    ErrorCode EnumerateGeometry(IGeometrySink& sink) noexcept override;
    void Reset() noexcept override;

    // Diagnostics/test hooks. None changes the product decision.
    bool ExternalFileDetected() const noexcept { return externalFileDetected_; }
    bool EvaluationRan() const noexcept { return evaluationRan_; }
    std::uint64_t EvaluatedTriangles() const noexcept { return evaluatedTriangles_; }
    std::uint64_t EmbeddedImageCount() const noexcept { return embeddedImageCount_; }
    std::uint64_t EmbeddedImagePixels() const noexcept { return embeddedImagePixels_; }
    bool OmittedUnsupportedGeometry() const noexcept { return omittedUnsupportedGeometry_; }
    bool HasVertexColors() const noexcept { return hasVertexColors_; }

private:
    // Per-stage bodies, run under the T06 stage-containment shim
    // (ContainmentStage.h). Non-noexcept so a product-owned `std::bad_alloc`
    // (the backing copy, the material table/index map and the per-face index
    // scratch) unwinds into the boundary instead of terminating at the frozen
    // `noexcept` public method.
    ErrorCode InitializeImpl(const AdapterInput& input);
    ErrorCode ParseImpl();
    ErrorCode EnumerateMaterialsImpl(IMaterialSink& sink);
    ErrorCode EnumerateGeometryImpl(IGeometrySink& sink);

    ErrorCode SourceReadFailure() const noexcept;
    ErrorCode LoadSourceBytes();
    ErrorCode EvaluateStaticPose() noexcept;
    ErrorCode Preflight() noexcept;
    void BuildMaterials();
    ErrorCode ValidateEmbeddedImages() noexcept;
    void FreeScene() noexcept;

    AdapterInput input_{};
    ufbx_scene* scene_ = nullptr;
    bool parsed_ = false;
    bool evaluationRan_ = false;
    bool externalFileDetected_ = false;
    bool omittedUnsupportedGeometry_ = false;
    bool hasVertexColors_ = false;
    std::uint64_t evaluatedTriangles_ = 0;
    std::uint64_t embeddedImageCount_ = 0;
    std::uint64_t embeddedImagePixels_ = 0;

    // Bounded backing buffer when the Shell stream exposes no contiguous view.
    std::span<const std::byte> bytes_{};
    std::vector<std::byte> ownedBytes_;
    std::optional<AllocationReservation> bytesReservation_;

    // 1-based material table; `materialIndex_` maps an evaluated-scene material
    // pointer to its 1-based index. `whiteMaterialIndex_` is the vertex-color
    // fallback, or 0 when unused.
    std::vector<model_core::MaterialPayload> materials_;
    std::unordered_map<const ufbx_material*, std::uint32_t> materialIndex_;
    std::uint32_t whiteMaterialIndex_ = 0;
    bool haveOrigin_ = false;
    double origin_[3] = {0.0, 0.0, 0.0};
};

} // namespace preview3d::provider
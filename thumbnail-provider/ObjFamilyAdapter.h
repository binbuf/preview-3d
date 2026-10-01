#pragma once

// T24 OBJ family adapter (ADR-0004: product adapter over ufbx).
//
// Renders Wavefront `.obj` geometry selected only by the routed `Family::Obj`
// CLSID -- never by content or extension -- through the frozen `IFamilyAdapter`
// lifecycle (FamilyAdapter.h). Parsing uses the provider-local pinned ufbx copy
// (the vcpkg `x64-windows-static-md` static library) configured so that path,
// external-file, geometry-cache, plug-in, script and environment-codec access
// are all disabled:
//
//   * `load_external_files = false` means `mtllib` and every texture reference
//     is never opened; the `open_file_cb` deny hook is the belt-and-braces
//     second line;
//   * the only bytes ufbx sees are the bounded Shell stream, copied into the
//     checked contiguous backing when the stream cannot expose a view.
//
// Geometry shape:
//   * ufbx splits OBJ by object and by group (`obj_merge_*` stay false), so each
//     `o`/`g` becomes its own mesh and a large disconnected component survives
//     the sampler;
//   * polygons are triangulated with `ufbx_triangulate_face`; missing normals
//     are generated and normalized by ufbx, supplied ones are normalized here;
//   * vertex colors are carried when the source has them (base color is forced
//     white so `vertexColor * baseColor` preserves the source);
//   * UVs are decoded but cannot cross `IGeometrySink`, whose frozen
//     `VertexSample` has no UV channel: the thumbnail path renders the neutral
//     material, so UVs are intentionally dropped.
//
// All external dependencies are ignored: an OBJ that references a `.mtl` or a
// texture still renders its neutral geometry and never triggers a file open.
// The translation unit stays PCH-free and free of GDI/COM so `Tests.Unit.exe`
// and `Tests.ProviderHost.exe` compile the exact source the DLL links.

#include "AllocationLedger.h"
#include "FamilyAdapter.h"
#include "ProviderTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Opaque: ufbx.h is not pulled into adapters' clients.
struct ufbx_scene;

namespace preview3d::provider {

class ObjAdapter final : public IFamilyAdapter {
public:
    ObjAdapter() = default;
    ~ObjAdapter() noexcept override;

    ObjAdapter(const ObjAdapter&) = delete;
    ObjAdapter& operator=(const ObjAdapter&) = delete;

    ErrorCode Initialize(const AdapterInput& input) noexcept override;
    ErrorCode Parse() noexcept override;
    ErrorCode EnumerateMaterials(IMaterialSink& sink) noexcept override;
    ErrorCode EnumerateGeometry(IGeometrySink& sink) noexcept override;
    void Reset() noexcept override;

    // Diagnostics/test hook: true when ufbx requested an external sidecar
    // (`.mtl`/texture) during Parse and the provider denied the open. The
    // provider never opens a path; a stream-contained OBJ leaves this false.
    bool ExternalFileDenied() const noexcept { return externalFileDenied_; }

private:
    ErrorCode SourceReadFailure() const noexcept;
    void FreeScene() noexcept;

    AdapterInput input_{};
    ufbx_scene* scene_ = nullptr;
    bool parsed_ = false;
    bool externalFileDenied_ = false;
    bool hasVertexColors_ = false;
    bool haveOrigin_ = false;
    double origin_[3] = {0.0, 0.0, 0.0};
    std::span<const std::byte> bytes_{};
    std::vector<std::byte> ownedBytes_;
    std::optional<AllocationReservation> bytesReservation_;
};

} // namespace preview3d::provider
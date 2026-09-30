// T13 family-adapter registry implementation (see FamilyAdapterRegistry.h).
//
// Free of the provider precompiled header and of GDI/COM so the same translation
// unit compiles into Preview3DThumbnailProvider.dll and Tests.Unit.exe.
//
// Each family task (T21 STL, T23 PLY, T24 OBJ, T25 glTF, T31 FBX, T32 3MF,
// T33 USD, T34 STEP) replaces the nullptr for its own `Family` with its adapter
// constructor. A family that is not linked returns nullptr, which
// RunThumbnailPipeline maps to ProviderOutcome::Unsupported.

#include "FamilyAdapterRegistry.h"

#include "FbxFamilyAdapter.h"
#include "GltfFamilyAdapter.h"
#include "ObjFamilyAdapter.h"
#include "PlyFamilyAdapter.h"
#include "StepFamilyAdapter.h"
#include "StlFamilyAdapter.h"
#include "ThreeMfFamilyAdapter.h"
#include "UsdFamilyAdapter.h"

namespace preview3d::provider {

std::unique_ptr<IFamilyAdapter> CreateFamilyAdapter(Family family) noexcept
{
    switch (family) {
        case Family::Stl:
            // T21: the product ASCII/binary STL adapter (StlAdapter.{h,cpp}).
            try {
                return std::make_unique<StlAdapter>();
            } catch (...) {
                return nullptr;
            }
        case Family::Ply:
            // T23: the product ASCII/binary PLY mesh/point adapter.
            try {
                return std::make_unique<PlyAdapter>();
            } catch (...) {
                return nullptr;
            }
        case Family::Obj:
            // T24: the product Wavefront OBJ adapter over the provider-local
            // pinned ufbx copy (external-file access disabled).
            try {
                return std::make_unique<ObjAdapter>();
            } catch (...) {
                return nullptr;
            }
        case Family::Gltf:
            // T25: the product glTF/GLB adapter over fastgltf (embedded data
            // URIs/GLB BIN only; bounded Draco/meshopt geometry and KTX2/WebP
            // images; no external sidecar).
            try {
                return std::make_unique<GltfAdapter>();
            } catch (...) {
                return nullptr;
            }
        case Family::Fbx:
            // T31: the product binary/ASCII FBX adapter over the provider-local
            // pinned ufbx copy (external-file access disabled, bounded static
            // pose with skin/blend baked).
            try {
                return std::make_unique<FbxAdapter>();
            } catch (...) {
                return nullptr;
            }
        case Family::ThreeMf:
            // T32: the product 3MF adapter over the provider-local pinned
            // lib3mf reader (bounded OPC/required-extension preflight, root
            // build traversal, colors, bounded beam/ball lattice; no worker).
            try {
                return std::make_unique<ThreeMfAdapter>();
            } catch (...) {
                return nullptr;
            }
        case Family::Usd:
            // T33: the product stream-only USDA/USDC/USDZ adapter over the
            // provider-local pinned TinyUSDZ reader (no composition, no external
            // resolution, no OpenUSD).
            try {
                return std::make_unique<UsdAdapter>();
            } catch (...) {
                return nullptr;
            }
        case Family::Step:
            // T34: the constrained STEP/STP adapter over the dedicated static
            // OCCT closure (Part-21 admission before OCCT, bounded XDE read and
            // low-detail tessellation; no Preview3DStepHost.exe launch).
            try {
                return std::make_unique<StepAdapter>();
            } catch (...) {
                return nullptr;
            }
        case Family::Unknown:
        default:
            return nullptr;
    }
}

} // namespace preview3d::provider
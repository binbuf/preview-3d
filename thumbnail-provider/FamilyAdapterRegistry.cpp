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

namespace preview3d::provider {

std::unique_ptr<IFamilyAdapter> CreateFamilyAdapter(Family family) noexcept
{
    switch (family) {
        case Family::Gltf:
        case Family::Stl:
        case Family::Ply:
        case Family::Obj:
        case Family::Fbx:
        case Family::ThreeMf:
        case Family::Usd:
        case Family::Step:
        case Family::Unknown:
        default:
            return nullptr;
    }
}

} // namespace preview3d::provider
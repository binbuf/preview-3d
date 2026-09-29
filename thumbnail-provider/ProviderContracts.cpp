// Interface freeze compile check (T04).
//
// This translation unit exists only to prove that the frozen adapter, sampler,
// rasterizer and routing headers compile inside the provider project under the
// repository's /W4 /WX /permissive- policy. It defines no runtime behavior and
// exports nothing.

#include "pch.h"

#include "CpuRasterizer.h"
#include "FamilyAdapter.h"
#include "FamilyRouting.h"
#include "GeometrySampler.h"
#include "ProviderTypes.h"

namespace preview3d::provider {

static_assert(FamilyRoutes().size() == 8u, "the eight-family roster is frozen");
static_assert(FamilyForClsid("{A592F425-EA68-4C88-BB96-020805D4BE56}") == Family::Gltf);
static_assert(FamilyForClsid("{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}") == Family::Stl);
static_assert(FamilyForClsid("{F4DC6119-E235-4BAC-8089-54EDD84F8492}") == Family::Ply);
static_assert(FamilyForClsid("{D4722752-C480-4D9C-BEBE-1A9B514A8846}") == Family::Obj);
static_assert(FamilyForClsid("{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}") == Family::Fbx);
static_assert(FamilyForClsid("{D8389A63-8526-454A-9892-72F3149484B9}") == Family::ThreeMf);
static_assert(FamilyForClsid("{E938BC70-4C08-4446-A15D-EE31576BFB48}") == Family::Usd);
static_assert(FamilyForClsid("{6EE961AC-AC3B-4958-A898-E30523FEE79D}") == Family::Step);
static_assert(FamilyForClsid("{00000000-0000-0000-0000-000000000000}") == Family::Unknown);

static_assert(RouteForClsid("{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}")->extensions.size() == 1u);
static_assert(RouteForClsid("{E938BC70-4C08-4446-A15D-EE31576BFB48}")->extensions.size() == 4u);
static_assert(kThumbnailHandlerShellExGuid[0] == '{');

static_assert(NeutralMaterial().alphaMode == static_cast<std::uint32_t>(model_core::AlphaModeId::Opaque));

} // namespace preview3d::provider
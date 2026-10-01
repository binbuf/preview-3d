// Interface freeze compile check (T04).
//
// This translation unit exists only to prove that the frozen adapter, sampler,
// rasterizer and routing headers compile inside the provider project under the
// repository's /W4 /WX /permissive- policy. It defines no runtime behavior and
// exports nothing.

#include "pch.h"

#include "AllocationLedger.h"
#include "CpuRasterizer.h"
#include "Deadline.h"
#include "FamilyAdapter.h"
#include "FamilyRouting.h"
#include "GeometrySampler.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"
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

// T06 budgets, deadline policy and HRESULT mapping (design/05, design/03).
static_assert(ProviderLimits::kStreamMaxBytes == 256ull * 1024 * 1024);
static_assert(ProviderLimits::kContiguousBackingMaxBytes == 128ull * 1024 * 1024);
static_assert(ProviderLimits::kAccountedScratchMaxBytes == 192ull * 1024 * 1024);
static_assert(ProviderLimits::kAllocationLedgerMaxBytes == 384ull * 1024 * 1024);
static_assert(ProviderLimits::kProcessCommitQualificationTargetBytes == 384ull * 1024 * 1024);
static_assert(ProviderLimits::kTrianglesInspectedMax == 2'000'000);
static_assert(ProviderLimits::kPointsInspectedMax == 6'000'000);
static_assert(ProviderLimits::kRasterizedSamplesMax == 250'000);
static_assert(ProviderLimits::kDecodedTexturePixelsMax == 32'000'000);
static_assert(ProviderLimits::kNodesMax == 10'000);
static_assert(ProviderLimits::kMaterialsMax == 4'096);
static_assert(ProviderLimits::kDracoDecodedWorkingSetMaxBytes == 96ull * 1024 * 1024);
static_assert(ProviderLimits::kDracoTrianglesMax == 1'000'000);
static_assert(AllocationLedger::kDefaultLimitBytes == ProviderLimits::kAllocationLedgerMaxBytes);
static_assert(Deadline::kTargetP95 == std::chrono::milliseconds{750});
static_assert(Deadline::kCooperativeStop == std::chrono::milliseconds{2000});
static_assert(ClassifyError(ErrorCode::OutOfMemory) == ProviderOutcome::OutOfMemory);
static_assert(HresultFor(ProviderOutcome::LimitExceeded) ==
              HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
// T13/ADR-0015: the degenerate cx == 0 request is an invalid argument.
static_assert(HresultFor(ProviderOutcome::BadArgument) == E_INVALIDARG);

// Checked helpers are usable at compile time for file-derived checks.
static_assert(FitsInRange(0, ProviderLimits::kStreamMaxBytes,
                          ProviderLimits::kStreamMaxBytes));
static_assert(!FitsInRange(0, ProviderLimits::kStreamMaxBytes + 1,
                           ProviderLimits::kStreamMaxBytes));
static_assert(!CheckedMultiply(UINT64_MAX, 2).has_value());

} // namespace preview3d::provider
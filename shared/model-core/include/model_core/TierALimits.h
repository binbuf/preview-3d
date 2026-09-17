#pragma once
#include <cstdint>
namespace model_core
{
constexpr uint64_t kTierAPrimarySourceBytes = 8ull * 1024 * 1024 * 1024;
constexpr uint64_t kTierAAllSourceBytes = 12ull * 1024 * 1024 * 1024;
constexpr uint32_t kTierATriangleLimit = 100000000;
constexpr uint32_t kTierAPointLimit = 100000000;
constexpr uint32_t kTierAVertexLimit = 300000000;
constexpr uint32_t kTierAObjectLimit = 100000;
constexpr uint32_t kTierAMaterialLimit = 65536;
constexpr uint32_t kTierAClusterTriangles = 65536;
constexpr uint32_t kTierAClusterPoints = 524288;
// One tail per logical occurrence, plus full clusters, all material slots,
// initial/refined texture identities, bounded preview primitives, and three
// warning/status/completion records. Scan/coarse/full each retain region IDs.
constexpr uint32_t kTierAGeometryCatalogLimit =
    kTierAObjectLimit + (kTierATriangleLimit + kTierAClusterTriangles - 1) / kTierAClusterTriangles +
    (kTierAPointLimit + kTierAClusterPoints - 1) / kTierAClusterPoints;
constexpr uint32_t kTierACatalogLimit =
    3 * kTierAGeometryCatalogLimit + 4096 + kTierAMaterialLimit + 2 * 4 * kTierAMaterialLimit + 3;
// Every nonempty section consumes an identity. This remains safe for a test
// window admitting just one chunk; the production byte window batches many.
constexpr uint32_t kTierABatchLimit = kTierACatalogLimit;
// Worst normalized geometry expands independently of source: STL's 50-byte
// facet can become 108 bytes; PLY polygon fans/accessor remapping are capped
// by the same valid-triangle ceiling rather than the encoded byte length.
constexpr uint64_t kTierAMaxNormalizedGeometryBytes =
    uint64_t(kTierATriangleLimit) * 108 + uint64_t(kTierAPointLimit) * 12;

// Tier B is the bounded, materializing path from design doc 03. ASCII STL,
// ASCII PLY, and OBJ/MTL are the shipping users. Keep these limits in the shared
// contract so the worker and the independently validating broker cannot
// silently disagree about the accepted expansion.
constexpr uint64_t kTierBPrimarySourceBytes = 2ull * 1024 * 1024 * 1024;
constexpr uint64_t kTierBAllSourceBytes = 4ull * 1024 * 1024 * 1024;
constexpr uint32_t kTierBTriangleLimit = 20'000'000;
constexpr uint32_t kTierBPointLimit = 20'000'000;
constexpr uint32_t kTierBVertexLimit = 60'000'000;
constexpr uint32_t kTierBIndexLimit = 3 * kTierBTriangleLimit;
constexpr uint32_t kTierBObjectLimit = 50'000;
constexpr uint32_t kTierBMaterialLimit = 32'768;
} // namespace model_core

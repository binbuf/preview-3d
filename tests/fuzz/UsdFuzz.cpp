#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "UsdZipPreflight.h"
#include "import_broker/SharedSectionValidator.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/OpenUsdIdentifier.h"

#include "tinyusdz.hh"
#include "tydra/render-data.hh"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace {

constexpr uint32_t kEnvelopeMagic = 0x5a465355; // "USFZ"
constexpr size_t kInputLimit = 2u * 1024u * 1024u;
constexpr size_t kNormalizedWalkLimit = 1u * 1024u * 1024u;

#pragma pack(push, 1)
struct Envelope {
    uint32_t magic;
    uint8_t domain;
    uint8_t flags;
    uint16_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(Envelope) == 8);

enum class Domain : uint8_t {
    UsdObjectGraph = 0,
    UsdzDirectory = 1,
    NormalizedOutput = 2,
    ControlFrame = 3,
    DependencyIdentifier = 4,
};

struct Input {
    Domain domain = Domain::UsdObjectGraph;
    uint8_t flags = 0;
    std::span<const std::byte> payload;
};

Input Decode(std::span<const std::byte> bytes)
{
    if (bytes.size() < sizeof(Envelope)) return {Domain::UsdObjectGraph, 0, bytes};
    Envelope envelope{};
    std::memcpy(&envelope, bytes.data(), sizeof(envelope));
    if (envelope.magic != kEnvelopeMagic)
        return {Domain::UsdObjectGraph, 0, bytes};
    return {static_cast<Domain>(envelope.domain % 5), envelope.flags,
            bytes.subspan(sizeof(envelope))};
}

void FuzzObjectGraph(std::span<const std::byte> bytes)
{
    if (bytes.empty()) return;
    const auto* raw = reinterpret_cast<const uint8_t*>(bytes.data());
    // TinyUSDZ has no enforceable allocator hook. Arbitrary crate mutation can
    // encode multi-gigabyte allocations before its advisory setting reacts, so
    // USDC mutation belongs in the real worker/Job regression lane. Keep its
    // byte classifier under sanitizer here and mutate only the USDA parser in
    // this long-lived in-process target.
    if (tinyusdz::IsUSDC(raw, bytes.size())) return;
    if (!tinyusdz::IsUSDA(raw, bytes.size())) return;
    tinyusdz::USDLoadOptions options{};
    options.num_threads = 1;
    // TinyUSDZ documents this as advisory. The standalone process also runs
    // with libFuzzer's RSS and timeout limits; product enforcement remains the
    // AppContainer Job limit exercised by ImportIsolation.
    options.max_memory_limit_in_mb = 64;
    tinyusdz::Stage stage;
    std::string warning;
    std::string error;
    if (!tinyusdz::LoadUSDFromMemory(raw, bytes.size(), "fuzz.usda", &stage, &warning,
                                    &error, options)) return;

    tinyusdz::tydra::RenderScene scene;
    tinyusdz::tydra::RenderSceneConverterEnv environment(stage);
    environment.usd_filename = "fuzz.usd";
    environment.timecode = tinyusdz::value::TimeCode::Default();
    environment.tinterp = tinyusdz::value::TimeSampleInterpolationType::Linear;
    environment.scene_config.load_texture_assets = false;
    environment.mesh_config.triangulate = true;
    environment.mesh_config.build_vertex_indices = true;
    environment.mesh_config.compute_normals = true;
    environment.mesh_config.compute_tangents_and_binormals = false;
    tinyusdz::tydra::RenderSceneConverter converter;
    if (!converter.ConvertToRenderScene(environment, &scene)) return;

    // Exercise the same parser-to-normalized object graph surfaces without a
    // GPU or output section. Cap the walk independently of parser allocation.
    uint64_t hash = 1469598103934665603ull;
    size_t walked = 0;
    for (const auto& mesh : scene.meshes) {
        if (++walked > kNormalizedWalkLimit) break;
        hash ^= mesh.points.size();
        hash *= 1099511628211ull;
        const size_t count = (std::min)(mesh.points.size(), kNormalizedWalkLimit - walked);
        for (size_t index = 0; index < count; ++index) {
            for (double value : mesh.points[index]) {
                uint64_t bits = 0;
                static_assert(sizeof(value) == sizeof(bits));
                std::memcpy(&bits, &value, sizeof(bits));
                hash = (hash ^ bits) * 1099511628211ull;
            }
        }
        walked += count;
    }
    volatile uint64_t sink = hash;
    (void)sink;
}

void FuzzUsdz(std::span<const std::byte> bytes, uint8_t flags)
{
    import_worker::UsdzPreflightLimits limits{};
    limits.maxEntries = 128;
    limits.maxPathDepth = 16;
    limits.maxEntryBytes = kInputLimit;
    limits.maxExpandedBytes = kInputLimit;
    limits.maxExpansionRatio = 16;
    import_worker::UsdzArchiveView archive;
    unsigned cancellationChecks = 0;
    const unsigned cancelAfter = flags & 15u;
    (void)import_worker::InspectUsdz(bytes, &archive, limits, [&] {
        return (flags & 0x80u) != 0 && ++cancellationChecks > cancelAfter;
    });
}

void FuzzControlFrame(std::span<const std::byte> bytes)
{
    // Control frames are at most 264 bytes by contract. Feeding the production
    // framed reader through an anonymous pipe covers oversized declarations,
    // truncation, and mutated OpenUSD resolver/request records.
    if (bytes.size() > sizeof(model_core::ControlMessageHeader)
            + model_core::kMaxControlPayloadBytes) return;
    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, nullptr, 512)) return;
    DWORD written = 0;
    if (!bytes.empty())
        (void)WriteFile(writeHandle, bytes.data(), static_cast<DWORD>(bytes.size()),
                        &written, nullptr);
    CloseHandle(writeHandle);
    const auto message = model_core::ReadControlMessage(readHandle);
    CloseHandle(readHandle);
    if (!message) return;

    if (message->header.opcode
            == static_cast<uint32_t>(model_core::ControlOpcode::RequestSidecarFile)
        && message->payload.size() == sizeof(model_core::RequestSidecarFileNotice)) {
        model_core::RequestSidecarFileNotice notice{};
        std::memcpy(&notice, message->payload.data(), sizeof(notice));
        volatile bool bounded = notice.relativePathLength
            <= model_core::kMaxSidecarRelativePathBytes;
        (void)bounded;
    } else if (message->header.opcode
                   == static_cast<uint32_t>(model_core::ControlOpcode::StartOpenUsdImportFromFile)
               && message->payload.size() == sizeof(model_core::ParseOpenUsdFileRequest)) {
        model_core::ParseOpenUsdFileRequest request{};
        std::memcpy(&request, message->payload.data(), sizeof(request));
        volatile bool closedFlags = (request.requestFlags
            & ~model_core::kImportRequestUsdExpectedMask) == 0;
        (void)closedFlags;
    }
}

void FuzzDependencyIdentifier(std::span<const std::byte> bytes)
{
    const size_t split = [&] {
        const auto found = std::find(bytes.begin(), bytes.end(), std::byte{0});
        return static_cast<size_t>(found - bytes.begin());
    }();
    const auto assetBytes = bytes.first(split);
    const auto anchorBytes = split < bytes.size() ? bytes.subspan(split + 1)
                                                  : std::span<const std::byte>{};
    if (assetBytes.size() >= model_core::kMaxOpenUsdIdentifierBytes
        || anchorBytes.size() >= model_core::kMaxOpenUsdIdentifierBytes) return;
    const std::string_view asset(reinterpret_cast<const char*>(assetBytes.data()),
                                 assetBytes.size());
    const std::string_view anchor(reinterpret_cast<const char*>(anchorBytes.data()),
                                  anchorBytes.size());
    const auto resolved = model_core::AnchorOpenUsdIdentifier(asset, anchor);
    if (resolved) (void)model_core::IsSafeOpenUsdIdentifier(*resolved);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!data || size > kInputLimit + sizeof(Envelope)) return 0;
    const Input input = Decode({reinterpret_cast<const std::byte*>(data), size});
    try {
        switch (input.domain) {
        case Domain::UsdObjectGraph:
            FuzzObjectGraph(input.payload);
            break;
        case Domain::UsdzDirectory:
            FuzzUsdz(input.payload, input.flags);
            break;
        case Domain::NormalizedOutput:
            (void)import_broker::ValidateAndCopySection(input.payload, 0, 4096);
            break;
        case Domain::ControlFrame:
            FuzzControlFrame(input.payload);
            break;
        case Domain::DependencyIdentifier:
            FuzzDependencyIdentifier(input.payload);
            break;
        }
    } catch (...) {
        // Parser failures may use exceptions internally; only sanitizer faults
        // and process failures are fuzz oracles for arbitrary bytes.
    }
    return 0;
}

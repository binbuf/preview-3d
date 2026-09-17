#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "UsdSpikeWorker.h"

#include "GenerationWorker.h"
#include "UsdZipPreflight.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include "tinyusdz.hh"
#include "tydra/render-data.hh"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace import_worker {
namespace {

struct MemorySample {
    uint64_t privateBytes = 0;
    uint64_t peakWorkingSetBytes = 0;
};

MemorySample SampleMemory()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) return {};
    return { static_cast<uint64_t>(counters.PrivateUsage),
             static_cast<uint64_t>(counters.PeakWorkingSetSize) };
}

uint64_t Microseconds(std::chrono::steady_clock::time_point begin)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count());
}

struct HashState {
    uint64_t value = 14695981039346656037ull;
    uint64_t bytes = 0;

    void Add(std::span<const std::byte> data)
    {
        for (const std::byte byte : data) {
            value ^= std::to_integer<uint8_t>(byte);
            value *= 1099511628211ull;
        }
        bytes += data.size();
    }

    template<class T> void Scalar(const T& scalar)
    {
        Add(std::as_bytes(std::span(&scalar, 1)));
    }

    template<class T> void Vector(const std::vector<T>& values)
    {
        const uint64_t count = values.size();
        Scalar(count);
        Add(std::as_bytes(std::span(values)));
    }

    void String(const std::string& text)
    {
        const uint64_t count = text.size();
        Scalar(count);
        Add(std::as_bytes(std::span(text.data(), text.size())));
    }
};

void HashNode(const tinyusdz::tydra::Node& node, HashState& hash, uint32_t& nodeCount)
{
    ++nodeCount;
    hash.String(node.prim_name);
    hash.String(node.abs_path);
    hash.Scalar(node.nodeType);
    hash.Scalar(node.id);
    hash.Scalar(node.has_resetXform);
    const uint64_t childCount = node.children.size();
    hash.Scalar(childCount);
    for (const auto& child : node.children) HashNode(child, hash, nodeCount);
}

bool HashScene(const tinyusdz::tydra::RenderScene& scene, UsdSpikePayloadHeader& header)
{
    HashState hash;
    hash.String(scene.meta.upAxis);
    hash.Scalar(scene.meta.metersPerUnit);
    header.nodeCount = 0;
    for (const auto& node : scene.nodes) HashNode(node, hash, header.nodeCount);
    for (const auto& mesh : scene.meshes) {
        hash.String(mesh.prim_name);
        hash.String(mesh.abs_path);
        hash.Scalar(mesh.doubleSided);
        hash.Scalar(mesh.is_rightHanded);
        hash.Scalar(mesh.displayColor);
        hash.Scalar(mesh.displayOpacity);
        hash.Vector(mesh.points);
        hash.Vector(mesh.faceVertexIndices());
        hash.Add(std::as_bytes(std::span(mesh.normals.data)));
        for (const auto& point : mesh.points) {
            if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2]))
                return false;
        }
    }
    header.meshCount = static_cast<uint32_t>(scene.meshes.size());
    header.materialCount = static_cast<uint32_t>(scene.materials.size());
    header.imageCount = static_cast<uint32_t>(scene.images.size());
    header.normalizedHash = hash.value;
    header.normalizedBytes = hash.bytes;
    return true;
}

struct ResolverContext {
    std::span<const std::byte> source;
    uint32_t calls = 0;
};

int ResolveAsset(const char* name, const std::vector<std::string>&, std::string* resolved,
                 std::string*, void* userdata)
{
    auto& context = *static_cast<ResolverContext*>(userdata);
    ++context.calls;
    if (!name || !resolved) return -1;
    *resolved = "broker-primary.usd";
    return 0;
}

int SizeAsset(const char*, uint64_t* size, std::string*, void* userdata)
{
    auto& context = *static_cast<ResolverContext*>(userdata);
    ++context.calls;
    if (!size) return -1;
    *size = context.source.size();
    return 0;
}

int ReadAsset(const char*, uint64_t requested, uint8_t* destination, uint64_t* read,
              std::string*, void* userdata)
{
    auto& context = *static_cast<ResolverContext*>(userdata);
    ++context.calls;
    if (!destination || !read || requested != context.source.size()) return -1;
    std::memcpy(destination, context.source.data(), context.source.size());
    *read = context.source.size();
    return 0;
}

UsdSpikeFormat Detect(std::span<const std::byte> source)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(source.data());
    if (tinyusdz::IsUSDA(bytes, source.size())) return UsdSpikeFormat::Usda;
    if (tinyusdz::IsUSDC(bytes, source.size())) return UsdSpikeFormat::Usdc;
    if (tinyusdz::IsUSDZ(bytes, source.size())) return UsdSpikeFormat::Usdz;
    return UsdSpikeFormat::Unknown;
}

void ClearResults(UsdSpikePayloadHeader& header)
{
    header.succeeded = 0;
    header.detectedFormat = 0;
    header.resolverCalls = 0;
    header.meshCount = 0;
    header.nodeCount = 0;
    header.materialCount = 0;
    header.imageCount = 0;
    header.normalizedHash = 0;
    header.normalizedBytes = 0;
    header.parseMicroseconds = 0;
    header.convertMicroseconds = 0;
    header.privateBytesBefore = 0;
    header.privateBytesAfterParse = 0;
    header.privateBytesAfterConvert = 0;
    header.peakWorkingSetBytes = 0;
}

bool RunProbe(UsdSpikePayloadHeader& header, std::span<const std::byte> source)
{
    ClearResults(header);
    const UsdSpikeFormat format = Detect(source);
    header.detectedFormat = static_cast<uint32_t>(format);
    if (format == UsdSpikeFormat::Unknown) return false;
    if (format == UsdSpikeFormat::Usdz && PreflightUsdz(source) != UsdzPreflightError::None)
        return false;

    const auto before = SampleMemory();
    header.privateBytesBefore = before.privateBytes;
    header.peakWorkingSetBytes = before.peakWorkingSetBytes;
    tinyusdz::USDLoadOptions options{};
    options.num_threads = 1;
    options.max_memory_limit_in_mb =
        header.mode == static_cast<uint32_t>(UsdSpikeMode::OneMiBAdvisoryLimit) ? 1 : 2048;
    const auto parseStart = std::chrono::steady_clock::now();
    InterlockedExchange(&header.state, 1);

    if (header.mode == static_cast<uint32_t>(UsdSpikeMode::ResolverLayer)) {
        ResolverContext context{ source };
        tinyusdz::AssetResolutionHandler handler{};
        handler.resolve_fun = ResolveAsset;
        handler.size_fun = SizeAsset;
        handler.read_fun = ReadAsset;
        handler.userdata = &context;
        tinyusdz::AssetResolutionResolver resolver;
        resolver.register_wildcard_asset_resolution_handler(handler);
        tinyusdz::Layer layer;
        std::string warning, error;
        const bool loaded = tinyusdz::LoadLayerFromAsset(
            resolver, "broker-primary.usd", &layer, &warning, &error, options);
        header.parseMicroseconds = Microseconds(parseStart);
        header.resolverCalls = context.calls;
        const auto after = SampleMemory();
        header.privateBytesAfterParse = after.privateBytes;
        header.privateBytesAfterConvert = after.privateBytes;
        header.peakWorkingSetBytes = (std::max)(header.peakWorkingSetBytes,
                                               after.peakWorkingSetBytes);
        if (!loaded || context.calls < 2) return false;
        HashState hash;
        hash.Add(source);
        header.normalizedHash = hash.value;
        header.normalizedBytes = hash.bytes;
        header.succeeded = 1;
        InterlockedExchange(&header.state, 3);
        return true;
    }

    tinyusdz::Stage stage;
    std::string warning, error;
    const bool loaded = tinyusdz::LoadUSDFromMemory(
        reinterpret_cast<const uint8_t*>(source.data()), source.size(),
        "broker-primary.usd", &stage, &warning, &error, options);
    header.parseMicroseconds = Microseconds(parseStart);
    const auto afterParse = SampleMemory();
    header.privateBytesAfterParse = afterParse.privateBytes;
    header.peakWorkingSetBytes = (std::max)(header.peakWorkingSetBytes,
                                           afterParse.peakWorkingSetBytes);
    if (!loaded) return false;
    InterlockedExchange(&header.state, 2);

    if (header.mode == static_cast<uint32_t>(UsdSpikeMode::HoldAfterParse)) {
        Sleep(5000);
        return false;
    }

    const auto convertStart = std::chrono::steady_clock::now();
    tinyusdz::tydra::RenderScene scene;
    tinyusdz::tydra::RenderSceneConverterEnv environment(stage);
    environment.usd_filename = "broker-primary.usd";
    environment.timecode = tinyusdz::value::TimeCode::Default();
    environment.tinterp = tinyusdz::value::TimeSampleInterpolationType::Linear;
    environment.scene_config.load_texture_assets = false;
    environment.mesh_config.triangulate = true;
    environment.mesh_config.build_vertex_indices = true;
    environment.mesh_config.compute_normals = true;
    environment.mesh_config.compute_tangents_and_binormals = false;
    tinyusdz::tydra::RenderSceneConverter converter;
    if (!converter.ConvertToRenderScene(environment, &scene)) return false;
    header.convertMicroseconds = Microseconds(convertStart);
    const auto afterConvert = SampleMemory();
    header.privateBytesAfterConvert = afterConvert.privateBytes;
    header.peakWorkingSetBytes = (std::max)(header.peakWorkingSetBytes,
                                           afterConvert.peakWorkingSetBytes);
    if (!HashScene(scene, header)) return false;
    header.succeeded = 1;
    InterlockedExchange(&header.state, 3);
    return true;
}

bool HandleRequest(HANDLE stdOut, const model_core::StartGenerationRequest& request)
{
    if (request.sectionByteCapacity < sizeof(UsdSpikePayloadHeader)
        || request.sectionByteCapacity > (std::numeric_limits<SIZE_T>::max)()) return false;
    platform::Win32Handle section(
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sectionHandleValue)));
    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ | FILE_MAP_WRITE,
                                          static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!view) return false;
    auto* header = reinterpret_cast<UsdSpikePayloadHeader*>(view.bytes().data());
    if (header->magic != kUsdSpikePayloadMagic
        || header->sourceByteLength > view.bytes().size() - sizeof(*header)
        || header->mode > static_cast<uint32_t>(UsdSpikeMode::OneMiBAdvisoryLimit)) return false;
    platform::Win32Handle cancellationEvent(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(header->cancellationEventHandleValue)));
    if (!cancellationEvent) return false;
    const auto source = view.bytes().subspan(sizeof(*header),
                                            static_cast<size_t>(header->sourceByteLength));
    try {
        RunProbe(*header, source);
    } catch (const std::bad_alloc&) {
        header->succeeded = 0;
    } catch (...) {
        header->succeeded = 0;
    }
    model_core::ChunksReadyNotice notice{};
    notice.generationId = request.generationId;
    notice.sectionBytesWritten = sizeof(*header);
    return model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady,
                                           &notice, sizeof(notice));
}

} // namespace

int RunUsdSpikePoolMode()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    for (;;) {
        auto message = model_core::ReadControlMessage(stdIn);
        if (!message) return 1;
        if (message->header.opcode == uint32_t(model_core::ControlOpcode::Shutdown)) return 0;
        if (message->header.opcode != uint32_t(model_core::ControlOpcode::StartGeneration)
            || message->payload.size() != sizeof(model_core::StartGenerationRequest)) return 1;
        model_core::StartGenerationRequest request{};
        std::memcpy(&request, message->payload.data(), sizeof(request));
        if (request.sceneVariant != kUsdSpikeSceneVariant || !HandleRequest(stdOut, request))
            return 1;
    }
}

} // namespace import_worker

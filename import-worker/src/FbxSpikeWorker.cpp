#include "FbxSpikeWorker.h"

#include "GenerationWorker.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"
#include "ufbx.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <limits>

namespace import_worker {
namespace {

constexpr size_t kAllocatorLimit = 64u * 1024u * 1024u;

struct SlowAllocator {
    FbxSpikePayloadHeader* header = nullptr;
    bool delayed = false;
};

void DelayInsideEvaluation(SlowAllocator& allocator)
{
    if (allocator.delayed) return;
    allocator.delayed = true;
    InterlockedExchange(&allocator.header->evaluationState, 2);
    // The host must enforce its 500 ms cooperative grace while this thread is
    // provably still below ufbx_evaluate_scene(). Five seconds leaves ample
    // scheduler margin; the Job Object should terminate us first.
    Sleep(5000);
}

void* SlowAllocate(void* user, size_t size)
{
    auto& allocator = *static_cast<SlowAllocator*>(user);
    DelayInsideEvaluation(allocator);
    return std::malloc(size);
}

void* SlowReallocate(void* user, void* pointer, size_t, size_t newSize)
{
    auto& allocator = *static_cast<SlowAllocator*>(user);
    DelayInsideEvaluation(allocator);
    return std::realloc(pointer, newSize);
}

void SlowFree(void*, void* pointer, size_t)
{
    std::free(pointer);
}

ufbx_allocator_opts SlowAllocatorOptions(SlowAllocator& allocator)
{
    ufbx_allocator_opts options{};
    options.allocator.alloc_fn = SlowAllocate;
    options.allocator.realloc_fn = SlowReallocate;
    options.allocator.free_fn = SlowFree;
    options.allocator.user = &allocator;
    options.memory_limit = kAllocatorLimit;
    options.allocation_limit = 1'000'000;
    return options;
}

bool RunEvaluationProbe(const model_core::StartGenerationRequest& request)
{
    if (request.sectionByteCapacity < sizeof(FbxSpikePayloadHeader)
        || request.sectionByteCapacity > (std::numeric_limits<SIZE_T>::max)()) return false;
    platform::Win32Handle section(
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sectionHandleValue)));
    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ | FILE_MAP_WRITE,
                                           static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!view) return false;
    auto* header = reinterpret_cast<FbxSpikePayloadHeader*>(view.bytes().data());
    if (header->magic != kFbxSpikePayloadMagic || header->reserved
        || header->sourceByteLength > view.bytes().size() - sizeof(*header)) return false;
    platform::Win32Handle cancellationEvent(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(header->cancellationEventHandleValue)));
    if (!cancellationEvent) return false;

    ufbx_load_opts loadOptions{};
    loadOptions.evaluate_skinning = true;
    loadOptions.evaluate_caches = false;
    loadOptions.load_external_files = false;
    loadOptions.generate_missing_normals = true;
    loadOptions.node_depth_limit = 256;
    loadOptions.temp_allocator.memory_limit = kAllocatorLimit;
    loadOptions.result_allocator.memory_limit = kAllocatorLimit;
    loadOptions.temp_allocator.allocation_limit = 1'000'000;
    loadOptions.result_allocator.allocation_limit = 1'000'000;
    loadOptions.filename = { "fixture.fbx", 11 };
    ufbx_error error{};
    const std::byte* source = view.bytes().data() + sizeof(*header);
    ufbx_scene* scene = ufbx_load_memory(source, static_cast<size_t>(header->sourceByteLength),
                                         &loadOptions, &error);
    if (!scene) return false;

    const ufbx_anim* animation = scene->anim;
    double time = 0.0;
    if (scene->anim_stacks.count) {
        animation = scene->anim_stacks.data[0]->anim;
        time = scene->anim_stacks.data[0]->time_begin;
    }
    InterlockedExchange(&header->evaluationState, 1);
    SlowAllocator slow{};
    slow.header = header;
    ufbx_evaluate_opts evaluateOptions{};
    evaluateOptions.temp_allocator = SlowAllocatorOptions(slow);
    evaluateOptions.result_allocator = SlowAllocatorOptions(slow);
    evaluateOptions.evaluate_skinning = true;
    evaluateOptions.evaluate_caches = false;
    // ufbx 0.23.0 has no evaluation progress callback. The duplicated event
    // intentionally remains unread here so the broker's hard backstop is the
    // only possible interruption once state reaches 2.
    ufbx_scene* evaluated = ufbx_evaluate_scene(scene, animation, time, &evaluateOptions, &error);
    if (evaluated) ufbx_free_scene(evaluated);
    ufbx_free_scene(scene);
    return false;
}

} // namespace

int RunFbxSpikePoolMode()
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
        if (request.sceneVariant == kFbxSpikeEvaluationSceneVariant) {
            RunEvaluationProbe(request);
            return 1;
        }
        HandleStartGeneration(stdOut, request);
    }
}

} // namespace import_worker

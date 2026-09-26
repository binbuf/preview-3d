// Before any Windows header: the app's own framework.h sets these, but this
// translation unit reaches windows.h through RenderThread.h's D3D12 includes
// first, and without NOMINMAX the min/max macros break std::min/std::max.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "RenderThread.h"
#include <cstdio>

#include <windows.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <utility>
#include <stdexcept>
#include <new>
#include <cstring>
#include <unordered_set>
#include <psapi.h>
#include "DetailView.h"

namespace {

// Bounded everywhere: `04-rendering-and-streaming.md:190` requires shutdown
// to "wait with finite diagnostics timeouts", and "a driver hang must not
// leave the UI thread waiting forever."
constexpr DWORD kStopTimeoutMs = 5000;
// The idle wait. `04-...:38`: "A timer keeps animation/loading indicators
// alive; there is no unconstrained busy loop."
constexpr DWORD kIdleWaitMs = 100;

double NowSeconds()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

bool IsDeviceLoss(HRESULT value)
{
    return value == DXGI_ERROR_DEVICE_REMOVED || value == DXGI_ERROR_DEVICE_RESET
        || value == DXGI_ERROR_DEVICE_HUNG;
}

// Merge a batch's missing-asset references into the accumulating model
// metadata without duplicates; request order is preserved.
void MergeMissingAssets(ModelData& metadata, const std::vector<std::wstring>& assets)
{
    for (const auto& asset : assets) {
        if (asset.empty()) continue;
        if (std::find(metadata.missingAssets.begin(), metadata.missingAssets.end(), asset)
            == metadata.missingAssets.end())
            metadata.missingAssets.push_back(asset);
    }
}

// The single place the user-visible warning banner text is composed, so the
// existing feature/texture warnings and the missing-asset warning can never
// drift apart across the terminal/detail/per-batch metadata snapshots.
void RebuildModelWarning(ModelData& metadata)
{
    metadata.warning.clear();
    if (metadata.importStatus.optionalFeatureWarnings)
        metadata.warning = L"Some optional glTF features are not supported. Their fallback representation is shown.";
    if (metadata.importStatus.textureWarnings) {
        metadata.warning += (metadata.warning.empty() ? L"" : L"\n");
        metadata.warning += L"Some textures could not be loaded. Fallback textures are shown.";
    }
    if (!metadata.missingAssets.empty()) {
        metadata.warning += (metadata.warning.empty() ? L"" : L"\n");
        metadata.warning += L"Some referenced assets could not be found.";
    }
}

} // namespace

RenderThread::~RenderThread()
{
    Stop();
}

std::uint64_t RenderThread::SmokeValue(unsigned field) const noexcept
{
    switch (field) {
    case 55: return accountedGpuBytes_.load();
    case 56: return targetGpuBytes_.load();
    case 57: return pendingGpuBytes_.load();
    case 58: return evictionCount_.load();
    case 59: { std::lock_guard<std::mutex> lock(uploads_->mutex); return uploads_->detailRequests; }
    case 60: return rejectedDetailCount_.load();
    case 61: { std::lock_guard<std::mutex> lock(uploads_->mutex); return uploads_->requestedDetails.size(); }
    case 64: return isUma_ || smokeUma_.load();
    case 66: return recoveryCount_.load();
    case 48: return coarseChunks_.load();
    case 49: return fineChunks_.load();
    case 50: return suppressedCoarse_.load();
    case 51: return coarseCompleteGeneration_.load();
    case 53: return scannedPrimitives_.load();
    case 54: return coarseAllocationBytes_.load();
    case 2: return firstBackgroundUs_.load(std::memory_order_acquire);
    case 3: return geometryUs_.load(std::memory_order_acquire);
    case 67: return loadingUiUs_.load(std::memory_order_acquire);
    case 68: return coarseUs_.load(std::memory_order_acquire);
    case 69: return verifiedBoundsUs_.load(std::memory_order_acquire);
    case 70: return refinementUs_.load(std::memory_order_acquire);
    case 71: return benchmarkInputToPresentUs_.load(std::memory_order_acquire);
    case 72: return benchDurationMs_;
    case 73: return static_cast<std::uint64_t>(benchFrames_);
    case 74: return benchmarkPresentedFrames_.load(std::memory_order_acquire);
    case 75: return baselineCpuBytes_;
    case 76: return cpuPolicyCap_;
    case 77: {
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory);
        return K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))
            ? memory.PrivateUsage : 0;
    }
    case 4: return presentedGeneration_.load(std::memory_order_acquire);
    case 5: return resizedExtent_.load(std::memory_order_acquire);
    case 6: { std::lock_guard<std::mutex> lock(uploads_->mutex); return uploads_->peakBytes; }
    case 7: return Stats().frames;
    case 8: return displayedChunks_.load(std::memory_order_acquire);
    case 12: {
        std::lock_guard<std::mutex> lock(cameraMutex_);
        float distance = float(camera_.distance);
        uint32_t bits; std::memcpy(&bits, &distance, sizeof(bits)); return bits;
    }
    case 31: { std::lock_guard<std::mutex> lock(cameraMutex_); uint64_t bits; std::memcpy(&bits,&camera_.targetDistance,sizeof(bits)); return bits; }
    case 11: return texturedChunks_.load(std::memory_order_acquire);
    case 37: return textureExtent_.load();
    case 38: return textureCount_.load();
    case 39: return textureMips_.load();
    case 35: return debugErrors_.load();
    case 36: return debugAvailable_.load();
    case 32: return pickCompletions_.load();
    case 33: return pickHits_.load();
    case 10: { std::lock_guard<std::mutex> lock(uploads_->mutex); return uploads_->bytes; }
    case 9: { std::lock_guard<std::mutex> lock(uploads_->mutex); return uploads_->peakCount; }
    default: return 0;
    }
}

void RenderThread::SetBenchFrames(int frames)
{
    benchFrames_ = frames;
    benchRemaining_ = frames;
}

void RenderThread::SetBenchmarkLimits(int frames, std::uint64_t durationMs)
{
    SetBenchFrames(frames);
    benchDurationMs_ = durationMs;
}

void RenderThread::NotifyLoadingStarted(std::uint64_t generation)
{
    loadingGeneration_.store(generation, std::memory_order_release);
    completeModelAwaitingPresentGeneration_.store(0, std::memory_order_release);
    completeModelPresentedGeneration_.store(0, std::memory_order_release);
    completeModelPresentedUs_.store(0, std::memory_order_release);
    loadingUiUs_.store(0, std::memory_order_release);
    coarseUs_.store(0, std::memory_order_release);
    verifiedBoundsUs_.store(0, std::memory_order_release);
    refinementUs_.store(0, std::memory_order_release);
    Invalidate();
}

bool RenderThread::Start(HWND window, std::wstring& error)
{
    wakeEvent_ = platform::Win32Handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    startedEvent_ = platform::Win32Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!wakeEvent_ || !startedEvent_) {
        error = L"The render thread's events could not be created.";
        return false;
    }

    try {
        thread_ = std::thread(&RenderThread::ThreadMain, this, window);
    } catch (const std::exception&) {
        error = L"The render coordinator thread could not be created.";
        return false;
    }
    // Graphics initialization and its driver calls remain on the render
    // thread. Failure is posted back to the UI asynchronously; WM_CREATE no
    // longer waits on device/swap-chain creation.
    return true;
}

void RenderThread::Stop()
{
    if (!thread_.joinable()) return;
    CancelUploads();
    { std::lock_guard<std::mutex> lock(uploads_->mutex); uploads_->stopped = true; uploads_->changed.notify_all(); }

    stopRequested_.store(true, std::memory_order_release);
    if (wakeEvent_) SetEvent(wakeEvent_.get());

    // Bounded join. If the render thread is wedged in a driver call, detach
    // rather than hang the UI thread forever -- process exit is the backstop
    // the design names.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kStopTimeoutMs);
    while (running_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        Sleep(1);
    }
    if (running_.load(std::memory_order_acquire)) {
        thread_.detach();
        return;
    }
    thread_.join();
}

void RenderThread::RequestResize(int width, int height)
{
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        // Coalesced: only the latest size matters. A live drag-resize sends
        // one WM_SIZE per mouse tick and every one of them used to drain the
        // GPU on the UI thread.
        resizePending_ = true;
        resizeWidth_ = width;
        resizeHeight_ = height;
    }
    Invalidate();
}

void RenderThread::RequestPick(int x, int y, uint64_t generation)
{
    { std::lock_guard<std::mutex> lock(commandMutex_); pickRequest_ = PickRequest{x,y,generation}; }
    Invalidate();
}

void RenderThread::CancelUploads()
{
    std::lock_guard<std::mutex> lock(uploads_->mutex);
    if (uploads_->cancellation) uploads_->cancellation->store(true, std::memory_order_release);
    uploads_->generation = 0;
    for (const auto& task : uploads_->tasks) if (!task.terminal) { uploads_->bytes -= task.bytes; --uploads_->count; }
    for (const auto& pub : uploads_->publications) if (!pub.task.terminal) { uploads_->bytes -= pub.task.bytes; --uploads_->count; }
    uploads_->tasks.clear();
    for (const auto& pub:uploads_->publications) uploads_->gpuPendingBytes-=pub.task.gpuBytes;
    uploads_->publications.clear(); // These resources have already completed the copy fence.
    uploads_->details.clear(); uploads_->requestedDetails.clear();
    uploads_->changed.notify_all();
}

std::function<void(d3d12_import_bridge::ImportResult)> RenderThread::BeginImport(
    std::uint64_t generation, std::wstring path, std::shared_ptr<std::atomic_bool> cancellation)
{
    CancelUploads();
    auto inbox = uploads_;
    scannedPrimitives_.store(0);
    { std::lock_guard<std::mutex> lock(inbox->mutex); inbox->generation = generation; inbox->path = path;
      inbox->cancellation = cancellation; }
    return [inbox, generation, path = std::move(path), cancellation](auto result) {
        size_t bytes = sizeof(UploadTask) + path.size() * sizeof(wchar_t)
            + result.meshes.capacity() * sizeof(d3d12_import_bridge::ImportedMesh)
            + result.images.capacity() * sizeof(d3d12_import_bridge::ImportedImage)
            + result.materials.capacity() * sizeof(d3d12_import_bridge::ImportedMaterial)
            + result.nodes.capacity() * sizeof(d3d12_import_bridge::ImportedNode)
            + result.instances.capacity() * sizeof(d3d12_import_bridge::ImportedInstance);
        for (const auto& mesh : result.meshes) bytes += mesh.payload.capacity();
        for (const auto& image : result.images) bytes += image.pixelBytes.capacity();

        if (bytes > inbox->byteLimit) throw std::length_error("batch exceeds upload queue cap");
        std::unique_lock<std::mutex> lock(inbox->mutex);
        while (!inbox->stopped && inbox->generation == generation && !cancellation->load()
            && (inbox->count == UploadInbox::countCap || bytes > inbox->byteLimit - inbox->bytes))
            inbox->changed.wait_for(lock, std::chrono::milliseconds(20));
        if (inbox->stopped || inbox->generation != generation || cancellation->load()) return;
        if (inbox->count + 1 == UploadInbox::countCap || bytes > (inbox->byteLimit - inbox->bytes)/2)
            result.status.flags |= model_core::kStatusPressure;
        inbox->tasks.push_back({std::move(result), generation, path, cancellation, bytes, false});
        inbox->bytes += bytes;
        ++inbox->count;
        inbox->peakCount = std::max(inbox->peakCount, inbox->count);
        inbox->peakBytes = std::max(inbox->peakBytes, inbox->bytes);
        inbox->changed.notify_all();
    };
}

void RenderThread::FinishImport(std::uint64_t generation, model_core::FileIdentity sourceIdentity,
                                std::vector<std::wstring> missingAssets)
{
    // The terminal marker follows every accepted batch. It carries no payload
    // and takes no capacity; at most one marker exists for the active generation.
    std::lock_guard<std::mutex> lock(uploads_->mutex);
    if (!uploads_->stopped && uploads_->generation == generation)
    {
        UploadTask task;
        task.generation = generation;
        task.terminal = true;
        task.path = uploads_->path;
        task.result.sourceIdentity = sourceIdentity;
        task.result.missingAssets = std::move(missingAssets);
        uploads_->tasks.push_back(std::move(task));
        uploads_->changed.notify_all();
    }
}

void RenderThread::UploadMain()
{
    SetThreadDescription(GetCurrentThread(), L"Preview3D Upload");
    D3D12ViewerPath uploader;
    uploader.device.AttachForUpload(path_.device.Device());
    D3D12UploadRing::CreateOptions options;
    options.initialCapacityBytes = 64ull * 1024 * 1024;
    options.maxCapacityBytes = 128ull * 1024 * 1024;
    if (copyDelayMs_) {
        options.initialCapacityBytes = 4096;
        options.growthIncrementBytes = 4096;
        options.maxCapacityBytes = 8192;
        options.maxBatchBytes = 4096;
    }
    if (copyDelayMs_ && smokeSectionBytes_>=4ull*1024*1024) {
        options.initialCapacityBytes=4ull*1024*1024;options.growthIncrementBytes=4ull*1024*1024;
        options.maxCapacityBytes=8ull*1024*1024;options.maxBatchBytes=512ull*1024;
    }
    const bool initialized = uploader.uploadRing.Initialize(uploader.device, options);
    auto inbox = uploads_;
    for (;;) {
        Publication pub;
        {
            std::unique_lock<std::mutex> lock(inbox->mutex);
            inbox->changed.wait(lock, [&] { return inbox->stopped || !inbox->tasks.empty(); });
            if (inbox->stopped) break;
            pub.task = std::move(inbox->tasks.front()); inbox->tasks.pop_front();
        }
        auto current = [&] {
            std::lock_guard<std::mutex> lock(inbox->mutex);
            return !inbox->stopped && inbox->generation == pub.task.generation
                && (!pub.task.cancellation || !pub.task.cancellation->load());
        };
        try {
            if (!pub.task.terminal && current()) {
                // Developer smoke gates the copy queue's execution, so the
                // test exercises real fence-incomplete destinations and ring
                // waits while the direct queue keeps presenting prior content.
                // The timer also releases the gate on cancel/close.
                Microsoft::WRL::ComPtr<ID3D12Fence> gate;
                std::jthread gateTimer;
                if (copyDelayMs_ && initialized && current()
                    && SUCCEEDED(uploader.device.Device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))) {
                    uploader.uploadRing.CopyQueue().Queue()->Wait(gate.Get(), 1);
                    gateTimer = std::jthread([gate, current, delay = copyDelayMs_](std::stop_token stop) {
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
                        while (!stop.stop_requested() && current() && std::chrono::steady_clock::now() < deadline) Sleep(5);
                        gate->Signal(1);
                    });
                }
                // Reserve aligned destination allocations before any CreateCommittedResource.
                // Pending reservations remain charged through copy completion and render acceptance.
                {
                    std::unique_lock<std::mutex> lock(inbox->mutex);
                    if (pub.task.result.detail && !pub.task.result.meshes.empty()) pub.task.detailIdentity=pub.task.result.meshes.front().chunkId;
                    inbox->mandatoryBytes=D3D12ViewerPath::EstimateUploadBytes(uploader.device.Device(),pub.task.result.meshes,{});
                    for (const auto& mesh:pub.task.result.meshes)
                        if (mesh.geometry.lodLevel==model_core::kFineLod && mesh.geometry.sourceRangeLength)
                            inbox->mandatoryBytes-=D3D12ViewerPath::EstimateUploadBytes(
                                uploader.device.Device(),std::span(&mesh,1),{},false);
                    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                    while (inbox->mandatoryBytes && !inbox->stopped && inbox->generation==pub.task.generation
                        && (!pub.task.cancellation || !pub.task.cancellation->load())
                        && inbox->mandatoryBytes<=inbox->gpuTargetBytes
                        && inbox->gpuBaseBytes+inbox->gpuPendingBytes>inbox->gpuTargetBytes-inbox->mandatoryBytes
                        && std::chrono::steady_clock::now()<deadline)
                        inbox->changed.wait_for(lock,std::chrono::milliseconds(20));
                    const uint64_t mandatory=inbox->mandatoryBytes; inbox->mandatoryBytes=0;
                    const uint64_t used=inbox->gpuBaseBytes+inbox->gpuPendingBytes;
                    uint64_t available=used<inbox->gpuTargetBytes ? inbox->gpuTargetBytes-used : 0;
                    available=mandatory<available ? available-mandatory : 0;
                    auto& meshes=pub.task.result.meshes;
                    meshes.erase(std::remove_if(meshes.begin(),meshes.end(),[&](const auto& mesh) {
                        if (mesh.geometry.lodLevel != model_core::kFineLod || !mesh.geometry.sourceRangeLength) return false;
                        const uint64_t bytes=D3D12ViewerPath::EstimateUploadBytes(
                            uploader.device.Device(),std::span(&mesh,1),{},false);
                        if (bytes>available) { ++rejectedDetailCount_; pub.task.result.status.flags|=model_core::kStatusPressure; return true; }
                        available-=bytes; return false;
                    }),meshes.end());
                    // Selecting an already-validated mip tail is neither image decode nor resampling.
                    auto& images=pub.task.result.images;
                    images.erase(std::remove_if(images.begin(),images.end(),[&](auto& image) {
                        const uint64_t proxyReserve=inbox->coarseGeneration==pub.task.generation ? 0 : model_core::kCoarseReservedBytes;
                        const uint64_t textureAvailable=(image.width>64 || image.height>64)
                            ? (available>proxyReserve ? available-proxyReserve : 0) : available;
                        while (image.mipLevels>1 && (image.width>64 || image.height>64)
                            && D3D12ViewerPath::EstimateUploadBytes(uploader.device.Device(),{},std::span(&image,1))>textureAvailable) {
                            const auto first=*model_core::ComputeImagePixelBytes(image.pixelFormat,image.width,image.height,1);
                            image.pixelBytes.erase(image.pixelBytes.begin(),image.pixelBytes.begin()+size_t(first));
                            image.width=std::max(1u,image.width/2); image.height=std::max(1u,image.height/2); --image.mipLevels;
                            pub.task.result.status.flags|=model_core::kStatusPressure;
                        }
                        const uint64_t bytes=D3D12ViewerPath::EstimateUploadBytes(uploader.device.Device(),{},std::span(&image,1));
                        if (bytes>available) {
                            pub.task.result.status.textureWarnings=std::min(64u,pub.task.result.status.textureWarnings+1);
                            pub.task.result.status.flags|=model_core::kStatusPressure; return true;
                        }
                        available-=bytes; return false;
                    }),images.end());
                    pub.task.gpuBytes=D3D12ViewerPath::EstimateUploadBytes(uploader.device.Device(),meshes,pub.task.result.images);
                    if (pub.task.gpuBytes>inbox->gpuTargetBytes || used>inbox->gpuTargetBytes-pub.task.gpuBytes) {
                        pub.task.gpuBytes=0; throw std::bad_alloc();
                    }
                    inbox->gpuPendingBytes+=pub.task.gpuBytes;
                }
                std::wstring error = initialized ? L"" : L"The upload coordinator could not be initialized.";
                uploader.uploadIsCancelled=[current] {return !current();};
                const bool ok = current() && initialized && !pub.task.result.forceUploadFailureForTesting && uploader.BeginUploadModel(pub.task.result.meshes,
                    pub.task.result.materials, pub.task.result.images, error,
                    pub.task.result.nodes, {});
                pub.task.result.ok = ok;
                if (!ok) pub.task.result.errorCode = pub.task.result.forceUploadFailureForTesting
                    ? model_core::ImportErrorCode::UploadFailure : uploader.uploadErrorCode;
                pub.task.result.errorDetails = error;
                if (!ok) { uploader.WaitForIdle(); uploader.ReclaimRetired(); }
                if (!ok && IsDeviceLoss(uploader.device.Device()->GetDeviceRemovedReason())) {
                    recoveryRequested_.store(true, std::memory_order_release);
                    Invalidate();
                }
                if (!ok && copyDelayMs_) std::fwprintf(stderr,L"Upload smoke failure: %ls\n",error.c_str());
                if (ok) {
                    bool completed = false;
                    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (current() && std::chrono::steady_clock::now() < timeout) {
                        if (uploader.PollUploads()) { completed = true; break; }
                        uploader.ReclaimRetired(); Sleep(2);
                    }
                    if (completed) {
                        pub.resources = std::move(uploader.model);
                        uploader.model = {}; uploader.hasModel = false;
                    } else {
                        uploader.ClearModel(); // Coordinator only; drain before discarding destinations.
                        pub.task.result.ok = false;
                        pub.task.result.errorCode = model_core::ImportErrorCode::UploadFailure;
                        pub.task.result.errorDetails = L"The geometry copy did not complete within its timeout.";
                    }
                }
                // Keep only compact metadata after the copy; capacity remains
                // charged until the render thread accepts or discards publication.
                for (auto& mesh : pub.task.result.meshes) std::vector<std::byte>().swap(mesh.payload);
                pub.task.result.images.clear();
            }
        } catch (const std::bad_alloc&) {
            uploader.WaitForIdle(); uploader.ReclaimRetired();
            pub.task.result.ok = false;
            pub.task.result.errorCode = model_core::ImportErrorCode::OutOfMemory;
            pub.task.result.errorDetails = L"There is not enough memory to display this batch.";
            pub.task.result.meshes.clear(); pub.task.result.images.clear();
        } catch (...) {
            uploader.WaitForIdle(); uploader.ReclaimRetired();
            pub.task.result.ok = false;
            pub.task.result.errorCode = model_core::ImportErrorCode::UploadFailure;
            pub.task.result.errorDetails = L"The upload coordinator stopped while processing this batch.";
            pub.task.result.meshes.clear(); pub.task.result.images.clear();
        }
        {
            std::lock_guard<std::mutex> lock(inbox->mutex);
            if (!inbox->stopped && inbox->generation == pub.task.generation
                && (!pub.task.cancellation || !pub.task.cancellation->load()))
                inbox->publications.push_back(std::move(pub));
            else if (!pub.task.terminal) { inbox->bytes -= pub.task.bytes; --inbox->count; inbox->gpuPendingBytes-=pub.task.gpuBytes; }
            inbox->changed.notify_all();
        }
        Invalidate();
        uploader.ReclaimRetired();
    }
    uploader.WaitForIdle(); // Covers failed/superseded destinations on the copy timeline.
}

void RenderThread::RequestClearModel()
{
    CancelUploads();
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        clearModelPending_ = true;
    }
    Invalidate();
}

void RenderThread::Invalidate()
{
    invalidated_.store(true, std::memory_order_release);
    if (wakeEvent_) SetEvent(wakeEvent_.get());
}

void RenderThread::SetUiAnimating(bool animating)
{
    const bool was = uiAnimating_.exchange(animating, std::memory_order_acq_rel);
    if (animating && !was) Invalidate();
}

void RenderThread::PublishFlightInput(const FlightInput& input)
{
    std::lock_guard<std::mutex> lock(cameraMutex_);
    flightInput_ = input;
}

void RenderThread::PublishViewportAspect(float aspect)
{
    std::lock_guard<std::mutex> lock(cameraMutex_);
    viewportAspect_ = aspect;
}

void RenderThread::PublishFrameInputs(const FlightInput& input, float aspect,
                                      std::shared_ptr<const OverlayFrame> overlay)
{
    {
        std::lock_guard<std::mutex> lock(cameraMutex_);
        flightInput_ = input;
        viewportAspect_ = aspect;
        frameOverlay_ = std::move(overlay);
    }
    // A WM_PAINT wake may have been consumed before this snapshot arrived.
    // Wake again after publication so the final UI state always gets painted.
    Invalidate();
}

RenderThread::StatsSnapshot RenderThread::Stats() const
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

RenderThread::StatsSnapshot RenderThread::BenchmarkStats() const
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    return benchmarkStats_;
}

void RenderThread::AssertOnRenderThread() const
{
    // `04-rendering-and-streaming.md:194`: "Developer builds assert queue
    // ownership". Turns the ownership rule into something the build enforces
    // rather than something a reviewer has to notice.
    assert(std::this_thread::get_id() == renderThreadId_ && "D3D12 state touched off the render thread");
}

bool RenderThread::InitializeOnThread(HWND window, std::wstring& error)
{
    window_=window;
    if (!path_.Initialize(window, error) || !budgetMonitor_.Initialize(path_.device,{},error)) return false;
    D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
    if (SUCCEEDED(path_.device.Device()->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE,&architecture,sizeof(architecture)))) isUma_=architecture.UMA;
    UpdateBudget();
    return true;
}

void RenderThread::ThreadMain(HWND window)
{
    renderThreadId_ = std::this_thread::get_id();
    running_.store(true, std::memory_order_release);

    // Above normal, not time-critical: this thread must beat ordinary
    // background work to its vsync deadline, but it must never be able to
    // starve the UI thread -- NFR-01's whole point is that the UI stays
    // responsive. Failure is non-fatal; it just means default scheduling.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"Preview3D Render");

    startOk_ = InitializeOnThread(window, startError_);
    SetEvent(startedEvent_.get());
    if (!startOk_) {
        auto failure = std::make_unique<RenderStartFailure>();
        failure->details = startError_;
        if (PostMessageW(window, kRenderStartFailedMessage, 0,
                         reinterpret_cast<LPARAM>(failure.get())))
            (void)failure.release();
        running_.store(false, std::memory_order_release);
        return;
    }

    uploadThread_ = std::thread(&RenderThread::UploadMain, this);
    if (benchDurationMs_)
        benchDeadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(benchDurationMs_);
#ifdef _DEBUG
    if (SUCCEEDED(path_.device.Device()->QueryInterface(IID_PPV_ARGS(&debugInfo_)))) {
        D3D12_MESSAGE_SEVERITY severities[] = {D3D12_MESSAGE_SEVERITY_CORRUPTION,D3D12_MESSAGE_SEVERITY_ERROR};
        D3D12_INFO_QUEUE_FILTER filter{}; filter.AllowList.NumSeverities = 2; filter.AllowList.pSeverityList = severities;
        debugInfo_->AddStorageFilterEntries(&filter); debugInfo_->AddRetrievalFilterEntries(&filter); debugAvailable_.store(1);
    }
#endif

    while (!stopRequested_.load(std::memory_order_acquire)) {
        DrainCommands();
        if (stopRequested_.load(std::memory_order_acquire)) break;
        if (recoveryRequested_.exchange(false, std::memory_order_acq_rel)) {
            deviceFatal_ = !RecoverDevice();
            if (deviceFatal_) break;
            continue;
        }

        // Before deciding whether to render: retire finished copies and, if
        // this is the tick the in-flight model became complete, swap it in
        // and notify. Cheap when nothing is outstanding.
        PumpUploads(window);
        bool picked = false;
        if (pendingPick_ && path_.PollPick(picked)) {
            ++pickCompletions_; pickHits_.store(picked);
            auto reply = std::make_unique<RenderPickResult>(RenderPickResult{pendingPick_->generation,picked});
            if (pendingPick_->generation == modelGeneration_
                && PostMessageW(window,kRenderPickCompleteMessage,0,reinterpret_cast<LPARAM>(reply.get()))) (void)reply.release();
            pendingPick_.reset();
        }
        if (!pendingPick_) {
            std::lock_guard<std::mutex> commandLock(commandMutex_);
            if (pickRequest_) {
                if (pickRequest_->generation == modelGeneration_ && pickRequest_->x >= 0 && pickRequest_->y >= 0
                    && UINT(pickRequest_->x) < path_.swapChain.Width() && UINT(pickRequest_->y) < path_.swapChain.Height()) {
                    path_.pickX = pickRequest_->x; path_.pickY = pickRequest_->y; pendingPick_ = pickRequest_;
                    invalidated_.store(true);
                }
                pickRequest_.reset();
            }
        }

        bool cameraMoving = false;
        {
            std::lock_guard<std::mutex> lock(cameraMutex_);
            cameraMoving = camera_.HasMotion();
        }
        const bool beforeDeadline = benchDurationMs_ == 0 || std::chrono::steady_clock::now() < benchDeadline_;
        const bool benching = benchRemaining_ > 0 && beforeDeadline;
        if (benchRemaining_ > 0 && !beforeDeadline) {
            benchRemaining_ = 0;
            PublishStats(true);
            benchComplete_.store(true, std::memory_order_release);
        }
        // An upload in flight keeps the loop awake: the copy fence is
        // polled, not waited on, so something has to come back and look.
        // This is what replaces the old blocking wait -- the viewport stays
        // live, still presenting the previous model, while bytes land.
        const bool uploading = path_.UploadInFlight() || pendingPick_.has_value();
        const bool wanted = invalidated_.exchange(false, std::memory_order_acq_rel)
            || uiAnimating_.load(std::memory_order_acquire) || cameraMoving || benching || uploading;

        if (!wanted) {
            WaitForSingleObject(wakeEvent_.get(), kIdleWaitMs);
            continue;
        }

        const auto presentedBeforeBenchmarkFrame = path_.frameStats.PresentedFrames();
        RenderOneFrame();
        if (deviceFatal_) break;

        if (benching && benchDurationMs_ && benchmarkFrameStats_.PresentedFrames()
            >= static_cast<std::uint64_t>(std::max(1, benchFrames_ - 120))) {
            benchRemaining_ = 0;
            PublishStats(true);
            benchComplete_.store(true, std::memory_order_release);
        } else if (benching && !benchDurationMs_
                   && path_.frameStats.PresentedFrames() > presentedBeforeBenchmarkFrame) {
            --benchRemaining_;
            // Discard the first 120 frames as warm-up -- shader/PSO and
            // first-touch costs say nothing about steady state.
            if (benchRemaining_ == benchFrames_ - 120) {
                // Legacy --frame-bench remains a steady-state stopwatch.
                // --benchmark retains startup/loading intervals as raw data.
                if (!benchDurationMs_) {
                    path_.frameStats.Reset();
                    path_.overlayTotalMs = 0.0;
                    path_.overlayPasses = 0;
                }
            }
            if (benchRemaining_ == 0) {
                PublishStats(true); // forced, so the final numbers are not a multiple-of-30 away
                benchComplete_.store(true, std::memory_order_release);
            }
        }
    }

    // Release D2D/D3D11On12 before the D3D12 objects, per `04-...:190`, and
    // make sure no GPU work outlives this thread.
    if (uploadThread_.joinable()) uploadThread_.join();
    if (!deviceFatal_) {
        path_.WaitForIdle();
        path_.ClearModel();
    }
    path_.overlay.Shutdown();

    running_.store(false, std::memory_order_release);
}

void RenderThread::DrainCommands()
{
    AssertOnRenderThread();

    bool doResize = false;
    int width = 0;
    int height = 0;
    bool doClear = false;

    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        doResize = std::exchange(resizePending_, false);
        width = resizeWidth_;
        height = resizeHeight_;
        doClear = std::exchange(clearModelPending_, false);

    }

    // Between frames, which is the "direct-fence-safe point" `04-...:21`
    // asks resize to happen at.
    if (doResize) {
        std::wstring resizeError;
        if (!path_.Resize(width, height, resizeError)) {
            const HRESULT reason = path_.device.Device()->GetDeviceRemovedReason();
            if (IsDeviceLoss(reason)) deviceFatal_ = !RecoverDevice();
            else invalidated_.store(true, std::memory_order_release);
        } else {
            resizedExtent_.store((static_cast<std::uint64_t>(width) << 32)
                                  | static_cast<std::uint32_t>(height), std::memory_order_release);
        }
    }

    if (doClear) {
        path_.ClearModel();
        modelGeneration_ = 0;
        hasModel_.store(false, std::memory_order_release);
        displaySnapshot_.store({});
        displayedChunks_.store(0, std::memory_order_release);
        coarseChunks_.store(0); fineChunks_.store(0); suppressedCoarse_.store(0);
        coarseCompleteGeneration_.store(0); coarseAllocationBytes_.store(0); scannedPrimitives_.store(0);
        texturedChunks_.store(0, std::memory_order_release);
        textureExtent_.store(0);textureCount_.store(0);textureMips_.store(0);
        stagedScene_ = {}; stagedGeneration_ = 0; materials_.clear();

    }

}

void RenderThread::PumpUploads(HWND window)
{
    AssertOnRenderThread();

    path_.ReclaimRetired();
    UpdateBudget();
    if (smokeEviction_.exchange(false)) {
        std::vector<uint32_t> fine;
        for (const auto& mesh:path_.model.meshes) if (mesh.sourceGeometry.lodLevel==model_core::kFineLod) fine.push_back(mesh.chunkId);
        path_.EvictFineChunks(fine); pauseDetailUntil_=NowSeconds()+2; UpdateResidencySmoke();
    }
    std::unique_lock<std::mutex> lock(uploads_->mutex);
    if (stagedGeneration_ && stagedGeneration_ != uploads_->generation) {
        stagedScene_ = {}; stagedGeneration_ = 0; stagedHaveBounds_ = false;
        materials_.clear(); stagedFailed_ = false; stagedMetadata_.reset(); haveSceneOrigin_ = false;
        stagedProxyMode_ = false; stagedProxyComplete_ = false;
        stagedPreviewOnly_ = false;
        scannedPrimitives_.store(0);
    }
    if (uploads_->publications.empty()) return;
    auto pub = std::move(uploads_->publications.front()); uploads_->publications.pop_front();
    if (!pub.task.terminal) { uploads_->bytes -= pub.task.bytes; --uploads_->count; }
    uploads_->gpuPendingBytes-=pub.task.gpuBytes;
    uploads_->gpuBaseBytes+=pub.task.gpuBytes; // conservatively charge until the next complete census
    if (pub.task.detailIdentity) { uploads_->requestedDetails.erase(pub.task.detailIdentity); if (pub.resources.meshes.empty()) pauseDetailUntil_=NowSeconds()+0.5; }
    uploads_->changed.notify_all();
    // Hold the generation lock through frame-boundary acceptance so a new
    // activation cannot race an old publication into the scene.
    if (pub.task.generation != uploads_->generation) return;
    if (stagedGeneration_ != pub.task.generation) {
        stagedGeneration_ = pub.task.generation; stagedScene_ = {};
        materials_.clear(); stagedHaveBounds_ = false; stagedFailed_ = false;
        stagedMetadata_ = std::make_shared<ModelData>(); haveSceneOrigin_ = false;
        stagedProxyMode_ = false; stagedProxyComplete_ = false;
        stagedPreviewOnly_ = false; initialTerminal_=false; scanCatalog_.clear();
        scannedPrimitives_.store(0);
    }
    auto message = std::make_unique<RenderUploadResult>();
    message->generation = pub.task.generation; message->path = pub.task.path;
    message->terminal = pub.task.terminal;
    message->refinement=pub.task.result.detail;
    if (pub.task.terminal) {
        if (stagedMetadata_) {
            MergeMissingAssets(*stagedMetadata_, pub.task.result.missingAssets);
            RebuildModelWarning(*stagedMetadata_);
        }
        message->ok = !stagedFailed_ && (!stagedProxyMode_ || stagedProxyComplete_) && modelGeneration_ == pub.task.generation && path_.hasModel && stagedHaveBounds_ && stagedMetadata_;
        if (!message->ok) { message->errorCode = model_core::ImportErrorCode::EmptyGeometry; message->errorDetails = L"The import completed without displayable geometry."; }
        if (message->ok && stagedMetadata_) {
            stagedMetadata_->sourceIdentity = pub.task.result.sourceIdentity;
            stagedMetadata_->boundsVerified = true;
            initialTerminal_=true;
            completeModelAwaitingPresentGeneration_.store(pub.task.generation, std::memory_order_release);
            stagedMetadata_->importStatus.flags = rejectedDetailCount_.load() ? model_core::kStatusPressure : 0;
            message->metadata = std::make_shared<const ModelData>(*stagedMetadata_);
        }
    } else if (!pub.task.result.ok) {
        stagedFailed_ = true;
        message->errorCode = pub.task.result.errorCode;
        message->errorDetails = pub.task.result.errorDetails;
    } else if (pub.task.result.detail) {
        for (auto& mesh:pub.resources.meshes) {
            if (std::none_of(path_.model.meshes.begin(),path_.model.meshes.end(),[&](const auto& old) { return old.chunkId==mesh.chunkId; }))
                path_.model.meshes.push_back(std::move(mesh));
        }
        if (path_.model.neutralTextures.empty())
            path_.model.neutralTextures=std::move(pub.resources.neutralTextures);
        std::wstring descriptorError;
        const bool descriptorsReady=path_.RebuildMaterialDescriptors(path_.model,descriptorError);
        D3D12ViewerPath::UpdateCoarseVisibility(path_.model);
        for (auto& mesh:path_.model.meshes) {
            const auto mat=materials_.find(mesh.materialChunkId);
            if (mat==materials_.end()) continue;
            mesh.material=mat->second.data;mesh.hasMaterial=true;
            const uint32_t ids[4]{mat->second.baseColorImageChunkId,mat->second.metallicRoughnessImageChunkId,
                                  mat->second.normalImageChunkId,mat->second.emissiveImageChunkId};
            for (UINT slot=0;slot<4;++slot) for (const auto& texture:path_.model.textures) if (texture.chunkId==ids[slot]) {
                mesh.textureIndices[slot]=int(texture.srvHeapIndex);
                if (slot==0) mesh.textureIndex=mesh.textureIndices[0];
                break;
            }
        }
        UpdateResidencySmoke();
        stagedMetadata_->stats.drawCallCount=int(displayedChunks_.load());
        message->ok=descriptorsReady; message->terminal=true;
        if (!descriptorsReady) {
            message->errorCode=model_core::ImportErrorCode::UploadFailure;
            message->errorDetails=descriptorError;
        }
        MergeMissingAssets(*stagedMetadata_, pub.task.result.missingAssets);
        RebuildModelWarning(*stagedMetadata_);
        message->metadata=std::make_shared<const ModelData>(*stagedMetadata_);
    } else {
        auto& metadata = *stagedMetadata_;
        for (const auto& imported:pub.task.result.meshes) {
            if (imported.geometry.lodLevel==model_core::kScanLod) scanCatalog_.emplace(imported.chunkId & ~model_core::kScanIdentity,imported.geometry);
            if (imported.geometry.lodLevel>=model_core::kScanLod) stagedProxyMode_=true;
            if (imported.geometry.lodLevel==model_core::kPreviewLod) stagedPreviewOnly_=true;
            if (imported.geometry.lodLevel==model_core::kScanLod && stagedPreviewOnly_) {
                stagedPreviewOnly_=false; stagedHaveBounds_=false;
                metadata.vertexCount=metadata.triangleCount=metadata.pointCount=0;
            }
        }
        if (pub.task.result.coarseComplete) { stagedProxyMode_=true; stagedProxyComplete_=true; }
        metadata.importStatus.flags = (metadata.importStatus.flags & model_core::kStatusRefining)
            | pub.task.result.status.flags | model_core::kStatusProvisional;
        metadata.importStatus.optionalFeatureWarnings = std::max(metadata.importStatus.optionalFeatureWarnings, pub.task.result.status.optionalFeatureWarnings);
        metadata.source = pub.task.result.scene;
        metadata.sourceIdentity = pub.task.result.sourceIdentity;
        metadata.stats.nodeCount = int(metadata.source.nodeCount);
        metadata.stats.meshCount = int(metadata.source.meshCount);
        metadata.stats.animationCount = int(metadata.source.animationCount);
        metadata.stats.skinCount = int(metadata.source.skinCount);
        metadata.stats.boneCount = int(metadata.source.boneCount);
        metadata.sourceUpAxis = metadata.source.upAxis == model_core::UpAxisId::X ? SourceUpAxis::X
            : metadata.source.upAxis == model_core::UpAxisId::Y ? SourceUpAxis::Y
            : metadata.source.upAxis == model_core::UpAxisId::Z ? SourceUpAxis::Z : SourceUpAxis::Unknown;
        DirectX::XMStoreFloat4x4(&metadata.upAxisCorrection,
            GroundAxisTransform(GroundAxis::Automatic, metadata.source.upAxis, false));
        std::unordered_set<uint32_t> instancedGeometry;
        for (const auto& instance:pub.task.result.instances)
            instancedGeometry.insert(instance.data.geometryChunkId);
        if (!haveSceneOrigin_ && !pub.task.result.instances.empty()) {
            std::memcpy(metadata.sceneOrigin,pub.task.result.instances.front().data.worldMin,sizeof(metadata.sceneOrigin));
            haveSceneOrigin_=true;
        }
        for (const auto& imported : pub.task.result.meshes) {
            const auto& geometry = imported.geometry;
            if (stagedProxyMode_ && geometry.lodLevel!=model_core::kScanLod
                && !(stagedPreviewOnly_ && geometry.lodLevel==model_core::kPreviewLod)) continue;
            const bool standaloneGeometry = !instancedGeometry.contains(imported.chunkId)
                && (geometry.geometryFlags & model_core::kGeometryReusableInstanceSource) == 0;
            if (standaloneGeometry && !haveSceneOrigin_) {
                std::memcpy(metadata.sceneOrigin, geometry.origin, sizeof(metadata.sceneOrigin)); haveSceneOrigin_ = true;
            }
            if (standaloneGeometry) {
            double minimum[3], maximum[3];
            for (unsigned axis=0; axis<3; ++axis) {
                // Subtract origins before adding local extrema; tiny residuals
                // remain intact even when an absolute double sum would round.
                const double offset = geometry.origin[axis] - metadata.sceneOrigin[axis];
                minimum[axis] = offset + double(geometry.localMin[axis]);
                maximum[axis] = offset + double(geometry.localMax[axis]);
            }
            if (!stagedHaveBounds_) {
                std::memcpy(metadata.relativeMin, minimum, sizeof(minimum));
                std::memcpy(metadata.relativeMax, maximum, sizeof(maximum)); stagedHaveBounds_ = true;
            } else {
                for (unsigned axis=0; axis<3; ++axis) {
                    metadata.relativeMin[axis] = std::min(metadata.relativeMin[axis],minimum[axis]);
                    metadata.relativeMax[axis] = std::max(metadata.relativeMax[axis],maximum[axis]);
                }
            }
            metadata.boundsMin = {float(metadata.relativeMin[0]),float(metadata.relativeMin[1]),float(metadata.relativeMin[2])};
            metadata.boundsMax = {float(metadata.relativeMax[0]),float(metadata.relativeMax[1]),float(metadata.relativeMax[2])};
            }
            metadata.vertexCount += imported.vertexCount;
            metadata.triangleCount += imported.topology == model_core::ChunkTopology::TriangleList ? imported.indexCount/3 : 0;
            metadata.pointCount += imported.topology == model_core::ChunkTopology::PointList ? imported.vertexCount : 0;
            metadata.stats.hasUv0 |= (geometry.geometryFlags & model_core::kGeometryHasUv0) != 0;
            metadata.stats.hasUv1 |= (geometry.geometryFlags & model_core::kGeometryHasUv1) != 0;
            metadata.stats.hasVertexColors |= (geometry.geometryFlags & model_core::kGeometryHasColors) != 0;
        }
        for (const auto& imported:pub.task.result.instances) {
            if (!imported.resolvedVisible) continue;
            double minimum[3],maximum[3];
            for(unsigned axis=0;axis<3;++axis){minimum[axis]=imported.data.worldMin[axis]-metadata.sceneOrigin[axis];
                maximum[axis]=imported.data.worldMax[axis]-metadata.sceneOrigin[axis];}
            if(!stagedHaveBounds_){std::memcpy(metadata.relativeMin,minimum,sizeof(minimum));std::memcpy(metadata.relativeMax,maximum,sizeof(maximum));stagedHaveBounds_=true;}
            else for(unsigned axis=0;axis<3;++axis){metadata.relativeMin[axis]=std::min(metadata.relativeMin[axis],minimum[axis]);
                metadata.relativeMax[axis]=std::max(metadata.relativeMax[axis],maximum[axis]);}
            metadata.boundsMin={float(metadata.relativeMin[0]),float(metadata.relativeMin[1]),float(metadata.relativeMin[2])};
            metadata.boundsMax={float(metadata.relativeMax[0]),float(metadata.relativeMax[1]),float(metadata.relativeMax[2])};
        }
        for (const auto& mat : pub.task.result.materials) {
            materials_.emplace(mat.chunkId, mat);
            ++metadata.stats.materialCount;
            metadata.stats.albedoTextureCount += mat.baseColorImageChunkId != 0;
            metadata.stats.normalTextureCount += mat.normalImageChunkId != 0;
            metadata.stats.specularMetallicTextureCount += mat.metallicRoughnessImageChunkId != 0;
            metadata.stats.emissiveTextureCount += mat.emissiveImageChunkId != 0;
            metadata.stats.hasConstantBaseColor |= mat.baseColorImageChunkId == 0;
            metadata.stats.hasConstantEmissiveColor |= mat.data.emissiveFactor[0] != 0 || mat.data.emissiveFactor[1] != 0 || mat.data.emissiveFactor[2] != 0;
            metadata.stats.hasTransparency |= mat.data.alphaMode != uint32_t(model_core::AlphaModeId::Opaque) || mat.data.baseColorFactor[3] < 1;
        }
        auto& destination = modelGeneration_ == pub.task.generation ? path_.model : stagedScene_;
        destination.coarseAllocationBytes+=pub.resources.coarseAllocationBytes;
        destination.meshes.insert(destination.meshes.end(), std::make_move_iterator(pub.resources.meshes.begin()),
            std::make_move_iterator(pub.resources.meshes.end()));
        for(const auto& instance:pub.task.result.instances){
            if(std::any_of(destination.meshes.begin(),destination.meshes.end(),[&](const auto& draw){return draw.instanceId==instance.data.instanceId;}))continue;
            auto geometry=std::find_if(destination.meshes.begin(),destination.meshes.end(),[&](const auto& draw){return draw.chunkId==instance.data.geometryChunkId;});
            if(geometry==destination.meshes.end()){stagedFailed_=true;message->errorCode=model_core::ImportErrorCode::UploadFailure;
                message->errorDetails=L"An instance's shared geometry was not available after upload.";continue;}
            auto draw=*geometry;draw.instanceId=instance.data.instanceId;draw.sourceNodeId=instance.data.nodeId;
            draw.materialChunkId=instance.data.materialChunkId;draw.drawEnabled=instance.resolvedVisible;
            std::memcpy(draw.instanceTransform,instance.worldTransform,sizeof(draw.instanceTransform));
            std::memcpy(draw.instanceBoundsMin,instance.data.worldMin,sizeof(draw.instanceBoundsMin));
            std::memcpy(draw.instanceBoundsMax,instance.data.worldMax,sizeof(draw.instanceBoundsMax));
            draw.mirrored=instance.mirrored;destination.meshes.push_back(std::move(draw));
        }
        if (destination.neutralTextures.empty())
            destination.neutralTextures=std::move(pub.resources.neutralTextures);
        if (stagedProxyComplete_ && !destination.coarseComplete) path_.RetirePreviewChunks(destination);
        destination.coarseComplete=stagedProxyComplete_;
        D3D12ViewerPath::UpdateCoarseVisibility(destination);
        if (stagedProxyComplete_) {
            metadata.boundsVerified=true;
            metadata.importStatus.flags=model_core::kStatusRefining;
            pendingCoarseGeneration_.store(pub.task.generation, std::memory_order_release);
            pendingVerifiedGeneration_.store(pub.task.generation, std::memory_order_release);
        }
        if (stagedProxyMode_ && !stagedPreviewOnly_) scannedPrimitives_.store(metadata.triangleCount+metadata.pointCount);
        metadata.importStatus.textureWarnings = std::max(metadata.importStatus.textureWarnings,
            std::max(pub.task.result.textureWarningCount, pub.task.result.status.textureWarnings));
        MergeMissingAssets(metadata, pub.task.result.missingAssets);
        RebuildModelWarning(metadata);
        D3D12ViewerPath::ModelResources displaced;
        for (auto& texture : pub.resources.textures) {
            auto old=std::find_if(destination.textures.begin(),destination.textures.end(),
                [&](const auto& candidate) { return candidate.chunkId==texture.chunkId; });
            if (old==destination.textures.end()) destination.textures.push_back(std::move(texture));
            else {
                if (old->resource->GetDesc().Width<=64 && std::none_of(destination.fallbackTextures.begin(),destination.fallbackTextures.end(),[&](const auto& low) { return low.chunkId==old->chunkId; }))
                    destination.fallbackTextures.push_back(*old);
                displaced.textures.push_back(std::move(*old));*old=std::move(texture);
            }
        }
        std::wstring descriptorError;
        const bool descriptorsReady=path_.RebuildMaterialDescriptors(destination,descriptorError);
        if (!descriptorsReady) {
            stagedFailed_=true;
            message->errorCode=model_core::ImportErrorCode::UploadFailure;
            message->errorDetails=descriptorError;
        }
        if (!displaced.textures.empty()) {
            uint64_t fence=0;for (const auto& frame:path_.frames) fence=std::max(fence,frame.fenceValue);
            path_.retiredModels.push_back({std::move(displaced),fence,0});
        }
        uint64_t extent=0,mips=0;
        for (const auto& texture:destination.textures) {
            const auto desc=texture.resource->GetDesc();extent=std::max(extent,desc.Width);mips=std::max(mips,uint64_t(desc.MipLevels));
        }
        textureExtent_.store(extent);textureCount_.store(destination.textures.size());textureMips_.store(mips);
        for (auto& mesh : destination.meshes) {
            auto mat = materials_.find(mesh.materialChunkId);
            if (mat == materials_.end()) continue; // Neutral, immutable until a dependency arrives.
            mesh.material=mat->second.data;mesh.hasMaterial=true;
            const uint32_t ids[4]{mat->second.baseColorImageChunkId,mat->second.metallicRoughnessImageChunkId,
                                  mat->second.normalImageChunkId,mat->second.emissiveImageChunkId};
            for (UINT slot=0;slot<4;++slot) for (const auto& texture:destination.textures) {
                if (texture.chunkId!=ids[slot]) continue;
                mesh.textureIndices[slot]=static_cast<int>(texture.srvHeapIndex);
                if (slot==0) mesh.textureIndex=mesh.textureIndices[0];
                break;
            }
        }
        const bool hasVisibleDraw = std::any_of(destination.meshes.begin(), destination.meshes.end(),
            [](const auto& draw) { return draw.drawEnabled; });
        if (descriptorsReady && stagedHaveBounds_ && hasVisibleDraw
            && (!stagedProxyMode_ || stagedProxyComplete_ || !path_.hasModel)) {
            if (modelGeneration_ != pub.task.generation) {
                uint64_t fence = 0;
                for (const auto& frame : path_.frames) fence = std::max(fence,frame.fenceValue);
                if (path_.hasModel) path_.retiredModels.push_back({std::move(path_.model),fence,0});
                path_.model = std::move(stagedScene_); stagedScene_ = {};
                modelGeneration_ = pub.task.generation;
                std::memcpy(path_.sceneOrigin, metadata.sceneOrigin, sizeof(path_.sceneOrigin));
                path_.sourceUpAxis = metadata.source.upAxis;
                path_.modelBoundsMin = metadata.boundsMin;
                path_.modelBoundsMax = metadata.boundsMax;
                path_.haveModelBounds = true;
                std::lock_guard<std::mutex> cameraLock(cameraMutex_);
                framingEpoch_ = interactionEpoch_.load();
                DirectX::XMFLOAT3 minimum, maximum;
                TransformBounds(metadata.boundsMin, metadata.boundsMax,
                    GroundAxisTransform(frameOverlay_->info.groundAxis, metadata.source.upAxis,
                        frameOverlay_->info.showNativeOrientation, frameOverlay_->info.groundAxisInverted), minimum, maximum);
                camera_.SetBounds(minimum, maximum, viewportAspect_);
            }
            path_.hasModel = true; hasModel_.store(true, std::memory_order_release);
            if (stagedProxyComplete_) coarseCompleteGeneration_.store(pub.task.generation);
            UpdateResidencySmoke();
            displayedChunks_.store(std::count_if(path_.model.meshes.begin(),path_.model.meshes.end(),[](const auto& mesh) { return mesh.drawEnabled; }), std::memory_order_release);
            texturedChunks_.store(std::count_if(path_.model.meshes.begin(), path_.model.meshes.end(),
                [](const auto& mesh) { return mesh.drawEnabled && mesh.textureIndex >= 0 && mesh.textureHeap; }), std::memory_order_release);
        }
        metadata.stats.drawCallCount = int(displayedChunks_.load());
        if (stagedHaveBounds_ && modelGeneration_ == pub.task.generation) {
            // A progressive scene can establish its first instance origin after
            // geometry has uploaded. Keep drawing in the same relative space as
            // the bounds used for the camera and grid.
            std::memcpy(path_.sceneOrigin, metadata.sceneOrigin, sizeof(path_.sceneOrigin));
            path_.modelBoundsMin = metadata.boundsMin;
            path_.modelBoundsMax = metadata.boundsMax;
            path_.haveModelBounds = true;
            std::lock_guard<std::mutex> cameraLock(cameraMutex_);
            DirectX::XMFLOAT3 minimum, maximum;
            TransformBounds(metadata.boundsMin, metadata.boundsMax,
                GroundAxisTransform(frameOverlay_->info.groundAxis, metadata.source.upAxis,
                    frameOverlay_->info.showNativeOrientation, frameOverlay_->info.groundAxisInverted), minimum, maximum);
            Camera framed = camera_; framed.SetBounds(minimum, maximum, viewportAspect_);
            if (interactionEpoch_.load() == framingEpoch_) camera_ = framed;
            else {
                camera_.homeX = framed.homeX; camera_.homeY = framed.homeY; camera_.homeZ = framed.homeZ;
                camera_.homeDistance = framed.homeDistance; camera_.homeBoundsMin = minimum; camera_.homeBoundsMax = maximum;
                camera_.homeOrientation = framed.homeOrientation; camera_.sceneRadius = framed.sceneRadius;
            }
        }
        message->metadata = std::make_shared<const ModelData>(metadata);
        message->ok = descriptorsReady;
        if (modelGeneration_ != pub.task.generation) return;
    }
    // Align cancel/recovery UI with the representation already accepted by the
    // render thread, even when its posted UI notification is still queued.
    if (stagedProxyComplete_) uploads_->coarseGeneration=pub.task.generation;
    uploads_->gpuBaseBytes=path_.AccountedAllocationBytes(&stagedScene_);
    if (message->ok && message->metadata)
        displaySnapshot_.store(std::make_shared<const RenderDisplaySnapshot>(RenderDisplaySnapshot{message->path,message->metadata}));
    if (message->ok && message->refinement)
        pendingRefinementGeneration_.store(message->generation, std::memory_order_release);
    if (!message->ok) message->errorSummary = message->errorCode == model_core::ImportErrorCode::OutOfMemory
        ? L"There is not enough memory to display this model."
        : message->errorCode == model_core::ImportErrorCode::EmptyGeometry ? L"This model has no displayable geometry."
        : L"The model was read but could not be displayed.";
    if (PostMessageW(window, kRenderUploadCompleteMessage, 0, reinterpret_cast<LPARAM>(message.get())))
        (void)message.release();
    invalidated_.store(true, std::memory_order_release);
}

void RenderThread::UpdateResidencySmoke()
{
    uint64_t coarse=0,fine=0,suppressed=0,preview=0;
    for (const auto& mesh:path_.model.meshes) {
        if (mesh.sourceGeometry.lodLevel==model_core::kCoarseLod) { ++coarse; suppressed+=!mesh.drawEnabled; }
        else if (mesh.drawEnabled) { if (mesh.sourceGeometry.lodLevel==model_core::kPreviewLod) ++preview; else ++fine; }
    }
    coarseChunks_.store(coarse); fineChunks_.store(fine); suppressedCoarse_.store(suppressed);
    coarseAllocationBytes_.store(path_.model.coarseAllocationBytes);
    displayedChunks_.store(coarse-suppressed+fine+preview);
    uint64_t extent=0,mips=0;
    for (const auto& texture:path_.model.textures) {
        const auto desc=texture.resource->GetDesc();
        extent=std::max(extent,desc.Width); mips=std::max(mips,uint64_t(desc.MipLevels));
    }
    textureExtent_.store(extent); textureCount_.store(path_.model.textures.size()); textureMips_.store(mips);
}

void RenderThread::RenderOneFrame()
{
    AssertOnRenderThread();

    double cameraTarget[3]{};
    DirectX::XMFLOAT4 eyeSelection;
    DirectX::XMFLOAT4X4 viewProjection{};
    DirectX::XMFLOAT4 orientation{};
    std::shared_ptr<const OverlayFrame> overlay;
    {
        // Ticking and composing under the lock, then rendering without it:
        // holding it across BeginFrame's fence and frame-latency waits would
        // block UI input for the length of a GPU frame.
        std::lock_guard<std::mutex> lock(cameraMutex_);
        const double now = NowSeconds();
        double elapsed = 0.0;
        if (lastFrameSeconds_ > 0.0) {
            const double gap = now - lastFrameSeconds_;
            if (gap < 0.25) elapsed = std::min(0.1, gap);
        }
        lastFrameSeconds_ = now;
        camera_.SetInput(flightInput_);
        if (flightInput_.right || flightInput_.up || flightInput_.forward || flightInput_.roll
            || flightInput_.orbitX || flightInput_.orbitY || flightInput_.panX || flightInput_.panY) ++interactionEpoch_;
        camera_.Update(elapsed);
        DirectX::XMStoreFloat4(&orientation, camera_.Orientation());
        overlay = frameOverlay_;
        cameraTarget[0] = camera_.targetX; cameraTarget[1] = camera_.targetY; cameraTarget[2] = camera_.targetZ;
        Camera relative = camera_; relative.targetX = relative.targetY = relative.targetZ = 0;
        DirectX::XMStoreFloat4(&eyeSelection,relative.EyePosition()); eyeSelection.w = overlay->info.selectionAmount;
        DirectX::XMStoreFloat4x4(&viewProjection,
                                  relative.ViewMatrix() * camera_.ProjectionMatrix(viewportAspect_));
    }

    DirectX::XMFLOAT4X4 modelTransform{};
    DirectX::XMStoreFloat4x4(&modelTransform,
        GroundAxisTransform(overlay->info.groundAxis, path_.sourceUpAxis, overlay->info.showNativeOrientation,
            overlay->info.groundAxisInverted));
    const bool visibleDetailPending = RequestVisibleDetail(viewProjection, cameraTarget, modelTransform);
    const auto framesBefore = path_.frameStats.PresentedFrames();
    path_.lastPresentResult = E_PENDING;
    if (hasModel_.load(std::memory_order_acquire)) {
        path_.RenderFrame(viewProjection, orientation, *overlay, cameraTarget, eyeSelection);
    } else {
        path_.RenderClearFrame(orientation, *overlay);
    }
    if (injectDeviceRemoval_.exchange(false)) path_.lastPresentResult = DXGI_ERROR_DEVICE_REMOVED;
    if (IsDeviceLoss(path_.lastPresentResult)) {
        deviceFatal_ = !RecoverDevice();
        return;
    }
    if (debugInfo_) {
        const auto count = debugInfo_->GetNumStoredMessagesAllowedByRetrievalFilter();
        if (copyDelayMs_ && count && !debugErrors_.load()) {
            // Bounded diagnostics for the explicit app smoke lane only.
            size_t length = 0; debugInfo_->GetMessage(0,nullptr,&length);
            if (length <= 4096) {
                std::vector<std::byte> storage(length);
                auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(debugInfo_->GetMessage(0,message,&length)))
                    std::fprintf(stderr,"D3D12 smoke error %u: %.1000s\n",unsigned(message->ID),message->pDescription);
            }
        }
        debugErrors_.store(count);
    }

    if (path_.frameStats.PresentedFrames() > framesBefore) {
        if (benchDurationMs_) {
            benchmarkFrameStats_.RecordPresent(benchmarkForceOccluded_ ? DXGI_STATUS_OCCLUDED : path_.lastPresentResult);
            benchmarkPresentedFrames_.fetch_add(1, std::memory_order_release);
        }
    }
    if (path_.frameStats.PresentedFrames() > framesBefore && path_.lastPresentResult == S_OK) {
        const auto nowUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (!hasModel_.load(std::memory_order_acquire) && firstBackgroundUs_.load() == 0)
            firstBackgroundUs_.store(nowUs, std::memory_order_release);
        if (modelGeneration_ != 0 && presentedGeneration_.load() != modelGeneration_) {
            geometryUs_.store(nowUs, std::memory_order_release);
            presentedGeneration_.store(modelGeneration_, std::memory_order_release);
        }
        if (!visibleDetailPending
            && completeModelAwaitingPresentGeneration_.load(std::memory_order_acquire) == modelGeneration_) {
            completeModelPresentedUs_.store(nowUs, std::memory_order_release);
            completeModelPresentedGeneration_.store(modelGeneration_, std::memory_order_release);
            completeModelAwaitingPresentGeneration_.store(0, std::memory_order_release);
        }
        const auto loading = loadingGeneration_.load(std::memory_order_acquire);
        if (loading && loadingUiUs_.load(std::memory_order_acquire) == 0)
            loadingUiUs_.store(nowUs, std::memory_order_release);
        if (modelGeneration_ != 0 && pendingCoarseGeneration_.load(std::memory_order_acquire) == modelGeneration_
            && coarseUs_.load(std::memory_order_acquire) == 0)
            coarseUs_.store(nowUs, std::memory_order_release);
        if (modelGeneration_ != 0 && pendingVerifiedGeneration_.load(std::memory_order_acquire) == modelGeneration_
            && verifiedBoundsUs_.load(std::memory_order_acquire) == 0)
            verifiedBoundsUs_.store(nowUs, std::memory_order_release);
        if (modelGeneration_ != 0 && pendingRefinementGeneration_.load(std::memory_order_acquire) == modelGeneration_
            && refinementUs_.load(std::memory_order_acquire) == 0)
            refinementUs_.store(nowUs, std::memory_order_release);
        const auto inputUs = benchmarkInputUs_.exchange(0, std::memory_order_acq_rel);
        if (inputUs && nowUs >= inputUs)
            benchmarkInputToPresentUs_.store(nowUs - inputUs, std::memory_order_release);
    }

    // Republish only occasionally, never every frame. FrameStats::P95Ms
    // allocates a vector and sorts the whole window; doing that inside the
    // frame path cost roughly 8 ms of p95 when it was measured -- an
    // allocation and a sort per frame is exactly the kind of thing that
    // shows up as an occasional missed vsync rather than as a slower mean.
    // Qualification keeps the entire bounded ring and publishes only once at
    // completion; sorting it every 30 frames would contaminate its own p95.
    if (benchDurationMs_) return;
    static constexpr std::uint64_t kStatsPublishInterval = 30;
    if (path_.frameStats.PresentedFrames() % kStatsPublishInterval != 0) return;
    PublishStats();
}

bool RenderThread::RecoverDevice()
{
    AssertOnRenderThread();
    auto notice = std::make_unique<RenderDeviceRecoveryResult>();
    if (const auto snapshot = DisplaySnapshot()) notice->path = snapshot->path;
    if (recoveryAttempted_) {
        notice->details = L"The graphics device failed again after one recovery attempt.";
        PostMessageW(window_, kRenderDeviceRecoveryMessage, 0, reinterpret_cast<LPARAM>(notice.release()));
        return false;
    }
    recoveryAttempted_ = true;
    ++recoveryCount_;
    const HRESULT removalReason = path_.device.Device()
        ? path_.device.Device()->GetDeviceRemovedReason() : E_POINTER;
    wchar_t diagnostic[160]{};
    swprintf_s(diagnostic,
               L"Preview3D device recovery: present/device HRESULT=0x%08X, removed reason=0x%08X\n",
               unsigned(path_.lastPresentResult), unsigned(removalReason));
    wchar_t optIn[2]{};
    if (GetEnvironmentVariableW(L"PREVIEW3D_DEVICE_DIAGNOSTICS", optIn, 2) == 1 && optIn[0] == L'1')
        OutputDebugStringW(diagnostic);

    CancelUploads();
    {
        std::lock_guard lock(uploads_->mutex);
        uploads_->stopped = true;
        uploads_->changed.notify_all();
    }
    if (uploadThread_.joinable()) uploadThread_.join();

    hasModel_.store(false, std::memory_order_release);
    displaySnapshot_.store({});
    debugInfo_.Reset();
    path_.overlay.Shutdown();
    path_.~D3D12ViewerPath();
    new (&path_) D3D12ViewerPath();
    budgetMonitor_.~DxgiBudgetMonitor();
    new (&budgetMonitor_) DxgiBudgetMonitor();
    {
        std::lock_guard lock(uploads_->mutex);
        uploads_->stopped = false;
        uploads_->generation = 0;
        uploads_->cancellation.reset();
    }

    std::wstring error;
    if (!InitializeOnThread(window_, error)) {
        notice->details = L"The graphics device could not be rebuilt once: " + error;
        PostMessageW(window_, kRenderDeviceRecoveryMessage, 0, reinterpret_cast<LPARAM>(notice.release()));
        return false;
    }
    uploadThread_ = std::thread(&RenderThread::UploadMain, this);
    if (benchDurationMs_)
        benchDeadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(benchDurationMs_);
    notice->recovered = true;
    notice->details = L"The graphics device was rebuilt; the model will be reconstructed from its retained source. ";
    notice->details += diagnostic;
    PostMessageW(window_, kRenderDeviceRecoveryMessage, 0, reinterpret_cast<LPARAM>(notice.release()));
    return true;
}

void RenderThread::PublishStats(bool includeRaw)
{
    const double overlayMean
        = path_.overlayPasses > 0 ? path_.overlayTotalMs / static_cast<double>(path_.overlayPasses) : 0.0;
    const FrameStats& source = benchDurationMs_ ? benchmarkFrameStats_ : path_.frameStats;
    StatsSnapshot snapshot;
    snapshot.meanMs = source.MeanMs();
    snapshot.medianMs = source.MedianMs();
    snapshot.p95Ms = source.P95Ms();
    snapshot.maxMs = source.MaxMs();
    snapshot.overlayMeanMs = overlayMean;
    snapshot.frames = source.PresentedFrames();
    snapshot.occluded = source.OccludedPresents();
    snapshot.failed = source.FailedPresents();
    if (includeRaw) snapshot.rawIntervalsMs = source.RawIntervalsMs();
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_ = snapshot;
        if (includeRaw) benchmarkStats_ = snapshot;
    }
}


void RenderThread::UpdateBudget()
{
    // Every tick observes a notification/drop, including idle presentation.
    budgetMonitor_.HasBudgetChangeSignaled();
    uint64_t target=std::min(budgetMonitor_.ComputeDetailTargetBytes(),smokeBudgetBytes_.load());
    if (isUma_ || smokeUma_.load()) {
        // UMA destinations consume the CPU allowance too. Charge the upload ring,
        // host queue and a bounded worker allowance before admitting destinations.
        const uint64_t cpuReserve=512ull*1024*1024;
        target=std::min(target,cpuPolicyCap_>cpuReserve ? cpuPolicyCap_-cpuReserve : 0);
    }
    std::lock_guard<std::mutex> lock(uploads_->mutex);
    uploads_->simulateUma=smokeUma_.load();
    uint64_t base=path_.AccountedAllocationBytes(&stagedScene_);
    if (base+uploads_->gpuPendingBytes+uploads_->mandatoryBytes>target && path_.model.coarseComplete) {
        path_.ShedTextureDetail();
        std::vector<ReadyResourceInfo> ready;
        uint64_t fineBytes=0;
        for (const auto& mesh:path_.model.meshes) if (mesh.sourceGeometry.lodLevel==model_core::kFineLod) {
            const uint64_t bytes=mesh.vertexAllocationBytes+mesh.indexAllocationBytes; fineBytes+=bytes;
            ready.push_back({mesh.vertexBuffer.Get(),modelGeneration_,mesh.chunkId,model_core::kFineLod,bytes,mesh.lastVisibleFrame,mesh.viewPriority});
        }
        const uint64_t reserved=base-fineBytes+uploads_->gpuPendingBytes+uploads_->mandatoryBytes;
        const auto plan=PlanEviction(SceneSnapshot(std::move(ready)),reserved<target ? target-reserved : 0);
        std::vector<uint32_t> identities;
        for (const auto& entry:plan) identities.push_back(entry.clusterId);
        path_.EvictFineChunks(identities); evictionCount_.fetch_add(identities.size());
        UpdateResidencySmoke();
        // Retirement is not free capacity. Both fences must finish first.
        base=path_.AccountedAllocationBytes(&stagedScene_);
        for (auto id:uploads_->details) uploads_->requestedDetails.erase(id);
        uploads_->details.clear();
        // Drop queued ids while retaining the single worker-owned request.
        // Recomputed view requests below never grow beyond 32 entries.
    }
    if (path_.model.coarseComplete && base>target && !uploads_->gpuPendingBytes && path_.retiredModels.empty()
        && !fineChunks_.load() && failedBudgetGeneration_!=modelGeneration_) {
        failedBudgetGeneration_=modelGeneration_;
        auto message=std::make_unique<RenderUploadResult>(); message->generation=modelGeneration_;
        message->errorCode=model_core::ImportErrorCode::OutOfMemory;
        message->errorSummary=L"There is not enough GPU memory to refine this model.";
        message->errorDetails=L"The complete preview and viewer resources exceed the current memory budget. Close other applications, then retry.";
        if (auto snapshot=displaySnapshot_.load()) message->path=snapshot->path;
        if (PostMessageW(window_,kRenderUploadCompleteMessage,0,reinterpret_cast<LPARAM>(message.get()))) message.release();
    }
    if (initialTerminal_ && path_.model.coarseComplete && scanCatalog_.size()>fineChunks_.load()) invalidated_.store(true);
    uploads_->gpuBaseBytes=base; uploads_->gpuTargetBytes=target;
    uploads_->changed.notify_all();
    accountedGpuBytes_.store(base+uploads_->gpuPendingBytes); targetGpuBytes_.store(target); pendingGpuBytes_.store(uploads_->gpuPendingBytes);
}

bool RenderThread::RequestVisibleDetail(const DirectX::XMFLOAT4X4& vp, const double target[3],
    const DirectX::XMFLOAT4X4& modelTransform)
{
    if (!initialTerminal_) return true;
    // Ordinary models have no scan catalog and are already at their final
    // representation when the terminal marker is accepted.
    if (scanCatalog_.empty()) return false;
    if (!path_.model.coarseComplete || NowSeconds() < pauseDetailUntil_) return true;
    std::lock_guard<std::mutex> lock(uploads_->mutex);
    if (uploads_->generation != modelGeneration_) return true;
    const auto aligned=[](uint64_t bytes) { return (bytes+65535)/65536*65536; };
    const auto cost=[&](const model_core::ChunkDescriptor& d) {
        return aligned(uint64_t(d.vertexCount)*model_core::VertexStrideForLayout(model_core::VertexLayoutId(d.vertexLayoutId)))+aligned(uint64_t(d.indexCount)*4);
    };
    // Replace queued view requests at every camera epoch. Only the one already
    // removed by the broker may continue; generation/copy filters guard it.
    for (auto id:uploads_->details) uploads_->requestedDetails.erase(id);
    uploads_->details.clear();
    std::unordered_set<uint32_t> resident;
    for (auto& mesh:path_.model.meshes) if (mesh.sourceGeometry.lodLevel==model_core::kFineLod) {
        resident.insert(mesh.chunkId);
        mesh.viewPriority=DetailViewPriority(mesh.sourceGeometry,path_.sceneOrigin,target,modelTransform,vp);
    }
    struct Candidate { uint32_t id; float priority; uint64_t bytes; };
    std::vector<Candidate> candidates;
    for (const auto& [id,d]:scanCatalog_) {
        if (resident.contains(id) || uploads_->requestedDetails.contains(id)) continue;
        const float priority=DetailViewPriority(d,path_.sceneOrigin,target,modelTransform,vp);
        if (!priority) continue;
        candidates.push_back({id,priority,cost(d)});
        std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b) { return a.priority!=b.priority ? a.priority>b.priority : a.id<b.id; });
        if (candidates.size()>32) candidates.pop_back();
    }
    uint64_t used=uploads_->gpuBaseBytes+uploads_->gpuPendingBytes;
    for (auto id:uploads_->requestedDetails) if (scanCatalog_.contains(id)) used+=cost(scanCatalog_.at(id));
    bool room=false;
    for (const auto& candidate:candidates) {
        if (used>=uploads_->gpuTargetBytes || candidate.bytes>uploads_->gpuTargetBytes-used) continue;
        // The broker has one reusable section and services one detail request
        // synchronously. Queueing 32 identities here cannot increase decode
        // concurrency; it only leaves stale work ahead of a budget drop or
        // camera reprioritization. Keep one admitted request at a time so
        // pressure recovery can select the current highest-priority region.
        if (uploads_->requestedDetails.size()==1) break;
        uploads_->details.push_back(candidate.id); uploads_->requestedDetails.insert(candidate.id);
        used+=candidate.bytes; room=true;
    }
    bool evictedForDetail = false;
    if (!room && !candidates.empty() && uploads_->requestedDetails.empty()) {
        std::vector<uint32_t> invisible;
        for (const auto& mesh:path_.model.meshes)
            if (mesh.sourceGeometry.lodLevel==model_core::kFineLod && mesh.viewPriority==0) invisible.push_back(mesh.chunkId);
        if (!invisible.empty()) {
            path_.EvictFineChunks(invisible); evictionCount_.fetch_add(invisible.size()); UpdateResidencySmoke();
            evictedForDetail = true;
        }
    }
    // requestedDetails covers queued, broker-owned, uploading and published
    // detail until PumpUploads has incorporated it. If nothing is outstanding
    // and no eviction can make room, the current on-screen representation is
    // the most complete one this view and memory budget can produce.
    const bool waitingForRetiredCapacity = !candidates.empty() && !path_.retiredModels.empty();
    return !uploads_->requestedDetails.empty() || evictedForDetail || waitingForRetiredCapacity;
}

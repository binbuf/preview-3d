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
#include <cstring>

namespace {

// Bounded everywhere: `04-rendering-and-streaming.md:190` requires shutdown
// to "wait with finite diagnostics timeouts", and "a driver hang must not
// leave the UI thread waiting forever."
constexpr DWORD kStartTimeoutMs = 10000;
constexpr DWORD kStopTimeoutMs = 5000;
// The idle wait. `04-...:38`: "A timer keeps animation/loading indicators
// alive; there is no unconstrained busy loop."
constexpr DWORD kIdleWaitMs = 100;

double NowSeconds()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

} // namespace

RenderThread::~RenderThread()
{
    Stop();
}

std::uint64_t RenderThread::SmokeValue(unsigned field) const noexcept
{
    switch (field) {
    case 2: return firstBackgroundUs_.load(std::memory_order_acquire);
    case 3: return geometryUs_.load(std::memory_order_acquire);
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

bool RenderThread::Start(HWND window, std::wstring& error)
{
    wakeEvent_ = platform::Win32Handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    startedEvent_ = platform::Win32Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!wakeEvent_ || !startedEvent_) {
        error = L"The render thread's events could not be created.";
        return false;
    }

    thread_ = std::thread(&RenderThread::ThreadMain, this, window);

    // The one deliberately synchronous handshake. It happens in WM_CREATE,
    // before there is any interaction to be responsive to, and the caller
    // has to know whether graphics started at all. Bounded so a wedged
    // driver cannot hang startup forever.
    if (WaitForSingleObject(startedEvent_.get(), kStartTimeoutMs) != WAIT_OBJECT_0) {
        error = L"The render thread did not start within its timeout.";
        stopRequested_.store(true, std::memory_order_release);
        SetEvent(wakeEvent_.get());
        if (thread_.joinable()) thread_.join();
        return false;
    }

    if (!startOk_) {
        error = startError_;
        if (thread_.joinable()) thread_.join();
        return false;
    }
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
    uploads_->generation = 0;
    for (const auto& task : uploads_->tasks) if (!task.terminal) { uploads_->bytes -= task.bytes; --uploads_->count; }
    for (const auto& pub : uploads_->publications) if (!pub.task.terminal) { uploads_->bytes -= pub.task.bytes; --uploads_->count; }
    uploads_->tasks.clear();
    uploads_->publications.clear(); // These resources have already completed the copy fence.
    uploads_->changed.notify_all();
}

std::function<void(d3d12_import_bridge::ImportResult)> RenderThread::BeginImport(
    std::uint64_t generation, std::wstring path, std::shared_ptr<std::atomic_bool> cancellation)
{
    CancelUploads();
    auto inbox = uploads_;
    { std::lock_guard<std::mutex> lock(inbox->mutex); inbox->generation = generation; inbox->path = path; }
    return [inbox, generation, path = std::move(path), cancellation](auto result) {
        size_t bytes = sizeof(UploadTask) + path.size() * sizeof(wchar_t)
            + result.meshes.capacity() * sizeof(d3d12_import_bridge::ImportedMesh)
            + result.images.capacity() * sizeof(d3d12_import_bridge::ImportedImage)
            + result.materials.capacity() * sizeof(d3d12_import_bridge::ImportedMaterial);
        for (const auto& mesh : result.meshes) bytes += mesh.payload.capacity();
        for (const auto& image : result.images) bytes += image.pixelBytes.capacity();

        if (bytes > inbox->byteLimit) throw std::runtime_error("batch exceeds upload queue cap");
        std::unique_lock<std::mutex> lock(inbox->mutex);
        while (!inbox->stopped && inbox->generation == generation && !cancellation->load()
            && (inbox->count == UploadInbox::countCap || bytes > inbox->byteLimit - inbox->bytes))
            inbox->changed.wait_for(lock, std::chrono::milliseconds(20));
        if (inbox->stopped || inbox->generation != generation || cancellation->load()) return;
        inbox->tasks.push_back({std::move(result), generation, path, cancellation, bytes, false});
        inbox->bytes += bytes;
        ++inbox->count;
        inbox->peakCount = std::max(inbox->peakCount, inbox->count);
        inbox->peakBytes = std::max(inbox->peakBytes, inbox->bytes);
        inbox->changed.notify_all();
    };
}

void RenderThread::FinishImport(std::uint64_t generation)
{
    // The terminal marker follows every accepted batch. It carries no payload
    // and takes no capacity; at most one marker exists for the active generation.
    std::lock_guard<std::mutex> lock(uploads_->mutex);
    if (!uploads_->stopped && uploads_->generation == generation) {
        UploadTask task; task.generation = generation; task.terminal = true; task.path = uploads_->path;
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
                std::wstring error = initialized ? L"" : L"The upload coordinator could not be initialized.";
                uploader.uploadIsCancelled=[current] {return !current();};
                const bool ok = current() && initialized && uploader.BeginUploadModel(pub.task.result.meshes,
                    pub.task.result.materials, pub.task.result.images, error);
                pub.task.result.ok = ok;
                pub.task.result.errorDetails = error;
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
                        pub.task.result.errorDetails = L"The geometry copy did not complete within its timeout.";
                    }
                }
                // Keep only compact metadata after the copy; capacity remains
                // charged until the render thread accepts or discards publication.
                for (auto& mesh : pub.task.result.meshes) std::vector<std::byte>().swap(mesh.payload);
                pub.task.result.images.clear();
            }
        } catch (const std::bad_alloc&) {
            uploader.WaitForIdle();
            pub.task.result.ok = false;
            pub.task.result.errorDetails = L"There is not enough memory to display this batch.";
            pub.task.result.meshes.clear(); pub.task.result.images.clear();
        } catch (...) {
            uploader.WaitForIdle();
            pub.task.result.ok = false;
            pub.task.result.errorDetails = L"The upload coordinator stopped while processing this batch.";
            pub.task.result.meshes.clear(); pub.task.result.images.clear();
        }
        {
            std::lock_guard<std::mutex> lock(inbox->mutex);
            if (!inbox->stopped && inbox->generation == pub.task.generation
                && (!pub.task.cancellation || !pub.task.cancellation->load()))
                inbox->publications.push_back(std::move(pub));
            else if (!pub.task.terminal) { inbox->bytes -= pub.task.bytes; --inbox->count; }
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

void RenderThread::AssertOnRenderThread() const
{
    // `04-rendering-and-streaming.md:194`: "Developer builds assert queue
    // ownership". Turns the ownership rule into something the build enforces
    // rather than something a reviewer has to notice.
    assert(std::this_thread::get_id() == renderThreadId_ && "D3D12 state touched off the render thread");
}

bool RenderThread::InitializeOnThread(HWND window, std::wstring& error)
{
    return path_.Initialize(window, error);
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
        running_.store(false, std::memory_order_release);
        return;
    }

    uploadThread_ = std::thread(&RenderThread::UploadMain, this);
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
        const bool benching = benchRemaining_ > 0;
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

        RenderOneFrame();

        if (benching) {
            --benchRemaining_;
            // Discard the first 120 frames as warm-up -- shader/PSO and
            // first-touch costs say nothing about steady state.
            if (benchRemaining_ == benchFrames_ - 120) {
                path_.frameStats.Reset();
                path_.overlayTotalMs = 0.0;
                path_.overlayPasses = 0;
            }
            if (benchRemaining_ == 0) {
                PublishStats(); // forced, so the final numbers are not a multiple-of-30 away
                benchComplete_.store(true, std::memory_order_release);
            }
        }
    }

    // Release D2D/D3D11On12 before the D3D12 objects, per `04-...:190`, and
    // make sure no GPU work outlives this thread.
    if (uploadThread_.joinable()) uploadThread_.join();
    path_.WaitForIdle();
    path_.ClearModel();
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
            // Nothing useful to do from here; the next frame will simply be
            // wrong-sized rather than crash. Device-loss handling, which is
            // the real answer, is a later chunk.
            invalidated_.store(true, std::memory_order_release);
        } else {
            resizedExtent_.store((static_cast<std::uint64_t>(width) << 32)
                                  | static_cast<std::uint32_t>(height), std::memory_order_release);
        }
    }

    if (doClear) {
        path_.ClearModel();
        modelGeneration_ = 0;
        hasModel_.store(false, std::memory_order_release);
        displayedChunks_.store(0, std::memory_order_release);
        texturedChunks_.store(0, std::memory_order_release);
        textureExtent_.store(0);textureCount_.store(0);textureMips_.store(0);
        stagedScene_ = {}; stagedGeneration_ = 0; materials_.clear();

    }

}

void RenderThread::PumpUploads(HWND window)
{
    AssertOnRenderThread();

    path_.ReclaimRetired();
    std::unique_lock<std::mutex> lock(uploads_->mutex);
    if (stagedGeneration_ && stagedGeneration_ != uploads_->generation) {
        stagedScene_ = {}; stagedGeneration_ = 0; stagedHaveBounds_ = false;
        materials_.clear(); stagedFailed_ = false; stagedMetadata_.reset(); haveSceneOrigin_ = false;
    }
    if (uploads_->publications.empty()) return;
    auto pub = std::move(uploads_->publications.front()); uploads_->publications.pop_front();
    if (!pub.task.terminal) { uploads_->bytes -= pub.task.bytes; --uploads_->count; }
    uploads_->changed.notify_all();
    // Hold the generation lock through frame-boundary acceptance so a new
    // activation cannot race an old publication into the scene.
    if (pub.task.generation != uploads_->generation) return;
    if (stagedGeneration_ != pub.task.generation) {
        stagedGeneration_ = pub.task.generation; stagedScene_ = {};
        materials_.clear(); stagedHaveBounds_ = false; stagedFailed_ = false;
        stagedMetadata_ = std::make_shared<ModelData>(); haveSceneOrigin_ = false;
    }
    auto message = std::make_unique<RenderUploadResult>();
    message->generation = pub.task.generation; message->path = pub.task.path;
    message->terminal = pub.task.terminal;
    if (pub.task.terminal) {
        message->ok = !stagedFailed_ && modelGeneration_ == pub.task.generation && path_.hasModel;
        if (!message->ok) message->errorDetails = L"The import completed without displayable geometry.";
        if (message->ok && stagedMetadata_) {
            stagedMetadata_->boundsVerified = true;
            message->metadata = std::make_shared<const ModelData>(*stagedMetadata_);
        }
    } else if (!pub.task.result.ok) {
        stagedFailed_ = true;
        message->errorDetails = pub.task.result.errorDetails;
    } else {
        auto& metadata = *stagedMetadata_;
        metadata.source = pub.task.result.scene;
        metadata.stats.nodeCount = int(metadata.source.nodeCount);
        metadata.stats.meshCount = int(metadata.source.meshCount);
        metadata.stats.animationCount = int(metadata.source.animationCount);
        metadata.stats.skinCount = int(metadata.source.skinCount);
        metadata.stats.boneCount = int(metadata.source.boneCount);
        metadata.sourceUpAxis = metadata.source.upAxis == model_core::UpAxisId::Y ? SourceUpAxis::Y : SourceUpAxis::Unknown;
        DirectX::XMStoreFloat4x4(&metadata.upAxisCorrection, metadata.sourceUpAxis == SourceUpAxis::Y
            ? DirectX::XMMatrixSet(1,0,0,0, 0,0,1,0, 0,-1,0,0, 0,0,0,1) : DirectX::XMMatrixIdentity());
        for (const auto& imported : pub.task.result.meshes) {
            const auto& geometry = imported.geometry;
            if (!haveSceneOrigin_) {
                std::memcpy(metadata.sceneOrigin, geometry.origin, sizeof(metadata.sceneOrigin)); haveSceneOrigin_ = true;
            }
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
            metadata.vertexCount += imported.vertexCount;
            metadata.triangleCount += imported.topology == model_core::ChunkTopology::TriangleList ? imported.indexCount/3 : 0;
            metadata.pointCount += imported.topology == model_core::ChunkTopology::PointList ? imported.vertexCount : 0;
            metadata.stats.hasUv0 |= (geometry.geometryFlags & model_core::kGeometryHasUv0) != 0;
            metadata.stats.hasUv1 |= (geometry.geometryFlags & model_core::kGeometryHasUv1) != 0;
            metadata.stats.hasVertexColors |= (geometry.geometryFlags & model_core::kGeometryHasColors) != 0;
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
        destination.meshes.insert(destination.meshes.end(), std::make_move_iterator(pub.resources.meshes.begin()),
            std::make_move_iterator(pub.resources.meshes.end()));
        if (pub.task.result.textureWarningCount)
            metadata.warning=L"Some textures could not be loaded. Fallback textures are shown.";
        D3D12ViewerPath::ModelResources displaced;
        for (auto& texture : pub.resources.textures) {
            auto old=std::find_if(destination.textures.begin(),destination.textures.end(),
                [&](const auto& candidate) { return candidate.chunkId==texture.chunkId; });
            if (old==destination.textures.end()) destination.textures.push_back(std::move(texture));
            else { displaced.textures.push_back(std::move(*old));*old=std::move(texture); }
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
            for (const auto& texture : destination.textures) if (texture.chunkId == mat->second.baseColorImageChunkId) {
                mesh.textureIndex = static_cast<int>(texture.srvHeapIndex);
                mesh.textureHeap = texture.heap; mesh.textureDescriptorSize = texture.descriptorSize;
                break;
            }
        }
        if (!destination.meshes.empty()) {
            if (modelGeneration_ != pub.task.generation) {
                uint64_t fence = 0;
                for (const auto& frame : path_.frames) fence = std::max(fence,frame.fenceValue);
                if (path_.hasModel) path_.retiredModels.push_back({std::move(path_.model),fence,0});
                path_.model = std::move(stagedScene_); stagedScene_ = {};
                modelGeneration_ = pub.task.generation;
                std::memcpy(path_.sceneOrigin, metadata.sceneOrigin, sizeof(path_.sceneOrigin));
                path_.sourceUpAxis = metadata.source.upAxis;
                std::lock_guard<std::mutex> cameraLock(cameraMutex_);
                framingEpoch_ = interactionEpoch_.load();
                DirectX::XMFLOAT3 minimum, maximum;
                const bool native = frameOverlay_->info.showNativeOrientation;
                TransformBounds(metadata.boundsMin, metadata.boundsMax, native ? DirectX::XMMatrixIdentity()
                    : DirectX::XMLoadFloat4x4(&metadata.upAxisCorrection), minimum, maximum);
                camera_.SetBounds(minimum, maximum, viewportAspect_);
            }
            path_.hasModel = true; hasModel_.store(true, std::memory_order_release);
            displayedChunks_.store(path_.model.meshes.size(), std::memory_order_release);
            texturedChunks_.store(std::count_if(path_.model.meshes.begin(), path_.model.meshes.end(),
                [](const auto& mesh) { return mesh.textureIndex >= 0 && mesh.textureHeap; }), std::memory_order_release);
        }
        metadata.stats.drawCallCount = int(path_.model.meshes.size());
        if (stagedHaveBounds_ && modelGeneration_ == pub.task.generation) {
            std::lock_guard<std::mutex> cameraLock(cameraMutex_);
            DirectX::XMFLOAT3 minimum, maximum;
            TransformBounds(metadata.boundsMin, metadata.boundsMax, frameOverlay_->info.showNativeOrientation
                ? DirectX::XMMatrixIdentity() : DirectX::XMLoadFloat4x4(&metadata.upAxisCorrection), minimum, maximum);
            Camera framed = camera_; framed.SetBounds(minimum, maximum, viewportAspect_);
            if (interactionEpoch_.load() == framingEpoch_) camera_ = framed;
            else {
                camera_.homeX = framed.homeX; camera_.homeY = framed.homeY; camera_.homeZ = framed.homeZ;
                camera_.homeDistance = framed.homeDistance; camera_.homeBoundsMin = minimum; camera_.homeBoundsMax = maximum;
                camera_.homeOrientation = framed.homeOrientation; camera_.sceneRadius = framed.sceneRadius;
            }
        }
        message->metadata = std::make_shared<const ModelData>(metadata);
        message->ok = true;
        if (destination.meshes.empty() && modelGeneration_ != pub.task.generation) return;
    }
    if (!message->ok) message->errorSummary = L"The model was read but could not be displayed.";
    if (PostMessageW(window, kRenderUploadCompleteMessage, 0, reinterpret_cast<LPARAM>(message.get())))
        (void)message.release();
    invalidated_.store(true, std::memory_order_release);
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

    const auto framesBefore = path_.frameStats.PresentedFrames();
    path_.lastPresentResult = E_PENDING;
    if (hasModel_.load(std::memory_order_acquire)) {
        path_.RenderFrame(viewProjection, orientation, *overlay, cameraTarget, eyeSelection);
    } else {
        path_.RenderClearFrame(orientation, *overlay);
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

    if (path_.frameStats.PresentedFrames() > framesBefore && path_.lastPresentResult == S_OK) {
        const auto nowUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (!hasModel_.load(std::memory_order_acquire) && firstBackgroundUs_.load() == 0)
            firstBackgroundUs_.store(nowUs, std::memory_order_release);
        if (modelGeneration_ != 0 && presentedGeneration_.load() != modelGeneration_) {
            geometryUs_.store(nowUs, std::memory_order_release);
            presentedGeneration_.store(modelGeneration_, std::memory_order_release);
        }
    }

    // Republish only occasionally, never every frame. FrameStats::P95Ms
    // allocates a vector and sorts the whole window; doing that inside the
    // frame path cost roughly 8 ms of p95 when it was measured -- an
    // allocation and a sort per frame is exactly the kind of thing that
    // shows up as an occasional missed vsync rather than as a slower mean.
    static constexpr std::uint64_t kStatsPublishInterval = 30;
    if (path_.frameStats.PresentedFrames() % kStatsPublishInterval != 0) return;
    PublishStats();
}

void RenderThread::PublishStats()
{
    const double overlayMean
        = path_.overlayPasses > 0 ? path_.overlayTotalMs / static_cast<double>(path_.overlayPasses) : 0.0;
    StatsSnapshot snapshot;
    snapshot.meanMs = path_.frameStats.MeanMs();
    snapshot.p95Ms = path_.frameStats.P95Ms();
    snapshot.overlayMeanMs = overlayMean;
    snapshot.frames = path_.frameStats.PresentedFrames();
    snapshot.occluded = path_.frameStats.OccludedPresents();
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_ = snapshot;
    }
}

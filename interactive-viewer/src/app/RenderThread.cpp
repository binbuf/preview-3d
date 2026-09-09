// Before any Windows header: the app's own framework.h sets these, but this
// translation unit reaches windows.h through RenderThread.h's D3D12 includes
// first, and without NOMINMAX the min/max macros break std::min/std::max.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "RenderThread.h"

#include <windows.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <utility>

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

void RenderThread::SetOverlayOptions(bool enabled, int primitives, int textRuns)
{
    overlayEnabled_ = enabled;
    overlayPrimitives_ = primitives;
    overlayTextRuns_ = textRuns;
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

void RenderThread::RequestUpload(d3d12_import_bridge::ImportResult result, std::uint64_t generation,
                                  std::wstring path)
{
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        uploadPending_ = PendingUpload{ std::move(result), generation, std::move(path) };
    }
    Invalidate();
}

void RenderThread::RequestClearModel()
{
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

void RenderThread::PublishFrameInputs(const FlightInput& input, float aspect)
{
    std::lock_guard<std::mutex> lock(cameraMutex_);
    flightInput_ = input;
    viewportAspect_ = aspect;
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
    path_.overlayEnabled = overlayEnabled_;
    path_.overlayPrimitives = overlayPrimitives_;
    path_.overlayTextRuns = overlayTextRuns_;
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

    while (!stopRequested_.load(std::memory_order_acquire)) {
        DrainCommands(window);
        if (stopRequested_.load(std::memory_order_acquire)) break;

        bool cameraMoving = false;
        {
            std::lock_guard<std::mutex> lock(cameraMutex_);
            cameraMoving = camera_.HasMotion();
        }
        const bool benching = benchRemaining_ > 0;
        const bool wanted = invalidated_.exchange(false, std::memory_order_acq_rel)
            || uiAnimating_.load(std::memory_order_acquire) || cameraMoving || benching;

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
    path_.WaitForIdle();
    path_.ClearModel();
    path_.overlay.Shutdown();

    running_.store(false, std::memory_order_release);
}

void RenderThread::DrainCommands(HWND window)
{
    AssertOnRenderThread();

    bool doResize = false;
    int width = 0;
    int height = 0;
    bool doClear = false;
    std::optional<PendingUpload> upload;
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        doResize = std::exchange(resizePending_, false);
        width = resizeWidth_;
        height = resizeHeight_;
        doClear = std::exchange(clearModelPending_, false);
        upload = std::move(uploadPending_);
        uploadPending_.reset();
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
        }
    }

    if (doClear) {
        path_.ClearModel();
        hasModel_.store(false, std::memory_order_release);
    }

    if (upload) {
        auto message = std::make_unique<RenderUploadResult>();
        message->generation = upload->generation;
        message->path = std::move(upload->path);

        std::wstring uploadError;
        if (path_.UploadModel(upload->result.meshes, upload->result.materials, upload->result.images,
                               uploadError)) {
            message->ok = true;
            hasModel_.store(true, std::memory_order_release);

            // Bounds now that the payloads are here. Unlike the old UI-thread
            // scan this also counts PositionOnly_F32 chunks, so a point cloud
            // contributes bounds instead of framing as if it were empty.
            DirectX::XMFLOAT3 boundsMin{};
            DirectX::XMFLOAT3 boundsMax{};
            bool haveBounds = false;
            auto grow = [&](float x, float y, float z) {
                if (!haveBounds) {
                    boundsMin = boundsMax = DirectX::XMFLOAT3{ x, y, z };
                    haveBounds = true;
                    return;
                }
                boundsMin.x = std::min(boundsMin.x, x);
                boundsMin.y = std::min(boundsMin.y, y);
                boundsMin.z = std::min(boundsMin.z, z);
                boundsMax.x = std::max(boundsMax.x, x);
                boundsMax.y = std::max(boundsMax.y, y);
                boundsMax.z = std::max(boundsMax.z, z);
            };
            for (const auto& mesh : upload->result.meshes) {
                if (mesh.vertexLayoutId == model_core::VertexLayoutId::PositionNormalUv0_F32) {
                    const auto* vertices
                        = reinterpret_cast<const model_core::VertexPositionNormalUv0F32*>(mesh.payload.data());
                    for (uint32_t i = 0; i < mesh.vertexCount; ++i) {
                        grow(vertices[i].px, vertices[i].py, vertices[i].pz);
                    }
                } else if (mesh.vertexLayoutId == model_core::VertexLayoutId::PositionOnly_F32) {
                    const auto* vertices
                        = reinterpret_cast<const model_core::VertexPositionOnlyF32*>(mesh.payload.data());
                    for (uint32_t i = 0; i < mesh.vertexCount; ++i) {
                        grow(vertices[i].x, vertices[i].y, vertices[i].z);
                    }
                }
            }
            if (haveBounds) {
                std::lock_guard<std::mutex> lock(cameraMutex_);
                camera_.SetBounds(boundsMin, boundsMax, viewportAspect_);
            }
        } else {
            message->ok = false;
            message->errorSummary = L"The model was read but could not be displayed.";
            message->errorDetails = uploadError;
        }

        // PostMessageW is non-blocking and thread-safe. Nothing here may call
        // a blocking window API: SetWindowTextW and friends marshal to the UI
        // thread and would deadlock the moment it is waiting on this one.
        if (!PostMessageW(window, kRenderUploadCompleteMessage, 0,
                           reinterpret_cast<LPARAM>(message.get()))) {
            return; // unique_ptr frees it
        }
        (void)message.release(); // the handler takes ownership
        invalidated_.store(true, std::memory_order_release);
    }
}

void RenderThread::RenderOneFrame()
{
    AssertOnRenderThread();

    DirectX::XMFLOAT4X4 viewProjection{};
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
        camera_.Update(elapsed);
        DirectX::XMStoreFloat4x4(&viewProjection,
                                  camera_.ViewMatrix() * camera_.ProjectionMatrix(viewportAspect_));
    }

    if (hasModel_.load(std::memory_order_acquire)) {
        path_.RenderFrame(viewProjection);
    } else {
        path_.RenderClearFrame();
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

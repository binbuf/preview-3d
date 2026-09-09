#pragma once

// The dedicated render thread NFR-02 requires: "A dedicated render thread
// MUST own direct-queue submission, swap-chain resize, and Present." Until
// now every frame ran on the UI thread inside the message loop, and
// D3D12ViewerPath::BeginFrame waits on a GPU fence -- which NFR-01 forbids
// on the UI thread and `09-quality-performance-and-security.md:289` lists
// among the things that cannot be waived for MVP.
//
// It also unblocks the D2D overlay: ADR-010 requires the D3D11On12 bridge to
// be created on the render thread and "never called from the UI thread".
//
// Ownership, per `04-rendering-and-streaming.md:25-32`:
//
//   UI thread     HWND, message pump, input/capture state, high-level AppState
//   render thread device, direct queue, swap chain, back buffers, allocators,
//                 render fence, pipelines, descriptor heaps, the overlay bridge
//
// D3D12ViewerPath is held privately here precisely so that split is
// structural rather than a convention: a public member on ViewerApp cannot
// stop UI-thread access, and "don't touch this from the UI thread" is not a
// rule that survives a 2900-line file.
//
// Scope: D3D12 only. The D3D11 default path keeps rendering on the UI thread
// untouched -- it is deleted later in this batch anyway.

#include "D3D12ViewerPath.h"
#include "platform/Win32Handle.h"

#include "Renderer.h" // Camera and FlightInput -- pure DirectXMath, no D3D11 coupling

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// Posted by the render thread when an upload finishes, so the UI thread can
// move to Ready or Failed without ever having touched the GPU. lParam is a
// heap-allocated RenderUploadResult the handler takes ownership of.
constexpr UINT kRenderUploadCompleteMessage = WM_APP + 4;

struct RenderUploadResult
{
    std::uint64_t generation = 0;
    bool ok = false;
    std::wstring path;
    std::wstring errorSummary;
    std::wstring errorDetails;
};

// A locked view of the Camera shared between the two threads.
//
// The camera cannot simply move to the render thread: the UI thread needs
// synchronous reads for gizmo hit-testing (Orientation) and picking
// (Projection/distance/EyePosition). And it cannot stay on the UI thread
// either, because Camera::Update integrates motion per frame -- if the UI
// thread drove it, inertia and fly-through would stall whenever no messages
// arrived. So both threads share it under this lock.
//
// Note Camera is a public-field aggregate, not an encapsulated object, and
// is written directly from outside (`camera.distance = ...`), so the lock has
// to cover raw field access too -- hence handing out a reference rather than
// wrapping individual methods.
//
// Deliberate deviation, recorded: `04-...:172` specifies the UI thread
// converting input into compact events that "the render thread drains before
// camera update", with move events coalescing and button/key/capture
// transitions not. That queue is a separate later chunk; this lock is the
// interim.
class LockedCamera
{
public:
    LockedCamera(std::mutex& mutex, Camera& camera)
        : lock_(mutex)
        , camera_(camera)
    {
    }

    Camera* operator->() const noexcept { return &camera_; }
    Camera& operator*() const noexcept { return camera_; }

private:
    std::unique_lock<std::mutex> lock_;
    Camera& camera_;
};

class RenderThread
{
public:
    // The camera is borrowed, not owned: there is exactly one, and the D3D11
    // path still uses it directly on the UI thread. When the render thread is
    // not running (the D3D11 default), LockCamera is simply an uncontended
    // lock and everything behaves as before.
    explicit RenderThread(Camera& camera)
        : camera_(camera)
    {
    }

    ~RenderThread();

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    // Configuration, all before Start().
    void SetOverlayOptions(bool enabled, int primitives, int textRuns);
    void SetBenchFrames(int frames);

    // Spawns the thread and waits, bounded, for it to create the device,
    // swap chain and pipelines. This one startup handshake is synchronous by
    // design -- it happens in WM_CREATE, before there is anything to be
    // responsive to, and the caller needs to know whether graphics started.
    bool Start(HWND window, std::wstring& error);

    // Signals the thread to stop and joins it with a bounded wait.
    // `04-...:190`: shutdown waits "with finite diagnostics timeouts" and "a
    // driver hang must not leave the UI thread waiting forever". Idempotent.
    void Stop();

    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    // --- commands, all non-blocking ---

    // Coalesced: only the most recent size survives, and the resize happens
    // between frames on the render thread. `04-...:21` requires resize to be
    // "coalesced and performed at a direct-fence-safe point; it never blocks
    // the UI message pump."
    void RequestResize(int width, int height);
    // Takes ownership of the imported data. The render thread uploads it,
    // computes bounds, applies them to the camera, and posts
    // kRenderUploadCompleteMessage back to `window`.
    void RequestUpload(d3d12_import_bridge::ImportResult result, std::uint64_t generation, std::wstring path);
    void RequestClearModel();
    // Marks the next frame as needed and wakes the thread.
    void Invalidate();

    // The UI thread's half of "is anything animating" -- the parts that are
    // UI state (Loading spinner, held navigation keys, transient HUDs). The
    // render thread ORs this with the camera's own motion, which it can see
    // directly.
    void SetUiAnimating(bool animating);

    // Built on the UI thread and published here, because BuildFlightInput
    // calls GetKeyState and GetClientRect -- both thread-affine. GetKeyState
    // on the render thread would return that thread's own input state, so
    // Shift-boost would break silently with no error anywhere.
    void PublishFlightInput(const FlightInput& input);

    // Also UI-owned: the viewport aspect accounts for chrome insets, which
    // are layout state the render thread has no view of.
    void PublishViewportAspect(float aspect);

    // Both at once, under one lock. The message loop publishes these
    // together every iteration, and two separate acquisitions there measurably
    // contend with the render thread's own once-per-frame acquisition.
    void PublishFrameInputs(const FlightInput& input, float aspect);

    // --- published state, safe from the UI thread ---

    bool HasModel() const noexcept { return hasModel_.load(std::memory_order_acquire); }
    bool BenchComplete() const noexcept { return benchComplete_.load(std::memory_order_acquire); }

    struct StatsSnapshot
    {
        double meanMs = 0.0;
        double p95Ms = 0.0;
        double overlayMeanMs = 0.0;
        std::uint64_t frames = 0;
        std::uint64_t occluded = 0;
    };
    StatsSnapshot Stats() const;

    LockedCamera LockCamera() { return LockedCamera(cameraMutex_, camera_); }

private:
    void ThreadMain(HWND window);
    bool InitializeOnThread(HWND window, std::wstring& error);
    void DrainCommands(HWND window);
    // Retires finished copies and, on the tick an in-flight model becomes
    // fully fence-complete, swaps it in and posts the held completion
    // message. Called every loop iteration; cheap when nothing is
    // outstanding.
    void PumpUploads(HWND window);
    void RenderOneFrame();
    // Snapshots frame statistics for the UI thread. Deliberately not called
    // every frame -- FrameStats::P95Ms sorts its whole window, and doing that
    // in the frame path is measurably visible in the p95 it reports.
    void PublishStats();
    void AssertOnRenderThread() const;

    D3D12ViewerPath path_;
    Camera& camera_;

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> invalidated_{ true };
    std::atomic<bool> uiAnimating_{ false };
    std::atomic<bool> hasModel_{ false };
    std::atomic<bool> benchComplete_{ false };

    // Guards `camera_`, `flightInput_` and `viewportAspect_`, which are all
    // consumed together at the top of a frame.
    mutable std::mutex cameraMutex_;
    FlightInput flightInput_{};
    float viewportAspect_ = 1.0f;

    // Guards the command inbox below.
    mutable std::mutex commandMutex_;
    bool resizePending_ = false;
    int resizeWidth_ = 0;
    int resizeHeight_ = 0;
    bool clearModelPending_ = false;
    struct PendingUpload
    {
        d3d12_import_bridge::ImportResult result;
        std::uint64_t generation = 0;
        std::wstring path;
    };
    std::optional<PendingUpload> uploadPending_;

    // Built when an upload is queued, posted only once its copies are
    // fence-complete. Render-thread-only, so it needs no lock: DrainCommands
    // and PumpUploads both run there. Dropped rather than posted if the
    // model is cleared or superseded before it lands.
    std::unique_ptr<RenderUploadResult> pendingUploadMessage_;

    mutable std::mutex statsMutex_;
    StatsSnapshot stats_;

    platform::Win32Handle wakeEvent_;
    platform::Win32Handle startedEvent_;
    bool startOk_ = false;
    std::wstring startError_;

    // Pre-Start configuration.
    bool overlayEnabled_ = false;
    int overlayPrimitives_ = 250;
    int overlayTextRuns_ = 40;
    int benchFrames_ = 0;
    int benchRemaining_ = 0;

    double lastFrameSeconds_ = 0.0;
    std::thread::id renderThreadId_{};
};

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
// All application rendering runs here through D3D12ViewerPath.

#include "D3D12ViewerPath.h"
#include "DxgiBudgetMonitor.h"
#include "platform/Win32Handle.h"

#include "Renderer.h" // Camera and FlightInput -- pure DirectXMath, no D3D11 coupling
#include <d3d12sdklayers.h>
#include <psapi.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Posted by the render thread when an upload finishes, so the UI thread can
// move to Ready or Failed without ever having touched the GPU. lParam is a
// heap-allocated RenderUploadResult the handler takes ownership of.
constexpr UINT kRenderUploadCompleteMessage = WM_APP + 4;
constexpr UINT kRenderPickCompleteMessage = WM_APP + 5;
constexpr UINT kRenderStartFailedMessage = WM_APP + 6;
constexpr UINT kRenderDeviceRecoveryMessage = WM_APP + 7;
struct RenderPickResult { uint64_t generation; bool hit; };
struct RenderStartFailure { std::wstring details; };
struct RenderDeviceRecoveryResult { bool recovered = false; std::wstring path; std::wstring details; };

struct RenderUploadResult
{
    std::uint64_t generation = 0;
    bool ok = false;
    bool terminal = false;
    bool refinement = false;
    std::wstring path;
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    std::wstring errorSummary;
    std::wstring errorDetails;
    std::shared_ptr<const ModelData> metadata;
};

struct RenderDisplaySnapshot {
    std::wstring path;
    std::shared_ptr<const ModelData> metadata;
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
    LockedCamera(std::mutex& mutex, Camera& camera, std::atomic<uint64_t>& epoch)
        : lock_(mutex)
        , camera_(camera)
        , before_(camera)
        , epoch_(epoch)
    {
    }
    ~LockedCamera() {
        if (camera_.targetX != before_.targetX || camera_.targetY != before_.targetY || camera_.targetZ != before_.targetZ
            || camera_.desiredX != before_.desiredX || camera_.desiredY != before_.desiredY || camera_.desiredZ != before_.desiredZ
            || camera_.targetDistance != before_.targetDistance || camera_.distance != before_.distance
            || std::memcmp(&camera_.orientation, &before_.orientation, sizeof(camera_.orientation))
            || std::memcmp(&camera_.desiredOrientation, &before_.desiredOrientation, sizeof(camera_.desiredOrientation))
            || camera_.projection != before_.projection) ++epoch_;
    }

    Camera* operator->() const noexcept { return &camera_; }
    Camera& operator*() const noexcept { return camera_; }

private:
    std::unique_lock<std::mutex> lock_;
    Camera& camera_;
    Camera before_;
    std::atomic<uint64_t>& epoch_;
};

class RenderThread
{
public:
    void RequestSmokeEviction() { smokeEviction_.store(true); Invalidate(); }
    void SetSmokeBudget(uint64_t bytes) { smokeBudgetBytes_.store(bytes); Invalidate(); }
    void SetSmokeUma(bool enabled) { smokeUma_.store(enabled); Invalidate(); }
    void SetSmokeUmaDevice() { path_.deviceOptions.preferUma=true; } // before Start only
    // Bounds the import process's private commit together with viewer growth
    // and pending GPU destinations. `capOverride` is for a dedicated host
    // (STEP/OCCT or USD compatibility) whose own Job ceiling is
    // min(4 GiB, 35% of RAM) rather than the general Tier-B scratch cap; a
    // legitimate large OCCT transfer must not be rejected as a resource limit
    // while the host Job still allows it. Zero keeps the general policy cap.
    std::function<bool(uint64_t)> CpuBudgetGuard(uint64_t capOverride = 0) {
        auto inbox=uploads_;
        return [this,inbox,capOverride](uint64_t workerBytes) {
            PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory);
            if (!K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))) return false;
            const auto baseline=baselineCpuBytes_, cap=capOverride ? capOverride : cpuPolicyCap_;
            const bool uma=isUma_;
            const uint64_t growth=memory.PrivateUsage>baseline ? memory.PrivateUsage-baseline : 0;
            std::lock_guard<std::mutex> lock(inbox->mutex); inbox->workerPrivateBytes=workerBytes;
            const uint64_t destinations=(uma || inbox->simulateUma) ? inbox->gpuBaseBytes+inbox->gpuPendingBytes : 0;
            return workerBytes<=cap && growth<=cap-workerBytes && destinations<=cap-workerBytes-growth;
        };
    }
    std::function<uint32_t()> DetailSource(std::uint64_t generation) {
        auto inbox=uploads_;
        return [inbox,generation] {
            std::lock_guard<std::mutex> lock(inbox->mutex);
            if (inbox->generation!=generation || inbox->details.empty()) return uint32_t(0);
            ++inbox->detailRequests;
            const auto identity=inbox->details.front(); inbox->details.pop_front(); return identity;
        };
    }
    // The camera is borrowed, not owned: UI input and the render loop share
    // exactly one camera, and both access it through LockCamera.
    explicit RenderThread(Camera& camera)
        : camera_(camera)
    {
        // Command-line activation may begin importing before the asynchronous
        // graphics thread is initialized. Establish the host CPU policy here
        // so its guard can never capture a transient zero-byte cap.
        MEMORYSTATUSEX physicalMemory{sizeof(physicalMemory)};
        cpuPolicyCap_=GlobalMemoryStatusEx(&physicalMemory)
            ? std::min(1536ull*1024*1024,physicalMemory.ullTotalPhys/4) : 1536ull*1024*1024;
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory);
        if (K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))
            baselineCpuBytes_=memory.PrivateUsage;
    }

    ~RenderThread();

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    // Configuration, all before Start().
    void SetBenchFrames(int frames);
    void SetBenchmarkLimits(int frames, std::uint64_t durationMs);
    void SetBenchmarkOccludedForTesting() { benchmarkForceOccluded_ = true; }
    void NotifyLoadingStarted(std::uint64_t generation);
    void NotifyBenchmarkInput(std::uint64_t timestampUs) {
        benchmarkInputUs_.store(timestampUs, std::memory_order_release);
        benchmarkInputToPresentUs_.store(0, std::memory_order_release);
        Invalidate();
    }

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
    // Creates a lifetime-safe batch sink for the broker thread. Capacity covers
    // queued, coordinator-owned and published batches until render acceptance.
    // Only this background sink may wait for capacity; cancellation wakes it.
    std::function<void(d3d12_import_bridge::ImportResult)> BeginImport(
        std::uint64_t generation, std::wstring path, std::shared_ptr<std::atomic_bool> cancellation);
    void FinishImport(std::uint64_t generation, model_core::FileIdentity sourceIdentity = {},
                      std::vector<std::wstring> missingAssets = {});
    void CancelUploads();
    void SetSmokeUploads(unsigned copyMs, uint64_t sectionBytes, bool delayBatches) {
        copyDelayMs_ = copyMs; smokeSectionBytes_ = sectionBytes; delayBatches_ = delayBatches;
    }
    void SetSmokeQueueCap(size_t bytes) { uploads_->byteLimit = bytes; }
    uint64_t SmokeSectionBytes() const { return smokeSectionBytes_; }
    bool DelayBatches() const { return delayBatches_; }
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

    // Inputs, aspect and immutable chrome snapshot together, under one lock.
    // Publishing wakes the renderer after the new UI state is available.
    void PublishFrameInputs(const FlightInput& input, float aspect, std::shared_ptr<const OverlayFrame> overlay);

    // --- published state, safe from the UI thread ---

    bool HasModel() const noexcept { return hasModel_.load(std::memory_order_acquire); }
    std::shared_ptr<const RenderDisplaySnapshot> DisplaySnapshot() const noexcept { return displaySnapshot_.load(); }
    bool HasCompleteModel() const noexcept {
        const auto snapshot = DisplaySnapshot();
        return snapshot && snapshot->metadata->boundsVerified;
    }
    bool BenchComplete() const noexcept { return benchComplete_.load(std::memory_order_acquire); }

    // Monotonic-clock microseconds, recorded only after a successful visible
    // Present. These are lifecycle smoke evidence, not ETW display timestamps.
    std::uint64_t SmokeValue(unsigned field) const noexcept;

    // The terminal import marker has been accepted, visible refinement has
    // drained, and the resulting model has reached a successful Present. The
    // UI uses this pair to finish the user-visible load timer without treating
    // an early proxy or coarse frame as a complete render.
    std::uint64_t CompleteModelPresentedGeneration() const noexcept {
        return completeModelPresentedGeneration_.load(std::memory_order_acquire);
    }
    std::uint64_t CompleteModelPresentedMicroseconds() const noexcept {
        return completeModelPresentedUs_.load(std::memory_order_acquire);
    }

    struct StatsSnapshot
    {
        double meanMs = 0.0;
        double medianMs = 0.0;
        double p95Ms = 0.0;
        double maxMs = 0.0;
        double overlayMeanMs = 0.0;
        std::uint64_t frames = 0;
        std::uint64_t occluded = 0;
        std::uint64_t failed = 0;
        std::vector<double> rawIntervalsMs;
    };
    StatsSnapshot Stats() const;
    StatsSnapshot BenchmarkStats() const;

    LockedCamera LockCamera() { return LockedCamera(cameraMutex_, camera_, interactionEpoch_); }
    void RequestPick(int x, int y, uint64_t generation);
    void InjectDeviceRemovalForTesting() { injectDeviceRemoval_.store(true); Invalidate(); }

private:
    void ThreadMain(HWND window);
    bool InitializeOnThread(HWND window, std::wstring& error);
    void DrainCommands();
    // Accepts one fence-complete publication between frames. Appends chunks
    // and resolves generation catalog entries without touching the copy lane.
    void PumpUploads(HWND window);
    void RenderOneFrame();
    bool RecoverDevice();
    // Snapshots frame statistics for the UI thread. Deliberately not called
    // every frame -- FrameStats::P95Ms sorts its whole window, and doing that
    // in the frame path is measurably visible in the p95 it reports.
    void PublishStats(bool includeRaw = false);
    void AssertOnRenderThread() const;

    D3D12ViewerPath path_;
    Camera& camera_;
    std::atomic<uint64_t> interactionEpoch_{0};
    uint64_t framingEpoch_ = 0;
    std::shared_ptr<ModelData> stagedMetadata_;
    bool haveSceneOrigin_ = false;

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> invalidated_{ true };
    std::atomic<bool> uiAnimating_{ false };
    std::atomic<bool> hasModel_{ false };
    std::atomic<std::shared_ptr<const RenderDisplaySnapshot>> displaySnapshot_;
    std::atomic<bool> benchComplete_{ false };
    std::atomic<std::uint64_t> firstBackgroundUs_{ 0 };
    std::atomic<std::uint64_t> geometryUs_{ 0 };
    std::atomic<std::uint64_t> loadingUiUs_{ 0 };
    std::atomic<std::uint64_t> coarseUs_{ 0 };
    std::atomic<std::uint64_t> verifiedBoundsUs_{ 0 };
    std::atomic<std::uint64_t> refinementUs_{ 0 };
    std::atomic<std::uint64_t> loadingGeneration_{ 0 };
    std::atomic<std::uint64_t> pendingCoarseGeneration_{ 0 };
    std::atomic<std::uint64_t> pendingVerifiedGeneration_{ 0 };
    std::atomic<std::uint64_t> pendingRefinementGeneration_{ 0 };
    std::atomic<std::uint64_t> benchmarkInputUs_{ 0 };
    std::atomic<std::uint64_t> benchmarkInputToPresentUs_{ 0 };
    std::atomic<std::uint64_t> benchmarkPresentedFrames_{ 0 };
    std::atomic<std::uint64_t> presentedGeneration_{ 0 };
    std::atomic<std::uint64_t> completeModelAwaitingPresentGeneration_{ 0 };
    std::atomic<std::uint64_t> completeModelPresentedGeneration_{ 0 };
    std::atomic<std::uint64_t> completeModelPresentedUs_{ 0 };
    std::atomic<std::uint64_t> resizedExtent_{ 0 };
    std::atomic<std::uint64_t> displayedChunks_{0}, texturedChunks_{0};
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> debugInfo_;
    std::atomic<uint64_t> debugErrors_{0}, debugAvailable_{0};
    std::atomic<uint64_t> textureExtent_{0},textureCount_{0},textureMips_{0};
    std::atomic<uint64_t> pickCompletions_{0}, pickHits_{0};
    std::uint64_t modelGeneration_ = 0; // render-thread-owned

    // Guards `camera_`, `flightInput_`, `viewportAspect_` and `frameOverlay_`, all
    // consumed together at the top of a frame.
    mutable std::mutex cameraMutex_;
    FlightInput flightInput_{};
    float viewportAspect_ = 1.0f;
    std::shared_ptr<const OverlayFrame> frameOverlay_ = std::make_shared<OverlayFrame>();

    // Guards the command inbox below.
    mutable std::mutex commandMutex_;
    bool resizePending_ = false;
    int resizeWidth_ = 0;
    int resizeHeight_ = 0;
    bool clearModelPending_ = false;
    struct PickRequest { int x, y; uint64_t generation; };
    std::optional<PickRequest> pickRequest_;
    std::optional<PickRequest> pendingPick_;
    struct UploadTask {
        d3d12_import_bridge::ImportResult result;
        std::uint64_t generation = 0;
        std::wstring path;
        std::shared_ptr<std::atomic_bool> cancellation;
        size_t bytes = 0;
        bool terminal = false;
        uint64_t gpuBytes = 0;
        uint32_t detailIdentity = 0;
    };
    struct Publication {
        UploadTask task;
        D3D12ViewerPath::ModelResources resources;
    };
    struct UploadInbox {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<UploadTask> tasks;
        std::deque<Publication> publications;
        size_t bytes = 0, count = 0, peakBytes = 0, peakCount = 0;
        size_t byteLimit = 128ull * 1024 * 1024;
        std::uint64_t generation = 0;
        std::wstring path;
        std::shared_ptr<std::atomic_bool> cancellation;
        bool stopped = false;
        std::deque<uint32_t> details;
        std::unordered_set<uint32_t> requestedDetails;
        uint64_t gpuBaseBytes=0, gpuPendingBytes=0, gpuTargetBytes=0, workerPrivateBytes=0, detailRequests=0;
        bool simulateUma=false;
        uint64_t mandatoryBytes=0, coarseGeneration=0;
        static constexpr size_t countCap = 4;
    };
    std::shared_ptr<UploadInbox> uploads_ = std::make_shared<UploadInbox>();
    std::thread uploadThread_;
    void UploadMain();
    unsigned copyDelayMs_ = 0;
    uint64_t smokeSectionBytes_ = 64ull * 1024 * 1024;
    bool delayBatches_ = false;
    std::unordered_map<uint32_t, d3d12_import_bridge::ImportedMaterial> materials_;
    D3D12ViewerPath::ModelResources stagedScene_;
    std::uint64_t stagedGeneration_ = 0;
    bool stagedHaveBounds_ = false, stagedFailed_ = false;
    bool stagedProxyMode_ = false, stagedProxyComplete_ = false;
    bool stagedPreviewOnly_ = false;
    std::atomic<bool> smokeEviction_{false};
    std::atomic<uint64_t> coarseChunks_{0}, fineChunks_{0}, suppressedCoarse_{0}, coarseCompleteGeneration_{0}, scannedPrimitives_{0};
    std::atomic<uint64_t> coarseAllocationBytes_{0};
    void UpdateResidencySmoke();
    void UpdateBudget();
    // Requests the next visible fine-detail region and reports whether the
    // current view can still change as a result of outstanding refinement.
    bool RequestVisibleDetail(const DirectX::XMFLOAT4X4& vp, const double target[3],
        const DirectX::XMFLOAT4X4& modelTransform);
    DxgiBudgetMonitor budgetMonitor_;
    std::atomic<bool> isUma_{false};
    uint64_t cpuPolicyCap_=0, baselineCpuBytes_=0;
    HWND window_=nullptr;
    uint64_t failedBudgetGeneration_=0;
    std::atomic<uint64_t> smokeBudgetBytes_{UINT64_MAX}, accountedGpuBytes_{0}, targetGpuBytes_{0}, pendingGpuBytes_{0};
    std::atomic<uint64_t> evictionCount_{0}, rejectedDetailCount_{0};
    std::atomic<bool> smokeUma_{false};
    bool initialTerminal_=false;
    bool recoveryAttempted_ = false;
    bool deviceFatal_ = false;
    std::atomic<bool> injectDeviceRemoval_{false};
    std::atomic<bool> recoveryRequested_{false};
    std::atomic<uint64_t> recoveryCount_{0};
    double pauseDetailUntil_=0;
    std::unordered_map<uint32_t,model_core::ChunkDescriptor> scanCatalog_;

    mutable std::mutex statsMutex_;
    StatsSnapshot stats_;
    StatsSnapshot benchmarkStats_;
    FrameStats benchmarkFrameStats_;

    platform::Win32Handle wakeEvent_;
    platform::Win32Handle startedEvent_;
    bool startOk_ = false;
    std::wstring startError_;

    // Pre-Start configuration.
    int benchFrames_ = 0;
    int benchRemaining_ = 0;
    std::uint64_t benchDurationMs_ = 0;
    std::chrono::steady_clock::time_point benchDeadline_{};
    bool benchmarkForceOccluded_ = false;

    double lastFrameSeconds_ = 0.0;
    std::thread::id renderThreadId_{};
};

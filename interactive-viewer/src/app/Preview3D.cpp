#include "framework.h"
#include "Preview3D.h"
#include "Accessibility.h"
#include "ActiveInstance.h"
#include "Chrome.h"
#include "ControlsDialog.h"
#include "D3D12ImportBridge.h"
#include "RenderThread.h"
#include "InfoPanel.h"
#include "Model.h"
#include "Renderer.h"
#include "NavGizmo.h"
#include "Settings.h"
#include "ShellIntegration.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <iomanip>
#include <sstream>
#include <bit>
#include <atomic>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t kWindowClass[] = L"Preview3DWindow";
constexpr wchar_t kApplicationName[] = L"Preview 3D";
// Sandboxed import completion is separate from render-thread upload completion.
constexpr UINT kD3D12ImportCompleteMessage = WM_APP + 3;
constexpr UINT kActivationMessage = WM_APP + 8;
constexpr UINT kAccessibilityQueryMessage = WM_APP + 9;
constexpr UINT kAccessibilityActionMessage = WM_APP + 10;
constexpr UINT kAccessibilityFocusMessage = WM_APP + 11;
constexpr UINT kAccessibilityStatusMessage = WM_APP + 12;
constexpr UINT kOpenWithLaunchFailedMessage = WM_APP + 13;
constexpr float kArrowPixelsPerSecond = 340.0f;
constexpr double kHudVisibleSeconds = 1.3;
constexpr double kHudFadeSeconds = 0.30;
constexpr double kToggleRepeatGuardSeconds = 0.30;
constexpr double kClickMaxSeconds = 0.5;
constexpr float kClickDragThresholdPixels = 6.0f;
// Caps how many queued messages the main loop drains before it is forced to
// re-check animation state and render, so any burst or self-sustaining
// message flood can never fully starve rendering.
constexpr int kMaxDrainedMessagesPerIteration = 32;
// On the D3D12 path the UI thread no longer renders, so it can block on
// messages -- but it still republishes flight input and the animating flag,
// which are what keep held keys and HUD timers alive. A bounded poll keeps
// that honest without a busy loop.
constexpr DWORD kUiPollIntervalMs = 8;
// A running --frame-bench only needs the UI thread to notice it finished, so
// this is deliberately coarse: polling at input rate makes the UI thread
// contend with the render thread for the camera lock roughly once per frame,
// which shows up directly in the frame-interval p95 the bench is measuring.
constexpr DWORD kBenchPollIntervalMs = 100;
// How long the pointer has to sit still over a toolbar button before its
// tooltip appears (see ComputeTooltipInfo/UpdateTooltipTracking) — longer
// than a system tooltip's default so it stays out of the way during normal
// clicking, but still short enough to answer "what does this button do?".
constexpr UINT_PTR kTooltipTimerId = 1;
constexpr UINT kTooltipDelayMs = 1500;

// Exclusive pointer state machine. Exactly one mode owns camera/selection
// input at a time; the mode is chosen at button-down and stays locked for the
// whole gesture, so mid-drag modifier changes never switch modes.
//
//   None        idle; gizmo hover tracking only
//   Orbit       LMB held on the canvas: orbit-drags the camera. An LMB
//               gesture that stays under the click/drag threshold also
//               click-selects on release (drag and click share LMB).
//   GizmoOrbit  LMB held on the gizmo ball: wrapped orbit drag (same math as
//               Orbit, kept distinct only because it started on the gizmo).
//   (Axis nodes/stems snap on press, so they never enter a drag mode.)
//   LightDrag   LMB held on the gizmo's outer light ring (Directional mode
//               only): drags the sun around the ring to rotate the light.
//               Deliberately separate from GizmoOrbit so grabbing the ring
//               never orbits the camera.
//   FlyLook     RMB held: Unreal-style raw-input capture; WASD/Q/E fly and
//               the wheel adjusts speed. The cursor returns on release.
//   Truck       MMB held: trucks along the world ground plane (flattened
//               forward/right), optionally snapped to the nearest world axis.
//   DollyDrag   Ctrl+MMB: smooth exponential dolly on vertical drag.
enum class PointerMode
{
    None,
    GizmoOrbit,
    LightDrag,
    FlyLook,
    Orbit,
    Truck,
    DollyDrag
};

struct D3D12CompleteMessage
{
    std::uint64_t generation = 0;
    std::wstring path;
    d3d12_import_bridge::ImportResult result;
};

struct ViewerApp
{
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND retryButton = nullptr;
    HWND openAnotherButton = nullptr;
    HWND copyButton = nullptr;
    HWND tooltip = nullptr;
    HFONT buttonFont = nullptr;
    UINT dpi = 96;
    float dpiScale = 1.0f;
    int toolbarHeight = 52;
    int bottomBarHeight = 44;
    bool infoPanelVisible = false;
    float infoPanelScrollOffset = 0.0f;   // logical px, clamped against content on wheel input
    bool speedFlyoutOpen = false;
    bool speedSliderDragging = false;
    bool zoomSliderDragging = false;
    bool infoButtonHover = false;
    bool infoButtonPressed = false;
    bool infoPanelCloseButtonHover = false;
    bool infoPanelCloseButtonPressed = false;
    bool fullscreenButtonHover = false;
    bool fullscreenButtonPressed = false;
    LightingMode lightingMode = LightingMode::Studio;
    float directionalLightAngle = 0.875f;
    // Elevation above the horizon in radians; atan(0.55) matches the previous
    // fixed key elevation. The gizmo sun's distance from center encodes it.
    float directionalLightElevation = 0.502f;
    int lightingButtonPressed = -1; // 0 Studio, 1 Clay, 2 Directional, 3 Wireframe
    int lightingButtonHover = -1;
    bool isFullscreen = false;
    // Hover-delay tooltip (see ComputeTooltipInfo/UpdateTooltipTracking):
    // tooltipTargetId identifies the hovered button (0 == none) so tracking
    // can tell "still the same button" from "moved to a new one" without
    // re-deriving text/rect every mouse move.
    int tooltipTargetId = 0;
    bool tooltipVisible = false;
    RECT tooltipAnchorRect{};
    std::wstring tooltipText;
    bool tooltipAnchorBelow = true;
    WINDOWPLACEMENT savedWindowPlacement{ sizeof(WINDOWPLACEMENT) };
    bool rendererReady = false;
    bool closing = false;
    bool showFrameStats = false; // --frame-stats: see UpdateTitle
    // --frame-bench N sustains rendering for timing the actual scene and chrome.
    int benchFrames = 0;
    bool benchmarkMode = false;
    int benchmarkFrameLimit = 1200;
    int benchmarkRepeat = 3;
    std::uint64_t benchmarkDurationMs = 10'000;
    std::wstring benchmarkResultPath;
    std::wstring benchmarkReference = L"compatibility";
    std::wstring benchmarkEtwPath;
    bool benchmarkOcclusion = false;
    std::uint64_t benchmarkStartedUs = 0;
    std::uint64_t benchmarkViewerBaselinePrivate = 0;
    std::atomic<std::uint64_t> benchmarkViewerPeakPrivate{0};
    std::atomic<std::uint64_t> benchmarkViewerPeakMapped{0};
    std::atomic<std::uint64_t> benchmarkWorkerPeakPrivate{0};
    std::atomic<std::uint64_t> benchmarkWorkerPeakMapped{0};
    std::atomic<std::uint64_t> benchmarkHeartbeatMaxUs{0};
    bool benchmarkInputSent = false;
    std::jthread benchmarkSampler;
    bool appSmoke = false; // opt-in, bounded test commands; no normal activation IPC
    active_instance::Coordinator activeInstance;
    IAccessible* accessible = nullptr;
    IRawElementProviderSimple* uiaAccessible = nullptr;
    viewer_accessibility::Control keyboardControl = viewer_accessibility::Control::None;
    bool highContrast = false;
    bool reduceMotion = false;
    // Deliberately NOT named `camera`: once the render thread exists, every
    // access has to go through renderThread.LockCamera(). Renaming turned
    // each of the ~30 existing uses into a compile error rather than a race
    // to be found by inspection. Declared before renderThread because the
    // thread borrows it by reference and member init follows declaration
    // order.
    Camera sharedCamera;
    // Owns the application's exclusive D3D12ViewerPath privately.
    RenderThread renderThread{ sharedCamera };
    NavGizmo gizmo;
    Chrome chrome;
    ViewerState state = ViewerState::Empty;
    PointerMode pointerMode = PointerMode::None;
    POINT lastPointer{};
    bool flyLook = false;               // RMB capture active
    POINT flyPressPoint{};              // screen point to restore the cursor to
    bool wrapDrag = false;              // cursor wrapping (infinite drag) active
    bool selectDragged = false;
    POINT selectDownPoint{};
    double selectDownSeconds = 0.0;
    double orbitVelocityX = 0.0;
    double orbitVelocityY = 0.0;
    double lastOrbitMoveSeconds = 0.0;
    double panVelocityX = 0.0;
    double panVelocityY = 0.0;
    double lastPanMoveSeconds = 0.0;
    bool moveForward = false;
    bool moveBackward = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool moveUp = false;
    bool moveDown = false;
    bool rollLeft = false;
    bool rollRight = false;
    bool arrowLeft = false;
    bool arrowRight = false;
    bool arrowUp = false;
    bool arrowDown = false;
    std::unordered_map<UINT32, POINT> touchPoints;
    POINT touchCenter{};
    double touchSpan = 0.0;
    std::wstring initialPath;
    std::wstring currentPath;
    std::wstring failedPath;
    std::wstring filename;
    std::uint64_t renderStartedMicroseconds = 0;
    bool renderPresentationPending = false;
    std::wstring renderDurationText;
    std::wstring warning;
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    import_broker::ImportStage errorStage = import_broker::ImportStage::OpenSource;
    model_core::ImportFailurePhase errorPhase = model_core::ImportFailurePhase::Unspecified;
    uint32_t faultForTesting = 0;
    std::wstring diagnosticPath;
    std::wstring smokePickerPath;
    bool holdUploadMessagesForTesting = false;
    std::wstring errorSummary;
    std::wstring errorDetails;
    std::shared_ptr<const ModelData> loadedModel; // immutable compact metadata; picking stays on the GPU
    uint64_t smokePickRequests = 0;
    bool meshSelected = false;
    bool gridVisible = true;
    bool axisSnapEnabled = false;
    // Display the loaded model in its native/source orientation instead of
    // this app's normalized Z-up correction (Model.h's ModelData::
    // upAxisCorrection). Persisted via Settings.h; default matches
    // ViewerSettings' default (normalized).
    bool showNativeOrientation = false;
    GroundAxis groundAxis = GroundAxis::Automatic;
    bool groundAxisInverted = false;
    bool hideCursorWhileDragging = true;
    bool settingsPanelOpen = false;
    std::wstring speedHudText;
    double speedHudUntil = 0.0;
    std::wstring modeHudText;
    double modeHudUntil = 0.0;
    int lastToggleId = 0;
    double lastToggleSeconds = 0.0;
    std::uint64_t generation = 0;
    // Last bounded STEP host phase/progress, written by the import thread and
    // read by the UI thread to keep a long CAD load legible. Zero phase means
    // no STEP progress has arrived (or the current open is not STEP).
    std::atomic<std::uint32_t> stepProgressPhase{0};
    std::atomic<std::uint32_t> stepProgressDone{0};
    std::atomic<std::uint32_t> stepProgressTotal{0};
    std::shared_ptr<std::atomic_bool> cancellation;
    std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);
    // Joinable background imports: cancellation is bounded by the broker's
    // cooperative grace/worker replacement contract, so close cannot leave
    // detached threads referring to HWND or app state after destruction.
    std::vector<std::jthread> importThreads;
};

std::wstring AccessibilityStatus(const ViewerApp& app);
void InvokeAccessible(ViewerApp& app, viewer_accessibility::Control control);

struct AccessibilityQueryRequest
{
    viewer_accessibility::Control control{};
    viewer_accessibility::ControlInfo result;
};

HBRUSH gBackgroundBrush = nullptr;
HWND gMainWindow = nullptr;

int Scale(const ViewerApp& app, int logical)
{
    return MulDiv(logical, static_cast<int>(app.dpi), 96);
}

double NowSeconds()
{
    static const double frequency = []() -> double
    {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / frequency;
}

std::uint64_t NowMicroseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void AtomicMaximum(std::atomic<std::uint64_t>& destination, std::uint64_t value)
{
    auto old = destination.load(std::memory_order_relaxed);
    while (old < value && !destination.compare_exchange_weak(old, value, std::memory_order_relaxed)) {}
}

std::uint64_t PrivateCommit(HANDLE process)
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    return K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))
        ? static_cast<std::uint64_t>(counters.PrivateUsage) : 0;
}

std::uint64_t CommittedMappedViews(HANDLE process)
{
    MEMORY_BASIC_INFORMATION region{};
    std::uint64_t total = 0;
    std::uintptr_t address = 0;
    while (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) == sizeof(region)) {
        if (region.State == MEM_COMMIT && region.Type == MEM_MAPPED)
            total += static_cast<std::uint64_t>(region.RegionSize);
        const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= address) break;
        address = next;
    }
    return total;
}

void SampleBenchmarkMemory(ViewerApp& app)
{
    AtomicMaximum(app.benchmarkViewerPeakPrivate, PrivateCommit(GetCurrentProcess()));
    AtomicMaximum(app.benchmarkViewerPeakMapped, CommittedMappedViews(GetCurrentProcess()));
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{sizeof(entry)};
    std::uint64_t workerPrivate = 0, workerMapped = 0;
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ParentProcessID != GetCurrentProcessId()
            || _wcsicmp(entry.szExeFile, L"Preview3DImportWorker.exe") != 0) continue;
        const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID);
        if (!process) continue;
        workerPrivate += PrivateCommit(process);
        workerMapped += CommittedMappedViews(process);
        CloseHandle(process);
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    AtomicMaximum(app.benchmarkWorkerPeakPrivate, workerPrivate);
    AtomicMaximum(app.benchmarkWorkerPeakMapped, workerMapped);
}

std::string JsonString(std::wstring_view value)
{
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(count > 0 ? static_cast<size_t>(count) : 0, '\0');
    if (count > 0) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), utf8.data(), count, nullptr, nullptr);
    std::string escaped = "\"";
    for (const unsigned char ch : utf8) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buffer[7]{}; sprintf_s(buffer, "\\u%04x", ch); escaped += buffer;
            } else escaped += static_cast<char>(ch);
        }
    }
    escaped += '"';
    return escaped;
}

double MilestoneMs(std::uint64_t timestamp, std::uint64_t start)
{
    return timestamp >= start ? static_cast<double>(timestamp - start) / 1000.0 : -1.0;
}

bool WriteBenchmarkResult(ViewerApp& app, int& exitCode)
{
    app.benchmarkSampler.request_stop();
    if (app.benchmarkSampler.joinable()) app.benchmarkSampler.join();
    SampleBenchmarkMemory(app);
    const auto stats = app.renderThread.BenchmarkStats();
    const double background = MilestoneMs(app.renderThread.SmokeValue(2), app.benchmarkStartedUs);
    const double loading = MilestoneMs(app.renderThread.SmokeValue(67), app.benchmarkStartedUs);
    const double geometry = MilestoneMs(app.renderThread.SmokeValue(3), app.benchmarkStartedUs);
    const double coarse = MilestoneMs(app.renderThread.SmokeValue(68), app.benchmarkStartedUs);
    const double verified = MilestoneMs(app.renderThread.SmokeValue(69), app.benchmarkStartedUs);
    const double refinement = MilestoneMs(app.renderThread.SmokeValue(70), app.benchmarkStartedUs);
    const double intervalGate = app.benchmarkReference == L"performance" ? 8.3 : 16.7;
    const double inputGate = app.benchmarkReference == L"performance" ? 16.0 : 33.0;
    const double coarseGate = app.initialPath.find(L"large") != std::wstring::npos ? 5000.0
        : app.initialPath.find(L"medium") != std::wstring::npos ? 2000.0 : 500.0;
    const auto viewerGrowth = app.benchmarkViewerPeakPrivate.load() > app.benchmarkViewerBaselinePrivate
        ? app.benchmarkViewerPeakPrivate.load() - app.benchmarkViewerBaselinePrivate : 0;
    const auto aggregateGrowth = viewerGrowth + app.benchmarkWorkerPeakPrivate.load();
    MEMORYSTATUSEX physicalMemory{sizeof(physicalMemory)};
    const auto scratchCap = GlobalMemoryStatusEx(&physicalMemory)
        ? std::min<std::uint64_t>(1024ull * 1024 * 1024, physicalMemory.ullTotalPhys / 4)
        : 1024ull * 1024 * 1024;
    const bool performanceReference = app.benchmarkReference == L"performance";
    const bool timingPassed = background >= 0 && loading >= 0 && geometry >= 0 && coarse >= 0 && verified >= 0
        && (!performanceReference || (background <= 150.0 && loading <= 200.0 && coarse <= coarseGate));
    const bool framePassed = stats.p95Ms <= intervalGate && stats.maxMs <= 50.0
        && stats.failed == 0 && stats.occluded == 0;
    const bool inputPassed = app.renderThread.SmokeValue(71) > 0
        && static_cast<double>(app.renderThread.SmokeValue(71)) / 1000.0 <= inputGate;
    const bool heartbeatPassed = app.benchmarkHeartbeatMaxUs.load() <= 100'000;
    const bool memoryPassed = aggregateGrowth <= 1536ull * 1024 * 1024;
    const bool queuePassed = app.renderThread.SmokeValue(6) <= 512ull * 1024 * 1024;
    const bool workerCapPassed = app.benchmarkWorkerPeakPrivate.load() <= import_broker::kImportWorkerCommitLimitBytes;
    // The worker does not yet publish allocator-category counters. Its whole
    // private commit is a conservative upper bound on live scratch: if that
    // stronger quantity is below the scratch cap, scratch necessarily is too.
    const bool scratchUpperBoundPassed = app.benchmarkWorkerPeakPrivate.load() <= scratchCap;
    const bool passed = timingPassed
        && framePassed && inputPassed && heartbeatPassed && memoryPassed && queuePassed
        && workerCapPassed && scratchUpperBoundPassed;
    exitCode = passed ? 0 : 2;

    std::ostringstream json;
    json << std::fixed << std::setprecision(3)
         << "{\n  \"schema\": 1,\n  \"mode\": \"scope-limited-mvp\",\n"
         << "  \"status\": \"" << (passed ? "pass" : "fail") << "\",\n"
         << "  \"viewerState\": " << static_cast<int>(app.state)
         << ", \"errorCode\": " << static_cast<unsigned>(app.errorCode)
         << ", \"errorStage\": " << static_cast<unsigned>(app.errorStage)
         << ", \"errorSummary\": " << JsonString(app.errorSummary)
         << ", \"errorDetails\": " << JsonString(app.errorDetails) << ",\n"
         << "  \"fixture\": " << JsonString(app.initialPath) << ",\n"
         << "  \"reference\": " << JsonString(app.benchmarkReference) << ",\n"
         << "  \"limits\": {\"durationMs\": " << app.benchmarkDurationMs
         << ", \"frameLimit\": " << app.benchmarkFrameLimit << ", \"repeatCount\": " << app.benchmarkRepeat
         << ", \"renderDurationMs\": " << app.renderThread.SmokeValue(72)
         << ", \"renderFrameLimitWithWarmup\": " << app.renderThread.SmokeValue(73)
         << ", \"renderObservedPresents\": " << app.renderThread.SmokeValue(74)
         << ", \"generalWorkerJobCommitBytes\": " << import_broker::kImportWorkerCommitLimitBytes << "},\n"
         << "  \"milestonesMs\": {\"firstBackground\": " << background << ", \"loadingUi\": " << loading
         << ", \"firstGeometry\": " << geometry << ", \"completeCoarse\": " << coarse
         << ", \"verifiedBounds\": " << verified << ", \"firstRefinement\": " << refinement << "},\n"
         << "  \"gates\": {\"timing\": " << (timingPassed ? "true" : "false")
         << ", \"startupTimingApplicable\": " << (performanceReference ? "true" : "false")
         << ", \"frameIntervals\": " << (framePassed ? "true" : "false")
         << ", \"inputToPresent\": " << (inputPassed ? "true" : "false")
         << ", \"heartbeat\": " << (heartbeatPassed ? "true" : "false")
         << ", \"aggregatePrivateCommit\": " << (memoryPassed ? "true" : "false")
         << ", \"uploadQueue\": " << (queuePassed ? "true" : "false")
         << ", \"workerJobCommit\": " << (workerCapPassed ? "true" : "false")
         << ", \"scratchUpperBound\": " << (scratchUpperBoundPassed ? "true" : "false") << "},\n"
         << "  \"frames\": {\"count\": " << stats.frames << ", \"meanMs\": " << stats.meanMs
         << ", \"medianMs\": " << stats.medianMs << ", \"p95Ms\": " << stats.p95Ms
         << ", \"maxMs\": " << stats.maxMs << ", \"presentFailures\": " << stats.failed
         << ", \"rawCapacity\": " << FrameStats::kCapacity
         << ", \"rawDropped\": " << (stats.frames > stats.rawIntervalsMs.size() + 1
                ? stats.frames - stats.rawIntervalsMs.size() - 1 : 0)
         << ", \"excluded\": {\"occluded\": " << stats.occluded
         << ", \"etwCorrelated\": false, \"etwSource\": " << JsonString(app.benchmarkEtwPath)
         << "}, \"rawIntervalsMs\": [";
    for (size_t i = 0; i < stats.rawIntervalsMs.size(); ++i) {
        if (i) json << ',';
        json << stats.rawIntervalsMs[i];
    }
    json << "]},\n  \"responsiveness\": {\"maxUiHeartbeatGapMs\": "
         << static_cast<double>(app.benchmarkHeartbeatMaxUs.load()) / 1000.0
         << ", \"inputToPresentMs\": " << static_cast<double>(app.renderThread.SmokeValue(71)) / 1000.0
         << ", \"cancelObservationMs\": null},\n"
         << "  \"memory\": {\"viewerBaselinePrivateBytes\": " << app.benchmarkViewerBaselinePrivate
         << ", \"viewerPeakPrivateBytes\": " << app.benchmarkViewerPeakPrivate.load()
         << ", \"workerPeakPrivateBytes\": " << app.benchmarkWorkerPeakPrivate.load()
         << ", \"viewerPeakMappedBytes\": " << app.benchmarkViewerPeakMapped.load()
         << ", \"workerPeakMappedBytes\": " << app.benchmarkWorkerPeakMapped.load()
         << ", \"viewerPolicyBaselineBytes\": " << app.renderThread.SmokeValue(75)
         << ", \"viewerPolicyCapBytes\": " << app.renderThread.SmokeValue(76)
         << ", \"viewerPrivateAtReportBytes\": " << app.renderThread.SmokeValue(77)
         << ", \"uploadQueuePeakBytes\": " << app.renderThread.SmokeValue(6)
         << ", \"gpuLivePendingRetiredBytes\": " << app.renderThread.SmokeValue(55)
         << ", \"gpuPendingBytes\": " << app.renderThread.SmokeValue(57)
         << ", \"scratchCapBytes\": " << scratchCap
         << ", \"scratchPrivateCommitUpperBoundBytes\": " << app.benchmarkWorkerPeakPrivate.load() << "},\n"
         << "  \"measurementNotes\": [\"Present-return intervals are retained raw; ETW classification is opt-in through the qualification harness.\","
            "\"Mapped committed views are reported separately from private commit.\","
            "\"Worker private commit is a conservative upper bound on scratch, not an allocator-category measurement.\","
            "\"The null cancellation field is not silently treated as a cancellation qualification.\"]\n}\n";
    const auto text = json.str();
    HANDLE output = INVALID_HANDLE_VALUE;
    bool closeOutput = false;
    if (!app.benchmarkResultPath.empty()) {
        output = CreateFileW(app.benchmarkResultPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        closeOutput = output != INVALID_HANDLE_VALUE;
    } else {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        output = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    DWORD written = 0;
    const bool ok = output != INVALID_HANDLE_VALUE && WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
        && written == text.size();
    if (closeOutput) CloseHandle(output);
    if (!ok) exitCode = 3;
    return ok;
}

// The bottom bar and Information panel only reserve screen space while a
// model is actually loaded and navigable (same condition as CanNavigate,
// inlined here since CanNavigate is defined later in this file).
bool HasNavigableModel(const ViewerApp& app)
{
    // Loading/error cards do not suspend interaction with usable prior content.
    return app.renderThread.HasModel();
}

// Reserved bottom-bar VIEWPORT inset (not the bar's own drawn height, see
// OverlayInfo::barBottomBarHeight/BuildOverlayInfo below): space is only reserved
// while a model is loaded, matching HasNavigableModel, and collapsed to 0 in
// Fullscreen so the viewport reclaims that space and fills the whole monitor
// — the bar itself keeps drawing there as a floating toolbar overlaying that
// now-full-bleed viewport instead of vanishing along with the reserved space.
int EffectiveBottomBarHeight(const ViewerApp& app)
{
    return (HasNavigableModel(app) && !app.isFullscreen) ? app.bottomBarHeight : 0;
}

// Reserved title-bar VIEWPORT inset (see EffectiveBottomBarHeight just
// above): the real toolbarHeight normally, but 0 in Fullscreen so the
// viewport, gizmo, and hit-testing all agree the whole client area is now
// live — Fullscreen actually reclaims the space the title bar used to
// occupy, rather than just resizing the window over the taskbar while
// leaving a dead strip of chrome at the top. The title bar keeps drawing
// there regardless, as a floating toolbar over that full-bleed viewport
// (OverlayInfo::barToolbarHeight, always the real height); NC hit-testing
// (WM_NCHITTEST) still uses this Effective value so a topmost, monitor-
// filling window in that state has no HTCAPTION/system-caption-button role,
// while the plain client-area mouse handlers use the real app.toolbarHeight
// instead so the floating toolbar's own buttons stay clickable.
int EffectiveToolbarHeight(const ViewerApp& app)
{
    return app.isFullscreen ? 0 : app.toolbarHeight;
}

// Fixed logical width of the Information panel, docked to the right edge
// while open; 0 when closed or no model is loaded. Mirrors
// InfoPanel::ComputeInfoPanelLayout's own clamp so callers that only need the
// width don't have to build a full layout (and don't recurse into an
// already-narrowed viewport width).
int InfoPanelWidthPixels(const ViewerApp& app)
{
    if (!app.infoPanelVisible || !HasNavigableModel(app)) return 0;
    RECT client{};
    GetClientRect(app.window, &client);
    return static_cast<int>(std::lround(ComputeInfoPanelLayout(
        client.right, client.bottom, app.toolbarHeight, app.bottomBarHeight, app.dpiScale).Width()));
}

// Client-px rect of the Information panel itself (as opposed to
// InfoPanelWidthPixels' viewport-inset width alone) — used to route
// WM_MOUSEWHEEL to the panel's own scrolling instead of camera dolly while
// the cursor is over it. Empty (all zero) when the panel is closed.
RECT InfoPanelRect(const ViewerApp& app)
{
    if (!app.infoPanelVisible || !HasNavigableModel(app)) return RECT{};
    RECT client{};
    GetClientRect(app.window, &client);
    const InfoPanelLayout layout = ComputeInfoPanelLayout(
        client.right, client.bottom, app.toolbarHeight, app.bottomBarHeight, app.dpiScale);
    return RECT{ static_cast<int>(std::lround(layout.left)), static_cast<int>(std::lround(layout.top)),
        static_cast<int>(std::lround(layout.right)), static_cast<int>(std::lround(layout.bottom)) };
}

// Small close button aligned with the fixed "Stats & Shading" header.
// Its center matches the header text row, while the panel body continues to
// start below both controls and scroll independently.
RECT InfoPanelCloseButtonRect(const ViewerApp& app)
{
    const RECT panel = InfoPanelRect(app);
    if (panel.right <= panel.left || panel.bottom <= panel.top) return RECT{};
    const int size = Scale(app, 28);
    const int right = panel.right - Scale(app, 12);
    const int top = panel.top + Scale(app, 15);
    return RECT{ right - size, top, right, top + size };
}

// The root transform currently mapping the selected source/model up axis into
// the app's fixed Z-up world: identity while "show native orientation" is on
// (or there's no model). Camera, grid, and the Information panel reason in this
// transformed (effective) space — see EffectiveBounds below — while picking
// (ClickSelect) must invert it to reach PickMesh's native-space vertices.
DirectX::XMMATRIX ActiveModelTransform(const ViewerApp& app)
{
    if (!app.loadedModel) return DirectX::XMMatrixIdentity();
    return GroundAxisTransform(app.groundAxis, app.loadedModel->source.upAxis,
        app.showNativeOrientation, app.groundAxisInverted);
}

// The bounds actually occupying the app's Z-up world right now. Every
// consumer that used to read ModelData::boundsMin/boundsMax directly for
// display/camera purposes must use this instead, since those raw fields stay
// in the model's native/source space regardless of the toggle.
void EffectiveBounds(const ViewerApp& app, DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax)
{
    if (!app.loadedModel)
    {
        outMin = DirectX::XMFLOAT3{};
        outMax = DirectX::XMFLOAT3{};
        return;
    }
    TransformBounds(app.loadedModel->boundsMin, app.loadedModel->boundsMax,
        ActiveModelTransform(app), outMin, outMax);
}

// How far app.infoPanelScrollOffset may go before the section list's last row
// reaches the panel's bottom edge — WM_MOUSEWHEEL clamps against this on
// every tick (D3D11On12Overlay::DrawInfoPanel clamps again from the same
// ComputeInfoPanelScrollMetrics when it draws, so the two can't disagree).
float InfoPanelMaxScroll(const ViewerApp& app)
{
    if (!app.infoPanelVisible || !app.loadedModel) return 0.0f;
    const RECT panel = InfoPanelRect(app);
    const float panelHeight = static_cast<float>(panel.bottom - panel.top);
    if (panelHeight <= 0.0f) return 0.0f;
    const std::vector<InfoPanelSection> sections = BuildInfoPanelSections(
        *app.loadedModel, app.showNativeOrientation, app.groundAxis);
    const InfoPanelScrollMetrics metrics = ComputeInfoPanelScrollMetrics(sections, app.dpiScale);
    const float visibleHeight = std::max(0.0f, panelHeight - metrics.headerHeight);
    return std::max(0.0f, metrics.contentHeight - visibleHeight);
}

float ViewportAspect(const ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    return static_cast<float>(std::max(1L, client.right - InfoPanelWidthPixels(app))) /
        static_cast<float>(std::max(1L, client.bottom - EffectiveToolbarHeight(app) - EffectiveBottomBarHeight(app)));
}

// The 3D viewport is the client area below the title bar, above the bottom
// bar, and left of the Information panel (when open) — full-window in
// Fullscreen, since both bars collapse to 0 there.
RECT ViewportRect(const ViewerApp& app)
{
    RECT viewport{};
    GetClientRect(app.window, &viewport);
    viewport.top = EffectiveToolbarHeight(app);
    viewport.bottom -= EffectiveBottomBarHeight(app);
    viewport.right -= InfoPanelWidthPixels(app);
    return viewport;
}

// Gate for pointer messages: true only inside the actual 3D viewport, so
// clicks landing in the bottom bar or the Information panel never reach
// camera navigation, gizmo hit-testing, or mesh picking.
bool PointInViewport(const ViewerApp& app, POINT point)
{
    const RECT viewport = ViewportRect(app);
    return PtInRect(&viewport, point) != FALSE;
}

std::wstring FileNameFromPath(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring FormatMultiplier(double value)
{
    std::wostringstream text;
    text << std::fixed << std::setprecision(value >= 10.0 ? 1 : 2) << value;
    return text.str();
}

double HudAlpha(double visibleUntil, double now)
{
    const double remaining = visibleUntil - now;
    if (remaining <= 0.0) return 0.0;
    return static_cast<double>(std::clamp(remaining / kHudFadeSeconds, 0.0, 1.0));
}

void ShowSpeedHud(ViewerApp& app, const std::wstring& text)
{
    app.speedHudText = text;
    app.speedHudUntil = NowSeconds() + kHudVisibleSeconds;
    InvalidateRect(app.window, nullptr, FALSE);
}

void ShowModeHud(ViewerApp& app, const std::wstring& text)
{
    app.modeHudText = text;
    app.modeHudUntil = NowSeconds() + kHudVisibleSeconds;
    InvalidateRect(app.window, nullptr, FALSE);
}

void UpdateTitle(const ViewerApp& app)
{
    std::wstring title = kApplicationName;
    if (!app.filename.empty()) title = app.filename + L" — " + kApplicationName;
    if (app.showFrameStats)
    {
        // Developer instrumentation behind --frame-stats: the title bar is
        // the one surface already readable from outside the process (the
        // screenshot harness reads MainWindowTitle).
        // Snapshot rather than reaching into the render thread's own state.
        // Note this runs only on the UI thread -- the render thread must
        // never call SetWindowTextW, which marshals and would block on the
        // very thread that may be waiting for it.
        const RenderThread::StatsSnapshot stats = app.renderThread.Stats();
        wchar_t buffer[192]{};
        swprintf_s(buffer, L"  ·  %.3f ms mean  %.3f ms p95  %llu frames  %llu occluded  overlay %.3f ms",
                    stats.meanMs, stats.p95Ms, static_cast<unsigned long long>(stats.frames),
                    static_cast<unsigned long long>(stats.occluded), stats.overlayMeanMs);
        title += buffer;
    }
    SetWindowTextW(app.window, title.c_str());
}

bool GetClientPointerPoint(HWND window, UINT32 pointerId, POINT& point)
{
    POINTER_INFO info{};
    if (!GetPointerInfo(pointerId, &info)) return false;
    point = info.ptPixelLocation;
    return ScreenToClient(window, &point) != FALSE;
}

void ResetTouchBaseline(ViewerApp& app)
{
    if (app.touchPoints.empty())
    {
        app.touchCenter = POINT{};
        app.touchSpan = 0.0;
        return;
    }
    auto first = app.touchPoints.begin();
    if (app.touchPoints.size() == 1)
    {
        app.touchCenter = first->second;
        app.touchSpan = 0.0;
        return;
    }
    auto second = std::next(first);
    app.touchCenter = POINT{ (first->second.x + second->second.x) / 2, (first->second.y + second->second.y) / 2 };
    const double x = static_cast<double>(first->second.x - second->second.x);
    const double y = static_cast<double>(first->second.y - second->second.y);
    app.touchSpan = std::sqrt(x * x + y * y);
}

bool CanNavigate(const ViewerApp& app)
{
    return HasNavigableModel(app);
}

bool HasNavigationInput(const ViewerApp& app)
{
    // Flight keys only count as motion while RMB capture is active.
    if (app.flyLook && (app.moveForward || app.moveBackward || app.moveLeft || app.moveRight ||
        app.moveUp || app.moveDown || app.rollLeft || app.rollRight)) return true;
    return app.arrowLeft || app.arrowRight || app.arrowUp || app.arrowDown;
}

void StopNavigation(ViewerApp& app)
{
    app.moveForward = false;
    app.moveBackward = false;
    app.moveLeft = false;
    app.moveRight = false;
    app.moveUp = false;
    app.moveDown = false;
    app.rollLeft = false;
    app.rollRight = false;
    app.arrowLeft = false;
    app.arrowRight = false;
    app.arrowUp = false;
    app.arrowDown = false;
    app.renderThread.LockCamera()->StopMotion();
}

bool SetNavigationKey(ViewerApp& app, WPARAM key, bool pressed)
{
    bool* state = nullptr;
    switch (key)
    {
    case 'W': state = &app.moveForward; break;
    case 'S': state = &app.moveBackward; break;
    case 'A': state = &app.moveLeft; break;
    case 'D': state = &app.moveRight; break;
    case 'E': state = &app.moveUp; break;
    case 'Q': state = &app.moveDown; break;
    case 'Z': state = &app.rollLeft; break;
    case 'C': state = &app.rollRight; break;
    case VK_LEFT: state = &app.arrowLeft; break;
    case VK_RIGHT: state = &app.arrowRight; break;
    case VK_UP: state = &app.arrowUp; break;
    case VK_DOWN: state = &app.arrowDown; break;
    default: return false;
    }
    *state = pressed;
    return true;
}

void UpdateGizmoLayout(ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    app.gizmo.UpdateLayout(client.right - InfoPanelWidthPixels(app), client.bottom,
        EffectiveToolbarHeight(app), EffectiveBottomBarHeight(app), app.dpiScale);
}

void UpdateChromeLayout(ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    app.chrome.UpdateLayout(client.right, app.toolbarHeight, app.dpiScale,
        HasNavigableModel(app), IsZoomed(app.window) != FALSE, app.isFullscreen);
}

// Speed flyout panel/track geometry, shared by drawing (BuildOverlayInfo) and
// input handling (WM_LBUTTONDOWN/MOUSEMOVE) so they can never disagree.
RECT SpeedFlyoutRect(const ViewerApp& app)
{
    const RECT speedButton = app.chrome.Button(Chrome::Part::Speed).rect;
    const int width = Scale(app, 220);
    const int height = Scale(app, 64);
    const int gap = Scale(app, 6);
    RECT client{};
    GetClientRect(app.window, &client);
    int left = std::min(speedButton.left, client.right - Scale(app, 8) - width);
    left = std::max(left, Scale(app, 8));
    const int top = app.toolbarHeight + gap;
    return RECT{ left, top, left + width, top + height };
}

RECT SpeedFlyoutTrackRect(const ViewerApp& app)
{
    const RECT panel = SpeedFlyoutRect(app);
    const int marginX = Scale(app, 16);
    const int trackCenterY = panel.top + Scale(app, 44);
    const int halfHeight = Scale(app, 8);
    return RECT{ panel.left + marginX, trackCenterY - halfHeight, panel.right - marginX, trackCenterY + halfHeight };
}

// Settings popup panel/row/switch geometry, shared by drawing (BuildOverlayInfo)
// and input handling (WM_LBUTTONDOWN) so they can never disagree. Mirrors
// SpeedFlyoutRect/SpeedFlyoutTrackRect just above, anchored under the
// Overflow ("...") button instead of the Speed button.
RECT SettingsPanelRect(const ViewerApp& app)
{
    const RECT overflowButton = app.chrome.Button(Chrome::Part::Overflow).rect;
    const int width = Scale(app, 280);
    const int height = Scale(app, 112);
    const int gap = Scale(app, 6);
    RECT client{};
    GetClientRect(app.window, &client);
    int left = std::min(overflowButton.left, client.right - Scale(app, 8) - width);
    left = std::max(left, Scale(app, 8));
    const int top = app.toolbarHeight + gap;
    return RECT{ left, top, left + width, top + height };
}

RECT SettingsToggleRowRect(const ViewerApp& app)
{
    const RECT panel = SettingsPanelRect(app);
    const int marginX = Scale(app, 16);
    return RECT{ panel.left + marginX, panel.top + Scale(app, 8),
        panel.right - marginX, panel.top + Scale(app, 52) };
}

RECT SettingsSwitchRect(const ViewerApp& app)
{
    const RECT row = SettingsToggleRowRect(app);
    const int switchWidth = Scale(app, 36);
    const int switchHeight = Scale(app, 20);
    const int centerY = (row.top + row.bottom) / 2;
    return RECT{ row.right - switchWidth, centerY - switchHeight / 2, row.right, centerY + switchHeight / 2 };
}

RECT SettingsCursorToggleRowRect(const ViewerApp& app)
{
    const RECT panel = SettingsPanelRect(app);
    const int marginX = Scale(app, 16);
    return RECT{ panel.left + marginX, panel.top + Scale(app, 56),
        panel.right - marginX, panel.bottom - Scale(app, 8) };
}

RECT SettingsCursorSwitchRect(const ViewerApp& app)
{
    const RECT row = SettingsCursorToggleRowRect(app);
    const int switchWidth = Scale(app, 36);
    const int switchHeight = Scale(app, 20);
    const int centerY = (row.top + row.bottom) / 2;
    return RECT{ row.right - switchWidth, centerY - switchHeight / 2, row.right, centerY + switchHeight / 2 };
}

void BeginWrappedDrag(ViewerApp& app, const POINT& point)
{
    app.wrapDrag = true;
    app.lastPointer = point;
    // Keep the cursor inside the viewport during the drag so wrapping can
    // always teleport before a display edge traps the gesture.
    RECT clip = ViewportRect(app);
    MapWindowPoints(app.window, nullptr, reinterpret_cast<LPPOINT>(&clip), 2);
    ClipCursor(&clip);
    if (app.hideCursorWhileDragging) SetCursor(nullptr);
}

void WrapCursorIfNeeded(ViewerApp& app)
{
    if (!app.wrapDrag) return;
    const RECT viewport = ViewportRect(app);
    const LONG margin = std::max<LONG>(2, Scale(app, 10));
    const LONG width = viewport.right - viewport.left;
    const LONG height = viewport.bottom - viewport.top;
    POINT point = app.lastPointer;
    LONG shiftX = 0;
    LONG shiftY = 0;
    if (width > margin * 2 + 2)
    {
        if (point.x <= viewport.left + margin) shiftX = width - margin * 2;
        else if (point.x >= viewport.right - margin) shiftX = -(width - margin * 2);
    }
    if (height > margin * 2 + 2)
    {
        if (point.y <= viewport.top + margin) shiftY = height - margin * 2;
        else if (point.y >= viewport.bottom - margin) shiftY = -(height - margin * 2);
    }
    if (shiftX == 0 && shiftY == 0) return;
    point.x += shiftX;
    point.y += shiftY;
    POINT screen = point;
    ClientToScreen(app.window, &screen);
    SetCursorPos(screen.x, screen.y);
    // The teleport posts one synthetic WM_MOUSEMOVE that lands exactly on
    // this point, so seeding lastPointer here keeps its delta at zero and the
    // drag continues seamlessly from the far side.
    app.lastPointer = point;
}

void EndPointer(ViewerApp& app)
{
    const bool cursorWasHidden = app.hideCursorWhileDragging && app.pointerMode != PointerMode::None;
    if (app.wrapDrag)
    {
        app.wrapDrag = false;
        ClipCursor(nullptr);
    }
    if (app.flyLook)
    {
        app.flyLook = false;
        SetCursorPos(app.flyPressPoint.x, app.flyPressPoint.y);
    }
    if (GetCapture() == app.window) ReleaseCapture();
    app.pointerMode = PointerMode::None;
    if (cursorWasHidden) SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

void TrackOrbitVelocity(ViewerApp& app, float deltaX, float deltaY)
{
    const double now = NowSeconds();
    const double gap = now - app.lastOrbitMoveSeconds;
    if (gap > 0.0005 && gap < 0.15)
    {
        app.orbitVelocityX = app.orbitVelocityX * 0.62 + (deltaX / gap) * 0.38;
        app.orbitVelocityY = app.orbitVelocityY * 0.62 + (deltaY / gap) * 0.38;
    }
    else
    {
        app.orbitVelocityX = 0.0;
        app.orbitVelocityY = 0.0;
    }
    app.lastOrbitMoveSeconds = now;
}

void TrackPanVelocity(ViewerApp& app, float deltaX, float deltaY)
{
    const double now = NowSeconds();
    const double gap = now - app.lastPanMoveSeconds;
    if (gap > 0.0005 && gap < 0.15)
    {
        app.panVelocityX = app.panVelocityX * 0.62 + (deltaX / gap) * 0.38;
        app.panVelocityY = app.panVelocityY * 0.62 + (deltaY / gap) * 0.38;
    }
    else
    {
        app.panVelocityX = 0.0;
        app.panVelocityY = 0.0;
    }
    app.lastPanMoveSeconds = now;
}

void ClickSelect(ViewerApp& app, const POINT& point)
{
    if (!app.loadedModel) return;
    if (app.appSmoke) ++app.smokePickRequests;
    app.renderThread.RequestPick(point.x, point.y, app.loadedModel->source.generationId);
}

void ApplySelection(ViewerApp& app, bool hit)
{
    if (hit && !app.meshSelected)
    {
        app.meshSelected = true;
    }
    else if (!hit && app.meshSelected)
    {
        app.meshSelected = false;
    }
    InvalidateRect(app.window, nullptr, FALSE);
}

void FrameSelectedOrAll(ViewerApp& app)
{
    if (!app.renderThread.HasModel()) return;
    const float aspect = ViewportAspect(app);
    if (app.meshSelected && app.loadedModel)
    {
        DirectX::XMFLOAT3 effectiveMin{};
        DirectX::XMFLOAT3 effectiveMax{};
        EffectiveBounds(app, effectiveMin, effectiveMax);
        app.renderThread.LockCamera()->FrameBox(effectiveMin, effectiveMax, aspect);
    }
    else
    {
        app.renderThread.LockCamera()->Fit(aspect);
    }
    InvalidateRect(app.window, nullptr, FALSE);
}

// Speed flyout slider range (logical 0..kSpeedSliderMax "pixels" along its
// D2D-drawn track — see the Speed flyout drawing/drag code below). Not a
// native trackbar: the flyout always reads app.renderThread.LockCamera()->FlySpeedScale() live
// each frame, so unlike the old always-visible toolbar slider there is no
// separate position to keep synced.
constexpr int kSpeedSliderMax = 100;
constexpr double kSpeedSliderMin = 0.05;   // matches Camera::kFlySpeedMin
constexpr double kSpeedSliderTop = 40.0;   // matches Camera::kFlySpeedMax

int SpeedSliderPositionFor(double speedScale)
{
    const double t = std::log(std::clamp(speedScale, kSpeedSliderMin, kSpeedSliderTop) / kSpeedSliderMin) /
        std::log(kSpeedSliderTop / kSpeedSliderMin);
    return static_cast<int>(std::lround(t * kSpeedSliderMax));
}

double SpeedForSliderPosition(int position)
{
    const double t = std::clamp(static_cast<double>(position), 0.0, static_cast<double>(kSpeedSliderMax)) / kSpeedSliderMax;
    return kSpeedSliderMin * std::pow(kSpeedSliderTop / kSpeedSliderMin, t);
}

void AdjustFlySpeed(ViewerApp& app, float wheelSteps)
{
    // One lock for the read-modify-write. Two LockCamera() calls in a single
    // expression would both be alive at once and self-deadlock -- std::mutex
    // is not recursive.
    double scaled = 0.0;
    {
        auto cam = app.renderThread.LockCamera();
        cam->SetFlySpeedScale(cam->FlySpeedScale() * std::pow(1.18, static_cast<double>(wheelSteps)));
        scaled = cam->FlySpeedScale();
    }
    ShowSpeedHud(app, L"Travel speed ×" + FormatMultiplier(scaled));
}

// Direct manipulation: sets speed immediately from a drag position within
// the Speed flyout's track (SpeedFlyoutTrackRect), same non-eased feel as
// the zoom slider's own direct-drag handling.
void SetFlySpeedFromFlyoutX(ViewerApp& app, int clientX)
{
    const RECT track = SpeedFlyoutTrackRect(app);
    const float t = std::clamp(static_cast<float>(clientX - track.left) / static_cast<float>(std::max(1L, track.right - track.left)), 0.0f, 1.0f);
    app.renderThread.LockCamera()->SetFlySpeedScale(SpeedForSliderPosition(static_cast<int>(std::lround(t * kSpeedSliderMax))));
}

constexpr int kZoomSliderMax = 1000;

// Zoom slider position increases as the camera moves closer (more zoomed
// in). Distance range mirrors Camera::Dolly's own clamp (Renderer.cpp) so
// the slider can always reach the same extremes wheel-zoom can, and is
// recomputed from the live scene radius rather than cached, since it
// changes with every loaded model.
int ZoomSliderPositionFor(const Camera& camera)
{
    const double minDistance = std::max(1e-6, camera.sceneRadius * 0.025);
    const double maxDistance = std::max(minDistance * 1.0001, camera.sceneRadius * 250.0);
    const double distance = std::clamp(camera.distance, minDistance, maxDistance);
    const double t = std::log(maxDistance / distance) / std::log(maxDistance / minDistance);
    return static_cast<int>(std::lround(std::clamp(t, 0.0, 1.0) * kZoomSliderMax));
}

double ZoomDistanceForSliderPosition(const Camera& camera, int position)
{
    const double minDistance = std::max(1e-6, camera.sceneRadius * 0.025);
    const double maxDistance = std::max(minDistance * 1.0001, camera.sceneRadius * 250.0);
    const double t = std::clamp(static_cast<double>(position), 0.0, static_cast<double>(kZoomSliderMax)) / kZoomSliderMax;
    return maxDistance * std::pow(minDistance / maxDistance, t);
}

float ZoomPercentFor(const Camera& camera)
{
    return static_cast<float>(100.0 * camera.homeDistance / std::max(1e-6, camera.distance));
}

// Logical width of the small icon buttons docked in the bottom bar (Info,
// Fullscreen) — see InfoButtonRect/FullscreenButtonRect below.
constexpr int kBottomBarButtonWidth = 34;
constexpr int kBottomBarButtonHeight = 28;

// Bottom-bar button rect centered vertically in the bar, right edge at
// `right` — shared layout math for the Fullscreen toggle (below). Always
// measured from the full client width, not the (possibly narrower) viewport
// left of the Information panel — the bottom bar spans the whole window and
// sits below where that panel stops (InfoPanel::ComputeInfoPanelLayout), so
// anchoring here to the client edge instead means opening/closing the panel
// never shifts these buttons.
RECT BottomBarButtonRect(const ViewerApp& app, int right)
{
    RECT client{};
    GetClientRect(app.window, &client);
    const int width = Scale(app, kBottomBarButtonWidth);
    const int height = Scale(app, kBottomBarButtonHeight);
    const int barY = client.bottom - app.bottomBarHeight;
    const int centerY = barY + app.bottomBarHeight / 2;
    return RECT{ right - width, centerY - height / 2, right, centerY + height / 2 };
}

// Fullscreen toggle: the very right of the bottom bar, after the zoom
// slider and its percent-text readout.
RECT FullscreenButtonRect(const ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    const int margin = Scale(app, 14);
    return BottomBarButtonRect(app, client.right - margin);
}

// Compact zoom slider, docked in the bottom-right of the bottom bar just
// left of the D2D-drawn percent readout (DrawBottomBar's labelWidth,
// Renderer.cpp — mirrored here so the two rects never drift apart), which
// in turn sits left of the Fullscreen button. Shared by drawing
// (BuildOverlayInfo) and input handling (WM_LBUTTONDOWN/MOUSEMOVE), the same
// split as the Speed flyout track above.
RECT ZoomTrackRect(const ViewerApp& app)
{
    const RECT fullscreenButton = FullscreenButtonRect(app);
    const int percentLabelWidth = Scale(app, 56);
    const int trackWidth = Scale(app, 110);
    const int gap = Scale(app, 14);
    const int right = fullscreenButton.left - gap - percentLabelWidth - gap;
    const int left = right - trackWidth;
    RECT client{};
    GetClientRect(app.window, &client);
    const int barY = client.bottom - app.bottomBarHeight;
    const int centerY = barY + app.bottomBarHeight / 2;
    const int halfHeight = Scale(app, 8);
    return RECT{ left, centerY - halfHeight, right, centerY + halfHeight };
}

// Info panel toggle: docked at the very left of the bottom bar, independent
// of the zoom/fullscreen group anchored to the right — so it never moves
// when the Information panel it opens and closes changes the viewport width,
// and the zoom/fullscreen group never moves when it's clicked.
RECT InfoButtonRect(const ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    const int margin = Scale(app, 14);
    const int width = Scale(app, kBottomBarButtonWidth);
    const int height = Scale(app, kBottomBarButtonHeight);
    const int barY = client.bottom - app.bottomBarHeight;
    const int centerY = barY + app.bottomBarHeight / 2;
    return RECT{ margin, centerY - height / 2, margin + width, centerY + height / 2 };
}

struct LightingToolbarLayout
{
    RECT bounds{};
    RECT studio{};
    RECT clay{};
    RECT directional{};
    RECT wireframe{};
};

// Centered segmented lighting control. It floats within the bottom chrome
// independently of the left information and right zoom groups, matching the
// compact transient toolbars used by Windows Photos editing surfaces. The
// Directional light has no slider here: its rotation lives on the navigation
// gizmo's outer ring (drag the sun), so the strip stays four equal buttons in
// every mode.
LightingToolbarLayout ComputeLightingToolbarLayout(const ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window,&client);
    const int buttonHeight=Scale(app,30);
    const int padding=Scale(app,5);
    const int gap=Scale(app,3);
    // Icon-only segmented control: every shading mode is the same square
    // button, so the glyphs read as one Blender-style strip instead of
    // leaving empty space where a text label used to sit.
    const int buttonWidth=Scale(app,34);
    const int studioWidth=buttonWidth;
    const int clayWidth=buttonWidth;
    const int directionalWidth=buttonWidth;
    const int wireWidth=buttonWidth;
    const int width=padding*2+studioWidth+clayWidth+directionalWidth+wireWidth+gap*3;
    const int centerY=client.bottom-app.bottomBarHeight/2;
    int left=(client.right-width)/2;
    const int safeLeft=InfoButtonRect(app).right+Scale(app,110);
    const int safeRight=ZoomTrackRect(app).left-Scale(app,10);
    if (safeRight-safeLeft>=width) left=std::clamp(left,safeLeft,safeRight-width);
    LightingToolbarLayout layout;
    layout.bounds={left,centerY-buttonHeight/2-padding,left+width,centerY+buttonHeight/2+padding};
    int x=left+padding;
    layout.studio={x,centerY-buttonHeight/2,x+studioWidth,centerY+buttonHeight/2};x+=studioWidth+gap;
    layout.clay={x,centerY-buttonHeight/2,x+clayWidth,centerY+buttonHeight/2};x+=clayWidth+gap;
    layout.directional={x,centerY-buttonHeight/2,x+directionalWidth,centerY+buttonHeight/2};x+=directionalWidth+gap;
    layout.wireframe={x,centerY-buttonHeight/2,x+wireWidth,centerY+buttonHeight/2};x+=wireWidth;
    return layout;
}

int HitLightingButton(const ViewerApp& app,POINT point)
{
    const auto layout=ComputeLightingToolbarLayout(app);
    if (PtInRect(&layout.studio,point)) return 0;
    if (PtInRect(&layout.clay,point)) return 1;
    if (PtInRect(&layout.directional,point)) return 2;
    if (PtInRect(&layout.wireframe,point)) return 3;
    return -1;
}

// "Directional light  315° / 29°" — azimuth then elevation, both in degrees.
std::wstring DirectionalLightHudText(const ViewerApp& app)
{
    constexpr float kRadiansToDegrees = 57.29577951308232f;
    return L"Directional light  "
        + std::to_wstring(static_cast<int>(std::lround(app.directionalLightAngle * 360.0f))) + L"\u00B0"
        + L" / " + std::to_wstring(static_cast<int>(std::lround(app.directionalLightElevation * kRadiansToDegrees))) + L"\u00B0";
}

// Rotates and raises the directional light so its gizmo sun lands under the
// pointer. Returns false only when the pointer cannot define either an angle
// (dead center) or an elevation. The horizontal position sets the azimuth and
// the distance from center sets elevation, so dragging the sun inward raises it.
bool SetDirectionalLightFromGizmoPoint(ViewerApp& app, POINT point)
{
    const auto orientation = app.renderThread.LockCamera()->Orientation();
    const float x = static_cast<float>(point.x);
    const float y = static_cast<float>(point.y);
    float angle = app.directionalLightAngle;
    const bool haveAngle = app.gizmo.LightAngleForPoint(orientation, x, y, app.directionalLightAngle, angle);
    float elevation = app.directionalLightElevation;
    const bool haveElevation = app.gizmo.LightElevationForPoint(x, y, elevation);
    if (!haveAngle && !haveElevation) return false;
    if (haveAngle) app.directionalLightAngle = angle;
    if (haveElevation) app.directionalLightElevation = elevation;
    app.modeHudText = DirectionalLightHudText(app);
    app.modeHudUntil=NowSeconds()+kHudVisibleSeconds;
    InvalidateRect(app.window,nullptr,FALSE);
    return true;
}

void SetLightingMode(ViewerApp& app,LightingMode mode)
{
    app.lightingMode=mode;
    app.modeHudText=mode==LightingMode::Studio ? L"Studio lighting"
        : mode==LightingMode::Clay ? L"Clay / Solid"
        : mode==LightingMode::Directional ? DirectionalLightHudText(app) : L"Wireframe";
    app.modeHudUntil=NowSeconds()+kHudVisibleSeconds;
    InvalidateRect(app.window,nullptr,FALSE);
}

// What ComputeTooltipInfo resolves the current hover into: an id (0 == none,
// otherwise unique per button so UpdateTooltipTracking can tell "still this
// button" from "moved to a new one"), the button's rect (so the bubble can
// anchor to it even after the mouse moves on), its label, and which side of
// the button the bubble belongs on.
struct TooltipInfo
{
    int id = 0;
    RECT rect{};
    std::wstring text;
    bool below = true;   // true: title-bar buttons; false: bottom-bar buttons
};

TooltipInfo ComputeTooltipInfo(const ViewerApp& app)
{
    // Suppressed mid-interaction (dragging a slider, a button already
    // pressed, the Speed flyout open) rather than just delayed, so a tooltip
    // never appears over something the user is actively using.
    if (app.chrome.pressed != Chrome::Part::None || app.infoButtonPressed || app.infoPanelCloseButtonPressed ||
        app.fullscreenButtonPressed || app.speedSliderDragging || app.zoomSliderDragging ||
        app.lightingButtonPressed>=0 ||
        app.speedFlyoutOpen || app.settingsPanelOpen)
    {
        return {};
    }
    if (app.chrome.hover == Chrome::Part::GroundAxis && app.loadedModel)
    {
        const GroundAxis current = ResolveGroundAxis(app.groundAxis, app.loadedModel->source.upAxis);
        const GroundAxis next = NextGroundAxis(current, app.loadedModel->source.upAxis);
        return { static_cast<int>(Chrome::Part::GroundAxis) + 1,
            app.chrome.Button(Chrome::Part::GroundAxis).rect,
            std::wstring(L"Ground axis ") + GroundAxisName(current) + L"; click for " + GroundAxisName(next), true };
    }
    if (app.chrome.hover == Chrome::Part::GroundDirection && app.loadedModel)
    {
        const GroundAxis axis = ResolveGroundAxis(app.groundAxis, app.loadedModel->source.upAxis);
        const wchar_t* currentSign = app.groundAxisInverted ? L"-" : L"+";
        const wchar_t* nextSign = app.groundAxisInverted ? L"+" : L"-";
        return { static_cast<int>(Chrome::Part::GroundDirection) + 1,
            app.chrome.Button(Chrome::Part::GroundDirection).rect,
            std::wstring(currentSign) + GroundAxisName(axis) + L" is up; click for " + nextSign + GroundAxisName(axis), true };
    }
    struct Entry { Chrome::Part part; const wchar_t* text; };
    static constexpr Entry kEntries[] = {
        { Chrome::Part::Grid, L"Toggle ground grid" },
        { Chrome::Part::AxisSnap, L"Snap truck to axis" },
        { Chrome::Part::Speed, L"Flight speed" },
        { Chrome::Part::Fit, L"Frame model in view" },
        { Chrome::Part::Reset, L"Reset view" },
        { Chrome::Part::Share, L"Share" },
        { Chrome::Part::Overflow, L"More options" },
        { Chrome::Part::OpenWith, L"Open with" },
    };
    for (const Entry& entry : kEntries)
    {
        if (app.chrome.hover == entry.part)
        {
            return { static_cast<int>(entry.part) + 1, app.chrome.Button(entry.part).rect, entry.text, true };
        }
    }
    if (app.infoButtonHover) return { 100, InfoButtonRect(app), L"Model information", false };
    if (app.fullscreenButtonHover) return { 101, FullscreenButtonRect(app), L"Fullscreen", false };
    if (app.infoPanelCloseButtonHover) return { 102, InfoPanelCloseButtonRect(app), L"Close information panel", true };
    if (app.lightingButtonHover>=0) {
        const auto layout=ComputeLightingToolbarLayout(app);
        const RECT rects[]={layout.studio,layout.clay,layout.directional,layout.wireframe};
        static constexpr const wchar_t* labels[]={
            L"Studio: neutral material lighting",L"Clay: inspect geometry without textures",
            L"Directional: rotate a sharp inspection light",L"Wireframe: show only mesh edges"};
        return {110+app.lightingButtonHover,rects[app.lightingButtonHover],labels[app.lightingButtonHover],false};
    }
    return {};
}

// Re-evaluates the hovered button and (re)starts/cancels the hover-delay
// timer whenever it changes. Called from every place chrome.hover,
// chrome.pressed, infoButtonHover/Pressed, or fullscreenButtonHover/Pressed
// can change, so the tooltip always tracks the true current hover target
// without needing its own dedicated mouse-tracking.
void UpdateTooltipTracking(ViewerApp& app)
{
    const TooltipInfo info = ComputeTooltipInfo(app);
    if (info.id == app.tooltipTargetId) return;
    const bool wasVisible = app.tooltipVisible;
    app.tooltipTargetId = info.id;
    app.tooltipAnchorRect = info.rect;
    app.tooltipText = info.text;
    app.tooltipAnchorBelow = info.below;
    app.tooltipVisible = false;
    KillTimer(app.window, kTooltipTimerId);
    if (info.id != 0) SetTimer(app.window, kTooltipTimerId, kTooltipDelayMs, nullptr);
    if (wasVisible) InvalidateRect(app.window, nullptr, FALSE);
}

// Direct manipulation: sets distance immediately (no easing), so the view
// tracks the thumb 1:1 while dragging, unlike wheel zoom which eases toward
// targetDistance.
void SetZoomFromTrackX(ViewerApp& app, int clientX)
{
    const RECT track = ZoomTrackRect(app);
    const float t = std::clamp(static_cast<float>(clientX - track.left) / static_cast<float>(std::max(1L, track.right - track.left)), 0.0f, 1.0f);
    const double distance = ZoomDistanceForSliderPosition(*app.renderThread.LockCamera(), static_cast<int>(std::lround(t * kZoomSliderMax)));
    app.renderThread.LockCamera()->distance = distance;
    app.renderThread.LockCamera()->targetDistance = distance;
}

// Guards toggle commands against keyboard auto-repeat: holding a key must not
// strobe the grid or the projection mode.
bool ConsumeToggleCommand(ViewerApp& app, int id)
{
    const double now = NowSeconds();
    if (app.lastToggleId == id && now - app.lastToggleSeconds < kToggleRepeatGuardSeconds) return false;
    app.lastToggleId = id;
    app.lastToggleSeconds = now;
    return true;
}

void ToggleGrid(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_GRID)) return;
    app.gridVisible = !app.gridVisible;
    ShowModeHud(app, app.gridVisible ? L"Ground grid shown" : L"Ground grid hidden");
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleProjection(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_PROJECTION)) return;
    const bool toOrthographic = app.renderThread.LockCamera()->Projection() == ProjectionMode::Perspective;
    app.renderThread.LockCamera()->SetProjection(toOrthographic ? ProjectionMode::Orthographic : ProjectionMode::Perspective);
    ShowModeHud(app, toOrthographic ? L"Orthographic" : L"Perspective");
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleAxisSnap(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_AXIS_SNAP)) return;
    app.axisSnapEnabled = !app.axisSnapEnabled;
    ShowModeHud(app, app.axisSnapEnabled ? L"Axis snap on" : L"Axis snap off");
    InvalidateRect(app.window, nullptr, FALSE);
}

void SaveViewerPreferences(const ViewerApp& app)
{
    ViewerSettings settings;
    settings.showNativeOrientation = app.showNativeOrientation;
    settings.groundAxis = app.groundAxis;
    settings.groundAxisInverted = app.groundAxisInverted;
    settings.hideCursorWhileDragging = app.hideCursorWhileDragging;
    SaveSettings(settings);
}

void CycleGroundAxis(ViewerApp& app)
{
    if (!CanNavigate(app) || !app.loadedModel || !ConsumeToggleCommand(app, ID_VIEW_GROUND_AXIS)) return;
    app.groundAxis = NextGroundAxis(app.groundAxis, app.loadedModel->source.upAxis);
    // An explicit grounding choice must be visible, so it supersedes native
    // orientation while retaining the existing setting as a separate toggle.
    app.showNativeOrientation = false;

    DirectX::XMFLOAT3 effectiveMin{};
    DirectX::XMFLOAT3 effectiveMax{};
    EffectiveBounds(app, effectiveMin, effectiveMax);
    app.renderThread.LockCamera()->SetBounds(effectiveMin, effectiveMax, ViewportAspect(app));
    ShowModeHud(app, std::wstring(L"Ground axis ") + GroundAxisName(app.groundAxis));
    SaveViewerPreferences(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleGroundDirection(ViewerApp& app)
{
    if (!CanNavigate(app) || !app.loadedModel || !ConsumeToggleCommand(app, ID_VIEW_GROUND_DIRECTION)) return;
    app.groundAxisInverted = !app.groundAxisInverted;
    app.showNativeOrientation = false;

    DirectX::XMFLOAT3 effectiveMin{};
    DirectX::XMFLOAT3 effectiveMax{};
    EffectiveBounds(app, effectiveMin, effectiveMax);
    app.renderThread.LockCamera()->SetBounds(effectiveMin, effectiveMax, ViewportAspect(app));
    const GroundAxis axis = ResolveGroundAxis(app.groundAxis, app.loadedModel->source.upAxis);
    ShowModeHud(app, std::wstring(L"Ground direction ") +
        (app.groundAxisInverted ? L"-" : L"+") + GroundAxisName(axis) + L" up");
    SaveViewerPreferences(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

// Persisted independently of whether a file is currently open (it's a
// standing preference, not a per-document action), but only re-homes the
// camera/grid when a model is actually loaded to apply against.
void ToggleShowNativeOrientation(ViewerApp& app)
{
    app.showNativeOrientation = !app.showNativeOrientation;
    if (HasNavigableModel(app) && app.loadedModel)
    {
        DirectX::XMFLOAT3 effectiveMin{};
        DirectX::XMFLOAT3 effectiveMax{};
        EffectiveBounds(app, effectiveMin, effectiveMax);
        // An instant re-home (not Fit/Reset, which animate): the model just
        // jumped ~90 degrees, so the old camera pose has no useful
        // relationship to the new one — treat this exactly like a fresh open.
        app.renderThread.LockCamera()->SetBounds(effectiveMin, effectiveMax, ViewportAspect(app));
        ShowModeHud(app, app.showNativeOrientation ? L"Native orientation" : L"Normalized orientation");
    }
    InvalidateRect(app.window, nullptr, FALSE);
    SaveViewerPreferences(app);
}

void ToggleHideCursorWhileDragging(ViewerApp& app)
{
    app.hideCursorWhileDragging = !app.hideCursorWhileDragging;
    InvalidateRect(app.window, nullptr, FALSE);
    SaveViewerPreferences(app);
}

void SnapViewCommand(ViewerApp& app, ViewDir view)
{
    if (!CanNavigate(app)) return;
    // Ctrl+Numpad1/3/7 selects the reverse views (Back, Left, Bottom).
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
    {
        if (view == ViewDir::Front) view = ViewDir::Back;
        else if (view == ViewDir::Right) view = ViewDir::Left;
        else if (view == ViewDir::Top) view = ViewDir::Bottom;
    }
    app.renderThread.LockCamera()->SnapToView(CanonicalViewOrientation(view));
    InvalidateRect(app.window, nullptr, FALSE);
}

void SetControlVisible(HWND control, bool visible)
{
    if (control && ((GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0) != visible)
        ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
}

void UpdateButtonAvailability(ViewerApp& app)
{
    SetControlVisible(app.retryButton, app.state == ViewerState::Failed);
    SetControlVisible(app.openAnotherButton, app.state == ViewerState::Failed);
    SetControlVisible(app.copyButton, app.state == ViewerState::Failed);
    EnableWindow(app.retryButton, app.failedPath.empty() ? FALSE : TRUE);
}

void LayoutControls(ViewerApp& app)
{
    if (!app.window) return;
    RECT client{};
    GetClientRect(app.window, &client);
    const RECT card = CalculateErrorCardRect(client.right, client.bottom, app.toolbarHeight, app.dpiScale);
    const int actionHeight = Scale(app, 32);
    const int actionGap = Scale(app, 8);
    const int retryWidth = Scale(app, 78);
    const int anotherWidth = Scale(app, 112);
    const int copyWidth = Scale(app, 104);
    int actionX = card.left + Scale(app, 28);
    const int actionY = card.bottom - Scale(app, 52);
    MoveWindow(app.retryButton, actionX, actionY, retryWidth, actionHeight, TRUE); actionX += retryWidth + actionGap;
    MoveWindow(app.openAnotherButton, actionX, actionY, anotherWidth, actionHeight, TRUE); actionX += anotherWidth + actionGap;
    MoveWindow(app.copyButton, actionX, actionY, copyWidth, actionHeight, TRUE);

}

// Toggling the panel changes the viewport width (InfoPanelWidthPixels), so
// it needs the same full relayout a resize would trigger.
void SetInfoPanelVisible(ViewerApp& app, bool visible)
{
    if (app.infoPanelVisible == visible) return;
    app.infoPanelVisible = visible;
    if (!visible)
    {
        app.infoPanelScrollOffset = 0.0f;
        app.infoPanelCloseButtonHover = false;
        app.infoPanelCloseButtonPressed = false;
        if (app.keyboardControl == viewer_accessibility::Control::InfoPanelClose)
            app.keyboardControl = viewer_accessibility::Control::Info;
    }
    LayoutControls(app);
    UpdateGizmoLayout(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleInfoPanel(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_INFO)) return;
    SetInfoPanelVisible(app, !app.infoPanelVisible);
}

void CloseInfoPanel(ViewerApp& app)
{
    SetInfoPanelVisible(app, false);
}

// Immersive-fullscreen toggle: expands the window to exactly cover its
// current monitor (hiding the OS resize border/drop-shadow/rounded corners,
// same recipe as most "fake fullscreen" apps), raises it above the taskbar
// (HWND_TOPMOST — matching monitor bounds alone doesn't make Explorer hide a
// plain top-level window behind it), and hides our own D2D title bar and
// bottom bar (EffectiveToolbarHeight/EffectiveBottomBarHeight, Preview3D.cpp;
// DrawOverlay, D3D11On12Overlay.cpp) so the viewport fills the whole screen — Maximize
// instead snaps to the work area, keeps the taskbar and our chrome visible,
// and never goes topmost, so the two stay distinct. Independent of whether a
// model is loaded, same as the caption buttons (while windowed).
void ToggleFullscreen(ViewerApp& app)
{
    if (!ConsumeToggleCommand(app, ID_VIEW_FULLSCREEN)) return;
    if (!app.isFullscreen)
    {
        app.savedWindowPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(app.window, &app.savedWindowPlacement);
        MONITORINFO monitorInfo{ sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(MonitorFromWindow(app.window, MONITOR_DEFAULTTOPRIMARY), &monitorInfo)) return;
        const int noRound = 1;   // DWMWCP_DONOTROUND: avoid corner clipping artifacts at exact monitor bounds
        DwmSetWindowAttribute(app.window, 33, &noRound, sizeof(noRound));
        app.isFullscreen = true;
        SetWindowPos(app.window, HWND_TOPMOST, monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    else
    {
        app.isFullscreen = false;
        const int cornerPreference = 2;   // DWMWCP_ROUNDSMALL, matches WM_CREATE
        DwmSetWindowAttribute(app.window, 33, &cornerPreference, sizeof(cornerPreference));
        SetWindowPlacement(app.window, &app.savedWindowPlacement);
        // HWND_NOTOPMOST undoes the HWND_TOPMOST above — otherwise the window
        // would stay pinned above every other app (taskbar included) even
        // after returning to windowed/maximized.
        SetWindowPos(app.window, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    LayoutControls(app);
    UpdateGizmoLayout(app);
    UpdateChromeLayout(app);
    InvalidateRect(app.window, nullptr, TRUE);
}

void RecreateButtonFont(ViewerApp& app)
{
    if (app.buttonFont) DeleteObject(app.buttonFont);
    app.buttonFont = CreateFontW(-Scale(app, 12), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    for (HWND button : { app.retryButton, app.openAnotherButton, app.copyButton })
    {
        if (button) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(app.buttonFont), TRUE);
    }
}

HWND CreateButton(ViewerApp& app, int id, const wchar_t* text)
{
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 10, 10, app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.instance, nullptr);
}

LRESULT CALLBACK ErrorButtonAccessibilitySubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR)
{
    // The top-level fragment provider owns the automation metadata for these
    // HWND buttons; their native tab, click, default-button, and dialog input
    // behavior remains handled by the standard button window procedure.
    if (message == WM_GETOBJECT && (static_cast<LONG>(lParam) == UiaRootObjectId ||
        static_cast<LONG>(lParam) == OBJID_CLIENT)) return 0;
    return DefSubclassProc(window, message, wParam, lParam);
}

void AddTooltip(ViewerApp& app, HWND control, const wchar_t* text)
{
    TOOLINFOW info{ sizeof(info) };
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = app.window;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(app.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

void CreateControls(ViewerApp& app)
{
    // Fit/Reset/Grid/Snap/Speed/Info/Share/Overflow/Open-With and the system
    // min/max/close now live in the D2D-drawn title bar (Chrome +
    // D3D11On12Overlay::DrawTitleBar) instead of as owner-drawn child buttons — see
    // the WM_NCHITTEST/WM_LBUTTONDOWN handling in WindowProcedure. The zoom
    // slider is D2D-drawn too (ZoomTrackRect/DrawBottomBar) with its own
    // pointer handling, same split as the Speed flyout. Only the
    // error-state action buttons remain real HWND controls.
    app.retryButton = CreateButton(app, ID_VIEW_RETRY, L"Retry");
    app.openAnotherButton = CreateButton(app, ID_VIEW_OPEN_ANOTHER, L"Open another");
    app.copyButton = CreateButton(app, ID_VIEW_COPY_DETAILS, L"Copy details");
    for (HWND button : { app.retryButton, app.openAnotherButton, app.copyButton })
        SetWindowSubclass(button, ErrorButtonAccessibilitySubclass, 1, 0);
    app.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, app.window, nullptr, app.instance, nullptr);
    SetWindowPos(app.tooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(app.tooltip, TTM_SETMAXTIPWIDTH, 0, Scale(app, 360));
    AddTooltip(app, app.retryButton, L"Try opening this file again");
    AddTooltip(app, app.openAnotherButton, L"Choose a different supported 3D model");
    AddTooltip(app, app.copyButton, L"Copy technical error details without the file path");
    RecreateButtonFont(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
}

void SyncDisplayedModel(ViewerApp& app)
{
    if (const auto displayed = app.renderThread.DisplaySnapshot()) {
        if (!app.loadedModel || app.loadedModel->source.generationId != displayed->metadata->source.generationId) app.meshSelected = false;
        app.loadedModel = displayed->metadata;
        app.currentPath = displayed->path;
        app.warning = displayed->metadata->warning;
        if (displayed->metadata->source.format == model_core::SourceFormatId::Step
            && displayed->metadata->importStatus.optionalFeatureWarnings)
            app.warning = L"Some STEP surfaces or assembly items could not be displayed.";
    }
}

void SetFailure(ViewerApp& app, const std::wstring& summary, const std::wstring& details,
    const std::wstring& failedPath = {},
    model_core::ImportErrorCode code = model_core::ImportErrorCode::InternalImporterFailure,
    import_broker::ImportStage stage = import_broker::ImportStage::OpenSource,
    model_core::ImportFailurePhase phase = model_core::ImportFailurePhase::Unspecified)
{
    if (app.cancellation) app.cancellation->store(true, std::memory_order_relaxed);
    app.renderThread.CancelUploads();
    SyncDisplayedModel(app);
    app.state = ViewerState::Failed;
    app.errorSummary = summary;
    app.errorDetails = details;
    app.failedPath = failedPath;
    app.diagnosticPath = failedPath;
    app.errorCode = code;
    app.errorStage = stage;
    app.errorPhase = phase;
    StopNavigation(app);
    EndPointer(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
    InvalidateRect(app.window, nullptr, FALSE);
    viewer_accessibility::Announce(app.window, app.uiaAccessible, AccessibilityStatus(app));
}

void CancelOpen(ViewerApp& app)
{
    if (app.state != ViewerState::Loading) return;
    if (app.cancellation) app.cancellation->store(true, std::memory_order_relaxed);
    ++app.generation;
    app.renderThread.CancelUploads();
    app.cancellation.reset();
    SyncDisplayedModel(app);
    app.state = app.renderThread.HasModel()
        ? (app.renderThread.HasCompleteModel() && app.loadedModel && app.loadedModel->boundsVerified ? ViewerState::Ready : ViewerState::Partial)
        : ViewerState::Empty;
    if (app.renderThread.HasModel()) app.filename = FileNameFromPath(app.currentPath);
    else app.filename.clear();
    UpdateTitle(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
    InvalidateRect(app.window, nullptr, FALSE);
    viewer_accessibility::Announce(app.window, app.uiaAccessible, AccessibilityStatus(app));
}

void BeginOpen(ViewerApp& app, std::wstring path)
{
    if (path.empty()) return;
    ++app.generation;
    if (app.cancellation)
    {
        app.cancellation->store(true, std::memory_order_relaxed);
        app.cancellation.reset();
    }
    if (path.rfind(L"\\\\", 0) == 0 && path.rfind(L"\\\\?\\", 0) != 0)
    {
        app.filename = FileNameFromPath(path);
        UpdateTitle(app);
        SetFailure(app, L"Remote model paths are not opened.",
            L"Choose a supported model stored on a local drive for this viewer.", path, model_core::ImportErrorCode::UnsafeReference);
        return;
    }
    const auto d3d12Format = d3d12_import_bridge::ClassifyByExtension(path);
    if (!d3d12Format)
    {
        app.filename = FileNameFromPath(path);
        UpdateTitle(app);
        SetFailure(app, L"This model format is not supported.",
            L"Open a .glb, .gltf, .stl, .ply, .obj, .fbx, .3mf, .usd, .usda, .usdc, .usdz, .step, or .stp file. Other model formats are deferred.", path, model_core::ImportErrorCode::UnsupportedFormat);
        return;
    }

    const auto alive = app.alive;
    const std::uint64_t generation = app.generation;
    const HWND window = app.window;

    app.diagnosticPath = path;
    app.stepProgressPhase.store(0, std::memory_order_relaxed);
    app.stepProgressDone.store(0, std::memory_order_relaxed);
    app.stepProgressTotal.store(0, std::memory_order_relaxed);
    app.state = ViewerState::Loading;
    app.renderStartedMicroseconds = NowMicroseconds();
    app.renderPresentationPending = false;
    app.renderDurationText.clear();
    app.renderThread.NotifyLoadingStarted(app.generation);
    StopNavigation(app);
    EndPointer(app);
    app.failedPath.clear();
    app.errorSummary.clear();
    app.errorDetails.clear();
    app.filename = FileNameFromPath(path);
    app.warning.clear();
    UpdateTitle(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
    InvalidateRect(app.window, nullptr, FALSE);
    viewer_accessibility::Announce(app.window, app.uiaAccessible, AccessibilityStatus(app));

    // The token abandons superseded imports and imports still running at close.
    // Catch exceptions here: escaping a std::thread entry would terminate the app.
    app.cancellation = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = app.cancellation;

    d3d12_import_bridge::SourceFormat format = *d3d12Format;
    auto sink = app.renderThread.BeginImport(generation, path, cancellation);
    const uint64_t sectionBytes = app.renderThread.SmokeSectionBytes();
    const bool delayBatches = app.renderThread.DelayBatches();
    const uint32_t faultForTesting = app.appSmoke ? app.faultForTesting : 0;
    auto detailSource = app.renderThread.DetailSource(generation);
    // The STEP host is a dedicated OCCT payload whose Job allows
    // min(4 GiB, 35% of RAM); the general Tier-B scratch cap would reject a
    // legitimate large transfer as a resource limit. The host Job remains the
    // hard bound.
    const uint64_t hostCommitCap = format == d3d12_import_bridge::SourceFormat::Step
        ? import_broker::DedicatedHostCommitLimitBytes() : 0;
    auto cpuGuard = app.renderThread.CpuBudgetGuard(hostCommitCap);
    auto* stepPhase = &app.stepProgressPhase;
    auto* stepDone = &app.stepProgressDone;
    auto* stepTotal = &app.stepProgressTotal;
    app.importThreads.emplace_back([window, generation, path, format, alive, cancellation, sink, detailSource, cpuGuard, delayBatches, sectionBytes, faultForTesting, stepPhase, stepDone, stepTotal]()
    {
        d3d12_import_bridge::ImportResult result;
        bool initialComplete = false;
        auto complete = [&](const model_core::FileIdentity& identity) {
            initialComplete = true;
            if (!alive->load() || cancellation->load()) return;
            d3d12_import_bridge::ImportResult terminal; terminal.ok=true; terminal.sourceIdentity=identity;
            auto* message = new (std::nothrow) D3D12CompleteMessage{generation,path,std::move(terminal)};
            if (message && !PostMessageW(window,kD3D12ImportCompleteMessage,0,reinterpret_cast<LPARAM>(message))) delete message;
        };
        auto stepProgress = [stepPhase, stepDone, stepTotal](const model_core::StepProgressNotice& notice) {
            // Publish the bounded phase/counts; the UI thread reads them to keep
            // a long CAD parse/tessellation legible instead of an apparent hang.
            stepTotal->store(notice.definitionTotal, std::memory_order_relaxed);
            stepDone->store(notice.definitionsMeshed, std::memory_order_relaxed);
            stepPhase->store(notice.phase, std::memory_order_relaxed);
        };
        try
        {
            result = d3d12_import_bridge::RunImport(format, path, generation, [cancellation]
            {
                return cancellation->load(std::memory_order_relaxed);
            }, sink, sectionBytes, delayBatches, faultForTesting, detailSource, complete, cpuGuard, stepProgress);
        }
        catch (const std::length_error&)
        {
            result.errorCode = model_core::ImportErrorCode::ResourceLimit;
            result.errorStage = import_broker::ImportStage::Upload;
            result.errorSummary = L"This model exceeds the upload capacity.";
            result.errorDetails = L"An accepted batch exceeded the bounded upload queue. Export a smaller model and retry.";
        }
        catch (const std::bad_alloc&)
        {
            result.errorCode = model_core::ImportErrorCode::OutOfMemory;
            result.errorStage = import_broker::ImportStage::WorkerReportedError;
            result.errorSummary = L"There is not enough memory to open this model.";
            result.errorDetails = L"Importing this model exceeded the available memory budget.";
        }
        catch (...)
        {
            result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
            result.errorStage = import_broker::ImportStage::WorkerReportedError;
            result.errorSummary = L"This model could not be previewed.";
            result.errorDetails = L"The importer stopped unexpectedly while reading the model.";
        }
        if (!alive->load(std::memory_order_relaxed) || (initialComplete && cancellation->load())) return;
        auto* message = new (std::nothrow) D3D12CompleteMessage{ generation, path, std::move(result) };
        if (message && !PostMessageW(window, kD3D12ImportCompleteMessage, 0, reinterpret_cast<LPARAM>(message)))
            delete message;
    });
}

void OpenDialog(ViewerApp& app)
{
    if (app.appSmoke && !app.smokePickerPath.empty()) {
        const auto path = std::move(app.smokePickerPath);
        app.smokePickerPath.clear(); BeginOpen(app,path); return;
    }
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        SetFailure(app, L"The Open dialog is unavailable.", L"Windows could not create the system file picker.");
        return;
    }
    const COMDLG_FILTERSPEC filters[] = {
        { L"Supported 3D models", L"*.glb;*.gltf;*.stl;*.ply;*.obj;*.fbx;*.3mf;*.usd;*.usda;*.usdc;*.usdz;*.step;*.stp" },
        { L"glTF models (*.glb; *.gltf)", L"*.glb;*.gltf" },
        { L"STL (*.stl)", L"*.stl" },
        { L"PLY meshes and points (*.ply)", L"*.ply" },
        { L"Wavefront OBJ with MTL (*.obj)", L"*.obj" },
        { L"Autodesk FBX (*.fbx)", L"*.fbx" },
        { L"3D Manufacturing Format (*.3mf)", L"*.3mf" },
        { L"Universal Scene Description (*.usd; *.usda; *.usdc; *.usdz)", L"*.usd;*.usda;*.usdc;*.usdz" },
        { L"STEP CAD models (*.step; *.stp)", L"*.step;*.stp" },
        { L"All files (*.*)", L"*.*" }
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetDefaultExtension(L"glb");
    dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const HRESULT shown = dialog->Show(app.window);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    if (FAILED(shown))
    {
        SetFailure(app, L"The Open dialog stopped unexpectedly.", L"Try dropping a supported local 3D model into the window.");
        return;
    }
    ComPtr<IShellItem> item;
    PWSTR selectedPath = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath)))
    {
        const std::wstring path(selectedPath);
        CoTaskMemFree(selectedPath);
        BeginOpen(app, path);
    }
}

void CopyErrorDetails(const ViewerApp& app)
{
    d3d12_import_bridge::ImportResult result;
    result.errorSummary = app.errorSummary; result.errorDetails = app.errorDetails;
    result.errorCode = app.errorCode; result.errorStage = app.errorStage; result.errorPhase = app.errorPhase;
    std::wstring text = d3d12_import_bridge::DiagnosticDetails(app.diagnosticPath, result);
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return;
    void* destination = GlobalLock(memory);
    if (!destination) { GlobalFree(memory); return; }
    std::memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (OpenClipboard(app.window))
    {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        CloseClipboard();
    }
    else
    {
        GlobalFree(memory);
    }
}

void ShowMoreMenu(ViewerApp& app)
{
    HMENU menu = CreatePopupMenu();
    if (!app.warning.empty())
    {
        AppendMenuW(menu, MF_STRING, ID_VIEW_DIAGNOSTICS, L"Model warnings…");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, ID_VIEW_CONTROLS, L"Controls\t?");
    AppendMenuW(menu, MF_STRING, ID_VIEW_SETTINGS, L"Settings…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"About Preview 3D");
    RECT button = app.chrome.Button(Chrome::Part::Overflow).rect;
    POINT anchor{ button.right, button.bottom };
    ClientToScreen(app.window, &anchor);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON, anchor.x, anchor.y, 0, app.window, nullptr);
    DestroyMenu(menu);
}

// Menu-invoked, not keyboard-repeatable, so no ConsumeToggleCommand guard is
// needed (unlike ToggleGrid/ToggleAxisSnap).
void ToggleSettingsPanel(ViewerApp& app)
{
    app.settingsPanelOpen = !app.settingsPanelOpen;
    InvalidateRect(app.window, nullptr, FALSE);
}

void DrawOwnerButton(ViewerApp& app, const DRAWITEMSTRUCT& item)
{
    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
    RECT bounds = item.rcItem;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const bool hot = (item.itemState & ODS_HOTLIGHT) != 0;
    // Grid/Snap/Info/Fit/Reset/Open moved to the D2D title bar (Chrome +
    // D3D11On12Overlay::DrawTitleBar); this now only draws the error-state buttons.
    const bool primary = item.CtlID == ID_VIEW_RETRY;
    const bool active = false;
    COLORREF fill = primary ? RGB(10, 132, 255) : RGB(58, 58, 60);
    COLORREF border = primary ? RGB(34, 146, 255) : RGB(73, 73, 76);
    COLORREF foreground = RGB(245, 245, 247);
    if (active) { fill = RGB(40, 44, 52); border = RGB(10, 132, 255); }
    if (hot) fill = primary ? RGB(32, 145, 255) : RGB(68, 68, 71);
    if (active && hot) fill = RGB(48, 54, 64);
    if (pressed) fill = primary ? RGB(0, 113, 227) : RGB(48, 48, 50);
    if (disabled) { fill = RGB(44, 44, 46); border = RGB(51, 51, 53); foreground = RGB(112, 112, 117); }

    const bool errorAction = item.CtlID == ID_VIEW_RETRY || item.CtlID == ID_VIEW_OPEN_ANOTHER ||
        item.CtlID == ID_VIEW_COPY_DETAILS;
    HBRUSH surfaceBrush = CreateSolidBrush(errorAction ? RGB(36, 36, 38) : RGB(44, 44, 46));
    FillRect(item.hDC, &bounds, surfaceBrush);
    DeleteObject(surfaceBrush);

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, focused ? RGB(100, 188, 255) : border);
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, bounds.left, bounds.top, bounds.right, bounds.bottom, Scale(app, 9), Scale(app, 9));
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, foreground);
    HGDIOBJ oldFont = SelectObject(item.hDC, app.buttonFont);
    if (pressed) OffsetRect(&bounds, 0, 1);
    DrawTextW(item.hDC, text, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, oldFont);
}

void RenderFallback(ViewerApp& app, HDC dc)
{
    RECT client{};
    GetClientRect(app.window, &client);
    FillRect(dc, &client, gBackgroundBrush);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(232, 234, 237));
    HFONT old = static_cast<HFONT>(SelectObject(dc, app.buttonFont));
    RECT text = client;
    text.left += Scale(app, 32);
    text.right -= Scale(app, 32);
    DrawTextW(dc, app.errorSummary.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
}

void HandleCommand(ViewerApp& app, int id)
{
    switch (id)
    {
    case ID_VIEW_OPEN:
    case ID_VIEW_OPEN_ANOTHER: OpenDialog(app); break;
    case ID_VIEW_FIT: FrameSelectedOrAll(app); break;
    case ID_VIEW_RESET:
        if (app.renderThread.HasModel())
        {
            app.renderThread.LockCamera()->Reset(ViewportAspect(app));
            InvalidateRect(app.window, nullptr, FALSE);
        }
        break;
    case ID_VIEW_GRID: ToggleGrid(app); break;
    case ID_VIEW_GROUND_AXIS: CycleGroundAxis(app); break;
    case ID_VIEW_GROUND_DIRECTION: ToggleGroundDirection(app); break;
    case ID_VIEW_AXIS_SNAP: ToggleAxisSnap(app); break;
    case ID_VIEW_INFO: ToggleInfoPanel(app); break;
    case ID_VIEW_FULLSCREEN: ToggleFullscreen(app); break;
    case ID_VIEW_FRONT: SnapViewCommand(app, ViewDir::Front); break;
    case ID_VIEW_RIGHT: SnapViewCommand(app, ViewDir::Right); break;
    case ID_VIEW_TOP: SnapViewCommand(app, ViewDir::Top); break;
    case ID_VIEW_PROJECTION: ToggleProjection(app); break;
    case ID_VIEW_MENU: ShowMoreMenu(app); break;
    case ID_VIEW_RETRY: if (!app.failedPath.empty()) BeginOpen(app, app.failedPath); break;
    case ID_VIEW_COPY_DETAILS: CopyErrorDetails(app); break;
    case ID_VIEW_CANCEL: CancelOpen(app); break;
    case ID_VIEW_CONTROLS: ShowControlsDialog(app.window); break;
    case ID_VIEW_SETTINGS: ToggleSettingsPanel(app); break;
    case ID_VIEW_DIAGNOSTICS:
        MessageBoxW(app.window, app.warning.c_str(), L"Model warnings", MB_OK | MB_ICONWARNING);
        break;
    case IDM_ABOUT:
        MessageBoxW(app.window, L"A native static viewer for glTF, OBJ, FBX, STL, PLY, the supported static 3MF preview subset, and USD-family models. 3MF includes Core, Materials, Production, and bounded Beam Lattice content. USD uses a fast isolated importer with a separate isolated OpenUSD compatibility host for bounded local composition.\n\nImports are bounded and local-only. No cloud, animation playback, editing, file modification, slicer-private multi-plate grouping, Explorer thumbnails, or persistent model cache.",
            L"About Preview 3D", MB_OK | MB_ICONINFORMATION);
        break;
    case IDM_EXIT: DestroyWindow(app.window); break;
    }
}

// Builds a categorized Open With menu from the cached/incrementally refreshed
// Shell catalog, plus the trailing system-picker fallback.
void ShowOpenWithMenu(ViewerApp& app)
{
    if (app.currentPath.empty()) return;
    std::vector<OpenWithEntry> entries = EnumerateOpenWithHandlers(app.currentPath);
    HMENU menu = CreatePopupMenu();
    constexpr UINT kBaseId = 40000;
    OpenWithGroup currentGroup = OpenWithGroup::Status;
    bool hasHeading = false;
    bool hasMenuItem = false;
    auto heading = [](OpenWithGroup group) -> const wchar_t* {
        switch (group)
        {
        case OpenWithGroup::Cad: return L"CAD";
        case OpenWithGroup::Modeling: return L"Modeling";
        case OpenWithGroup::Printing: return L"3D printing";
        case OpenWithGroup::Recommended: return L"Recommended by Windows";
        default: return nullptr;
        }
    };
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        const auto& entry = entries[index];
        if (const wchar_t* label = heading(entry.group); label && (!hasHeading || currentGroup != entry.group))
        {
            if (hasMenuItem) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, label);
            currentGroup = entry.group;
            hasHeading = true;
        }
        else if (entry.group == OpenWithGroup::Fallback && hasMenuItem)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        const UINT flags = entry.enabled ? MF_STRING : MF_STRING | MF_DISABLED | MF_GRAYED;
        AppendMenuW(menu, flags, entry.enabled ? kBaseId + static_cast<UINT>(index) : 0,
            entry.displayName.c_str());
        hasMenuItem = true;
    }
    RECT button = app.chrome.Button(Chrome::Part::OpenWith).rect;
    POINT anchor{ button.left, button.bottom };
    ClientToScreen(app.window, &anchor);
    const int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
        anchor.x, anchor.y, 0, app.window, nullptr);
    DestroyMenu(menu);
    if (selected >= static_cast<int>(kBaseId) && static_cast<std::size_t>(selected - kBaseId) < entries.size())
    {
        const auto& entry = entries[selected - kBaseId];
        if (entry.invoke && !entry.invoke(app.currentPath))
            ShowModeHud(app, L"That app is no longer available");
    }
}

void DoShare(ViewerApp& app)
{
    if (app.currentPath.empty()) return;
    std::wstring error;
    if (!ShowWindowsShare(app.window, app.currentPath, error))
    {
        ShowModeHud(app, L"Share isn't available right now");
    }
}

void ToggleSpeedFlyout(ViewerApp& app)
{
    if (!CanNavigate(app)) return;
    app.speedFlyoutOpen = !app.speedFlyoutOpen;
    InvalidateRect(app.window, nullptr, FALSE);
}

// Dispatches a click on one of the D2D-drawn title-bar buttons (Chrome +
// D3D11On12Overlay::DrawTitleBar). Reuses HandleCommand for the actions that already
// have a command ID (kept working via Ctrl+O/accelerators too); the rest
// (Speed flyout, Share, Open With) are new to the title bar.
void HandleChromeAction(ViewerApp& app, Chrome::Part part)
{
    switch (part)
    {
    case Chrome::Part::Grid: HandleCommand(app, ID_VIEW_GRID); break;
    case Chrome::Part::GroundAxis: HandleCommand(app, ID_VIEW_GROUND_AXIS); break;
    case Chrome::Part::GroundDirection: HandleCommand(app, ID_VIEW_GROUND_DIRECTION); break;
    case Chrome::Part::AxisSnap: HandleCommand(app, ID_VIEW_AXIS_SNAP); break;
    case Chrome::Part::Speed: ToggleSpeedFlyout(app); break;
    case Chrome::Part::Fit: HandleCommand(app, ID_VIEW_FIT); break;
    case Chrome::Part::Reset: HandleCommand(app, ID_VIEW_RESET); break;
    case Chrome::Part::Share: DoShare(app); break;
    case Chrome::Part::Overflow: HandleCommand(app, ID_VIEW_MENU); break;
    case Chrome::Part::OpenWith: ShowOpenWithMenu(app); break;
    default: break;
    }
}

viewer_accessibility::ControlInfo AccessibleInfo(ViewerApp& app, viewer_accessibility::Control control)
{
    using viewer_accessibility::Control;
    viewer_accessibility::ControlInfo info;
    info.focused = app.keyboardControl == control && GetFocus() == app.window;
    const bool model = HasNavigableModel(app);
    auto chrome = [&](Chrome::Part part, const wchar_t* name, const wchar_t* description = L"") {
        const auto& button = app.chrome.Button(part);
        info.name = name; info.description = description; info.rect = button.rect;
        info.visible = button.visible; info.enabled = button.enabled;
    };
    switch (control)
    {
    case Control::Grid: chrome(Chrome::Part::Grid, L"Ground grid", L"Show or hide the ground grid"); info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.gridVisible; break;
    case Control::GroundAxis:
    {
        chrome(Chrome::Part::GroundAxis, L"Model ground axis", L"Cycle the model axis treated as vertical");
        const GroundAxis effective = app.loadedModel
            ? ResolveGroundAxis(app.groundAxis, app.loadedModel->source.upAxis) : GroundAxis::Z;
        info.value = GroundAxisName(effective);
        break;
    }
    case Control::GroundDirection:
    {
        chrome(Chrome::Part::GroundDirection, L"Ground direction", L"Use the opposite side of the selected axis as up");
        info.role = ROLE_SYSTEM_CHECKBUTTON;
        info.checked = app.groundAxisInverted;
        const GroundAxis effective = app.loadedModel
            ? ResolveGroundAxis(app.groundAxis, app.loadedModel->source.upAxis) : GroundAxis::Z;
        info.value = std::wstring(app.groundAxisInverted ? L"Negative " : L"Positive ") + GroundAxisName(effective) + L" is up";
        break;
    }
    case Control::AxisSnap: chrome(Chrome::Part::AxisSnap, L"Axis snap", L"Snap truck movement to the nearest world axis"); info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.axisSnapEnabled; break;
    case Control::Speed: chrome(Chrome::Part::Speed, L"Travel speed", L"Open the flight-speed control"); break;
    case Control::Fit: chrome(Chrome::Part::Fit, L"Fit selection or model"); break;
    case Control::Reset: chrome(Chrome::Part::Reset, L"Reset view"); break;
    case Control::Share: chrome(Chrome::Part::Share, L"Share"); break;
    case Control::More: chrome(Chrome::Part::Overflow, L"More options"); break;
    case Control::OpenWith: chrome(Chrome::Part::OpenWith, L"Open with"); break;
    case Control::Minimize: chrome(Chrome::Part::Minimize, L"Minimize"); break;
    case Control::Maximize: chrome(Chrome::Part::Maximize, IsZoomed(app.window) ? L"Restore" : L"Maximize"); break;
    case Control::Close: chrome(Chrome::Part::Close, L"Close"); break;
    case Control::Info:
        info.name=L"Model information"; info.description=L"Show or hide Stats and Shading"; info.rect=InfoButtonRect(app);
        info.visible=model; info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.infoPanelVisible; break;
    case Control::InfoPanelClose:
        info.name=L"Close model information"; info.description=L"Close Stats and Shading";
        info.rect=InfoPanelCloseButtonRect(app); info.visible=model && app.infoPanelVisible; break;
    case Control::Zoom:
    {
        info.name=L"Zoom"; info.description=L"Adjust camera zoom"; info.rect=ZoomTrackRect(app); info.visible=model; info.role=ROLE_SYSTEM_SLIDER;
        auto camera=app.renderThread.LockCamera(); info.value=std::to_wstring(static_cast<int>(std::lround(ZoomPercentFor(*camera))))+L" percent"; break;
    }
    case Control::Fullscreen:
        info.name=L"Fullscreen"; info.description=L"Enter or leave fullscreen"; info.rect=FullscreenButtonRect(app);
        info.visible=model; info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.isFullscreen; break;
    case Control::LightingStudio:
        info.name=L"Studio lighting"; info.description=L"Neutral colorless lighting for evaluating PBR materials";
        info.rect=ComputeLightingToolbarLayout(app).studio;info.visible=model;info.role=ROLE_SYSTEM_RADIOBUTTON;
        info.checked=app.lightingMode==LightingMode::Studio;break;
    case Control::LightingClay:
        info.name=L"Clay or solid shading";info.description=L"Matte gray material for inspecting geometry";
        info.rect=ComputeLightingToolbarLayout(app).clay;info.visible=model;info.role=ROLE_SYSTEM_RADIOBUTTON;
        info.checked=app.lightingMode==LightingMode::Clay;break;
    case Control::LightingDirectional:
        info.name=L"Directional lighting";info.description=L"Sharp rotatable light for inspecting surface detail";
        info.rect=ComputeLightingToolbarLayout(app).directional;info.visible=model;info.role=ROLE_SYSTEM_RADIOBUTTON;
        info.checked=app.lightingMode==LightingMode::Directional;break;
    case Control::DirectionalLightAngle:
    {
        // The visual slider is gone; the fragment anchors to the navigation
        // gizmo's outer ring, where the light is now rotated by dragging the
        // sun. Arrow keys still nudge it for keyboard users.
        float centerX=0.0f,centerY=0.0f,radius=0.0f;
        app.gizmo.RingBounds(centerX,centerY,radius);
        info.name=L"Directional light angle";
        info.description=L"Rotate the inspection light by dragging its sun around the navigation gizmo";
        info.rect=RECT{static_cast<LONG>(centerX-radius),static_cast<LONG>(centerY-radius),
            static_cast<LONG>(centerX+radius),static_cast<LONG>(centerY+radius)};
        info.visible=model&&app.lightingMode==LightingMode::Directional;info.role=ROLE_SYSTEM_SLIDER;
        info.value=std::to_wstring(static_cast<int>(std::lround(app.directionalLightAngle*360.0f)))+L" degrees";break;
    }
    case Control::DirectionalLightElevation:
    {
        // Same gizmo anchor as the angle; drag the sun toward or away from the
        // center to raise/lower it, or nudge with the arrow keys.
        float centerX=0.0f,centerY=0.0f,radius=0.0f;
        app.gizmo.RingBounds(centerX,centerY,radius);
        info.name=L"Directional light elevation";
        info.description=L"Raise or lower the inspection light by dragging its sun toward or away from the gizmo center";
        info.rect=RECT{static_cast<LONG>(centerX-radius),static_cast<LONG>(centerY-radius),
            static_cast<LONG>(centerX+radius),static_cast<LONG>(centerY+radius)};
        info.visible=model&&app.lightingMode==LightingMode::Directional;info.role=ROLE_SYSTEM_SLIDER;
        info.value=std::to_wstring(static_cast<int>(std::lround(app.directionalLightElevation*57.29577951308232f)))+L" degrees";break;
    }
    case Control::Wireframe:
        info.name=L"Wireframe";info.description=L"Show only mesh edges with all triangle surfaces hidden";
        info.rect=ComputeLightingToolbarLayout(app).wireframe;info.visible=model;info.role=ROLE_SYSTEM_RADIOBUTTON;
        info.checked=app.lightingMode==LightingMode::Wireframe;break;
    case Control::SpeedSlider:
    {
        info.name=L"Travel speed"; info.description=L"Adjust flight speed"; info.rect=SpeedFlyoutTrackRect(app);
        info.visible=app.speedFlyoutOpen; info.role=ROLE_SYSTEM_SLIDER;
        auto camera=app.renderThread.LockCamera(); info.value=L"times "+FormatMultiplier(camera->FlySpeedScale()); break;
    }
    case Control::NativeOrientation:
        info.name=L"Show model in its original orientation"; info.rect=SettingsToggleRowRect(app);
        info.visible=app.settingsPanelOpen; info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.showNativeOrientation; break;
    case Control::HideCursorWhileDragging:
        info.name=L"Hide cursor while dragging"; info.description=L"Hide the mouse pointer during viewport camera drags";
        info.rect=SettingsCursorToggleRowRect(app); info.visible=app.settingsPanelOpen;
        info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.hideCursorWhileDragging; break;
    case Control::GizmoPositiveX: case Control::GizmoNegativeX: case Control::GizmoPositiveY:
    case Control::GizmoNegativeY: case Control::GizmoPositiveZ: case Control::GizmoNegativeZ:
    {
        static constexpr const wchar_t* names[] = { L"View from positive X", L"View from negative X", L"View from positive Y",
            L"View from negative Y", L"View from positive Z", L"View from negative Z" };
        const int index=static_cast<int>(control)-static_cast<int>(Control::GizmoPositiveX);
        info.name=names[index]; info.description=L"Snap the camera to this axis"; info.visible=model;
        DirectX::XMFLOAT4 orientation{}; { auto camera=app.renderThread.LockCamera(); orientation=camera->orientation; }
        const auto geometry=app.gizmo.ComputeDraw(DirectX::XMLoadFloat4(&orientation));
        const int axis=index/2; const bool positive=index%2==0;
        const auto node=positive?geometry.positive[axis]:geometry.negative[axis];
        const float radius=positive?geometry.nodeRadius:geometry.dotRadius;
        info.rect={LONG(geometry.centerX+node.x-radius),LONG(geometry.centerY+node.y-radius),
            LONG(geometry.centerX+node.x+radius),LONG(geometry.centerY+node.y+radius)};
        break;
    }
    case Control::ErrorRetry: case Control::ErrorOpenAnother: case Control::ErrorCopyDetails:
    {
        HWND button=control==Control::ErrorRetry?app.retryButton:control==Control::ErrorOpenAnother?app.openAnotherButton:app.copyButton;
        info.name=control==Control::ErrorRetry?L"Retry":control==Control::ErrorOpenAnother?L"Open another":L"Copy details";
        RECT screen{};GetWindowRect(button,&screen);POINT corners[2]={{screen.left,screen.top},{screen.right,screen.bottom}};
        MapWindowPoints(nullptr,app.window,corners,2);info.rect={corners[0].x,corners[0].y,corners[1].x,corners[1].y};
        info.visible=app.state==ViewerState::Failed;info.enabled=IsWindowEnabled(button)!=FALSE;info.focused=GetFocus()==button;
        break;
    }
    default: break;
    }
    return info;
}

void FocusAccessible(ViewerApp& app, viewer_accessibility::Control control)
{
    app.keyboardControl = control;
    SetFocus(app.window);
    if (control==viewer_accessibility::Control::ErrorRetry) SetFocus(app.retryButton);
    else if (control==viewer_accessibility::Control::ErrorOpenAnother) SetFocus(app.openAnotherButton);
    else if (control==viewer_accessibility::Control::ErrorCopyDetails) SetFocus(app.copyButton);
    InvalidateRect(app.window, nullptr, FALSE);
}

viewer_accessibility::ControlInfo QueryAccessibleMarshaled(ViewerApp& app, viewer_accessibility::Control control)
{
    if (GetCurrentThreadId()==GetWindowThreadProcessId(app.window,nullptr)) return AccessibleInfo(app,control);
    AccessibilityQueryRequest request{control,{}};
    SendMessageW(app.window,kAccessibilityQueryMessage,0,reinterpret_cast<LPARAM>(&request));
    return request.result;
}

void InvokeAccessibleMarshaled(ViewerApp& app, viewer_accessibility::Control control)
{
    if (GetCurrentThreadId()==GetWindowThreadProcessId(app.window,nullptr)) InvokeAccessible(app,control);
    else SendMessageW(app.window,kAccessibilityActionMessage,static_cast<WPARAM>(control),0);
}

void FocusAccessibleMarshaled(ViewerApp& app, viewer_accessibility::Control control)
{
    if (GetCurrentThreadId()==GetWindowThreadProcessId(app.window,nullptr)) FocusAccessible(app,control);
    else SendMessageW(app.window,kAccessibilityFocusMessage,static_cast<WPARAM>(control),0);
}

std::wstring AccessibilityStatusMarshaled(ViewerApp& app)
{
    if (GetCurrentThreadId()==GetWindowThreadProcessId(app.window,nullptr)) return AccessibilityStatus(app);
    std::wstring result;
    SendMessageW(app.window,kAccessibilityStatusMessage,0,reinterpret_cast<LPARAM>(&result));
    return result;
}

void InvokeAccessible(ViewerApp& app, viewer_accessibility::Control control)
{
    using viewer_accessibility::Control;
    switch (control)
    {
    case Control::Grid: HandleCommand(app,ID_VIEW_GRID); break;
    case Control::GroundAxis: HandleCommand(app,ID_VIEW_GROUND_AXIS); break;
    case Control::GroundDirection: HandleCommand(app,ID_VIEW_GROUND_DIRECTION); break;
    case Control::AxisSnap: HandleCommand(app,ID_VIEW_AXIS_SNAP); break;
    case Control::Speed: ToggleSpeedFlyout(app); break;
    case Control::Fit: HandleCommand(app,ID_VIEW_FIT); break;
    case Control::Reset: HandleCommand(app,ID_VIEW_RESET); break;
    case Control::Share: DoShare(app); break;
    case Control::More: ShowMoreMenu(app); break;
    case Control::OpenWith: ShowOpenWithMenu(app); break;
    case Control::Minimize: ShowWindow(app.window,SW_MINIMIZE); break;
    case Control::Maximize: ShowWindow(app.window,IsZoomed(app.window)?SW_RESTORE:SW_MAXIMIZE); break;
    case Control::Close: PostMessageW(app.window,WM_CLOSE,0,0); break;
    case Control::Info: ToggleInfoPanel(app); break;
    case Control::InfoPanelClose: CloseInfoPanel(app); break;
    case Control::Fullscreen: ToggleFullscreen(app); break;
    case Control::LightingStudio: SetLightingMode(app,LightingMode::Studio); break;
    case Control::LightingClay: SetLightingMode(app,LightingMode::Clay); break;
    case Control::LightingDirectional: SetLightingMode(app,LightingMode::Directional); break;
    case Control::Wireframe: SetLightingMode(app,LightingMode::Wireframe); break;
    case Control::NativeOrientation: ToggleShowNativeOrientation(app); break;
    case Control::HideCursorWhileDragging: ToggleHideCursorWhileDragging(app); break;
    case Control::GizmoPositiveX: SnapViewCommand(app,ViewDir::Right); break;
    case Control::GizmoNegativeX: SnapViewCommand(app,ViewDir::Left); break;
    case Control::GizmoPositiveY: SnapViewCommand(app,ViewDir::Back); break;
    case Control::GizmoNegativeY: SnapViewCommand(app,ViewDir::Front); break;
    case Control::GizmoPositiveZ: SnapViewCommand(app,ViewDir::Top); break;
    case Control::GizmoNegativeZ: SnapViewCommand(app,ViewDir::Bottom); break;
    case Control::ErrorRetry: HandleCommand(app,ID_VIEW_RETRY); break;
    case Control::ErrorOpenAnother: HandleCommand(app,ID_VIEW_OPEN_ANOTHER); break;
    case Control::ErrorCopyDetails: HandleCommand(app,ID_VIEW_COPY_DETAILS); break;
    default: break;
    }
}

std::wstring AccessibilityStatus(const ViewerApp& app)
{
    if (app.state==ViewerState::Loading) return L"Loading "+d3d12_import_bridge::SourceFormatLabel(app.diagnosticPath);
    if (app.state==ViewerState::Failed) return app.errorSummary+L" "+app.errorDetails;
    if (!app.warning.empty()) return L"Model loaded with warnings. "+app.warning;
    if (app.state==ViewerState::Ready) return app.filename.empty()?L"Model ready":app.filename+L" ready";
    if (app.state==ViewerState::Partial) return L"Preview cancelled; incomplete geometry remains visible";
    return L"No model open";
}

bool MoveAccessibleFocus(ViewerApp& app, bool backward)
{
    const auto controls=viewer_accessibility::VisibleControls([&](auto control){return AccessibleInfo(app,control);});
    if (controls.empty()) return false;
    auto found=std::find(controls.begin(),controls.end(),app.keyboardControl);
    std::ptrdiff_t index=found==controls.end()?(backward?0:-1):std::distance(controls.begin(),found);
    index=(index+(backward?-1:1)+static_cast<std::ptrdiff_t>(controls.size()))%static_cast<std::ptrdiff_t>(controls.size());
    FocusAccessible(app,controls[static_cast<std::size_t>(index)]);
    NotifyWinEvent(EVENT_OBJECT_FOCUS,app.window,OBJID_CLIENT,static_cast<LONG>(index+1));
    return true;
}

bool HandleAccessibleKey(ViewerApp& app, WPARAM key)
{
    using viewer_accessibility::Control;
    if (key==VK_TAB) return MoveAccessibleFocus(app,(GetKeyState(VK_SHIFT)&0x8000)!=0);
    if (app.keyboardControl==Control::None) return false;
    if (key==VK_RETURN || key==VK_SPACE) { InvokeAccessible(app,app.keyboardControl); return true; }
    const int direction=(key==VK_RIGHT || key==VK_UP)?1:(key==VK_LEFT || key==VK_DOWN)?-1:0;
    if (!direction) return false;
    if (app.keyboardControl==Control::Zoom)
    {
        auto camera=app.renderThread.LockCamera();
        const int position=std::clamp(ZoomSliderPositionFor(*camera)+direction*25,0,kZoomSliderMax);
        const double distance=ZoomDistanceForSliderPosition(*camera,position); camera->distance=distance; camera->targetDistance=distance;
        InvalidateRect(app.window,nullptr,FALSE); return true;
    }
    if (app.keyboardControl==Control::SpeedSlider) { AdjustFlySpeed(app,static_cast<float>(direction)); return true; }
    if (app.keyboardControl==Control::DirectionalLightAngle)
    {
        app.directionalLightAngle=std::clamp(app.directionalLightAngle+direction/36.0f,0.0f,1.0f);
        InvalidateRect(app.window,nullptr,FALSE);return true;
    }
    if (app.keyboardControl==Control::DirectionalLightElevation)
    {
        constexpr float kRadiansToDegrees=57.29577951308232f;
        app.directionalLightElevation=std::clamp(
            app.directionalLightElevation+direction*5.0f/kRadiansToDegrees,0.0f,1.4835f);
        InvalidateRect(app.window,nullptr,FALSE);return true;
    }
    return false;
}

FlightInput BuildFlightInput(const ViewerApp& app)
{
    FlightInput input;
    if (!CanNavigate(app)) return input;
    // Unreal-style flight only while RMB capture is active.
    if (app.flyLook)
    {
        input.right = (app.moveRight ? 1.0f : 0.0f) - (app.moveLeft ? 1.0f : 0.0f);
        input.up = (app.moveUp ? 1.0f : 0.0f) - (app.moveDown ? 1.0f : 0.0f);
        input.forward = (app.moveForward ? 1.0f : 0.0f) - (app.moveBackward ? 1.0f : 0.0f);
        input.roll = (app.rollRight ? 1.0f : 0.0f) - (app.rollLeft ? 1.0f : 0.0f);
        input.fast = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    }
    const float arrowsX = (app.arrowRight ? 1.0f : 0.0f) - (app.arrowLeft ? 1.0f : 0.0f);
    const float arrowsY = (app.arrowDown ? 1.0f : 0.0f) - (app.arrowUp ? 1.0f : 0.0f);
    if (input.fast)
    {
        input.panX = arrowsX * kArrowPixelsPerSecond;
        input.panY = arrowsY * kArrowPixelsPerSecond;
    }
    else
    {
        input.orbitX = arrowsX * kArrowPixelsPerSecond;
        input.orbitY = arrowsY * kArrowPixelsPerSecond;
    }
    RECT client{};
    GetClientRect(app.window, &client);
    input.viewportHeight = static_cast<float>(std::max(1L, client.bottom - EffectiveToolbarHeight(app) - EffectiveBottomBarHeight(app)));
    return input;
}

// Everything in IsAnimating that is UI-owned state. The render thread ORs
// this with the camera's own motion, which it can see directly under the
// camera lock -- so the UI thread does not have to take that lock just to
// answer "should a frame happen".
bool IsAnimatingWithoutCamera(const ViewerApp& app)
{
    const double now = NowSeconds();
    return (app.state == ViewerState::Loading && !app.reduceMotion) || app.renderPresentationPending
        || HasNavigationInput(app) || now < app.speedHudUntil
        || now < app.modeHudUntil;
}

// Keep the UI's loading loop alive until the render thread has presented the
// completed model and drained the visible fine-detail refinement for its
// initial view. This intentionally excludes early progressive-preview frames.
void FinishRenderTimerIfPresented(ViewerApp& app)
{
    if (!app.renderPresentationPending || app.renderStartedMicroseconds == 0) return;
    if (app.renderThread.CompleteModelPresentedGeneration() != app.generation) return;

    const std::uint64_t presentedMicroseconds = app.renderThread.CompleteModelPresentedMicroseconds();
    if (presentedMicroseconds < app.renderStartedMicroseconds) return;

    std::wostringstream duration;
    duration << std::fixed << std::setprecision(2)
             << static_cast<double>(presentedMicroseconds - app.renderStartedMicroseconds) / 1'000'000.0 << L" s";
    app.renderDurationText = duration.str();
    app.renderStartedMicroseconds = 0;
    app.renderPresentationPending = false;
}

// Bounded, product-owned description of the STEP host's current phase. Never
// kernel text; counts are the host's N-of-M definition progress.
std::wstring StepProgressText(std::uint32_t phase, std::uint32_t done, std::uint32_t total)
{
    switch (phase) {
    case model_core::kStepPhasePreflight: return L" • checking STEP text";
    case model_core::kStepPhaseRead: return L" • reading STEP entities";
    case model_core::kStepPhaseTransfer: return L" • building CAD model";
    case model_core::kStepPhasePlan: return L" • planning definitions";
    case model_core::kStepPhaseMesh:
        return total ? L" • tessellating " + std::to_wstring(done) + L" of " + std::to_wstring(total)
                     : L" • tessellating shapes";
    case model_core::kStepPhaseEmit:
        return total ? L" • preparing geometry " + std::to_wstring(done) + L" of " + std::to_wstring(total)
                     : L" • preparing geometry";
    default: return {};
    }
}

// Builds the UI snapshot for the render thread's Direct2D chrome pass.
OverlayInfo BuildOverlayInfo(ViewerApp& app)
{
    OverlayInfo overlay;
    overlay.state = app.state;
    overlay.filename = app.filename;
    d3d12_import_bridge::ImportResult failure; failure.errorStage = app.errorStage; failure.errorPhase = app.errorPhase;
    overlay.failureContext = d3d12_import_bridge::SourceFormatLabel(app.diagnosticPath) + L"  •  " + d3d12_import_bridge::FailurePhaseLabel(failure);
    overlay.loadingStatus = L"Loading " + d3d12_import_bridge::SourceFormatLabel(app.diagnosticPath);
    if (app.state == ViewerState::Loading) {
        const auto stepPhase = app.stepProgressPhase.load(std::memory_order_relaxed);
        if (stepPhase != 0)
            overlay.loadingStatus += StepProgressText(stepPhase,
                app.stepProgressDone.load(std::memory_order_relaxed),
                app.stepProgressTotal.load(std::memory_order_relaxed));
    }
    if (app.state == ViewerState::Partial) overlay.loadingStatus = L"Preview cancelled • incomplete geometry";
    if (app.loadedModel && app.loadedModel->source.generationId == app.generation) {
        const auto flags = app.loadedModel->importStatus.flags;
        overlay.loadingStatus += (flags & model_core::kStatusPressure) ? L" • waiting for upload capacity"
            : (flags & model_core::kStatusRefining) ? L" • refining textures" : L" • verifying complete geometry";
    }
    overlay.errorSummary = app.errorSummary;
    overlay.errorDetails = app.errorDetails;
    overlay.warning = app.warning;
    overlay.renderDurationText = app.renderDurationText;
    overlay.renderTimerRunning = app.renderStartedMicroseconds != 0
        && (app.state == ViewerState::Loading || app.renderPresentationPending);
    overlay.animationPhase = app.reduceMotion ? 0.0f : static_cast<float>(GetTickCount64() % 1400) / 1400.0f;
    overlay.dpiScale = app.dpiScale;
    overlay.toolbarHeight = EffectiveToolbarHeight(app);
    overlay.bottomBarHeight = EffectiveBottomBarHeight(app);
    // Always the bars' real height, Fullscreen included — see
    // OverlayInfo::barToolbarHeight/barBottomBarHeight (Renderer.h) for why
    // these are kept separate from the viewport-inset pair just above.
    overlay.barToolbarHeight = app.toolbarHeight;
    overlay.barBottomBarHeight = HasNavigableModel(app) ? app.bottomBarHeight : 0;
    overlay.infoPanelWidth = InfoPanelWidthPixels(app);
    if (overlay.infoPanelWidth > 0 && app.loadedModel)
    {
        overlay.infoPanelSections = BuildInfoPanelSections(
            *app.loadedModel, app.showNativeOrientation, app.groundAxis);
        overlay.infoPanelScrollOffset = app.infoPanelScrollOffset;
        overlay.infoPanelCloseButtonRect = InfoPanelCloseButtonRect(app);
        overlay.infoPanelCloseButtonHover = app.infoPanelCloseButtonHover;
        overlay.infoPanelCloseButtonPressed = app.infoPanelCloseButtonPressed;
    }
    overlay.zoomPercent = ZoomPercentFor(*app.renderThread.LockCamera());
    if (overlay.barBottomBarHeight > 0)
    {
        overlay.zoomTrackRect = ZoomTrackRect(app);
        overlay.zoomSliderT = static_cast<float>(ZoomSliderPositionFor(*app.renderThread.LockCamera())) / kZoomSliderMax;
        overlay.infoButtonRect = InfoButtonRect(app);
        overlay.infoButtonHover = app.infoButtonHover;
        overlay.infoButtonPressed = app.infoButtonPressed;
        overlay.fullscreenButtonRect = FullscreenButtonRect(app);
        overlay.fullscreenButtonHover = app.fullscreenButtonHover;
        overlay.fullscreenButtonPressed = app.fullscreenButtonPressed;
        const auto lighting=ComputeLightingToolbarLayout(app);
        overlay.lightingToolbarRect=lighting.bounds;
        overlay.studioButtonRect=lighting.studio;
        overlay.clayButtonRect=lighting.clay;
        overlay.directionalButtonRect=lighting.directional;
        overlay.wireframeButtonRect=lighting.wireframe;
        overlay.studioButtonHover=app.lightingButtonHover==0;
        overlay.clayButtonHover=app.lightingButtonHover==1;
        overlay.directionalButtonHover=app.lightingButtonHover==2;
        overlay.wireframeButtonHover=app.lightingButtonHover==3;
        overlay.studioButtonPressed=app.lightingButtonPressed==0;
        overlay.clayButtonPressed=app.lightingButtonPressed==1;
        overlay.directionalButtonPressed=app.lightingButtonPressed==2;
        overlay.wireframeButtonPressed=app.lightingButtonPressed==3;
    }
    overlay.isFullscreen = app.isFullscreen;
    overlay.lightingMode=app.lightingMode;
    overlay.directionalLightAngle=app.directionalLightAngle;
    overlay.directionalLightElevation=app.directionalLightElevation;
    overlay.hasModel = app.renderThread.HasModel();
    overlay.gridVisible = app.gridVisible;
    overlay.axisSnapEnabled = app.axisSnapEnabled;
    overlay.groundAxis = app.groundAxis;
    overlay.groundAxisInverted = app.groundAxisInverted;
    overlay.effectiveGroundAxis = app.loadedModel
        ? ResolveGroundAxis(app.groundAxis, app.loadedModel->source.upAxis) : GroundAxis::Z;
    DirectX::XMStoreFloat4x4(&overlay.modelTransform, ActiveModelTransform(app));
    overlay.infoPanelVisible = app.infoPanelVisible;
    overlay.speedFlyoutOpen = app.speedFlyoutOpen && HasNavigableModel(app);
    if (overlay.speedFlyoutOpen)
    {
        overlay.speedFlyoutRect = SpeedFlyoutRect(app);
        overlay.speedFlyoutTrackRect = SpeedFlyoutTrackRect(app);
        overlay.speedSliderT = static_cast<float>(SpeedSliderPositionFor(app.renderThread.LockCamera()->FlySpeedScale())) / kSpeedSliderMax;
        overlay.speedValueText = L"×" + FormatMultiplier(app.renderThread.LockCamera()->FlySpeedScale());
    }
    overlay.settingsPanelOpen = app.settingsPanelOpen;
    overlay.showNativeOrientation = app.showNativeOrientation;
    overlay.hideCursorWhileDragging = app.hideCursorWhileDragging;
    if (overlay.settingsPanelOpen)
    {
        overlay.settingsPanelRect = SettingsPanelRect(app);
        overlay.nativeOrientationRowRect = SettingsToggleRowRect(app);
        overlay.nativeOrientationSwitchRect = SettingsSwitchRect(app);
        overlay.hideCursorRowRect = SettingsCursorToggleRowRect(app);
        overlay.hideCursorSwitchRect = SettingsCursorSwitchRect(app);
    }
    overlay.selectionAmount = app.meshSelected ? 1.0f : 0.0f;
    const double now = NowSeconds();
    overlay.speedHud = app.speedHudText;
    overlay.speedHudAlpha = app.reduceMotion ? (now < app.speedHudUntil ? 1.0f : 0.0f) : static_cast<float>(HudAlpha(app.speedHudUntil, now));
    overlay.modeHud = app.modeHudText;
    overlay.modeHudAlpha = app.reduceMotion ? (now < app.modeHudUntil ? 1.0f : 0.0f) : static_cast<float>(HudAlpha(app.modeHudUntil, now));
    overlay.tooltipVisible = app.tooltipVisible;
    overlay.tooltipAnchorRect = app.tooltipAnchorRect;
    overlay.tooltipText = app.tooltipText;
    overlay.tooltipBelow = app.tooltipAnchorBelow;
    overlay.highContrast = app.highContrast;
    if (app.keyboardControl != viewer_accessibility::Control::None)
    {
        const auto focused = AccessibleInfo(app, app.keyboardControl);
        overlay.keyboardFocusVisible = focused.visible && GetFocus() == app.window;
        overlay.keyboardFocusRect = focused.rect;
    }
    UpdateChromeLayout(app);
    return overlay;
}

void RefreshSystemPreferences(ViewerApp& app)
{
    HIGHCONTRASTW contrast{sizeof(contrast)};
    app.highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0)
        && (contrast.dwFlags&HCF_HIGHCONTRASTON)!=0;
    BOOL animations=TRUE;
    app.reduceMotion = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0) && !animations;
    app.renderThread.LockCamera()->reduceMotion = app.reduceMotion;
}

void ActivatePrimaryWindow(ViewerApp& app)
{
    if (IsIconic(app.window)) ShowWindow(app.window,SW_RESTORE);
    if (!SetForegroundWindow(app.window))
    {
        FLASHWINFO flash{sizeof(flash),app.window,FLASHW_TRAY|FLASHW_TIMERNOFG,3,0};
        FlashWindowEx(&flash);
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    ViewerApp* app = reinterpret_cast<ViewerApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<ViewerApp*>(create->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);

    switch (message)
    {
    case kAccessibilityQueryMessage:
        if (auto* request=reinterpret_cast<AccessibilityQueryRequest*>(lParam)) request->result=AccessibleInfo(*app,request->control);
        return 0;
    case kAccessibilityActionMessage:
        InvokeAccessible(*app,static_cast<viewer_accessibility::Control>(wParam)); return 0;
    case kAccessibilityFocusMessage:
        FocusAccessible(*app,static_cast<viewer_accessibility::Control>(wParam)); return 0;
    case kAccessibilityStatusMessage:
        if (auto* result=reinterpret_cast<std::wstring*>(lParam)) *result=AccessibilityStatus(*app);
        return 0;
    case kOpenWithLaunchFailedMessage:
        ShowModeHud(*app, L"That app is no longer available");
        return 0;
    case kActivationMessage:
        for (auto& command : app->activeInstance.Drain())
        {
            ActivatePrimaryWindow(*app);
            if (command.type == active_instance::CommandType::Open && !app->closing)
                BeginOpen(*app, std::move(command.path));
        }
        return 0;
    case WM_GETOBJECT:
        if (static_cast<LONG>(lParam) == UiaRootObjectId && app->uiaAccessible)
            return UiaReturnRawElementProvider(window, wParam, lParam, app->uiaAccessible);
        if (static_cast<LONG>(lParam) == OBJID_CLIENT && app->accessible)
            return LresultFromObject(IID_IAccessible, wParam, app->accessible);
        break;
    case WM_APP + 104:
        if (!app->appSmoke || wParam > 86) return 0;
        if (wParam == 67 && (lParam == 96 || lParam == 144 || lParam == 192)) {
            app->dpi=static_cast<UINT>(lParam); app->dpiScale=static_cast<float>(app->dpi)/96.0f;
            app->toolbarHeight=Scale(*app,52); app->bottomBarHeight=Scale(*app,44);
            RecreateButtonFont(*app); LayoutControls(*app); UpdateGizmoLayout(*app); UpdateChromeLayout(*app);
            InvalidateRect(window,nullptr,TRUE); return 1;
        }
        if (wParam == 68) {
            app->highContrast=(lParam&1)!=0; app->reduceMotion=(lParam&2)!=0;
            app->renderThread.LockCamera()->reduceMotion=app->reduceMotion;
            InvalidateRect(window,nullptr,TRUE); return 1;
        }
        if (wParam == 69) return (app->highContrast?1:0)|(app->reduceMotion?2:0);
        if (wParam == 52) { app->renderThread.RequestSmokeEviction(); return 1; }
        if (wParam == 47) return app->loadedModel ? static_cast<LRESULT>(app->loadedModel->source.generationId) : 0;
        if (wParam == 46) { app->holdUploadMessagesForTesting = lParam != 0; return 1; }
        if (wParam == 45) return app->renderThread.HasCompleteModel();
        if (wParam == 44) { app->faultForTesting = lParam >= 0 && lParam <= 5 ? uint32_t(lParam) : 0; return 1; }
        if (wParam == 41) return static_cast<LRESULT>(app->errorCode);
        if (wParam == 42) return static_cast<LRESULT>(app->errorStage);
        if (wParam == 43) { CopyErrorDetails(*app); return 1; }
        if (wParam == 0) return static_cast<LRESULT>(app->state) + 1;
        if (wParam == 1) return static_cast<LRESULT>(app->generation);
        if (wParam == 21) return app->showNativeOrientation;
        if (wParam == 70) {
            if (lParam < static_cast<LPARAM>(GroundAxis::Automatic) || lParam > static_cast<LPARAM>(GroundAxis::Z)) return 0;
            app->groundAxis = static_cast<GroundAxis>(lParam);
            app->showNativeOrientation = false;
            if (app->loadedModel) {
                DirectX::XMFLOAT3 minimum{}, maximum{};
                EffectiveBounds(*app, minimum, maximum);
                app->renderThread.LockCamera()->SetBounds(minimum, maximum, ViewportAspect(*app));
            }
            InvalidateRect(window,nullptr,FALSE);
            return static_cast<LRESULT>(app->groundAxis);
        }
        if (wParam == 71) return static_cast<LRESULT>(app->groundAxis);
        if (wParam == 78) {
            app->groundAxisInverted = lParam != 0;
            app->showNativeOrientation = false;
            if (app->loadedModel) {
                DirectX::XMFLOAT3 minimum{}, maximum{};
                EffectiveBounds(*app, minimum, maximum);
                app->renderThread.LockCamera()->SetBounds(minimum, maximum, ViewportAspect(*app));
            }
            InvalidateRect(window,nullptr,FALSE);
            return app->groundAxisInverted;
        }
        if (wParam == 79) return app->groundAxisInverted;
        if (wParam == 34) return app->smokePickRequests;
        if (wParam >= 13 && wParam <= 29) {
            if (!app->loadedModel) return 0;
            const auto& metadata = *app->loadedModel;
            switch (wParam) {
            case 13: return static_cast<LRESULT>(metadata.vertexCount);
            case 14: return static_cast<LRESULT>(metadata.triangleCount);
            case 15: return static_cast<LRESULT>(metadata.pointCount);
            case 16: return metadata.boundsVerified;
            case 17: return static_cast<LRESULT>(metadata.source.format);
            case 18: return metadata.stats.hasUv0;
            case 19: return metadata.stats.materialCount;
            case 20: return metadata.stats.nodeCount;
            case 21: return app->showNativeOrientation;
            case 22: return app->meshSelected;
            case 23: case 24: case 25: {
                double dimensions[3] = { metadata.relativeMax[0]-metadata.relativeMin[0],
                    metadata.relativeMax[1]-metadata.relativeMin[1], metadata.relativeMax[2]-metadata.relativeMin[2] };
                PermuteGroundedDimensions(dimensions, app->groundAxis, metadata.source.upAxis, app->showNativeOrientation);
                return static_cast<LRESULT>(std::bit_cast<uint64_t>(dimensions[wParam-23]));
            }
            case 26: return static_cast<LRESULT>(std::bit_cast<uint64_t>(app->renderThread.LockCamera()->distance));
            case 27: return static_cast<LRESULT>(std::bit_cast<uint64_t>(app->renderThread.LockCamera()->homeDistance));
            case 28: return metadata.vertices.size() + metadata.indices.size();
            case 29: {
                const RECT viewport = ViewportRect(*app);
                const float x = metadata.pointCount ? metadata.boundsMin.x : (metadata.boundsMin.x+metadata.boundsMax.x)*0.5f;
                const float y = metadata.pointCount ? metadata.boundsMin.y : (metadata.boundsMin.y+metadata.boundsMax.y)*0.5f;
                const float z = metadata.pointCount ? metadata.boundsMin.z : (metadata.boundsMin.z+metadata.boundsMax.z)*0.5f;
                auto camera = app->renderThread.LockCamera();
                const auto clip = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(x,y,z,1), ActiveModelTransform(*app)
                    * camera->ViewMatrix() * camera->ProjectionMatrix(ViewportAspect(*app)));
                const int pixelX = viewport.left + int((DirectX::XMVectorGetX(clip)+1)*0.5f*(viewport.right-viewport.left));
                const int pixelY = viewport.top + int((1-DirectX::XMVectorGetY(clip))*0.5f*(viewport.bottom-viewport.top));
                return MAKELPARAM(pixelX,pixelY);
            }
            }
        }
        if (wParam==62) { app->renderThread.SetSmokeBudget(uint64_t(lParam)*1024*1024); return 1; }
        if (wParam==63) { app->renderThread.SetSmokeUma(lParam!=0); return 1; }
        if (wParam==65) { app->renderThread.InjectDeviceRemovalForTesting(); return 1; }
        if (wParam==40) return static_cast<LRESULT>(app->warning.size());
        // STEP-007 evidence: last bounded STEP host phase (closed kStepPhase*)
        // and its N-of-M definition counts. Zero phase means no STEP event yet.
        if (wParam == 84) return static_cast<LRESULT>(app->stepProgressPhase.load(std::memory_order_relaxed));
        if (wParam == 85) return static_cast<LRESULT>(app->stepProgressDone.load(std::memory_order_relaxed));
        if (wParam == 86) return static_cast<LRESULT>(app->stepProgressTotal.load(std::memory_order_relaxed));
        if (wParam >= 80 && wParam <= 83) {
            if (!app->loadedModel) return 0;
            const auto& stats = app->loadedModel->stats;
            if (wParam == 80) return stats.meshCount;
            if (wParam == 81) return stats.animationCount;
            if (wParam == 82) return stats.skinCount;
            return stats.boneCount;
        }
        if (wParam == 30) { ToggleShowNativeOrientation(*app); return app->showNativeOrientation; }
        return static_cast<LRESULT>(app->renderThread.SmokeValue(static_cast<unsigned>(wParam)));
    case WM_COPYDATA:
    {
        if (!app->appSmoke || !lParam) return 0;
        const auto& data = *reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if ((data.dwData != 104 && data.dwData != 105 && data.dwData != 106 && data.dwData != 107) || !data.lpData ||
            data.cbData < sizeof(wchar_t) || data.cbData > 32768 || data.cbData % sizeof(wchar_t)) return 0;
        const auto* text = static_cast<const wchar_t*>(data.lpData);
        const size_t length = data.cbData / sizeof(wchar_t);
        if (text[length - 1] != L'\0' || wcsnlen(text, length) != length - 1) return 0;
        if (data.dwData == 106) { app->smokePickerPath.assign(text,length-1); return 1; }
        if (data.dwData == 107) {
            // WM_DROPFILES carries process-local storage that an external smoke
            // driver cannot manufacture safely. Exercise the same one-path
            // opening boundary after the shell has decoded that storage.
            BeginOpen(*app, std::wstring(text, length - 1));
            return static_cast<LRESULT>(app->generation);
        }
        BeginOpen(*app, std::wstring(text, length - 1));
        // Cancel before dispatching completion notices: deterministic pending-
        // generation cancellation, independent of machine/fixture speed.
        if (data.dwData == 105) CancelOpen(*app);
        return static_cast<LRESULT>(app->generation);
    }
    case WM_CREATE:
    {
        app->dpi = GetDpiForWindow(window);
        app->dpiScale = static_cast<float>(app->dpi) / 96.0f;
        app->toolbarHeight = Scale(*app, 52);
        app->bottomBarHeight = Scale(*app, 44);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        const int cornerPreference = 2;
        DwmSetWindowAttribute(window, 33, &cornerPreference, sizeof(cornerPreference));
        const COLORREF captionColor = RGB(36, 36, 38);
        const COLORREF borderColor = RGB(58, 58, 60);
        DwmSetWindowAttribute(window, 35, &captionColor, sizeof(captionColor));
        DwmSetWindowAttribute(window, 34, &borderColor, sizeof(borderColor));
        DragAcceptFiles(window, TRUE);
        RegisterPointerInputTarget(window, PT_TOUCH);
        RegisterPointerInputTarget(window, PT_PEN);
        {
            // Raw mouse input for fly-look: relative device deltas, not cursor-
            // position deltas, so look is not quantized to screen pixels and
            // isn't affected by Windows' pointer-acceleration curve — this is
            // what makes look feel smooth/analog instead of steppy.
            RAWINPUTDEVICE mouseDevice{};
            mouseDevice.usUsagePage = 0x01;   // HID_USAGE_PAGE_GENERIC
            mouseDevice.usUsage = 0x02;       // HID_USAGE_GENERIC_MOUSE
            mouseDevice.dwFlags = 0;
            mouseDevice.hwndTarget = window;
            RegisterRawInputDevices(&mouseDevice, 1, sizeof(mouseDevice));
        }
        CreateControls(*app);
        UpdateGizmoLayout(*app);
        UpdateChromeLayout(*app);
        RefreshSystemPreferences(*app);
        const auto accessibilityAlive=app->alive;
        app->accessible = viewer_accessibility::CreateProvider(window,
            [app,accessibilityAlive](auto control) { return accessibilityAlive->load()?QueryAccessibleMarshaled(*app, control):viewer_accessibility::ControlInfo{}; },
            [app,accessibilityAlive](auto control) { if(accessibilityAlive->load())InvokeAccessibleMarshaled(*app, control); },
            [app,accessibilityAlive](auto control) { if(accessibilityAlive->load())FocusAccessibleMarshaled(*app, control); },
            [app,accessibilityAlive] { return accessibilityAlive->load()?AccessibilityStatusMarshaled(*app):std::wstring{}; });
        app->uiaAccessible = viewer_accessibility::CreateUiaProvider(window,
            [app,accessibilityAlive](auto control) { return accessibilityAlive->load()?QueryAccessibleMarshaled(*app, control):viewer_accessibility::ControlInfo{}; },
            [app,accessibilityAlive](auto control) { if(accessibilityAlive->load())InvokeAccessibleMarshaled(*app, control); },
            [app,accessibilityAlive](auto control) { if(accessibilityAlive->load())FocusAccessibleMarshaled(*app, control); },
            [app,accessibilityAlive] { return accessibilityAlive->load()?AccessibilityStatusMarshaled(*app):std::wstring{}; });
        std::wstring renderError;
        if (app->benchmarkMode)
        {
            if (app->benchmarkOcclusion) app->renderThread.SetBenchmarkOccludedForTesting();
            app->renderThread.SetBenchmarkLimits(app->benchFrames,
                app->benchmarkDurationMs * static_cast<std::uint64_t>(app->benchmarkRepeat));
        }
        else
            app->renderThread.SetBenchFrames(app->benchFrames);
        auto overlay = std::make_shared<OverlayFrame>();
        overlay->info = BuildOverlayInfo(*app);
        overlay->gizmo = app->gizmo;
        overlay->chrome = app->chrome;
        app->renderThread.PublishFrameInputs(BuildFlightInput(*app), ViewportAspect(*app), std::move(overlay));
        app->rendererReady = app->renderThread.Start(window, renderError);
        if (!app->rendererReady)
        {
            app->errorSummary = L"Graphics could not be started.";
            app->errorDetails = renderError;
            app->state = ViewerState::Failed;
        }
        else
        {
            d3d12_import_bridge::EnsureImportSandboxPrepared();
        }
        UpdateButtonAvailability(*app);
        std::wstring activationError;
        if (!app->activeInstance.StartListener(window, kActivationMessage, activationError))
            SetFailure(*app, L"Single-instance activation is unavailable.", activationError);
        if (!app->initialPath.empty() && app->rendererReady) BeginOpen(*app, app->initialPath);
        return 0;
    }
    // --- Custom title bar: removes the native caption (WM_NCCALCSIZE) while
    // keeping the resizable frame, then takes over hit-testing so our own
    // D2D-drawn min/max/close (Chrome + D3D11On12Overlay::DrawTitleBar) behave like
    // real caption buttons — including DWM's Snap Layout hover flyout on
    // Maximize, via DwmDefWindowProc passthrough on every NC message below.
    // Standard recipe for "client-area title bar with a real resizable
    // frame" (same approach Windows Terminal uses).
    case WM_NCCALCSIZE:
        if (wParam)
        {
            if (app->isFullscreen)
            {
                // Give the client area the ENTIRE proposed window rect (all
                // four insets, not just the top) — otherwise the still-
                // registered WS_THICKFRAME resize-border insets on the
                // left/right/bottom eat a few pixels off the monitor-filling
                // rect ToggleFullscreen requests, leaving a sliver of desktop
                // visible along those edges instead of covering the monitor.
                return 0;
            }
            NCCALCSIZE_PARAMS& params = *reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            const LONG proposedTop = params.rgrc[0].top;
            DefWindowProcW(window, message, wParam, lParam);
            // Only the top inset (the native caption) is given back to the
            // client area; DefWindowProc's left/right/bottom resize-border
            // insets are kept so edge/corner resize still works below.
            params.rgrc[0].top = proposedTop;
            return 0;
        }
        // wParam == FALSE: lParam is a plain RECT* (the proposed window
        // rect), sent only for the window's very first sizing at creation
        // (later resizes/moves use the wParam==TRUE form above). Returning 0
        // without touching it makes the client rect equal the full window
        // rect, so the native caption never appears even for one frame.
        return 0;
    case WM_NCHITTEST:
    {
        LRESULT dwmResult = 0;
        if (DwmDefWindowProc(window, message, wParam, lParam, &dwmResult)) return dwmResult;

        const LRESULT defaultHit = DefWindowProcW(window, message, wParam, lParam);
        // Outside the client-rendered title bar (including the thin resize
        // border DefWindowProc still reports around the whole window), trust
        // its own edge/corner result rather than overriding it.
        if (defaultHit != HTCLIENT) return defaultHit;

        POINT clientPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window, &clientPoint);

        // Top-edge/top-corner resize: WM_NCCALCSIZE above hands the entire
        // top inset back to the client area, so DefWindowProc's own hit-test
        // (just consulted above) never reports HTTOP/HTTOPLEFT/HTTOPRIGHT for
        // it — the standard gotcha with this "keep a resizable frame but
        // remove the native caption" recipe (Windows Terminal's non-client
        // island window has the same manual carve-out). Detect that strip
        // ourselves, using the same metrics DefWindowProc uses for its own
        // border, so the window can still be resized — including diagonally
        // from its top corners — by dragging near the top edge.
        if (!app->isFullscreen && !IsZoomed(window))
        {
            const int resizeBorder = GetSystemMetrics(SM_CXPADDEDBORDER) + GetSystemMetrics(SM_CYSIZEFRAME);
            if (clientPoint.y < resizeBorder)
            {
                RECT client{};
                GetClientRect(window, &client);
                const int cornerWidth = resizeBorder * 2;
                if (clientPoint.x < cornerWidth) return HTTOPLEFT;
                if (clientPoint.x >= client.right - cornerWidth) return HTTOPRIGHT;
                return HTTOP;
            }
        }

        // In Fullscreen, EffectiveToolbarHeight collapses to 0: the floating
        // toolbar (drawn and hit-tested as an ordinary client-area overlay by
        // the WM_LBUTTONDOWN/MOUSEMOVE/UP handlers below, same as the bottom
        // bar's buttons) has no non-client role, so every point here reports
        // HTCLIENT rather than routing through Chrome::HitTest's HTCAPTION —
        // dragging a topmost, monitor-filling window would just look broken.
        if (clientPoint.y >= EffectiveToolbarHeight(*app)) return HTCLIENT;
        switch (app->chrome.HitTest(clientPoint))
        {
        case Chrome::Part::Minimize: return HTMINBUTTON;
        case Chrome::Part::Maximize: return HTMAXBUTTON;
        case Chrome::Part::Close: return HTCLOSE;
        case Chrome::Part::Caption: return HTCAPTION;
        default: return HTCLIENT;
        }
    }
    case WM_NCLBUTTONDOWN:
    {
        LRESULT dwmResult = 0;
        if (DwmDefWindowProc(window, message, wParam, lParam, &dwmResult)) return dwmResult;
        if (wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE)
        {
            app->chrome.pressed = wParam == HTMINBUTTON ? Chrome::Part::Minimize
                : wParam == HTMAXBUTTON ? Chrome::Part::Maximize : Chrome::Part::Close;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_NCLBUTTONUP:
    {
        LRESULT dwmResult = 0;
        if (DwmDefWindowProc(window, message, wParam, lParam, &dwmResult)) return dwmResult;
        const Chrome::Part pressedPart = app->chrome.pressed;
        app->chrome.pressed = Chrome::Part::None;
        InvalidateRect(window, nullptr, FALSE);
        if (wParam == HTMINBUTTON && pressedPart == Chrome::Part::Minimize) { ShowWindow(window, SW_MINIMIZE); return 0; }
        if (wParam == HTMAXBUTTON && pressedPart == Chrome::Part::Maximize)
        {
            ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        if (wParam == HTCLOSE && pressedPart == Chrome::Part::Close) { DestroyWindow(window); return 0; }
        break;
    }
    case WM_NCMOUSEMOVE:
    {
        LRESULT dwmResult = 0;
        if (DwmDefWindowProc(window, message, wParam, lParam, &dwmResult)) return dwmResult;
        Chrome::Part newHover = Chrome::Part::None;
        if (wParam == HTMINBUTTON) newHover = Chrome::Part::Minimize;
        else if (wParam == HTMAXBUTTON) newHover = Chrome::Part::Maximize;
        else if (wParam == HTCLOSE) newHover = Chrome::Part::Close;
        if (app->chrome.hover != newHover)
        {
            app->chrome.hover = newHover;
            if (newHover != Chrome::Part::None)
            {
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE | TME_NONCLIENT, window, 0 };
                TrackMouseEvent(&track);
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    }
    case WM_NCMOUSELEAVE:
    {
        LRESULT dwmResult = 0;
        if (DwmDefWindowProc(window, message, wParam, lParam, &dwmResult)) return dwmResult;
        if (app->chrome.hover == Chrome::Part::Minimize || app->chrome.hover == Chrome::Part::Maximize ||
            app->chrome.hover == Chrome::Part::Close)
        {
            app->chrome.hover = Chrome::Part::None;
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_VIEW_OPEN_INITIAL) BeginOpen(*app, app->initialPath);
        else HandleCommand(*app, LOWORD(wParam));
        return 0;
    case WM_DRAWITEM:
        DrawOwnerButton(*app, *reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (app->rendererReady)
        {
            // Deliberately does NOT render. WM_PAINT is reachable from
            // any nested modal loop -- TrackPopupMenu, MessageBoxW,
            // IFileOpenDialog::Show, the DWM move/size loop -- so
            // rendering here would present from the UI thread while the
            // render thread is also presenting. Just ask for a frame.
            app->renderThread.Invalidate();
        }
        else RenderFallback(*app, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SIZE:
        LayoutControls(*app);
        UpdateGizmoLayout(*app);
        UpdateChromeLayout(*app);
        if (app->rendererReady && wParam != SIZE_MINIMIZED)
        {
            // Posted and coalesced; the resize happens between frames on
            // the render thread. Previously this blocked the UI thread in
            // a full GPU drain once per drag tick.
            app->renderThread.PublishViewportAspect(ViewportAspect(*app));
            app->renderThread.RequestResize(LOWORD(lParam), HIWORD(lParam));
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED:
    {
        app->dpi = HIWORD(wParam);
        app->dpiScale = static_cast<float>(app->dpi) / 96.0f;
        app->toolbarHeight = Scale(*app, 52);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        RecreateButtonFont(*app);
        LayoutControls(*app);
        UpdateGizmoLayout(*app);
        UpdateChromeLayout(*app);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        RefreshSystemPreferences(*app);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_GETMINMAXINFO:
    {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(*app, 480);
        info->ptMinTrackSize.y = Scale(*app, 360);
        return 0;
    }
    case WM_DROPFILES:
    {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count != 1)
        {
            DragFinish(drop);
            SetFailure(*app, L"Open one model at a time.", L"Drop exactly one supported local 3D model into the viewer.");
            return 0;
        }
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, 0, path.data(), length + 1);
        path.resize(length);
        DragFinish(drop);
        BeginOpen(*app, path);
        return 0;
    }
    case WM_POINTERDOWN:
        if (CanNavigate(*app))
        {
            const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
            POINT point{};
            if (GetClientPointerPoint(window, pointerId, point) && PointInViewport(*app, point))
            {
                SetFocus(window);
                SetCapture(window);
                app->touchPoints.insert_or_assign(pointerId, point);
                ResetTouchBaseline(*app);
            }
        }
        return 0;
    case WM_POINTERUPDATE:
        if (CanNavigate(*app))
        {
            const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
            const auto found = app->touchPoints.find(pointerId);
            POINT point{};
            if (found != app->touchPoints.end() && GetClientPointerPoint(window, pointerId, point))
            {
                if (app->touchPoints.size() == 1)
                {
                    app->renderThread.LockCamera()->Orbit(static_cast<float>(point.x - found->second.x), static_cast<float>(point.y - found->second.y));
                    found->second = point;
                    ResetTouchBaseline(*app);
                }
                else
                {
                    found->second = point;
                    auto first = app->touchPoints.begin();
                    auto second = std::next(first);
                    const POINT center{ (first->second.x + second->second.x) / 2, (first->second.y + second->second.y) / 2 };
                    const double spanX = static_cast<double>(first->second.x - second->second.x);
                    const double spanY = static_cast<double>(first->second.y - second->second.y);
                    const double span = std::sqrt(spanX * spanX + spanY * spanY);
                    RECT client{}; GetClientRect(window, &client);
                    app->renderThread.LockCamera()->Pan(static_cast<float>(center.x - app->touchCenter.x), static_cast<float>(center.y - app->touchCenter.y),
                        static_cast<float>(client.bottom - EffectiveToolbarHeight(*app)));
                    if (app->touchSpan > 1.0 && span > 1.0)
                    {
                        app->renderThread.LockCamera()->Dolly(static_cast<float>(std::log(span / app->touchSpan) / 0.16));
                    }
                    app->touchCenter = center;
                    app->touchSpan = span;
                }
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_POINTERUP:
    case WM_POINTERCAPTURECHANGED:
    {
        const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        app->touchPoints.erase(pointerId);
        if (app->touchPoints.empty() && GetCapture() == window && app->pointerMode == PointerMode::None) ReleaseCapture();
        ResetTouchBaseline(*app);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            if (app->hideCursorWhileDragging && app->pointerMode != PointerMode::None)
            {
                SetCursor(nullptr);
                return TRUE;
            }
            if (app->gizmo.hover != NavGizmo::Part::None && app->pointerMode == PointerMode::None && CanNavigate(*app))
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            if (app->chrome.hover != Chrome::Part::None)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            if (app->infoButtonHover || app->infoPanelCloseButtonHover || app->fullscreenButtonHover
                || app->lightingButtonHover>=0)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_MOUSELEAVE:
        if (app->gizmo.hover != NavGizmo::Part::None)
        {
            app->gizmo.hover = NavGizmo::Part::None;
            InvalidateRect(window, nullptr, FALSE);
        }
        // Min/Max/Close hover is owned by WM_NCMOUSELEAVE, not this.
        if (app->chrome.hover != Chrome::Part::None && app->chrome.hover != Chrome::Part::Minimize &&
            app->chrome.hover != Chrome::Part::Maximize && app->chrome.hover != Chrome::Part::Close)
        {
            app->chrome.hover = Chrome::Part::None;
            InvalidateRect(window, nullptr, FALSE);
        }
        if (app->infoButtonHover || app->infoPanelCloseButtonHover || app->fullscreenButtonHover || app->lightingButtonHover>=0)
        {
            app->infoButtonHover = false;
            app->infoPanelCloseButtonHover = false;
            app->fullscreenButtonHover = false;
            app->lightingButtonHover = -1;
            InvalidateRect(window, nullptr, FALSE);
        }
        UpdateTooltipTracking(*app);
        return 0;
    case WM_LBUTTONDOWN:
    {
        const POINT downPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (app->speedFlyoutOpen)
        {
            const RECT track = SpeedFlyoutTrackRect(*app);
            const RECT panel = SpeedFlyoutRect(*app);
            RECT hitTrack = track;
            InflateRect(&hitTrack, 0, Scale(*app, 8));
            if (PtInRect(&hitTrack, downPoint))
            {
                SetCapture(window);
                app->speedSliderDragging = true;
                SetFlySpeedFromFlyoutX(*app, downPoint.x);
                InvalidateRect(window, nullptr, FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
            app->speedFlyoutOpen = false;
            InvalidateRect(window, nullptr, FALSE);
            if (PtInRect(&panel, downPoint)) return 0;
            // A click outside the panel closes it and still falls through,
            // so clicking a different title-bar button both dismisses the
            // flyout and performs that click in one action.
        }
        if (app->settingsPanelOpen)
        {
            const RECT switchRect = SettingsSwitchRect(*app);
            const RECT cursorSwitchRect = SettingsCursorSwitchRect(*app);
            const RECT panel = SettingsPanelRect(*app);
            if (PtInRect(&switchRect, downPoint))
            {
                ToggleShowNativeOrientation(*app);
                return 0;
            }
            if (PtInRect(&cursorSwitchRect, downPoint))
            {
                ToggleHideCursorWhileDragging(*app);
                return 0;
            }
            app->settingsPanelOpen = false;
            InvalidateRect(window, nullptr, FALSE);
            if (PtInRect(&panel, downPoint)) return 0;
            // Same outside-click semantics as the Speed flyout above: close
            // and still fall through, so a click on another button both
            // dismisses this panel and performs that click in one action.
        }
        if (app->infoPanelVisible)
        {
            const RECT closeButton = InfoPanelCloseButtonRect(*app);
            if (PtInRect(&closeButton, downPoint))
            {
                SetCapture(window);
                app->infoPanelCloseButtonPressed = true;
                InvalidateRect(window, nullptr, FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
        }
        // The raw toolbarHeight, not EffectiveToolbarHeight (0 in Fullscreen)
        // — the action buttons stay clickable there as a floating toolbar
        // overlaying the full-monitor viewport (WM_NCHITTEST above already
        // routes these points to plain client messages instead of NC ones).
        if (downPoint.y < app->toolbarHeight)
        {
            const Chrome::Part part = app->chrome.HitTest(downPoint);
            if (part != Chrome::Part::None && part != Chrome::Part::Caption)
            {
                SetCapture(window);
                app->chrome.pressed = part;
                InvalidateRect(window, nullptr, FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
        }
        if (CanNavigate(*app))
        {
            const int lightingButton=HitLightingButton(*app,downPoint);
            if (lightingButton>=0)
            {
                SetCapture(window);
                app->lightingButtonPressed=lightingButton;
                InvalidateRect(window,nullptr,FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
            RECT hitTrack = ZoomTrackRect(*app);
            InflateRect(&hitTrack, 0, Scale(*app, 8));
            if (PtInRect(&hitTrack, downPoint))
            {
                SetCapture(window);
                app->zoomSliderDragging = true;
                SetZoomFromTrackX(*app, downPoint.x);
                InvalidateRect(window, nullptr, FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
            const RECT infoButton = InfoButtonRect(*app);
            if (PtInRect(&infoButton, downPoint))
            {
                SetCapture(window);
                app->infoButtonPressed = true;
                InvalidateRect(window, nullptr, FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
            const RECT fullscreenButton = FullscreenButtonRect(*app);
            if (PtInRect(&fullscreenButton, downPoint))
            {
                SetCapture(window);
                app->fullscreenButtonPressed = true;
                InvalidateRect(window, nullptr, FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
        }
        if (PointInViewport(*app, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }) && CanNavigate(*app))
        {
            SetFocus(window);
            const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const bool lightRing = app->lightingMode == LightingMode::Directional;
            const NavGizmo::Part part = app->gizmo.HitTest(app->renderThread.LockCamera()->Orientation(),
                static_cast<float>(point.x), static_cast<float>(point.y), lightRing,
                app->directionalLightAngle, app->directionalLightElevation);
            if (part == NavGizmo::Part::Light)
            {
                // Outer light ring: drag the sun to rotate (and raise/lower) the
                // directional light. Never orbits the camera.
                SetCapture(window);
                app->pointerMode = PointerMode::LightDrag;
                app->renderThread.LockCamera()->CancelInertia();
                SetDirectionalLightFromGizmoPoint(*app, point);
            }
            else if (part == NavGizmo::Part::Ball)
            {
                SetCapture(window);
                app->pointerMode = PointerMode::GizmoOrbit;
                BeginWrappedDrag(*app, point);
                app->renderThread.LockCamera()->CancelInertia();
                app->orbitVelocityX = 0.0;
                app->orbitVelocityY = 0.0;
                app->lastOrbitMoveSeconds = NowSeconds();
            }
            else if (part != NavGizmo::Part::None)
            {
                // Axis node/stem: snap immediately on press.
                app->renderThread.LockCamera()->SnapToView(CanonicalViewOrientation(app->gizmo.ViewFor(part)));
                InvalidateRect(window, nullptr, FALSE);
            }
            else
            {
                // Plain LMB: orbit-drags immediately; a release that never
                // exceeded the click/drag threshold click-selects instead.
                SetCapture(window);
                app->pointerMode = PointerMode::Orbit;
                BeginWrappedDrag(*app, point);
                app->renderThread.LockCamera()->CancelInertia();
                app->orbitVelocityX = 0.0;
                app->orbitVelocityY = 0.0;
                app->lastOrbitMoveSeconds = NowSeconds();
                app->selectDragged = false;
                app->selectDownPoint = point;
                app->selectDownSeconds = NowSeconds();
            }
        }
        return 0;
    }
    case WM_MBUTTONDOWN:
        if (PointInViewport(*app, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }) && CanNavigate(*app))
        {
            SetFocus(window);
            SetCapture(window);
            const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            // Plain MMB trucks along the ground plane; Ctrl+MMB dollies.
            app->pointerMode = control ? PointerMode::DollyDrag : PointerMode::Truck;
            BeginWrappedDrag(*app, point);
            if (app->pointerMode == PointerMode::Truck)
            {
                app->renderThread.LockCamera()->CancelInertia();
                app->panVelocityX = 0.0;
                app->panVelocityY = 0.0;
                app->lastPanMoveSeconds = NowSeconds();
            }
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (PointInViewport(*app, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }) && CanNavigate(*app))
        {
            SetFocus(window);
            SetCapture(window);
            app->pointerMode = PointerMode::FlyLook;
            app->flyLook = true;
            GetCursorPos(&app->flyPressPoint);
            // Look is driven by WM_INPUT's raw relative deltas (registered at
            // WM_CREATE), not cursor-position deltas, so there is no screen
            // edge to fall off and nothing to recenter. Cursor visibility is
            // controlled by the persisted drag setting.
            if (app->hideCursorWhileDragging) SetCursor(nullptr);
        }
        return 0;
    case WM_MOUSEMOVE:
    {
        const POINT movePoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (app->speedSliderDragging)
        {
            if (GetCapture() == window) SetFlySpeedFromFlyoutX(*app, movePoint.x);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (app->zoomSliderDragging)
        {
            if (GetCapture() == window) SetZoomFromTrackX(*app, movePoint.x);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (app->chrome.pressed != Chrome::Part::None) return 0;
        if (app->infoButtonPressed || app->infoPanelCloseButtonPressed || app->fullscreenButtonPressed
            || app->lightingButtonPressed>=0) return 0;
        if (app->pointerMode == PointerMode::None)
        {
            const RECT panelCloseButton = InfoPanelCloseButtonRect(*app);
            const bool overPanelClose = app->infoPanelVisible && PtInRect(&panelCloseButton, movePoint) != FALSE;
            if (overPanelClose != app->infoPanelCloseButtonHover)
            {
                app->infoPanelCloseButtonHover = overPanelClose;
                if (overPanelClose)
                {
                    TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                    TrackMouseEvent(&track);
                }
                InvalidateRect(window, nullptr, FALSE);
            }
            // Idle hover tracking: the title-bar action buttons above the
            // viewport, the gizmo within it. (Min/Max/Close hover is tracked
            // separately via WM_NCMOUSEMOVE, since those points are always
            // non-client.)
            if (movePoint.y < app->toolbarHeight)
            {
                const Chrome::Part hit = app->chrome.HitTest(movePoint);
                const Chrome::Part effective = hit == Chrome::Part::Caption ? Chrome::Part::None : hit;
                if (effective != app->chrome.hover)
                {
                    app->chrome.hover = effective;
                    if (effective != Chrome::Part::None)
                    {
                        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                        TrackMouseEvent(&track);
                    }
                    InvalidateRect(window, nullptr, FALSE);
                }
                if (app->gizmo.hover != NavGizmo::Part::None)
                {
                    app->gizmo.hover = NavGizmo::Part::None;
                    InvalidateRect(window, nullptr, FALSE);
                }
                UpdateTooltipTracking(*app);
                return 0;
            }
            if (app->chrome.hover != Chrome::Part::None)
            {
                app->chrome.hover = Chrome::Part::None;
                InvalidateRect(window, nullptr, FALSE);
            }
            // Bottom-bar Info/Fullscreen hover tracking (only while that bar
            // is actually shown, i.e. a model is loaded).
            if (CanNavigate(*app))
            {
                RECT client{};
                GetClientRect(window, &client);
                if (movePoint.y >= client.bottom - app->bottomBarHeight)
                {
                    const RECT infoButton = InfoButtonRect(*app);
                    const RECT fullscreenButton = FullscreenButtonRect(*app);
                    const bool overInfo = PtInRect(&infoButton, movePoint) != FALSE;
                    const bool overFullscreen = PtInRect(&fullscreenButton, movePoint) != FALSE;
                    const int overLighting=HitLightingButton(*app,movePoint);
                    if (overInfo != app->infoButtonHover || overFullscreen != app->fullscreenButtonHover
                        || overLighting!=app->lightingButtonHover)
                    {
                        app->infoButtonHover = overInfo;
                        app->fullscreenButtonHover = overFullscreen;
                        app->lightingButtonHover=overLighting;
                        if (overInfo || overFullscreen || overLighting>=0)
                        {
                            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                            TrackMouseEvent(&track);
                        }
                        InvalidateRect(window, nullptr, FALSE);
                    }
                }
                else if (app->infoButtonHover || app->fullscreenButtonHover || app->lightingButtonHover>=0)
                {
                    app->infoButtonHover = false;
                    app->fullscreenButtonHover = false;
                    app->lightingButtonHover=-1;
                    InvalidateRect(window, nullptr, FALSE);
                }
            }
            if (CanNavigate(*app) && PointInViewport(*app, movePoint))
            {
                const NavGizmo::Part part = app->gizmo.HitTest(app->renderThread.LockCamera()->Orientation(),
                    static_cast<float>(movePoint.x), static_cast<float>(movePoint.y),
                    app->lightingMode == LightingMode::Directional,
                    app->directionalLightAngle, app->directionalLightElevation);
                if (part != app->gizmo.hover)
                {
                    app->gizmo.hover = part;
                    if (part != NavGizmo::Part::None)
                    {
                        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                        TrackMouseEvent(&track);
                    }
                    InvalidateRect(window, nullptr, FALSE);
                }
            }
            else if (app->gizmo.hover != NavGizmo::Part::None)
            {
                app->gizmo.hover = NavGizmo::Part::None;
                InvalidateRect(window, nullptr, FALSE);
            }
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (GetCapture() == window)
        {
            const POINT pointer{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const float deltaX = static_cast<float>(pointer.x - app->lastPointer.x);
            const float deltaY = static_cast<float>(pointer.y - app->lastPointer.y);
            app->lastPointer = pointer;
            switch (app->pointerMode)
            {
            case PointerMode::FlyLook:
                // Look is driven by WM_INPUT (raw deltas); the cursor is just
                // kept hidden here when requested since its position is unused.
                if (app->hideCursorWhileDragging) SetCursor(nullptr);
                break;
            case PointerMode::Orbit:
            case PointerMode::GizmoOrbit:
                TrackOrbitVelocity(*app, deltaX, deltaY);
                app->renderThread.LockCamera()->Orbit(deltaX, deltaY);
                WrapCursorIfNeeded(*app);
                break;
            case PointerMode::Truck:
                TrackPanVelocity(*app, deltaX, deltaY);
                app->renderThread.LockCamera()->Truck(deltaX, deltaY,
                    static_cast<float>(ViewportRect(*app).bottom - ViewportRect(*app).top), app->axisSnapEnabled);
                WrapCursorIfNeeded(*app);
                break;
            case PointerMode::DollyDrag:
                app->renderThread.LockCamera()->DollyDrag(deltaY);
                WrapCursorIfNeeded(*app);
                break;
            case PointerMode::LightDrag:
                SetDirectionalLightFromGizmoPoint(*app, pointer);
                break;
            default: break;
            }
            // A plain-LMB orbit gesture also owns click-select: track whether
            // it stayed under the drag threshold, so a release without a real
            // drag still click-selects (drag and click share the button).
            if (app->pointerMode == PointerMode::Orbit &&
                std::abs(pointer.x - app->selectDownPoint.x) + std::abs(pointer.y - app->selectDownPoint.y) >
                    kClickDragThresholdPixels)
            {
                app->selectDragged = true;
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (app->speedSliderDragging)
        {
            app->speedSliderDragging = false;
            if (GetCapture() == window) ReleaseCapture();
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->zoomSliderDragging)
        {
            app->zoomSliderDragging = false;
            if (GetCapture() == window) ReleaseCapture();
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->chrome.pressed != Chrome::Part::None)
        {
            const POINT upPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const Chrome::Part pressedPart = app->chrome.pressed;
            app->chrome.pressed = Chrome::Part::None;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            if (upPoint.y < app->toolbarHeight && app->chrome.HitTest(upPoint) == pressedPart)
            {
                HandleChromeAction(*app, pressedPart);
            }
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->lightingButtonPressed>=0)
        {
            const POINT upPoint{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            const int pressed=app->lightingButtonPressed;
            app->lightingButtonPressed=-1;
            if (GetCapture()==window) ReleaseCapture();
            if (HitLightingButton(*app,upPoint)==pressed)
            {
                if (pressed==0) SetLightingMode(*app,LightingMode::Studio);
                else if (pressed==1) SetLightingMode(*app,LightingMode::Clay);
                else if (pressed==2) SetLightingMode(*app,LightingMode::Directional);
                else SetLightingMode(*app,LightingMode::Wireframe);
            }
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->infoButtonPressed)
        {
            const POINT upPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            app->infoButtonPressed = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            const RECT infoButton = InfoButtonRect(*app);
            if (PtInRect(&infoButton, upPoint)) HandleCommand(*app, ID_VIEW_INFO);
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->infoPanelCloseButtonPressed)
        {
            const POINT upPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            app->infoPanelCloseButtonPressed = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            const RECT closeButton = InfoPanelCloseButtonRect(*app);
            if (PtInRect(&closeButton, upPoint)) CloseInfoPanel(*app);
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->fullscreenButtonPressed)
        {
            const POINT upPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            app->fullscreenButtonPressed = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            const RECT fullscreenButton = FullscreenButtonRect(*app);
            if (PtInRect(&fullscreenButton, upPoint)) HandleCommand(*app, ID_VIEW_FULLSCREEN);
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->pointerMode == PointerMode::Orbit && !app->selectDragged && CanNavigate(*app) &&
            NowSeconds() - app->selectDownSeconds < kClickMaxSeconds)
        {
            ClickSelect(*app, app->selectDownPoint);
        }
        if ((app->pointerMode == PointerMode::Orbit || app->pointerMode == PointerMode::GizmoOrbit) &&
            CanNavigate(*app) && NowSeconds() - app->lastOrbitMoveSeconds < 0.07)
        {
            app->renderThread.LockCamera()->SeedOrbitInertia(static_cast<float>(app->orbitVelocityX),
                static_cast<float>(app->orbitVelocityY));
        }
        EndPointer(*app);
        return 0;
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        if (app->pointerMode == PointerMode::Truck && CanNavigate(*app) &&
            NowSeconds() - app->lastPanMoveSeconds < 0.07)
        {
            app->renderThread.LockCamera()->SeedPanInertia(static_cast<float>(app->panVelocityX),
                static_cast<float>(app->panVelocityY));
        }
        EndPointer(*app);
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        app->lightingButtonPressed=-1;
        EndPointer(*app);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (PointInViewport(*app, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }) && CanNavigate(*app))
        {
            FrameSelectedOrAll(*app);
        }
        return 0;
    case WM_MOUSEWHEEL:
    {
        POINT wheelPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };   // screen coords for this message
        ScreenToClient(window, &wheelPoint);
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        const RECT infoPanel = InfoPanelRect(*app);
        if (app->infoPanelVisible && PtInRect(&infoPanel, wheelPoint))
        {
            const float maxScroll = InfoPanelMaxScroll(*app);
            app->infoPanelScrollOffset = std::clamp(
                app->infoPanelScrollOffset - steps * static_cast<float>(Scale(*app, 48)), 0.0f, maxScroll);
            InvalidateRect(window, nullptr, FALSE);
        }
        else if (CanNavigate(*app))
        {
            if (app->flyLook) AdjustFlySpeed(*app, steps);
            else
            {
                app->renderThread.LockCamera()->Dolly(steps);
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_INPUT:
        if (app->flyLook)
        {
            UINT size = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
            if (size > 0 && size <= sizeof(RAWINPUT))
            {
                RAWINPUT raw{};
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == size &&
                    raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 &&
                    (raw.data.mouse.lLastX != 0 || raw.data.mouse.lLastY != 0))
                {
                    // Look is accumulated under the camera lock and consumed by
                    // the render thread when it updates the next frame.
                    app->renderThread.LockCamera()->AccumulateLook(static_cast<float>(raw.data.mouse.lLastX), static_cast<float>(raw.data.mouse.lLastY));
                    app->renderThread.PublishFlightInput(BuildFlightInput(*app));
                    app->renderThread.Invalidate();
                    InvalidateRect(window, nullptr, FALSE);
                }
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_KEYDOWN:
        if (wParam == VK_OEM_2 && GetKeyState(VK_SHIFT) < 0) { ShowControlsDialog(window); return 0; }
        if (wParam == VK_F11) { ToggleFullscreen(*app); return 0; }
        if (wParam == VK_ESCAPE)
        {
            if (app->isFullscreen) { ToggleFullscreen(*app); return 0; }
            CancelOpen(*app);
            return 0;
        }
        if (HandleAccessibleKey(*app, wParam)) return 0;
        if (!CanNavigate(*app)) break;
        if (SetNavigationKey(*app, wParam, true)) return 0;
        if (wParam == VK_OEM_PLUS || wParam == VK_ADD) app->renderThread.LockCamera()->Dolly(1.0f);
        else if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) app->renderThread.LockCamera()->Dolly(-1.0f);
        else break;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KEYUP:
        if (SetNavigationKey(*app, wParam, false)) return 0;
        break;
    case WM_KILLFOCUS:
        StopNavigation(*app);
        EndPointer(*app);
        if (app->speedFlyoutOpen) { app->speedFlyoutOpen = false; InvalidateRect(window, nullptr, FALSE); }
        if (app->settingsPanelOpen) { app->settingsPanelOpen = false; InvalidateRect(window, nullptr, FALSE); }
        return 0;
    case WM_CONTEXTMENU:
        return 0;
    case WM_SYSKEYDOWN:
        if (wParam == 'M' && (lParam & (1u << 29))) { ShowMoreMenu(*app); return 0; }
        break;
    case kD3D12ImportCompleteMessage:
    {
        std::unique_ptr<D3D12CompleteMessage> complete(reinterpret_cast<D3D12CompleteMessage*>(lParam));
        if (!complete || complete->generation != app->generation || app->state == ViewerState::Failed) return 0;
        if (complete->result.errorCode == model_core::ImportErrorCode::Cancelled) {
            CancelOpen(*app); return 0;
        }
        if (!complete->result.ok)
        {
            SetFailure(*app, complete->result.errorSummary, complete->result.errorDetails, complete->path, complete->result.errorCode, complete->result.errorStage, complete->result.errorPhase);
            return 0;
        }
        app->renderThread.FinishImport(complete->generation, complete->result.sourceIdentity);
        return 0;
    }
    case kRenderStartFailedMessage:
    {
        std::unique_ptr<RenderStartFailure> failure(reinterpret_cast<RenderStartFailure*>(lParam));
        app->rendererReady = false;
        if (app->cancellation) app->cancellation->store(true, std::memory_order_relaxed);
        app->importThreads.clear();
        import_broker::ShutdownImportWorkerPool();
        import_broker::ShutdownCompatibilityHost();
        app->renderThread.CancelUploads();
        SetFailure(*app, L"Graphics could not be started.",
                   failure ? failure->details : L"The render thread stopped during initialization.");
        return 0;
    }
    case kRenderDeviceRecoveryMessage:
    {
        std::unique_ptr<RenderDeviceRecoveryResult> recovery(
            reinterpret_cast<RenderDeviceRecoveryResult*>(lParam));
        if (!recovery) return 0;
        if (recovery->recovered && !recovery->path.empty()) {
            BeginOpen(*app, recovery->path);
        } else {
            SetFailure(*app, L"Graphics recovery failed.", recovery->details, recovery->path,
                       model_core::ImportErrorCode::UploadFailure, import_broker::ImportStage::Upload);
        }
        return 0;
    }
    case kRenderPickCompleteMessage:
    {
        std::unique_ptr<RenderPickResult> picked(reinterpret_cast<RenderPickResult*>(lParam));
        if (picked && app->loadedModel && picked->generation == app->loadedModel->source.generationId)
            ApplySelection(*app, picked->hit);
        return 0;
    }
    case kRenderUploadCompleteMessage:
    {
        std::unique_ptr<RenderUploadResult> uploaded(reinterpret_cast<RenderUploadResult*>(lParam));
        if (!uploaded || uploaded->generation != app->generation || app->state == ViewerState::Failed) return 0;
        if (app->appSmoke && app->holdUploadMessagesForTesting) return 0;
        if (!uploaded->ok)
        {
            SetFailure(*app, uploaded->errorSummary, uploaded->errorDetails, uploaded->path, uploaded->errorCode, import_broker::ImportStage::Upload);
            return 0;
        }
        if (uploaded->metadata) {
            if (!app->loadedModel || app->loadedModel->source.generationId != uploaded->generation) app->meshSelected = false;
            app->loadedModel = uploaded->metadata;
            app->warning=uploaded->metadata->warning;
        }
        app->currentPath = uploaded->path;
        app->filename = FileNameFromPath(uploaded->path);
        app->state = uploaded->terminal ? ViewerState::Ready : ViewerState::Loading;
        // Refinement publications also use `terminal` to mean that particular
        // update is complete. Only the non-refinement marker ends the original
        // file-open operation and should arm the load-to-present timer.
        if (uploaded->terminal && !uploaded->refinement && app->renderStartedMicroseconds != 0)
            app->renderPresentationPending = true;
        app->failedPath.clear();
        app->errorSummary.clear();
        app->errorDetails.clear();
        UpdateTitle(*app);
        if (uploaded->terminal && !uploaded->refinement && GetForegroundWindow()==window) SetFocus(window);
        UpdateButtonAvailability(*app);
        LayoutControls(*app);
        InvalidateRect(window, nullptr, FALSE);
        if (uploaded->terminal || !app->warning.empty())
            viewer_accessibility::Announce(window, app->uiaAccessible, AccessibilityStatus(*app));
        return 0;
    }
    case WM_TIMER:
        if (wParam == kTooltipTimerId)
        {
            KillTimer(window, kTooltipTimerId);
            if (app->tooltipTargetId != 0)
            {
                app->tooltipVisible = true;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_DESTROY:
        app->closing = true;
        app->alive->store(false, std::memory_order_relaxed);
        if(app->uiaAccessible) UiaDisconnectProvider(app->uiaAccessible);
        app->activeInstance.Stop();
        if (app->cancellation) app->cancellation->store(true, std::memory_order_relaxed);
        if (app->buttonFont) { DeleteObject(app->buttonFont); app->buttonFont = nullptr; }
        // Stop() signals, then joins with a bounded wait and drains the GPU
        // on the render thread itself. `04-rendering-and-streaming.md:190`:
        // shutdown waits "with finite diagnostics timeouts" and "a driver
        // hang must not leave the UI thread waiting forever".
        app->renderThread.Stop();
        if (app->accessible) { app->accessible->Release(); app->accessible = nullptr; }
        if (app->uiaAccessible) { app->uiaAccessible->Release(); app->uiaAccessible = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

ATOM RegisterViewerClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_PREVIEW3D));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = gBackgroundBrush;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SMALL));
    return RegisterClassExW(&windowClass);
}

bool CreateMainWindow(ViewerApp& app, int showCommand)
{
    const UINT dpi = GetDpiForSystem();
    RECT windowBounds{ 0, 0, MulDiv(1000, dpi, 96), MulDiv(720, dpi, 96) };
    AdjustWindowRectExForDpi(&windowBounds, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_ACCEPTFILES | WS_EX_CONTROLPARENT, dpi);
    const int width = windowBounds.right - windowBounds.left;
    const int height = windowBounds.bottom - windowBounds.top;
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int x = monitor.rcWork.left + std::max<LONG>(0, (monitor.rcWork.right - monitor.rcWork.left - width) / 2);
    const int y = monitor.rcWork.top + std::max<LONG>(0, (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2);
    gMainWindow = CreateWindowExW(WS_EX_ACCEPTFILES | WS_EX_CONTROLPARENT, kWindowClass, kApplicationName,
        WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr, app.instance, &app);
    if (!gMainWindow) return false;
    ShowWindow(gMainWindow, showCommand);
    UpdateWindow(gMainWindow);
    return true;
}

// Smart App Control evaluates every executable image separately and has no
// per-app exception, so an unsigned engineering build can be blocked even when
// the main executable is allowed. Detect it up front so the user gets an
// explanation instead of a Bad Image dialog from a bundled DLL. The policy
// value is 0 when Smart App Control is off; any other value means it is active.
bool IsSmartAppControlActive()
{
    DWORD state = 0;
    DWORD stateSize = sizeof(state);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\CI\\Policy",
        L"VerifiedAndReputablePolicyState",
        RRF_RT_REG_DWORD, nullptr, &state, &stateSize);
    return status == ERROR_SUCCESS && state != 0;
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand)
{
    const auto processStartedUs = NowMicroseconds();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT comResult = E_FAIL;

    ViewerApp app;
    app.instance = instance;
    app.benchmarkStartedUs = processStartedUs;
    const ViewerSettings settings = LoadSettings();
    app.showNativeOrientation = settings.showNativeOrientation;
    app.groundAxis = settings.groundAxis;
    app.groundAxisInverted = settings.groundAxisInverted;
    app.hideCursorWhileDragging = settings.hideCursorWhileDragging;
    bool commandLineInvalid = false;
    bool bypassSingleInstance = false;
    bool activationSmoke = false;
    int argumentCount = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments)
    {
        // Accept --d3d12 as a deprecated no-op for existing launch scripts.
        // The first remaining argument is the initial file path.
        for (int i = 1; i < argumentCount; ++i)
        {
            if (_wcsicmp(arguments[i], L"--open") == 0)
            {
                if (++i >= argumentCount || !app.initialPath.empty() || arguments[i][0] == L'\0') commandLineInvalid = true;
                else app.initialPath = arguments[i];
            }
            else if (_wcsicmp(arguments[i], L"--new-instance") == 0) bypassSingleInstance = true;
            else if (_wcsicmp(arguments[i], L"--d3d12") == 0) continue;
            else if (_wcsicmp(arguments[i], L"--app-smoke") == 0) app.appSmoke = true;
            else if (_wcsicmp(arguments[i], L"--activation-smoke") == 0) {
                app.appSmoke = true;
                activationSmoke = true;
            }
            else if (_wcsicmp(arguments[i], L"--uma-budget-smoke") == 0) {
                app.appSmoke=true; app.renderThread.SetSmokeUmaDevice();
            }
            else if (_wcsicmp(arguments[i], L"--coarse-proxy-smoke") == 0) {
                app.appSmoke = true; app.renderThread.SetSmokeUploads(750, 4ull*1024*1024, false);
            }
            else if (_wcsicmp(arguments[i], L"--texture-mip-smoke") == 0) {
                app.appSmoke = true; app.renderThread.SetSmokeUploads(750, 4ull*1024*1024, true);
            }
            else if (_wcsicmp(arguments[i], L"--texture-batch-smoke") == 0) {
                // Protocol v7's complete 64-byte vertex layout makes the
                // smallest ordinary triangle batch larger than the former
                // 420-byte seam; 640 still splits the material/image catalog.
                app.appSmoke = true; app.renderThread.SetSmokeUploads(750, 640, false);
            }
            else if (_wcsicmp(arguments[i], L"--queue-smoke") == 0) {
                app.appSmoke = true; app.renderThread.SetSmokeUploads(750, 8192, false);
            }
            else if (_wcsicmp(arguments[i], L"--queue-byte-smoke") == 0) {
                app.appSmoke = true; app.renderThread.SetSmokeUploads(750, 8192, false);
                app.renderThread.SetSmokeQueueCap(16 * 1024);
            }
            else if (_wcsicmp(arguments[i], L"--progressive-smoke") == 0) {
                app.appSmoke = true; app.renderThread.SetSmokeUploads(750, 4096, true);
            }
            else if (_wcsicmp(arguments[i], L"--frame-stats") == 0) app.showFrameStats = true;
            else if (_wcsicmp(arguments[i], L"--benchmark") == 0) app.benchmarkMode = true;
            else if (_wcsnicmp(arguments[i], L"--benchmark=", 12) == 0) {
                app.benchmarkMode = true;
                app.initialPath = arguments[i] + 12;
            }
            else if (_wcsnicmp(arguments[i], L"--benchmark-result=", 19) == 0)
                app.benchmarkResultPath = arguments[i] + 19;
            else if (_wcsnicmp(arguments[i], L"--benchmark-duration-ms=", 24) == 0)
                app.benchmarkDurationMs = _wcstoui64(arguments[i] + 24, nullptr, 10);
            else if (_wcsnicmp(arguments[i], L"--benchmark-frames=", 19) == 0)
                app.benchmarkFrameLimit = _wtoi(arguments[i] + 19);
            else if (_wcsnicmp(arguments[i], L"--benchmark-repeat=", 19) == 0)
                app.benchmarkRepeat = _wtoi(arguments[i] + 19);
            else if (_wcsnicmp(arguments[i], L"--benchmark-reference=", 22) == 0)
                app.benchmarkReference = arguments[i] + 22;
            else if (_wcsnicmp(arguments[i], L"--benchmark-etw=", 16) == 0)
                app.benchmarkEtwPath = arguments[i] + 16;
            else if (_wcsicmp(arguments[i], L"--benchmark-worker-budget-failure") == 0) {
                app.appSmoke = true; app.faultForTesting = 6;
            }
            else if (_wcsicmp(arguments[i], L"--benchmark-occluded") == 0)
                app.benchmarkOcclusion = true;
            // Deprecated spike flags are no-ops; every frame paints real chrome.
            else if (_wcsicmp(arguments[i], L"--overlay-spike") == 0 ||
                     _wcsnicmp(arguments[i], L"--overlay-spike=", 16) == 0) continue;
            else if (_wcsnicmp(arguments[i], L"--frame-bench=", 14) == 0)
            {
                app.benchFrames = _wtoi(arguments[i] + 14);
                app.showFrameStats = true;
            }
            else if (arguments[i][0] == L'-') commandLineInvalid = true;
            else if (app.initialPath.empty()) app.initialPath = arguments[i];
            else commandLineInvalid = true;
        }
        LocalFree(arguments);
    }

    if (!app.initialPath.empty())
    {
        std::wstring normalized, pathError;
        if (!active_instance::NormalizeForwardPath(app.initialPath, normalized, pathError)) commandLineInvalid = true;
        else app.initialPath = std::move(normalized);
    }
    if (commandLineInvalid)
    {
        MessageBoxW(nullptr, L"Usage: Preview3D.exe [model-path]\n       Preview3D.exe --open <model-path>",
            kApplicationName, MB_OK | MB_ICONERROR);
        return 2;
    }

    if (app.benchmarkMode) {
        const bool validReference = app.benchmarkReference == L"performance" || app.benchmarkReference == L"compatibility";
        if (app.initialPath.empty() || app.benchmarkFrameLimit < 1 || app.benchmarkFrameLimit > 100'000
            || app.benchmarkRepeat < 1 || app.benchmarkRepeat > 100 || app.benchmarkDurationMs < 100
            || app.benchmarkDurationMs > 600'000 || !validReference) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
            const char message[] = "Invalid --benchmark arguments (fixture, frames 1..100000, duration 100..600000 ms, repeat 1..100, reference performance|compatibility).\n";
            DWORD written = 0; WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, sizeof(message) - 1, &written, nullptr);
            if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
            if (SUCCEEDED(comResult)) CoUninitialize();
            return 64;
        }
        app.showFrameStats = true;
        const auto requested = static_cast<std::uint64_t>(app.benchmarkFrameLimit) * app.benchmarkRepeat + 120;
        app.benchFrames = static_cast<int>(std::min<std::uint64_t>(requested, 10'000'000));
        app.benchmarkViewerBaselinePrivate = PrivateCommit(GetCurrentProcess());
        app.benchmarkSampler = std::jthread([&app](std::stop_token stop) {
            while (!stop.stop_requested()) {
                SampleBenchmarkMemory(app);
                Sleep(50);
            }
        });
    }

    std::wstring instanceError;
    const bool developerRun = bypassSingleInstance || (app.appSmoke && !activationSmoke)
        || app.benchmarkMode || app.benchFrames > 0;
    const auto instanceRole = app.activeInstance.Initialize(developerRun, instanceError);
    if (instanceRole == active_instance::Coordinator::Role::Failed)
    {
        MessageBoxW(nullptr, instanceError.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
        return 5;
    }
    if (instanceRole == active_instance::Coordinator::Role::Secondary)
    {
        active_instance::Command command;
        command.type = app.initialPath.empty() ? active_instance::CommandType::Activate : active_instance::CommandType::Open;
        command.path = app.initialPath;
        if (app.activeInstance.Forward(command, instanceError)) return 0;
        MessageBoxW(nullptr, instanceError.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
        return 3;
    }

    // Explain an unsigned engineering build up front on Smart App Control
    // machines. It cannot be bypassed per app, and it may later refuse a bundled
    // dependency (for example worker\zstd.dll with status 0xC0E90002). Skipped
    // for smoke/benchmark runs so automation is never interrupted by a dialog.
    if (!app.appSmoke && !app.benchmarkMode && app.benchFrames == 0 && IsSmartAppControlActive())
    {
        MessageBoxW(nullptr,
            L"Smart App Control is active on this PC, and this Preview 3D build is unsigned.\n\n"
            L"Smart App Control has no per-app exception, so Windows may block the app or one of the DLLs bundled beside it (for example worker\\zstd.dll, error status 0xC0E90002).\n\n"
            L"If Preview 3D is blocked, turn off Smart App Control under Windows Security > App & browser control > Smart App Control settings.",
            kApplicationName, MB_OK | MB_ICONWARNING);
    }

    comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX commonControls{ sizeof(commonControls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&commonControls);
    HIGHCONTRASTW startupContrast{sizeof(startupContrast)};
    const bool startupHighContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(startupContrast),&startupContrast,0)
        && (startupContrast.dwFlags&HCF_HIGHCONTRASTON)!=0;
    gBackgroundBrush = CreateSolidBrush(startupHighContrast ? GetSysColor(COLOR_WINDOW) : RGB(28, 28, 30));

    if (!RegisterViewerClass(instance) || !CreateMainWindow(app, showCommand))
    {
        ShutdownOpenWithCatalog();
        if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }
    InitializeOpenWithCatalog(gMainWindow, kOpenWithLaunchFailedMessage);

    HACCEL accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDC_PREVIEW3D));
    MSG message{};
    int exitCode = 0;
    bool quitting = false;
    bool benchReported = false;
    std::uint64_t lastHeartbeatUs = NowMicroseconds();
    while (!quitting)
    {
        const auto heartbeatUs = NowMicroseconds();
        if (app.benchmarkMode) AtomicMaximum(app.benchmarkHeartbeatMaxUs, heartbeatUs - lastHeartbeatUs);
        lastHeartbeatUs = heartbeatUs;
        // Bounded to a single burst: a self-recentering FlyLook mouse-move can
        // otherwise repost itself indefinitely (some input stacks emit a fresh
        // WM_MOUSEMOVE for every SetCursorPos, even a no-op one) and never let
        // PeekMessageW go empty, starving the render check below forever.
        int drained = 0;
        while (drained < kMaxDrainedMessagesPerIteration && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            ++drained;
            if (message.message == WM_QUIT)
            {
                if (!app.benchmarkMode || exitCode == 0)
                    exitCode = static_cast<int>(message.wParam);
                quitting = true;
                break;
            }
            // Accelerators are translated first: IsDialogMessageW would
            // otherwise consume keydowns before the hotkeys ever see them.
            // Escape is dispatched straight through instead: the window has
            // WS_EX_CONTROLPARENT (for the error-state child buttons), which
            // makes IsDialogMessageW treat it as dialog-like and silently eat
            // Escape as a "cancel" keystroke before WM_KEYDOWN's own
            // Escape-exits-Fullscreen/cancel-open handling ever runs.
            if (message.message == WM_KEYDOWN && (message.wParam == VK_ESCAPE ||
                (message.wParam == VK_TAB && GetFocus() == gMainWindow)))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            else if (!TranslateAcceleratorW(gMainWindow, accelerators, &message))
            {
                if (gMainWindow && IsDialogMessageW(gMainWindow, &message)) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (quitting) break;

        // The render thread owns the frame loop; publish UI inputs and animation
        // state, then wait for messages without blocking in Present.
        bool uiAnimating = false;
        if (gMainWindow && app.rendererReady)
        {
            FinishRenderTimerIfPresented(app);
            uiAnimating = !IsIconic(gMainWindow) && IsAnimatingWithoutCamera(app);
            auto overlay = std::make_shared<OverlayFrame>();
            overlay->info = BuildOverlayInfo(app);
            overlay->gizmo = app.gizmo;
            overlay->chrome = app.chrome;
            app.renderThread.PublishFrameInputs(BuildFlightInput(app), ViewportAspect(app), std::move(overlay));
            app.renderThread.SetUiAnimating(uiAnimating);
            if (app.benchmarkMode && !app.benchmarkInputSent && app.renderThread.SmokeValue(3) != 0) {
                app.benchmarkInputSent = true;
                app.renderThread.NotifyBenchmarkInput(NowMicroseconds());
                PostMessageW(gMainWindow, WM_KEYDOWN, VK_LEFT, 0);
                PostMessageW(gMainWindow, WM_KEYUP, VK_LEFT, 0);
            }
            if (GetUpdateRect(gMainWindow, nullptr, FALSE))
            {
                ValidateRect(gMainWindow, nullptr);
                app.renderThread.Invalidate();
            }
            // The bench writes its final numbers into the title from
            // here, on the UI thread -- the render thread must never
            // touch the window.
            if (app.benchFrames > 0 && app.renderThread.BenchComplete() && !benchReported)
            {
                benchReported = true;
                UpdateTitle(app);
                if (app.benchmarkMode) {
                    WriteBenchmarkResult(app, exitCode);
                    PostMessageW(gMainWindow, WM_CLOSE, 0, 0);
                }
            }
        }
        // Only poll while something UI-owned is animating (held keys,
        // HUD timers, the loading spinner) -- those are the states whose
        // input has to be republished. Otherwise block as before, so an
        // idle UI thread neither burns CPU nor competes with the render
        // thread for the camera lock. A running bench also has to poll,
        // or nothing is left to notice it finished.
        const bool benchWaiting = app.benchFrames > 0 && !benchReported;
        const DWORD wait = uiAnimating ? kUiPollIntervalMs
            : benchWaiting             ? (app.benchmarkMode ? kUiPollIntervalMs : kBenchPollIntervalMs)
                                        : INFINITE;
        MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    // The window/render lane is already gone. Join loader lanes before
    // shutting down their process managers so no lease can outlive its pool;
    // both shutdowns are bounded and kill-on-close remains the hard backstop.
    app.importThreads.clear();
    import_broker::ShutdownCompatibilityHost();
    import_broker::ShutdownImportWorkerPool();
    ShutdownOpenWithCatalog();
    if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
    if (SUCCEEDED(comResult)) CoUninitialize();
    return exitCode;
}

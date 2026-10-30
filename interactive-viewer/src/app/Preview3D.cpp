#include "framework.h"
#include "Preview3D.h"
#include "AboutDialog.h"
#include "Accessibility.h"
#include "ActiveInstance.h"
#include "Chrome.h"
#include "ControlsDialog.h"
#include "D3D12ImportBridge.h"
#include "RenderThread.h"
#include "InfoPanel.h"
#include "Localization.h"
#include "Model.h"
#include "Renderer.h"
#include "NavGizmo.h"
#include "OpenWithPopup.h"
#include "OpenOptions.h"
#include "Settings.h"
#include "ShellIntegration.h"
#include "SafeFileOps.h"
#include "UpdateApply.h"
#include "UpdateCheck.h"
#include "Version.h"
#include "VersionCompare.h"
#include "platform/ProcessMitigations.h"
#include "platform/SourcePathPolicy.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <iomanip>
#include <sstream>
#include <bit>
#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <span>
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
constexpr UINT kOpenWithCatalogChangedMessage = WM_APP + 14;
constexpr UINT kOpenWithIconsReadyMessage = WM_APP + 15;
constexpr UINT kUpdateCheckCompleteMessage = WM_APP + 16;
constexpr UINT kUpdateDownloadCompleteMessage = WM_APP + 17;
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

struct UpdateDownloadResult
{
    bool ok = false;
    std::wstring installerPath;
    std::wstring expectedSha256;
    std::wstring error;
};

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

struct OpenWithItem
{
    std::wstring displayName;
    std::wstring handlerKey;
    std::wstring iconPath;
    int iconIndex = 0;
    OpenWithGroup group = OpenWithGroup::Recommended;
    bool recommended = false;
    std::shared_ptr<const open_with::IconPixels> icon;
    std::function<bool(const std::wstring& path)> invoke;
};

struct OpenWithPopupState
{
    bool open = false;
    bool loading = false;
    std::wstring extension;
    std::vector<OpenWithItem> apps;
    open_with_ui::Layout layout{};
    int hovered = -1;   // app index, or apps.size() for the fallback tile
    int focused = -1;
    int pressed = -1;
    int scrollOffset = 0;
    std::vector<std::wstring> requestedIcons;
};

struct ViewerApp
{
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND retryButton = nullptr;
    HWND openAnotherButton = nullptr;
    HWND openAsButton = nullptr;
    HWND copyButton = nullptr;
    HWND tooltip = nullptr;
    // Owns the text buffers referenced by the tooltip control for its lifetime
    // (a deque so pointers to earlier entries survive later insertions).
    std::deque<std::wstring> tooltipTexts;
    HFONT buttonFont = nullptr;
    UINT dpi = 96;
    float dpiScale = 1.0f;
    int toolbarHeight = 52;
    int bottomBarHeight = 44;
    bool infoPanelVisible = false;
    float infoPanelScrollOffset = 0.0f;   // logical px, clamped against content on wheel input
    bool speedFlyoutOpen = false;
    bool speedSliderDragging = false;
    // Orbit (turntable auto-rotate): while enabled, hovering the Orbit toolbar
    // button reveals a speed slider (see OrbitFlyoutRect / OrbitSpeedFlyoutVisible).
    bool orbitEnabled = false;
    double orbitSpeedDegreesPerSecond = 30.0;
    bool orbitSpeedFlyoutHover = false;
    bool orbitSpeedSliderDragging = false;
    bool zoomSliderDragging = false;
    bool infoButtonHover = false;
    bool infoButtonPressed = false;
    bool infoPanelCloseButtonHover = false;
    bool infoPanelCloseButtonPressed = false;
    bool fullscreenButtonHover = false;
    bool fullscreenButtonPressed = false;
    bool warningButtonHover = false;
    bool warningButtonPressed = false;
    // Floating top-left viewport Reset button. `resetViewPending` keeps the UI
    // loop polling while the camera animates back home; `resetViewButtonAlpha`
    // fades the button in/out around ViewMoved(), and `resetViewButtonFading`
    // keeps polling until the fade has actually reached its target.
    bool resetViewButtonHover = false;
    bool resetViewButtonPressed = false;
    bool resetViewPending = false;
    float resetViewButtonAlpha = 0.0f;
    bool resetViewButtonFading = false;
    bool resetViewButtonFadeInitialized = false;
    float resetViewButtonFadeTarget = 0.0f;
    double lastResetViewFadeSeconds = 0.0;
    // The gizmo's roll arrows fade in/out with the face-view state, using the
    // same timing as the Reset button above.
    float rollGlyphAlpha = 0.0f;
    bool rollGlyphFading = false;
    bool rollGlyphFadeInitialized = false;
    float rollGlyphFadeTarget = 0.0f;
    double lastRollGlyphFadeSeconds = 0.0;
    LightingMode lightingMode = LightingMode::Studio;
    // Catalog-driven view state applied once when a document first opens.
    // `activePresentation` is what the info panel reports; an explicit user
    // action (lighting button or grid toggle) pins the session so a reload or
    // refinement cannot stomp the override.
    model_core::PresentationProfile activePresentation = model_core::PresentationProfile::Default;
    bool presentationOverridden = false;
    std::wstring presentationSourcePath;
    // T15 view state. Presets seed these defaults; explicit exposure/background
    // changes pin their field for the rest of the session so a later preset
    // application cannot stomp the user's choice.
    viewer_presentation::PresentationPreset activePreset = viewer_presentation::PresentationPreset::StudioPbr;
    viewer_presentation::ToneMapMode toneMapMode = viewer_presentation::ToneMapMode::Neutral;
    viewer_presentation::BackgroundMode backgroundMode = viewer_presentation::BackgroundMode::Dark;
    uint32_t backgroundColor = 0x001C1C1Eu;
    bool groundHighlightVisible = true;
    float exposure = 1.0f;
    float studioIntensity = 1.0f;
    float keyLightScale = 1.0f;
    float fillLightScale = 1.0f;
    float rimLightScale = 1.0f;
    bool exposureOverridden = false;
    bool toneMapOverridden = false;
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
    bool captureClean = false; // T20: omit viewport overlays from capture evidence
    // T21 Debug inspection state. UI-thread only; the render thread owns the
    // channel, readback and material snapshot behind RenderThread methods.
    int debugMaterialIndex = 0;
    int debugSampleX0 = 0, debugSampleY0 = 0, debugSampleX1 = 1, debugSampleY1 = 1;
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
    OpenWithPopupState openWith;
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
    // --format / Open As: a catalog descriptor that replaces extension/content
    // candidate selection. Null means normal classification.
    const model_core::FormatDescriptor* forcedFormat = nullptr;
    bool skipContentCheck = false;
    // Path-free "opened as X; detected Y" text kept across the import so the
    // success warning and diagnostics can report the user override.
    std::wstring openFormatNotice;
    std::wstring currentPath;
    std::wstring failedPath;
    std::wstring filename;
    std::uint64_t renderStartedMicroseconds = 0;
    bool renderPresentationPending = false;
    std::wstring renderDurationText;
    std::wstring warning;
    // Authored relative references whose sidecar assets could not be found for
    // the loaded model. Non-empty makes the warning badge offer a folder picker
    // and a re-import.
    std::vector<std::wstring> missingAssets;
    // Directories the user has offered as asset search roots. Applied to the
    // immediate re-import; a fresh Open/Drop clears them.
    std::vector<std::wstring> assetSearchRoots;
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
    bool shadowVisible = true;
    bool shadowOverridden = false;
    bool axisSnapEnabled = false;
    // Display the loaded model in its native/source orientation instead of
    // this app's normalized Z-up correction (Model.h's ModelData::
    // upAxisCorrection). Persisted via Settings.h; default matches
    // ViewerSettings' default (normalized).
    bool showNativeOrientation = false;
    GroundAxis groundAxis = GroundAxis::Automatic;
    bool groundAxisInverted = false;
    bool hideCursorWhileDragging = true;
    // Persisted via Settings.h; mirrors RenderThread::SetHideModelUntilComplete.
    bool hideModelUntilComplete = true;
    bool settingsPanelOpen = false;
    // Active UI language pack code ("" == follow the Windows display language),
    // mirrored from ViewerSettings so the Settings panel can show and change it.
    std::wstring language;
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
    // Non-empty only when the background release check found a version newer
    // than PREVIEW3D_VERSION. Drawing, hit-testing, the tooltip, and the
    // accessibility tree all key the update badge off this one field.
    std::wstring updateVersion;
    std::jthread updateCheckThread;
    // One download at a time. The worker only downloads and verifies; the UI
    // thread owns the confirmation, handoff, and shutdown.
    std::jthread updateDownloadThread;
    bool updateDownloading = false;
    bool updateRestartPending = false;
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
    if (!app.benchmarkResultPath.empty()) {
        // The path was validated and confined to the app-owned directory when
        // the command line was parsed; write it reparse-safely with a unique
        // temp name rather than truncating `path` in place.
        std::wstring writeError;
        const bool ok = preview3d::safeio::WriteFileAtomically(app.benchmarkResultPath,
            std::as_bytes(std::span(text)), true, writeError);
        if (!ok) exitCode = 3;
        return ok;
    }
    HANDLE output = INVALID_HANDLE_VALUE;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    const bool ok = output != INVALID_HANDLE_VALUE && WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
        && written == text.size();
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
        *app.loadedModel, app.showNativeOrientation, app.groundAxis, app.activePresentation);
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

// Turntable auto-orbit (Orbit toolbar button). Speed is stored in degrees per
// second for the UI and handed to the camera in radians per second.
constexpr double kOrbitSliderMax = 100.0;
constexpr double kOrbitSpeedMinDegrees = 5.0;
constexpr double kOrbitSpeedMaxDegrees = 180.0;
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

void ApplyOrbitRate(ViewerApp& app)
{
    app.renderThread.LockCamera()->SetTurntableSpeed(
        app.orbitEnabled ? app.orbitSpeedDegreesPerSecond * kDegreesToRadians : 0.0);
}

void SetOrbitEnabled(ViewerApp& app, bool enabled)
{
    app.orbitEnabled = enabled;
    if (!enabled)
    {
        app.orbitSpeedFlyoutHover = false;
        app.orbitSpeedSliderDragging = false;
    }
    ApplyOrbitRate(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void AdjustOrbitSpeed(ViewerApp& app, double deltaDegrees)
{
    app.orbitSpeedDegreesPerSecond = std::clamp(app.orbitSpeedDegreesPerSecond + deltaDegrees,
        kOrbitSpeedMinDegrees, kOrbitSpeedMaxDegrees);
    ApplyOrbitRate(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleOrbit(ViewerApp& app)
{
    if (!CanNavigate(app)) return;
    SetOrbitEnabled(app, !app.orbitEnabled);
    ShowModeHud(app, app.orbitEnabled ? Loc("hud.orbitOn", L"Orbit on")
                                      : Loc("hud.orbitOff", L"Orbit off"));
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
        HasNavigableModel(app), IsZoomed(app.window) != FALSE, app.isFullscreen,
        !app.updateVersion.empty());
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

// Orbit speed flyout: same geometry as the Speed flyout, anchored under the
// Orbit button. Only visible while auto-orbit is on and the pointer is over
// the button or the panel itself.
RECT OrbitFlyoutRect(const ViewerApp& app)
{
    const RECT orbitButton = app.chrome.Button(Chrome::Part::Orbit).rect;
    const int width = Scale(app, 220);
    const int height = Scale(app, 64);
    const int gap = Scale(app, 6);
    RECT client{};
    GetClientRect(app.window, &client);
    int left = std::min(orbitButton.left, client.right - Scale(app, 8) - width);
    left = std::max(left, Scale(app, 8));
    const int top = app.toolbarHeight + gap;
    return RECT{ left, top, left + width, top + height };
}

RECT OrbitFlyoutTrackRect(const ViewerApp& app)
{
    const RECT panel = OrbitFlyoutRect(app);
    const int marginX = Scale(app, 16);
    const int trackCenterY = panel.top + Scale(app, 44);
    const int halfHeight = Scale(app, 8);
    return RECT{ panel.left + marginX, trackCenterY - halfHeight, panel.right - marginX, trackCenterY + halfHeight };
}

// Extends the panel up to the title bar so moving the pointer from the Orbit
// button down into the slider never crosses a hover gap that would close it.
RECT OrbitFlyoutHoverRect(const ViewerApp& app)
{
    RECT rect = OrbitFlyoutRect(app);
    rect.top = std::min<LONG>(rect.top, app.toolbarHeight);
    return rect;
}

bool OrbitSpeedFlyoutVisible(const ViewerApp& app)
{
    if (!app.orbitEnabled || !HasNavigableModel(app)) return false;
    return app.chrome.hover == Chrome::Part::Orbit || app.orbitSpeedFlyoutHover || app.orbitSpeedSliderDragging;
}

int OrbitSliderPositionFor(double degreesPerSecond)
{
    const double t = (std::clamp(degreesPerSecond, kOrbitSpeedMinDegrees, kOrbitSpeedMaxDegrees)
        - kOrbitSpeedMinDegrees) / (kOrbitSpeedMaxDegrees - kOrbitSpeedMinDegrees);
    return static_cast<int>(std::lround(t * kOrbitSliderMax));
}

double OrbitSpeedForSliderPosition(int position)
{
    const double t = std::clamp(static_cast<double>(position), 0.0, kOrbitSliderMax) / kOrbitSliderMax;
    return kOrbitSpeedMinDegrees + t * (kOrbitSpeedMaxDegrees - kOrbitSpeedMinDegrees);
}

void SetOrbitSpeedFromFlyoutX(ViewerApp& app, int clientX)
{
    const RECT track = OrbitFlyoutTrackRect(app);
    const float t = std::clamp(static_cast<float>(clientX - track.left) / static_cast<float>(std::max(1L, track.right - track.left)), 0.0f, 1.0f);
    app.orbitSpeedDegreesPerSecond = OrbitSpeedForSliderPosition(static_cast<int>(std::lround(t * kOrbitSliderMax)));
    ApplyOrbitRate(app);
}

// Settings popup panel/row/switch geometry, shared by drawing (BuildOverlayInfo)
// and input handling (WM_LBUTTONDOWN) so they can never disagree. Mirrors
// SpeedFlyoutRect/SpeedFlyoutTrackRect just above, anchored under the
// Overflow ("...") button instead of the Speed button.
RECT SettingsPanelRect(const ViewerApp& app)
{
    const RECT overflowButton = app.chrome.Button(Chrome::Part::Overflow).rect;
    const int width = Scale(app, 300);
    const int height = Scale(app, 292);
    const int gap = Scale(app, 6);
    RECT client{};
    GetClientRect(app.window, &client);
    int left = std::min(overflowButton.left, client.right - Scale(app, 8) - width);
    left = std::max(left, Scale(app, 8));
    const int top = app.toolbarHeight + gap;
    // Keep the panel inside a short/narrow client instead of letting the
    // language row fall off the bottom. Rows themselves stay scale-relative.
    const int available = client.bottom - top - Scale(app, 8);
    const int panelHeight = std::max(Scale(app, 252),
        std::min(height, available > 0 ? available : Scale(app, 252)));
    return RECT{ left, top, left + width, top + panelHeight };
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
        panel.right - marginX, panel.top + Scale(app, 100) };
}

RECT SettingsCursorSwitchRect(const ViewerApp& app)
{
    const RECT row = SettingsCursorToggleRowRect(app);
    const int switchWidth = Scale(app, 36);
    const int switchHeight = Scale(app, 20);
    const int centerY = (row.top + row.bottom) / 2;
    return RECT{ row.right - switchWidth, centerY - switchHeight / 2, row.right, centerY + switchHeight / 2 };
}

RECT SettingsHideUntilCompleteRowRect(const ViewerApp& app)
{
    const RECT panel = SettingsPanelRect(app);
    const int marginX = Scale(app, 16);
    return RECT{ panel.left + marginX, panel.top + Scale(app, 104),
        panel.right - marginX, panel.top + Scale(app, 148) };
}

RECT SettingsHideUntilCompleteSwitchRect(const ViewerApp& app)
{
    const RECT row = SettingsHideUntilCompleteRowRect(app);
    const int switchWidth = Scale(app, 36);
    const int switchHeight = Scale(app, 20);
    const int centerY = (row.top + row.bottom) / 2;
    return RECT{ row.right - switchWidth, centerY - switchHeight / 2, row.right, centerY + switchHeight / 2 };
}

// Background and ground-highlight rows sit between the existing switches and
// the language selector. The panel grows accordingly; all rects still use the
// same Scale()/clamp helper as the rest of the chrome, so DPI and narrow
// windows keep the same relative layout.
RECT SettingsBackgroundRowRect(const ViewerApp& app)
{
    const RECT panel = SettingsPanelRect(app);
    const int marginX = Scale(app, 16);
    return RECT{ panel.left + marginX, panel.top + Scale(app, 152),
        panel.right - marginX, panel.top + Scale(app, 196) };
}

RECT SettingsGroundHighlightRowRect(const ViewerApp& app)
{
    const RECT panel = SettingsPanelRect(app);
    const int marginX = Scale(app, 16);
    return RECT{ panel.left + marginX, panel.top + Scale(app, 200),
        panel.right - marginX, panel.top + Scale(app, 244) };
}

RECT SettingsGroundHighlightSwitchRect(const ViewerApp& app)
{
    const RECT row = SettingsGroundHighlightRowRect(app);
    const int switchWidth = Scale(app, 36);
    const int switchHeight = Scale(app, 20);
    const int centerY = (row.top + row.bottom) / 2;
    return RECT{ row.right - switchWidth, centerY - switchHeight / 2, row.right, centerY + switchHeight / 2 };
}

// Language row: a native popup menu button.
RECT SettingsLanguageRowRect(const ViewerApp& app)
{
    const RECT panel = SettingsPanelRect(app);
    const int marginX = Scale(app, 16);
    return RECT{ panel.left + marginX, panel.top + Scale(app, 248),
        panel.right - marginX, panel.bottom - Scale(app, 8) };
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
    ShowSpeedHud(app, LocFormat("hud.travelSpeed", L"Travel speed ×{0}", { FormatMultiplier(scaled) }));
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

// Bottom-right warning badge: the same content-anchored box the renderer draws
// (above the bottom bar, left of the Information panel). Empty whenever no
// warning is active, so input handling can treat an empty rect as "no badge".
// Shared by drawing (BuildOverlayInfo) and WM_LBUTTONDOWN/MOUSEMOVE/UP, so the
// two can never disagree about where the badge is.
RECT WarningButtonRect(const ViewerApp& app)
{
    if (app.warning.empty() || !HasNavigableModel(app)) return RECT{};
    RECT client{};
    GetClientRect(app.window, &client);
    const int size = Scale(app, 30);
    const int margin = Scale(app, 14);
    const int contentRight = client.right - InfoPanelWidthPixels(app);
    const int contentBottom = client.bottom - (HasNavigableModel(app) ? app.bottomBarHeight : 0);
    return RECT{ contentRight - margin - size, contentBottom - margin - size,
                 contentRight - margin, contentBottom - margin };
}

// Floating Reset-view button pinned to the top-left of the 3D viewport (not
// the title bar). Shared by drawing (BuildOverlayInfo), input handling, and
// accessibility, so all three agree on the circle's bounds.
RECT ResetViewButtonRect(const ViewerApp& app)
{
    if (!HasNavigableModel(app)) return RECT{};
    const RECT viewport = ViewportRect(app);
    const int size = Scale(app, 38);
    const int margin = Scale(app, 14);
    const int left = viewport.left + margin;
    // In Fullscreen the viewport starts at y=0 but the floating title-bar
    // buttons still occupy their usual strip, so stay below it there too.
    const int top = static_cast<int>(std::max<LONG>(viewport.top, app.toolbarHeight)) + margin;
    return RECT{ left, top, left + size, top + size };
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
    return LocFormat("hud.directionalLight", L"Directional light  {0}\u00B0 / {1}\u00B0",
        { std::to_wstring(static_cast<int>(std::lround(app.directionalLightAngle * 360.0f))),
          std::to_wstring(static_cast<int>(std::lround(app.directionalLightElevation * kRadiansToDegrees))) });
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

// Applies a material-aware presentation preset exactly once for a document.
// Point clouds use the existing round colored splat pipeline (which is unlit),
// so they keep Studio selected for the toolbar while the grid is suppressed.
// Studio CAD uses the neutral Clay review material; the PBR profile uses the
// material-aware Studio shader. This function does not call SetLightingMode,
// because that would mark the choice as a user override.
void SaveViewerPreferences(const ViewerApp& app);

void ApplyPresentationSettings(ViewerApp& app, model_core::PresentationProfile profile,
                               viewer_presentation::PresentationPreset preset)
{
    const auto settings = viewer_presentation::SettingsForPreset(preset);
    app.activePresentation = profile;
    app.activePreset = preset;
    app.studioIntensity = settings.environmentIntensity;
    app.keyLightScale = settings.keyScale;
    app.fillLightScale = settings.fillScale;
    app.rimLightScale = settings.rimScale;
    if (!app.exposureOverridden) app.exposure = settings.exposure;
    if (!app.toneMapOverridden) app.toneMapMode = settings.toneMap;
    // T21a: background and ground highlight are global user preferences. A
    // preset (including Scan/Transparent) never rewrites them.
    app.shadowVisible = viewer_presentation::ResolveShadowVisible(
        app.shadowOverridden, app.shadowVisible, settings.shadowEnabled);
    switch (preset)
    {
    case viewer_presentation::PresentationPreset::StudioCad:
        app.lightingMode = LightingMode::Clay;
        break;
    case viewer_presentation::PresentationPreset::PointCloud:
    case viewer_presentation::PresentationPreset::ScanSculpt:
    case viewer_presentation::PresentationPreset::Transparent:
    case viewer_presentation::PresentationPreset::StudioPbr:
    default:
        app.lightingMode = LightingMode::Studio;
        break;
    }
    app.gridVisible = settings.gridVisible;
}

void ApplyPresentationProfile(ViewerApp& app, model_core::PresentationProfile profile,
                              const viewer_presentation::MaterialSummary& summary)
{
    const auto preset = viewer_presentation::SelectPresentationPreset(summary, profile);
    ApplyPresentationSettings(app, profile, preset);
}

void ApplyManualPresentationPreset(ViewerApp& app, viewer_presentation::PresentationPreset preset)
{
    app.presentationOverridden = true;
    // An explicit preset choice is the user's view state: apply its exposure
    // rather than preserving an earlier manual exposure. Background and ground
    // highlight remain global appearance preferences.
    app.exposureOverridden = false;
    app.toneMapOverridden = false;
    app.shadowOverridden = false;
    const auto profile = model_core::FindBySourceFormatId(
        app.loadedModel ? app.loadedModel->source.format : model_core::SourceFormatId::Unknown);
    ApplyPresentationSettings(app, profile ? profile->presentation : model_core::PresentationProfile::Default,
                              preset);
}

void ShowPresentationHud(ViewerApp& app)
{
    app.modeHudText = std::wstring(viewer_presentation::PresetName(app.activePreset)) + L"  \u2022  "
        + viewer_presentation::BackgroundName(app.backgroundMode) + L"  \u2022  "
        + viewer_presentation::ToneMapName(app.toneMapMode) + L"  \u2022  exp "
        + std::to_wstring(app.exposure).substr(0, 4);
    app.modeHudUntil = NowSeconds() + kHudVisibleSeconds;
}

std::wstring BackgroundDisplayLabel(const ViewerApp& app)
{
    switch (app.backgroundMode)
    {
    case viewer_presentation::BackgroundMode::MidGrey:
        return Loc("settings.background.gray", L"Studio gray");
    case viewer_presentation::BackgroundMode::Custom:
    {
        wchar_t text[16]{};
        swprintf_s(text, L"#%06X", app.backgroundColor & 0x00FFFFFFu);
        return text;
    }
    case viewer_presentation::BackgroundMode::Transparent:
        return Loc("settings.background.transparent", L"Transparent (capture alpha)");
    case viewer_presentation::BackgroundMode::Checkerboard:
        return Loc("settings.background.checker", L"Checkerboard (transparency review)");
    case viewer_presentation::BackgroundMode::Dark:
    default:
        return Loc("settings.background.dark", L"Windows Photos dark");
    }
}

void AdjustExposure(ViewerApp& app, float delta)
{
    app.exposure = viewer_presentation::ClampExposure(app.exposure + delta);
    app.exposureOverridden = true;
    ShowPresentationHud(app);
    SaveViewerPreferences(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void SetBackgroundMode(ViewerApp& app, viewer_presentation::BackgroundMode mode)
{
    app.backgroundMode = mode;
    ShowPresentationHud(app);
    SaveViewerPreferences(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void CycleBackground(ViewerApp& app)
{
    const int next = (static_cast<int>(app.backgroundMode) + 1) % 5;
    SetBackgroundMode(app, static_cast<viewer_presentation::BackgroundMode>(next));
}

void ToggleGroundHighlight(ViewerApp& app)
{
    app.groundHighlightVisible = !app.groundHighlightVisible;
    ShowModeHud(app, app.groundHighlightVisible
        ? Loc("hud.groundHighlightShown", L"Ground highlight shown")
        : Loc("hud.groundHighlightHidden", L"Ground highlight hidden"));
    SaveViewerPreferences(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

// Background dropdown: a native popup menu, matching the language selector's
// established convention. Custom color opens the standard color dialog.
void ShowBackgroundMenu(ViewerApp& app)
{
    HMENU menu = CreatePopupMenu();
    const std::wstring labels[] = {
        Loc("settings.background.dark", L"Windows Photos dark"),
        Loc("settings.background.gray", L"Studio gray"),
        Loc("settings.background.custom", L"Custom color\u2026"),
        Loc("settings.background.transparent", L"Transparent (capture alpha)"),
        Loc("settings.background.checker", L"Checkerboard (transparency review)"),
    };
    // Menu order follows the enum only for the three opaque choices; the two
    // transparency modes are listed last with accurate labels.
    static constexpr int order[] = { 0, 1, 4, 2, 3 };
    static constexpr UINT kBackgroundMenuBase = 51000;
    for (int index = 0; index < 5; ++index) {
        const UINT flags = MF_STRING | (static_cast<int>(app.backgroundMode) == order[index]
            ? MF_CHECKED : 0u);
        AppendMenuW(menu, flags, kBackgroundMenuBase + order[index], labels[index].c_str());
    }
    const RECT row = SettingsBackgroundRowRect(app);
    POINT anchor{ row.left, row.bottom };
    ClientToScreen(app.window, &anchor);
    const int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
        anchor.x, anchor.y, 0, app.window, nullptr);
    DestroyMenu(menu);
    if (selected < static_cast<int>(kBackgroundMenuBase)
        || selected >= static_cast<int>(kBackgroundMenuBase) + 5) return;
    const int mode = selected - static_cast<int>(kBackgroundMenuBase);
    if (mode != static_cast<int>(viewer_presentation::BackgroundMode::Custom)) {
        SetBackgroundMode(app, static_cast<viewer_presentation::BackgroundMode>(mode));
        return;
    }
    // Native custom color picker. Cancel leaves the saved color untouched.
    static COLORREF customColors[16]{};
    CHOOSECOLORW choose{};
    choose.lStructSize = sizeof(choose);
    choose.hwndOwner = app.window;
    choose.rgbResult = RGB((app.backgroundColor >> 16) & 0xFFu,
                           (app.backgroundColor >> 8) & 0xFFu,
                           app.backgroundColor & 0xFFu);
    choose.lpCustColors = customColors;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&choose)) return;
    app.backgroundColor = (static_cast<uint32_t>(GetRValue(choose.rgbResult)) << 16)
        | (static_cast<uint32_t>(GetGValue(choose.rgbResult)) << 8)
        | static_cast<uint32_t>(GetBValue(choose.rgbResult));
    SetBackgroundMode(app, viewer_presentation::BackgroundMode::Custom);
}

void CycleToneMap(ViewerApp& app)
{
    const int next = (static_cast<int>(app.toneMapMode) + 1) % 4;
    app.toneMapMode = static_cast<viewer_presentation::ToneMapMode>(next);
    app.toneMapOverridden = true;
    ShowPresentationHud(app);
    SaveViewerPreferences(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void SetLightingMode(ViewerApp& app,LightingMode mode)
{
    // An explicit toolbar choice wins for the rest of the session; opening or
    // reloading a document must not silently replace it.
    app.presentationOverridden = true;
    app.lightingMode=mode;
    app.modeHudText=mode==LightingMode::Studio ? Loc("hud.studioLighting", L"Studio lighting")
        : mode==LightingMode::Clay ? Loc("hud.claySolid", L"Clay / Solid")
        : mode==LightingMode::Directional ? DirectionalLightHudText(app) : Loc("hud.wireframe", L"Wireframe");
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
        app.fullscreenButtonPressed || app.resetViewButtonPressed || app.speedSliderDragging ||
        app.zoomSliderDragging || app.lightingButtonPressed>=0 || app.orbitSpeedSliderDragging ||
        OrbitSpeedFlyoutVisible(app) || app.speedFlyoutOpen || app.settingsPanelOpen)
    {
        return {};
    }
    if (app.chrome.hover == Chrome::Part::GroundAxis && app.loadedModel)
    {
        const GroundAxis current = ResolveGroundAxis(app.groundAxis, app.loadedModel->source.upAxis);
        const GroundAxis next = NextGroundAxis(current, app.loadedModel->source.upAxis);
        return { static_cast<int>(Chrome::Part::GroundAxis) + 1,
            app.chrome.Button(Chrome::Part::GroundAxis).rect,
            LocFormat("tooltip.groundAxis", L"Ground axis {0}; click for {1}",
                { GroundAxisName(current), GroundAxisName(next) }), true };
    }
    if (app.chrome.hover == Chrome::Part::GroundDirection && app.loadedModel)
    {
        const GroundAxis axis = ResolveGroundAxis(app.groundAxis, app.loadedModel->source.upAxis);
        const wchar_t* currentSign = app.groundAxisInverted ? L"-" : L"+";
        const wchar_t* nextSign = app.groundAxisInverted ? L"+" : L"-";
        const std::wstring axisName = GroundAxisName(axis);
        return { static_cast<int>(Chrome::Part::GroundDirection) + 1,
            app.chrome.Button(Chrome::Part::GroundDirection).rect,
            LocFormat("tooltip.groundDirection", L"{0}{1} is up; click for {2}{3}",
                { std::wstring(currentSign), axisName, std::wstring(nextSign), axisName }), true };
    }
    if (app.chrome.hover == Chrome::Part::Update && !app.updateVersion.empty())
    {
        return { 120, app.chrome.Button(Chrome::Part::Update).rect,
            LocFormat("tooltip.update", L"Update {0} is available!", { L"v" + app.updateVersion }), true };
    }
    struct Entry { Chrome::Part part; const char* key; const wchar_t* text; };
    static constexpr Entry kEntries[] = {
        { Chrome::Part::Grid, "tooltip.grid", L"Toggle ground grid" },
        { Chrome::Part::AxisSnap, "tooltip.axisSnap", L"Snap truck to axis" },
        { Chrome::Part::Speed, "tooltip.speed", L"Flight speed" },
        { Chrome::Part::Orbit, "tooltip.orbit", L"Orbit model (auto-rotate)" },
        { Chrome::Part::Fit, "tooltip.fit", L"Frame model in view" },
        { Chrome::Part::Share, "tooltip.share", L"Share" },
        { Chrome::Part::Overflow, "tooltip.more", L"More options" },
        { Chrome::Part::OpenWith, "tooltip.openWith", L"Open with" },
    };
    for (const Entry& entry : kEntries)
    {
        if (app.chrome.hover == entry.part)
        {
            return { static_cast<int>(entry.part) + 1, app.chrome.Button(entry.part).rect, Loc(entry.key, entry.text), true };
        }
    }
    if (app.resetViewButtonHover) return { 104, ResetViewButtonRect(app), Loc("tooltip.reset", L"Reset view"), true };
    if (app.infoButtonHover) return { 100, InfoButtonRect(app), Loc("tooltip.info", L"Model information"), false };
    if (app.fullscreenButtonHover) return { 101, FullscreenButtonRect(app), Loc("tooltip.fullscreen", L"Fullscreen"), false };
    if (app.infoPanelCloseButtonHover) return { 102, InfoPanelCloseButtonRect(app), Loc("tooltip.infoClose", L"Close information panel"), true };
    if (app.warningButtonHover) return { 103, WarningButtonRect(app),
        app.missingAssets.empty() ? Loc("tooltip.warnings", L"Model warnings")
                                  : Loc("tooltip.missingAssets", L"Missing assets — click to locate them"), false };
    if (app.lightingButtonHover>=0) {
        const auto layout=ComputeLightingToolbarLayout(app);
        const RECT rects[]={layout.studio,layout.clay,layout.directional,layout.wireframe};
        struct LightingLabel { const char* key; const wchar_t* text; };
        static constexpr LightingLabel labels[]={
            {"tooltip.lightingStudio", L"Studio: neutral material lighting"},
            {"tooltip.lightingClay", L"Clay: inspect geometry without textures"},
            {"tooltip.lightingDirectional", L"Directional: rotate a sharp inspection light"},
            {"tooltip.lightingWireframe", L"Wireframe: show only mesh edges"}};
        return {110+app.lightingButtonHover,rects[app.lightingButtonHover],Loc(labels[app.lightingButtonHover].key,labels[app.lightingButtonHover].text),false};
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
    if (app.openWith.open)
    {
        const bool wasVisible = app.tooltipVisible;
        app.tooltipTargetId = 0;
        app.tooltipVisible = false;
        KillTimer(app.window, kTooltipTimerId);
        if (wasVisible) InvalidateRect(app.window, nullptr, FALSE);
        return;
    }
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
    app.presentationOverridden = true;
    app.gridVisible = !app.gridVisible;
    ShowModeHud(app, app.gridVisible ? Loc("hud.gridShown", L"Ground grid shown")
                                     : Loc("hud.gridHidden", L"Ground grid hidden"));
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleShadow(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_SHADOW)) return;
    app.shadowVisible = !app.shadowVisible;
    app.shadowOverridden = true;
    ShowModeHud(app, app.shadowVisible ? Loc("hud.shadowShown", L"Contact shadow shown")
                                       : Loc("hud.shadowHidden", L"Contact shadow hidden"));
    SaveViewerPreferences(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleProjection(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_PROJECTION)) return;
    const bool toOrthographic = app.renderThread.LockCamera()->Projection() == ProjectionMode::Perspective;
    app.renderThread.LockCamera()->SetProjection(toOrthographic ? ProjectionMode::Orthographic : ProjectionMode::Perspective);
    ShowModeHud(app, toOrthographic ? Loc("hud.orthographic", L"Orthographic")
                                    : Loc("hud.perspective", L"Perspective"));
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleAxisSnap(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_AXIS_SNAP)) return;
    app.axisSnapEnabled = !app.axisSnapEnabled;
    ShowModeHud(app, app.axisSnapEnabled ? Loc("hud.axisSnapOn", L"Axis snap on")
                                         : Loc("hud.axisSnapOff", L"Axis snap off"));
    InvalidateRect(app.window, nullptr, FALSE);
}

void SaveViewerPreferences(const ViewerApp& app)
{
    ViewerSettings settings;
    settings.version = ViewerSettings::kCurrentVersion;
    settings.showNativeOrientation = app.showNativeOrientation;
    settings.groundAxis = app.groundAxis;
    settings.groundAxisInverted = app.groundAxisInverted;
    settings.hideCursorWhileDragging = app.hideCursorWhileDragging;
    settings.hideModelUntilComplete = app.hideModelUntilComplete;
    settings.language = app.language;
    settings.toneMapMode = static_cast<int>(app.toneMapMode);
    settings.backgroundMode = static_cast<int>(app.backgroundMode);
    settings.backgroundColor = app.backgroundColor & 0x00FFFFFFu;
    settings.groundHighlightEnabled = app.groundHighlightVisible;
    settings.exposure = app.exposure;
    settings.groundShadowEnabled = app.shadowVisible;
    settings.groundShadowOverridden = app.shadowOverridden;
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
    ShowModeHud(app, LocFormat("hud.groundAxis", L"Ground axis {0}", { GroundAxisName(app.groundAxis) }));
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
    ShowModeHud(app, LocFormat("hud.groundDirection", L"Ground direction {0}{1} up",
        { app.groundAxisInverted ? L"-" : L"+", GroundAxisName(axis) }));
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
        ShowModeHud(app, app.showNativeOrientation ? Loc("hud.nativeOrientation", L"Native orientation")
                                                   : Loc("hud.normalizedOrientation", L"Normalized orientation"));
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

void ToggleHideModelUntilComplete(ViewerApp& app)
{
    app.hideModelUntilComplete = !app.hideModelUntilComplete;
    app.renderThread.SetHideModelUntilComplete(app.hideModelUntilComplete);
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
    auto camera = app.renderThread.LockCamera();
    camera->SnapToView(ViewOrientationFrom(camera->TargetOrientation(), view));
    InvalidateRect(app.window, nullptr, FALSE);
}

// Applies one incremental 90-degree turn from the gizmo's outer glyphs. It
// composes on the in-flight target so clicking faster than the snap animation
// still accumulates exact quarter turns rather than drifting.
void TurnViewCommand(ViewerApp& app, ViewTurn turn)
{
    if (!CanNavigate(app)) return;
    auto camera = app.renderThread.LockCamera();
    camera->SnapToView(TurnViewOrientation(camera->TargetOrientation(), turn));
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
    SetControlVisible(app.openAsButton, app.state == ViewerState::Failed);
    SetControlVisible(app.copyButton, app.state == ViewerState::Failed);
    EnableWindow(app.retryButton, app.failedPath.empty() ? FALSE : TRUE);
    EnableWindow(app.openAsButton, app.failedPath.empty() ? FALSE : TRUE);
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
    const int openAsWidth = Scale(app, 96);
    const int copyWidth = Scale(app, 104);
    int actionX = card.left + Scale(app, 28);
    const int actionY = card.bottom - Scale(app, 52);
    MoveWindow(app.retryButton, actionX, actionY, retryWidth, actionHeight, TRUE); actionX += retryWidth + actionGap;
    MoveWindow(app.openAnotherButton, actionX, actionY, anotherWidth, actionHeight, TRUE); actionX += anotherWidth + actionGap;
    MoveWindow(app.openAsButton, actionX, actionY, openAsWidth, actionHeight, TRUE); actionX += openAsWidth + actionGap;
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
void CloseOpenWithPopup(ViewerApp& app);

void ToggleFullscreen(ViewerApp& app)
{
    if (!ConsumeToggleCommand(app, ID_VIEW_FULLSCREEN)) return;
    CloseOpenWithPopup(app);
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
    for (HWND button : { app.retryButton, app.openAnotherButton, app.openAsButton, app.copyButton })
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

void AddTooltip(ViewerApp& app, HWND control, std::wstring text)
{
    // The tooltip control keeps the pointer, so the buffer must outlive this
    // call; app.tooltipTexts owns it.
    app.tooltipTexts.push_back(std::move(text));
    TOOLINFOW info{ sizeof(info) };
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = app.window;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t*>(app.tooltipTexts.back().c_str());
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
    app.retryButton = CreateButton(app, ID_VIEW_RETRY, Loc("action.retry", L"Retry").c_str());
    app.openAnotherButton = CreateButton(app, ID_VIEW_OPEN_ANOTHER, Loc("action.openAnother", L"Open another").c_str());
    app.openAsButton = CreateButton(app, ID_VIEW_OPEN_AS, Loc("action.openAs", L"Open as\u2026").c_str());
    app.copyButton = CreateButton(app, ID_VIEW_COPY_DETAILS, Loc("action.copyDetails", L"Copy details").c_str());
    for (HWND button : { app.retryButton, app.openAnotherButton, app.openAsButton, app.copyButton })
        SetWindowSubclass(button, ErrorButtonAccessibilitySubclass, 1, 0);
    app.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, app.window, nullptr, app.instance, nullptr);
    SetWindowPos(app.tooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(app.tooltip, TTM_SETMAXTIPWIDTH, 0, Scale(app, 360));
    AddTooltip(app, app.retryButton, Loc("action.retryTip", L"Try opening this file again"));
    AddTooltip(app, app.openAnotherButton, Loc("action.openAnotherTip", L"Choose a different supported 3D model"));
    AddTooltip(app, app.openAsButton, Loc("action.openAsTip", L"Open this file with a chosen 3D format"));
    AddTooltip(app, app.copyButton, Loc("action.copyDetailsTip", L"Copy technical error details without the file path"));
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
        if (!app.openFormatNotice.empty()) {
            if (!app.warning.empty()) app.warning += L"\n";
            app.warning += app.openFormatNotice;
        }
        if (displayed->metadata->source.format == model_core::SourceFormatId::Step
            && displayed->metadata->importStatus.optionalFeatureWarnings)
            app.warning = Loc("warning.stepSurfaces", L"Some STEP surfaces or assembly items could not be displayed.");
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
    if (!app.openFormatNotice.empty()) {
        if (!app.errorDetails.empty()) app.errorDetails += L"\r\n\r\n";
        app.errorDetails += app.openFormatNotice;
    }
    app.failedPath = failedPath;
    app.diagnosticPath = failedPath;
    app.errorCode = code;
    app.errorStage = stage;
    app.errorPhase = phase;
    StopNavigation(app);
    SetOrbitEnabled(app, false);
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
    app.renderThread.RevealIncompleteModel();
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

void BeginOpen(ViewerApp& app, std::wstring path, std::vector<std::wstring> assetSearchRoots = {},
    const model_core::FormatDescriptor* forcedFormat = nullptr)
{
    if (path.empty()) return;
    CloseOpenWithPopup(app);
    app.assetSearchRoots = assetSearchRoots;
    app.forcedFormat = forcedFormat;
    app.openFormatNotice.clear();
    ++app.generation;
    if (app.cancellation)
    {
        app.cancellation->store(true, std::memory_order_relaxed);
        app.cancellation.reset();
    }
    // Reject remote/device and relative paths before any import worker is
    // asked to open them. The classifier normalizes separators on a copy, so a
    // forward-slash UNC ("//server/share") and "\\?\UNC\server\..." are caught
    // here rather than only by the broker's post-open canonical check; a
    // relative path is refused because the primary source must be absolute.
    if (platform::ClassifySourcePath(path) != platform::SourcePathKind::LocalAbsolute)
    {
        app.filename = FileNameFromPath(path);
        UpdateTitle(app);
        SetFailure(app, Loc("failure.remotePath.summary", L"Remote model paths are not opened."),
            Loc("failure.remotePath.details", L"Choose a supported model stored on a local drive for this viewer."),
            path, model_core::ImportErrorCode::UnsafeReference);
        return;
    }
    // One bounded probe supplies the extension candidate, the content
    // candidate and diagnostics. --skip-content-check deliberately treats the
    // suffix as the only candidate. A forced format (--format / Open As)
    // replaces candidate selection but is still validated by the worker/host.
    const auto classification = d3d12_import_bridge::ClassifyForOpen(
        path, app.skipContentCheck && !forcedFormat);
    if (classification.reason == model_core::ClassificationReason::Mismatch
        || classification.reason == model_core::ClassificationReason::Content)
    {
        const auto diagnostic = d3d12_import_bridge::ClassificationDiagnostic(classification);
        OutputDebugStringW((L"Preview3D " + diagnostic + L"\n").c_str());
    }

    const model_core::FormatDescriptor* chosen = forcedFormat;
    if (!chosen)
    {
        const auto decision = model_core::DecideOpenFormat(classification);
        if (decision.action == model_core::OpenFormatAction::AskUser)
        {
            // Never silently pick one side of a weak/missing-suffix match. The
            // failure card offers Open As with both candidates listed.
            const auto detected = decision.format && decision.format->displayName
                ? Loc(decision.format->displayNameKey, decision.format->displayName)
                : Loc("format.unknown", L"Unknown");
            app.openFormatNotice = LocFormat("info.detectedAs",
                L"Detected {0}.", { detected });
            app.filename = FileNameFromPath(path);
            UpdateTitle(app);
            if (decision.reason == model_core::ClassificationReason::Mismatch) {
                const auto suffix = decision.extensionCandidate && decision.extensionCandidate->displayName
                    ? Loc(decision.extensionCandidate->displayNameKey, decision.extensionCandidate->displayName)
                    : Loc("format.unknown", L"Unknown");
                SetFailure(app,
                    Loc("failure.openAs.summary", L"The file suffix does not match its content."),
                    LocFormat("failure.openAs.details",
                        L"Detected {0}, but the suffix selects {1}. Choose Open As to pick the importer.",
                        { detected, suffix }),
                    path, model_core::ImportErrorCode::UnsupportedFormat);
            } else {
                SetFailure(app,
                    Loc("failure.openAs.noSuffixSummary", L"This file needs an explicit format."),
                    LocFormat("failure.openAs.noSuffixDetails",
                        L"Detected {0}, but the suffix is missing or unknown. Choose Open As to open it with the {0} importer.",
                        { detected }),
                    path, model_core::ImportErrorCode::UnsupportedFormat);
            }
            return;
        }
        chosen = decision.format;
    }

    const auto d3d12Format = d3d12_import_bridge::SourceFormatForFormatDescriptor(chosen);
    if (!d3d12Format)
    {
        app.filename = FileNameFromPath(path);
        UpdateTitle(app);
        if (chosen) {
            // A forced catalog entry whose importer is not shipped in this
            // release (reserved identity) is a typed failure, not a crash.
            app.openFormatNotice = LocFormat("info.openedAsUnavailable",
                L"The {0} importer is not available in this build.",
                { Loc(chosen->displayNameKey, chosen->displayName) });
            SetFailure(app,
                Loc("failure.formatUnavailable.summary", L"This format is not supported by this build."),
                Loc("failure.formatUnavailable.details",
                    L"Choose a supported format in Open As or update Preview 3D."),
                path, model_core::ImportErrorCode::UnsupportedFormat);
        } else {
            SetFailure(app, Loc("failure.unsupported.summary", L"This model format is not supported."),
                Loc("failure.unsupported.details", L"Open a .glb, .gltf, .stl, .ply, .obj, .fbx, .3mf, .usd, .usda, .usdc, .usdz, .step, or .stp file. Other model formats are deferred."),
                path, model_core::ImportErrorCode::UnsupportedFormat);
        }
        return;
    }

    if (forcedFormat)
    {
        const auto opened = Loc(forcedFormat->displayNameKey, forcedFormat->displayName);
        app.openFormatNotice = LocFormat("info.openedAs", L"Opened as {0}.", { opened });
        if (classification.content && classification.content->family != forcedFormat->family)
        {
            const auto detected = Loc(classification.content->displayNameKey,
                classification.content->displayName);
            app.openFormatNotice += L" " + LocFormat("info.detectedAs",
                L"Detected {0}.", { detected });
        }
        app.forcedFormat = forcedFormat;
    }
    else
    {
        app.forcedFormat = nullptr;
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
    SetOrbitEnabled(app, false);
    EndPointer(app);
    app.failedPath.clear();
    app.errorSummary.clear();
    app.errorDetails.clear();
    app.filename = FileNameFromPath(path);
    app.warning.clear();
    app.missingAssets.clear();
    app.warningButtonHover = false;
    app.warningButtonPressed = false;
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
    const uint64_t hostCommitCap =
        (format == d3d12_import_bridge::SourceFormat::Step
         || format == d3d12_import_bridge::SourceFormat::Iges
         || format == d3d12_import_bridge::SourceFormat::Brep
         || format == d3d12_import_bridge::SourceFormat::Xbf
         || format == d3d12_import_bridge::SourceFormat::Fcstd)
        ? import_broker::DedicatedHostCommitLimitBytes() : 0;
    auto cpuGuard = app.renderThread.CpuBudgetGuard(hostCommitCap);
    auto* stepPhase = &app.stepProgressPhase;
    auto* stepDone = &app.stepProgressDone;
    auto* stepTotal = &app.stepProgressTotal;
    app.importThreads.emplace_back([window, generation, path, format, alive, cancellation, sink, detailSource, cpuGuard, delayBatches, sectionBytes, faultForTesting, stepPhase, stepDone, stepTotal, assetSearchRoots]()
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
            }, sink, sectionBytes, delayBatches, faultForTesting, detailSource, complete, cpuGuard, stepProgress, assetSearchRoots);
        }
        catch (const std::length_error&)
        {
            result.errorCode = model_core::ImportErrorCode::ResourceLimit;
            result.errorStage = import_broker::ImportStage::Upload;
            result.errorSummary = Loc("failure.uploadCapacity.summary", L"This model exceeds the upload capacity.");
            result.errorDetails = Loc("failure.uploadCapacity.details", L"An accepted batch exceeded the bounded upload queue. Export a smaller model and retry.");
        }
        catch (const std::bad_alloc&)
        {
            result.errorCode = model_core::ImportErrorCode::OutOfMemory;
            result.errorStage = import_broker::ImportStage::WorkerReportedError;
            result.errorSummary = Loc("failure.oom.summary", L"There is not enough memory to open this model.");
            result.errorDetails = Loc("failure.oom.details", L"Importing this model exceeded the available memory budget.");
        }
        catch (...)
        {
            result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
            result.errorStage = import_broker::ImportStage::WorkerReportedError;
            result.errorSummary = Loc("failure.generic.summary", L"This model could not be previewed.");
            result.errorDetails = Loc("failure.generic.details", L"The importer stopped unexpectedly while reading the model.");
        }
        if (!alive->load(std::memory_order_relaxed) || (initialComplete && cancellation->load())) return;
        auto* message = new (std::nothrow) D3D12CompleteMessage{ generation, path, std::move(result) };
        if (message && !PostMessageW(window, kD3D12ImportCompleteMessage, 0, reinterpret_cast<LPARAM>(message)))
            delete message;
    });
}

// Failure/open-flow Open As: show the detected/suffix candidates first, then
// every importable catalog format. Stable names are ASCII and kept in the
// menu label so power users can map the choice to --format.
void ShowOpenAsMenu(ViewerApp& app)
{
    if (app.failedPath.empty()) return;
    const auto classification = d3d12_import_bridge::ClassifyForOpen(
        app.failedPath, app.skipContentCheck && !app.forcedFormat);
    std::vector<const model_core::FormatDescriptor*> choices;
    auto addChoice = [&](const model_core::FormatDescriptor* descriptor) {
        if (!model_core::IsImportableFormat(descriptor)) return;
        for (const auto* existing : choices) if (existing == descriptor) return;
        choices.push_back(descriptor);
    };
    // Detected content and the suffix candidate are offered first; a
    // disagreement therefore exposes both sides rather than silently picking.
    if (classification.selected) addChoice(classification.selected);
    if (classification.extension && classification.extension != classification.selected)
        addChoice(classification.extension);
    const std::size_t preferred = choices.size();
    for (const auto* descriptor : open_options::OpenAsFormats()) addChoice(descriptor);
    if (choices.empty()) return;

    HMENU menu = CreatePopupMenu();
    constexpr UINT kOpenAsMenuBase = 60000;
    for (std::size_t index = 0; index < choices.size(); ++index) {
        if (index == preferred && preferred > 0) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const auto* descriptor = choices[index];
        std::wstring label = Loc(descriptor->displayNameKey, descriptor->displayName);
        label += L"  (";
        label += std::wstring(descriptor->stableName.begin(), descriptor->stableName.end());
        label += L")";
        AppendMenuW(menu, MF_STRING, kOpenAsMenuBase + static_cast<UINT>(index), label.c_str());
    }
    RECT button = app.chrome.Button(Chrome::Part::Overflow).rect;
    POINT anchor{ button.right, button.bottom };
    ClientToScreen(app.window, &anchor);
    const int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
        anchor.x, anchor.y, 0, app.window, nullptr);
    DestroyMenu(menu);
    if (selected >= static_cast<int>(kOpenAsMenuBase)
        && selected < static_cast<int>(kOpenAsMenuBase + choices.size())) {
        const auto* forced = choices[static_cast<std::size_t>(selected - kOpenAsMenuBase)];
        BeginOpen(app, app.failedPath, app.assetSearchRoots, forced);
    }
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
        SetFailure(app, Loc("failure.openDialogUnavailable.summary", L"The Open dialog is unavailable."),
        Loc("failure.openDialogUnavailable.details", L"Windows could not create the system file picker."));
        return;
    }
    // Filter labels are localized; the extension patterns are not. Both are
    // generated from the format catalog so a new format cannot be added to the
    // open dialog by hand. The vectors outlive SetFileTypes.
    std::wstring allPattern;
    std::vector<std::wstring> filterNames;
    std::vector<std::wstring> filterPatterns;
    auto appendPattern = [](std::wstring& pattern, const std::wstring& extension) {
        if (!pattern.empty()) pattern += L';';
        pattern += L"*." + extension;
    };
    std::vector<model_core::FormatId> families;
    for (const auto& descriptor : model_core::Formats()) {
        if (!model_core::ProgIdForFormat(descriptor) || descriptor.extensions.empty()) continue;
        for (const auto extension : descriptor.extensions)
            appendPattern(allPattern, std::wstring(extension));
        bool knownFamily = false;
        for (const auto family : families) if (family == descriptor.family) { knownFamily = true; break; }
        if (knownFamily) continue;
        families.push_back(descriptor.family);
        std::wstring pattern;
        for (const auto& candidate : model_core::Formats()) {
            if (candidate.family != descriptor.family) continue;
            for (const auto extension : candidate.extensions)
                appendPattern(pattern, std::wstring(extension));
        }
        const std::wstring display = Loc(descriptor.displayNameKey, descriptor.displayName);
        filterNames.push_back(LocFormat("open.filter.format", L"{0} (*{1})", { display, pattern }));
        filterPatterns.push_back(pattern);
    }
    filterNames.insert(filterNames.begin(), Loc("open.filter.all", L"Supported 3D models"));
    filterPatterns.insert(filterPatterns.begin(), allPattern);
    filterNames.push_back(Loc("open.filter.allFiles", L"All files (*.*)"));
    filterPatterns.push_back(L"*.*");
    std::vector<COMDLG_FILTERSPEC> filters(filterNames.size());
    for (std::size_t index = 0; index < filterNames.size(); ++index)
    {
        filters[index].pszName = filterNames[index].c_str();
        filters[index].pszSpec = filterPatterns[index].c_str();
    }
    dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
    dialog->SetDefaultExtension(L"glb");
    dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const HRESULT shown = dialog->Show(app.window);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    if (FAILED(shown))
    {
        SetFailure(app, Loc("failure.openDialogFailed.summary", L"The Open dialog stopped unexpectedly."),
            Loc("failure.openDialogFailed.details", L"Try dropping a supported local 3D model into the window."));
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
        AppendMenuW(menu, MF_STRING, ID_VIEW_DIAGNOSTICS, Loc("menu.modelWarnings", L"Model warnings…").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING | (app.shadowVisible ? MF_CHECKED : MF_UNCHECKED),
        ID_VIEW_SHADOW, Loc("menu.groundShadow", L"Ground contact shadow\tH").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_VIEW_CONTROLS, Loc("menu.controls", L"Controls\t?").c_str());
    AppendMenuW(menu, MF_STRING, ID_VIEW_SETTINGS, Loc("menu.settings", L"Settings…").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, Loc("menu.about", L"About Preview 3D").c_str());
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
    CloseOpenWithPopup(app);
    app.settingsPanelOpen = !app.settingsPanelOpen;
    InvalidateRect(app.window, nullptr, FALSE);
}

constexpr UINT kLanguageMenuBase = 50000;

// Switches the active language pack without a restart: every string resolved
// at draw time (chrome, tooltips, panels, menus) updates immediately. Strings
// already captured into app state (an error card's summary, for example) keep
// their previous language until that state is replaced, matching how most
// desktop apps treat an in-place language change.
void ApplyLanguage(ViewerApp& app, const std::wstring& code)
{
    app.language = code;
    LocalizationSetLanguage(code);
    SaveViewerPreferences(app);
    UpdateTitle(app);
    InvalidateRect(app.window, nullptr, FALSE);
    viewer_accessibility::Announce(app.window, app.uiaAccessible, AccessibilityStatus(app));
}

void ShowLanguageMenu(ViewerApp& app)
{
    HMENU menu = CreatePopupMenu();
    const std::wstring systemLabel = Loc("settings.language.system", L"System default (Windows language)");
    AppendMenuW(menu, MF_STRING, kLanguageMenuBase, systemLabel.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const std::vector<LanguageInfo>& languages = LocalizationAvailableLanguages();
    const std::wstring current = LocalizationLanguage();
    // Menu items reference the label buffers for the duration of
    // TrackPopupMenu, so they must outlive the AppendMenuW calls.
    std::vector<std::wstring> labels;
    labels.reserve(languages.size());
    for (std::size_t index = 0; index < languages.size(); ++index)
    {
        labels.push_back(languages[index].displayName + L"  (" + languages[index].code + L")");
        const UINT flags = MF_STRING | (languages[index].code == current ? MF_CHECKED : 0u);
        AppendMenuW(menu, flags, kLanguageMenuBase + 1 + static_cast<UINT>(index), labels.back().c_str());
    }

    const RECT row = SettingsLanguageRowRect(app);
    POINT anchor{ row.left, row.bottom };
    ClientToScreen(app.window, &anchor);
    const int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
        anchor.x, anchor.y, 0, app.window, nullptr);
    DestroyMenu(menu);

    if (selected == static_cast<int>(kLanguageMenuBase))
    {
        ApplyLanguage(app, std::wstring());
    }
    else if (selected >= static_cast<int>(kLanguageMenuBase) + 1)
    {
        const std::size_t index = static_cast<std::size_t>(selected - (kLanguageMenuBase + 1));
        if (index < languages.size()) ApplyLanguage(app, languages[index].code);
    }
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
        item.CtlID == ID_VIEW_OPEN_AS || item.CtlID == ID_VIEW_COPY_DETAILS;
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

// Composes the full warning text shown by the badge and the diagnostics menu:
// the existing feature/texture warnings plus the named missing assets, so the
// user can see exactly which files could not be found.
std::wstring BuildModelWarningText(const ViewerApp& app)
{
    std::wstring text = app.warning;
    if (!app.missingAssets.empty())
    {
        if (!text.empty()) text += L"\n";
        text += Loc("warning.missingAssetsHeader", L"Missing assets:");
        for (const auto& asset : app.missingAssets)
        {
            // Worker-supplied references can contain control characters that
            // spoof dialog text; strip them and bound the per-reference length
            // before the text reaches any control.
            text += L"\n  \u2022 ";
            text += preview3d::safeio::SanitizeDisplayText(asset, 256);
        }
    }
    return text;
}

struct AssetWarningDialogContext
{
    const std::wstring* text = nullptr;
    bool hasMissingAssets = false;
};

INT_PTR CALLBACK AssetWarningDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INITDIALOG:
    {
        auto* context = reinterpret_cast<AssetWarningDialogContext*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
        // The dialog template carries English fallbacks; the localized strings
        // are applied here so no per-language resource script is needed.
        SetWindowTextW(dialog, Loc("assetWarning.caption", L"Model assets").c_str());
        SetDlgItemTextW(dialog, IDC_ASSETWARNING_INSTRUCTION,
            Loc("assetWarning.instruction", L"Some assets referenced by this model could not be found. To load them, choose the folder that contains the missing files.").c_str());
        if (context && context->text)
            SetDlgItemTextW(dialog, IDC_ASSETWARNING_TEXT, context->text->c_str());
        if (context && context->hasMissingAssets)
            SetDlgItemTextW(dialog, IDC_ASSETWARNING_LOCATE, Loc("assetWarning.locate", L"Locate folder\u2026").c_str());
        else
            EnableWindow(GetDlgItem(dialog, IDC_ASSETWARNING_LOCATE), FALSE);
        SetDlgItemTextW(dialog, IDCANCEL, Loc("action.close", L"Close").c_str());
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_ASSETWARNING_LOCATE:
            EndDialog(dialog, IDC_ASSETWARNING_LOCATE);
            return TRUE;
        case IDOK:
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default: break;
        }
        break;
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default: break;
    }
    return FALSE;
}

// Opens the folder picker, adds the chosen directory to the active search
// roots, then re-imports the current model so the missing assets can resolve.
// The chosen root is trusted (the user picked it) but still passes the same
// canonical-containment check as every other sidecar root.
void LocateMissingAssets(ViewerApp& app)
{
    if (app.currentPath.empty()) return;
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        MessageBoxW(app.window, Loc("assetWarning.folderPickerUnavailable", L"Windows could not create the folder picker.").c_str(),
            Loc("assetWarning.caption", L"Model assets").c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(Loc("assetWarning.pickFolder", L"Select the folder that contains the missing assets").c_str());
    const HRESULT shown = dialog->Show(app.window);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    if (FAILED(shown))
    {
        MessageBoxW(app.window, Loc("assetWarning.folderPickerFailed", L"The folder picker stopped unexpectedly.").c_str(),
            Loc("assetWarning.caption", L"Model assets").c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    ComPtr<IShellItem> item;
    PWSTR selectedPath = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath)))
        return;
    std::wstring folder(selectedPath);
    CoTaskMemFree(selectedPath);
    if (folder.empty()) return;
    if (std::find(app.assetSearchRoots.begin(), app.assetSearchRoots.end(), folder) == app.assetSearchRoots.end())
        app.assetSearchRoots.push_back(folder);
    BeginOpen(app, app.currentPath, app.assetSearchRoots);
}

// Shows the warning/missing-asset detail dialog. When the warning stands for
// missing assets, its action button opens the folder picker and re-imports.
void ShowModelWarnings(ViewerApp& app)
{
    if (app.warning.empty() && app.missingAssets.empty()) return;
    const std::wstring text = BuildModelWarningText(app);
    AssetWarningDialogContext context{ &text, !app.missingAssets.empty() };
    const INT_PTR result = DialogBoxParamW(app.instance, MAKEINTRESOURCEW(IDD_ASSETWARNING), app.window,
                                            AssetWarningDialogProc, reinterpret_cast<LPARAM>(&context));
    if (result == IDC_ASSETWARNING_LOCATE)
        LocateMissingAssets(app);
}

void HandleCommand(ViewerApp& app, int id)
{
    switch (id)
    {
    case ID_VIEW_OPEN:
    case ID_VIEW_OPEN_ANOTHER: OpenDialog(app); break;
    case ID_VIEW_OPEN_AS: ShowOpenAsMenu(app); break;
    case ID_VIEW_FIT: FrameSelectedOrAll(app); break;
    case ID_VIEW_RESET:
        if (app.renderThread.HasModel())
        {
            app.renderThread.LockCamera()->Reset(ViewportAspect(app));
            // Keep the UI loop polling until the camera actually reaches home,
            // so the floating Reset button can vanish on the frame it does.
            app.resetViewPending = true;
            InvalidateRect(app.window, nullptr, FALSE);
        }
        break;
    case ID_VIEW_GRID: ToggleGrid(app); break;
    case ID_VIEW_SHADOW: ToggleShadow(app); break;
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
    case ID_VIEW_RETRY: if (!app.failedPath.empty()) BeginOpen(app, app.failedPath, app.assetSearchRoots, app.forcedFormat); break;
    case ID_VIEW_COPY_DETAILS: CopyErrorDetails(app); break;
    case ID_VIEW_CANCEL: CancelOpen(app); break;
    case ID_VIEW_CONTROLS: ShowControlsDialog(app.window); break;
    case ID_VIEW_SETTINGS: ToggleSettingsPanel(app); break;
    case ID_VIEW_DIAGNOSTICS:
        ShowModelWarnings(app);
        break;
    case IDM_ABOUT: ShowAboutDialog(app.window); break;
    case IDM_EXIT: DestroyWindow(app.window); break;
    }
}

std::wstring OpenWithExtension(const std::wstring& path)
{
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= path.size()) return {};
    std::wstring extension = path.substr(dot);
    for (auto& character : extension)
        character = static_cast<wchar_t>(towlower(character));
    return extension;
}

wchar_t OpenWithMonogram(const std::wstring& name)
{
    for (wchar_t character : name)
    {
        if (character <= L' ') continue;
        return static_cast<wchar_t>(towupper(character));
    }
    return L'?';
}

std::wstring OpenWithGroupDescription(OpenWithGroup group)
{
    switch (group)
    {
    case OpenWithGroup::Cad: return Loc("openWith.cad", L"CAD");
    case OpenWithGroup::Modeling: return Loc("openWith.modeling", L"Modeling");
    case OpenWithGroup::Printing: return Loc("openWith.printing", L"3D printing");
    case OpenWithGroup::Recommended: return Loc("openWith.recommended", L"Recommended by Windows");
    default: return {};
    }
}

int OpenWithItemCount(const ViewerApp& app)
{
    return static_cast<int>(app.openWith.apps.size()) + 1; // apps plus fallback
}

void UpdateOpenWithLayout(ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    open_with_ui::LayoutInput input;
    input.anchor = app.chrome.Button(Chrome::Part::OpenWith).rect;
    input.client = client;
    input.toolbarHeight = app.toolbarHeight;
    input.scale = app.dpiScale;
    input.appCount = static_cast<int>(app.openWith.apps.size());
    input.loading = app.openWith.loading;
    input.scrollOffset = app.openWith.scrollOffset;
    app.openWith.layout = open_with_ui::ComputeLayout(input);
    app.openWith.scrollOffset = app.openWith.layout.scrollOffset;
}

int OpenWithIndexAtPoint(const ViewerApp& app, POINT point)
{
    if (!app.openWith.open) return -1;
    if (PtInRect(&app.openWith.layout.fallback, point))
        return static_cast<int>(app.openWith.apps.size());
    return open_with_ui::AppTileAtPoint(app.openWith.layout, point);
}

bool OpenWithPointInPanel(const ViewerApp& app, POINT point)
{
    return app.openWith.open && PtInRect(&app.openWith.layout.panel, point);
}

void RequestOpenWithIconsForVisible(ViewerApp& app)
{
    const int pixelSize = std::clamp(Scale(app, 32), 16, 64);
    std::vector<OpenWithIconRequest> requests;
    for (const auto& item : app.openWith.apps)
    {
        if (item.icon || item.iconPath.empty() || item.handlerKey.empty()) continue;
        if (std::find(app.openWith.requestedIcons.begin(), app.openWith.requestedIcons.end(),
            item.handlerKey) != app.openWith.requestedIcons.end()) continue;
        app.openWith.requestedIcons.push_back(item.handlerKey);
        requests.push_back({ item.handlerKey, item.iconPath, item.iconIndex, pixelSize });
    }
    if (!requests.empty()) RequestOpenWithIcons(std::move(requests));
}

void DrainOpenWithIcons(ViewerApp& app)
{
    const auto results = TakeReadyOpenWithIcons();
    if (results.empty()) return;
    bool changed = false;
    for (const auto& result : results)
    {
        if (result.pixels.Empty()) continue;
        for (auto& item : app.openWith.apps)
        {
            if (item.handlerKey != result.handlerKey) continue;
            item.icon = std::make_shared<open_with::IconPixels>(result.pixels);
            changed = true;
            break;
        }
    }
    if (changed && app.openWith.open) InvalidateRect(app.window, nullptr, FALSE);
}

constexpr int kOpenWithTileSlots = 8;

int OpenWithSlotForApp(const ViewerApp& app, int appIndex)
{
    const auto& visible = app.openWith.layout.visibleTiles;
    for (std::size_t index = 0; index < visible.size() && index < kOpenWithTileSlots; ++index)
        if (visible[index] == appIndex) return static_cast<int>(index);
    return -1;
}

int OpenWithAppForSlot(const ViewerApp& app, int slot)
{
    if (slot < 0 || slot >= kOpenWithTileSlots) return -1;
    const auto& visible = app.openWith.layout.visibleTiles;
    if (static_cast<std::size_t>(slot) >= visible.size()) return -1;
    return visible[static_cast<std::size_t>(slot)];
}

void FocusOpenWithItem(ViewerApp& app, int index)
{
    const int count = OpenWithItemCount(app);
    if (count <= 0) return;
    app.openWith.focused = std::clamp(index, 0, count - 1);
    if (app.openWith.focused < static_cast<int>(app.openWith.apps.size()))
    {
        app.openWith.scrollOffset = open_with_ui::ScrollToRevealTile(
            app.openWith.layout, app.openWith.scrollOffset, app.openWith.focused);
        UpdateOpenWithLayout(app);
    }
    app.keyboardControl = viewer_accessibility::Control::OpenWith;
    if (app.openWith.focused == static_cast<int>(app.openWith.apps.size()))
        app.keyboardControl = viewer_accessibility::Control::OpenWithMoreApps;
    else
    {
        const int slot = OpenWithSlotForApp(app, app.openWith.focused);
        if (slot >= 0)
            app.keyboardControl = static_cast<viewer_accessibility::Control>(
                static_cast<int>(viewer_accessibility::Control::OpenWithTile0) + slot);
    }
    InvalidateRect(app.window, nullptr, FALSE);
}

void CloseOpenWithPopup(ViewerApp& app)
{
    if (!app.openWith.open) return;
    app.openWith.open = false;
    app.openWith.hovered = -1;
    app.openWith.focused = -1;
    app.openWith.pressed = -1;
    app.openWith.requestedIcons.clear();
    app.openWith.apps.clear();
    app.openWith.loading = false;
    app.openWith.scrollOffset = 0;
    const int control = static_cast<int>(app.keyboardControl);
    const int tileStart = static_cast<int>(viewer_accessibility::Control::OpenWithTile0);
    const int tileEnd = static_cast<int>(viewer_accessibility::Control::OpenWithTile7);
    if ((control >= tileStart && control <= tileEnd) ||
        app.keyboardControl == viewer_accessibility::Control::OpenWithMoreApps)
        app.keyboardControl = viewer_accessibility::Control::OpenWith;
    InvalidateRect(app.window, nullptr, FALSE);
}

void ShowOpenWithPopup(ViewerApp& app, bool keyboard)
{
    if (app.currentPath.empty() || app.isFullscreen || !HasNavigableModel(app)) return;
    DrainOpenWithIcons(app);
    OpenWithCatalog catalog = EnumerateOpenWithHandlers(app.currentPath);

    std::vector<OpenWithItem> previous = std::move(app.openWith.apps);
    auto previousIcon = [&](const std::wstring& key) -> std::shared_ptr<const open_with::IconPixels> {
        for (const auto& item : previous)
            if (item.handlerKey == key) return item.icon;
        return {};
    };

    app.openWith.apps.clear();
    app.openWith.apps.reserve(catalog.apps.size());
    for (auto& entry : catalog.apps)
    {
        OpenWithItem item;
        item.displayName = std::move(entry.displayName);
        item.handlerKey = std::move(entry.handlerKey);
        item.iconPath = std::move(entry.iconPath);
        item.iconIndex = entry.iconIndex;
        item.group = entry.group;
        item.recommended = entry.recommended;
        item.icon = previousIcon(item.handlerKey);
        item.invoke = std::move(entry.invoke);
        app.openWith.apps.push_back(std::move(item));
    }
    app.openWith.loading = catalog.loading;
    app.openWith.extension = OpenWithExtension(app.currentPath);
    app.speedFlyoutOpen = false;
    app.settingsPanelOpen = false;
    app.openWith.open = true;
    app.openWith.hovered = -1;
    app.openWith.pressed = -1;
    app.openWith.scrollOffset = 0;
    app.openWith.requestedIcons.clear();
    UpdateOpenWithLayout(app);
    RequestOpenWithIconsForVisible(app);
    app.openWith.focused = -1;
    if (keyboard)
        FocusOpenWithItem(app, 0);
    if (app.uiaAccessible)
        viewer_accessibility::Announce(app.window, app.uiaAccessible,
            LocFormat("a11y.openWithList", L"Open with, {0} apps",
                { std::to_wstring(app.openWith.apps.size()) }));
    UpdateTooltipTracking(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleOpenWithPopup(ViewerApp& app, bool keyboard)
{
    if (app.openWith.open) CloseOpenWithPopup(app);
    else ShowOpenWithPopup(app, keyboard);
}

void RefreshOpenWithPopup(ViewerApp& app)
{
    if (!app.openWith.open) return;
    if (app.currentPath.empty() || app.isFullscreen || !HasNavigableModel(app))
    {
        CloseOpenWithPopup(app);
        return;
    }

    std::wstring focusedKey;
    if (app.openWith.focused >= 0 &&
        app.openWith.focused < static_cast<int>(app.openWith.apps.size()))
        focusedKey = app.openWith.apps[app.openWith.focused].handlerKey;
    const bool focusWasFallback = app.openWith.focused == static_cast<int>(app.openWith.apps.size()) &&
        app.keyboardControl == viewer_accessibility::Control::OpenWithMoreApps;

    OpenWithCatalog catalog = EnumerateOpenWithHandlers(app.currentPath);
    std::vector<OpenWithItem> previous = std::move(app.openWith.apps);
    auto previousIcon = [&](const std::wstring& key) -> std::shared_ptr<const open_with::IconPixels> {
        for (const auto& item : previous)
            if (item.handlerKey == key) return item.icon;
        return {};
    };

    app.openWith.apps.clear();
    app.openWith.apps.reserve(catalog.apps.size());
    for (auto& entry : catalog.apps)
    {
        OpenWithItem item;
        item.displayName = std::move(entry.displayName);
        item.handlerKey = std::move(entry.handlerKey);
        item.iconPath = std::move(entry.iconPath);
        item.iconIndex = entry.iconIndex;
        item.group = entry.group;
        item.recommended = entry.recommended;
        item.icon = previousIcon(item.handlerKey);
        item.invoke = std::move(entry.invoke);
        app.openWith.apps.push_back(std::move(item));
    }
    app.openWith.loading = catalog.loading;
    app.openWith.hovered = -1;
    app.openWith.pressed = -1;

    app.openWith.focused = -1;
    if (!focusedKey.empty())
        for (std::size_t index = 0; index < app.openWith.apps.size(); ++index)
            if (app.openWith.apps[index].handlerKey == focusedKey)
            {
                app.openWith.focused = static_cast<int>(index);
                break;
            }

    UpdateOpenWithLayout(app);
    RequestOpenWithIconsForVisible(app);
    if (focusWasFallback && focusedKey.empty() && !app.openWith.apps.empty())
    {
        // The loading state became a real list; move keyboard focus to the
        // first app instead of leaving it on the fallback tile.
        FocusOpenWithItem(app, 0);
    }
    else if (focusWasFallback && app.openWith.apps.empty())
    {
        app.openWith.focused = static_cast<int>(app.openWith.apps.size());
        app.keyboardControl = viewer_accessibility::Control::OpenWithMoreApps;
    }
    else if (app.openWith.focused < 0 && !focusedKey.empty())
    {
        // The focused app disappeared; move to the first surviving item.
        FocusOpenWithItem(app, 0);
    }
    else if (app.openWith.focused >= 0)
    {
        const int slot = OpenWithSlotForApp(app, app.openWith.focused);
        if (slot >= 0)
            app.keyboardControl = static_cast<viewer_accessibility::Control>(
                static_cast<int>(viewer_accessibility::Control::OpenWithTile0) + slot);
        else if (app.openWith.focused == static_cast<int>(app.openWith.apps.size()))
            app.keyboardControl = viewer_accessibility::Control::OpenWithMoreApps;
        else
            app.keyboardControl = viewer_accessibility::Control::OpenWith;
    }
    InvalidateRect(app.window, nullptr, FALSE);
}

void InvokeOpenWithItem(ViewerApp& app, int index)
{
    if (index < 0) return;
    if (index >= static_cast<int>(app.openWith.apps.size()))
    {
        const std::wstring path = app.currentPath;
        CloseOpenWithPopup(app);
        if (!ShowOpenWithDialog(path))
            ShowModeHud(app, Loc("hud.openWithUnavailable", L"Open with isn't available right now"));
        return;
    }

    OpenWithItem item = std::move(app.openWith.apps[static_cast<std::size_t>(index)]);
    const std::wstring path = app.currentPath;
    CloseOpenWithPopup(app);
    if (item.invoke && !item.invoke(path))
        ShowModeHud(app, Loc("hud.appUnavailable", L"That app is no longer available"));
}

void ScrollOpenWith(ViewerApp& app, int direction)
{
    if (!app.openWith.open || app.openWith.layout.maxScroll <= 0) return;
    const int step = Scale(app, 100) * direction;
    app.openWith.scrollOffset = open_with_ui::ClampScrollOffset(
        app.openWith.layout, app.openWith.scrollOffset + step);
    UpdateOpenWithLayout(app);
    if (app.openWith.focused >= 0 &&
        std::find(app.openWith.layout.visibleTiles.begin(), app.openWith.layout.visibleTiles.end(),
            app.openWith.focused) == app.openWith.layout.visibleTiles.end())
    {
        app.openWith.focused = app.openWith.layout.visibleTiles.empty()
            ? -1 : app.openWith.layout.visibleTiles.front();
        if (app.openWith.focused >= 0)
        {
            const int slot = OpenWithSlotForApp(app, app.openWith.focused);
            if (slot >= 0)
                app.keyboardControl = static_cast<viewer_accessibility::Control>(
                    static_cast<int>(viewer_accessibility::Control::OpenWithTile0) + slot);
            else if (app.openWith.focused == static_cast<int>(app.openWith.apps.size()))
                app.keyboardControl = viewer_accessibility::Control::OpenWithMoreApps;
            else
                app.keyboardControl = viewer_accessibility::Control::OpenWith;
        }
    }
    InvalidateRect(app.window, nullptr, FALSE);
}

bool HandleOpenWithKey(ViewerApp& app, WPARAM key)
{
    if (!app.openWith.open) return false;
    const int count = OpenWithItemCount(app);
    switch (key)
    {
    case VK_ESCAPE:
        CloseOpenWithPopup(app);
        app.keyboardControl = viewer_accessibility::Control::OpenWith;
        return true;
    case VK_LEFT:
    case VK_UP:
        if (count > 0)
            FocusOpenWithItem(app, app.openWith.focused <= 0 ? count - 1 : app.openWith.focused - 1);
        return true;
    case VK_RIGHT:
    case VK_DOWN:
        if (count > 0)
            FocusOpenWithItem(app, app.openWith.focused < 0 ? 0 : (app.openWith.focused + 1) % count);
        return true;
    case VK_HOME:
        if (count > 0) FocusOpenWithItem(app, 0);
        return true;
    case VK_END:
        if (count > 0) FocusOpenWithItem(app, count - 1);
        return true;
    case VK_RETURN:
    case VK_SPACE:
        if (app.openWith.focused >= 0) InvokeOpenWithItem(app, app.openWith.focused);
        return true;
    case VK_TAB:
        CloseOpenWithPopup(app);
        app.keyboardControl = viewer_accessibility::Control::OpenWith;
        return false;
    default:
        return false;
    }
}

void DoShare(ViewerApp& app)
{
    if (app.currentPath.empty()) return;
    std::wstring error;
    if (!ShowWindowsShare(app.window, app.currentPath, error))
    {
        ShowModeHud(app, Loc("hud.shareUnavailable", L"Share isn't available right now"));
    }
}

void ToggleSpeedFlyout(ViewerApp& app)
{
    if (!CanNavigate(app)) return;
    CloseOpenWithPopup(app);
    app.speedFlyoutOpen = !app.speedFlyoutOpen;
    InvalidateRect(app.window, nullptr, FALSE);
}

// Clicking the badge used to open the Releases page. For a per-machine
// install it now downloads the published setup, verifies its SHA-256 against
// the adjacent release asset, and hands the restart to a non-elevated helper
// so setup can run with /S and the app can reopen the current file afterwards.
// Portable copies keep the browser fallback because there is no installer
// registration to replace.
void BeginUpdateFromBadge(ViewerApp& app)
{
    if (app.updateVersion.empty()) return;
    if (app.updateDownloading || app.updateRestartPending) return;

    std::wstring installDirectory;
    std::wstring installError;
    if (!preview3d_update::IsInstalledCopy(installDirectory, installError))
    {
        // Always open the fixed project releases URL, never anything supplied
        // by the update response.
        ShellExecuteW(app.window, L"open", preview3d_update::kReleasesPageUrl, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    const std::wstring message = LocFormat("update.confirmRestart",
        L"Preview 3D {0} will close, install the update, and reopen your current file.",
        { L"v" + app.updateVersion });
    if (MessageBoxW(app.window, message.c_str(),
            Loc("update.caption", L"Preview 3D update").c_str(),
            MB_OKCANCEL | MB_ICONINFORMATION | MB_DEFBUTTON1) != IDOK)
    {
        return;
    }

    app.updateDownloading = true;
    ShowModeHud(app, LocFormat("hud.updateDownloading", L"Downloading update {0}...",
        { L"v" + app.updateVersion }));

    const HWND window = app.window;
    const std::wstring version = app.updateVersion;
    const std::shared_ptr<std::atomic_bool> alive = app.alive;
    app.updateDownloadThread = std::jthread([window, version, alive](std::stop_token stop)
    {
        auto result = std::make_unique<UpdateDownloadResult>();
        std::wstring directory;
        if (preview3d::safeio::AppDataDirectory(directory))
        {
            directory += L"\\Updates";
            result->ok = preview3d_update::DownloadInstaller(
                version, directory, result->installerPath,
                result->expectedSha256, result->error, stop);
        }
        else
        {
            result->error = L"the application data directory is unavailable";
        }

        if (stop.stop_requested() || !alive || !alive->load(std::memory_order_relaxed)) return;
        if (!PostMessageW(window, kUpdateDownloadCompleteMessage, 0,
                reinterpret_cast<LPARAM>(result.get())))
        {
            return;
        }
        result.release();
    });
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
    case Chrome::Part::Orbit: ToggleOrbit(app); break;
    case Chrome::Part::Fit: HandleCommand(app, ID_VIEW_FIT); break;
    case Chrome::Part::Share: DoShare(app); break;
    case Chrome::Part::Overflow: HandleCommand(app, ID_VIEW_MENU); break;
    case Chrome::Part::OpenWith: ToggleOpenWithPopup(app, false); break;
    case Chrome::Part::Update: BeginUpdateFromBadge(app); break;
    default: break;
    }
}

viewer_accessibility::ControlInfo AccessibleInfo(ViewerApp& app, viewer_accessibility::Control control)
{
    using viewer_accessibility::Control;
    viewer_accessibility::ControlInfo info;
    info.focused = app.keyboardControl == control && GetFocus() == app.window;
    const bool model = HasNavigableModel(app);
    auto chrome = [&](Chrome::Part part, const std::wstring& name, const std::wstring& description = {}) {
        const auto& button = app.chrome.Button(part);
        info.name = name; info.description = description; info.rect = button.rect;
        info.visible = button.visible; info.enabled = button.enabled;
    };
    switch (control)
    {
    case Control::Grid: chrome(Chrome::Part::Grid, Loc("a11y.grid", L"Ground grid"), Loc("a11y.gridDesc", L"Show or hide the ground grid")); info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.gridVisible; break;
    case Control::GroundAxis:
    {
        chrome(Chrome::Part::GroundAxis, Loc("a11y.groundAxis", L"Model ground axis"), Loc("a11y.groundAxisDesc", L"Cycle the model axis treated as vertical"));
        const GroundAxis effective = app.loadedModel
            ? ResolveGroundAxis(app.groundAxis, app.loadedModel->source.upAxis) : GroundAxis::Z;
        info.value = GroundAxisName(effective);
        break;
    }
    case Control::GroundDirection:
    {
        chrome(Chrome::Part::GroundDirection, Loc("a11y.groundDirection", L"Ground direction"), Loc("a11y.groundDirectionDesc", L"Use the opposite side of the selected axis as up"));
        info.role = ROLE_SYSTEM_CHECKBUTTON;
        info.checked = app.groundAxisInverted;
        const GroundAxis effective = app.loadedModel
            ? ResolveGroundAxis(app.groundAxis, app.loadedModel->source.upAxis) : GroundAxis::Z;
        info.value = LocFormat(app.groundAxisInverted ? "a11y.groundDirectionNegative" : "a11y.groundDirectionPositive",
            app.groundAxisInverted ? L"Negative {0} is up" : L"Positive {0} is up", { GroundAxisName(effective) });
        break;
    }
    case Control::AxisSnap: chrome(Chrome::Part::AxisSnap, Loc("a11y.axisSnap", L"Axis snap"), Loc("a11y.axisSnapDesc", L"Snap truck movement to the nearest world axis")); info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.axisSnapEnabled; break;
    case Control::Speed: chrome(Chrome::Part::Speed, Loc("chrome.travelSpeed", L"Travel speed"), Loc("a11y.speedDesc", L"Open the flight-speed control")); break;
    case Control::Orbit: chrome(Chrome::Part::Orbit, Loc("a11y.orbit", L"Orbit model"), Loc("a11y.orbitDesc", L"Continuously rotate the model; hover for speed")); info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.orbitEnabled; break;
    case Control::Fit: chrome(Chrome::Part::Fit, Loc("a11y.fit", L"Fit selection or model")); break;
    case Control::Reset:
        info.name = Loc("a11y.reset", L"Reset view");
        info.rect = ResetViewButtonRect(app);
        info.visible = model && app.renderThread.LockCamera()->ViewMoved();
        info.enabled = info.visible;
        break;
    case Control::Share: chrome(Chrome::Part::Share, Loc("a11y.share", L"Share")); break;
    case Control::More: chrome(Chrome::Part::Overflow, Loc("tooltip.more", L"More options")); break;
    case Control::OpenWith: chrome(Chrome::Part::OpenWith, Loc("a11y.openWith", L"Open with")); break;
    case Control::UpdateAvailable:
        chrome(Chrome::Part::Update,
            app.updateVersion.empty() ? Loc("a11y.updateAvailable", L"Update available")
                : LocFormat("a11y.updateVersion", L"Update {0} is available", { L"v" + app.updateVersion }),
            Loc("a11y.updateDesc", L"Download and install the newer version"));
        break;
    case Control::Minimize: chrome(Chrome::Part::Minimize, Loc("a11y.minimize", L"Minimize")); break;
    case Control::Maximize: chrome(Chrome::Part::Maximize, IsZoomed(app.window) ? Loc("a11y.restore", L"Restore") : Loc("a11y.maximize", L"Maximize")); break;
    case Control::Close: chrome(Chrome::Part::Close, Loc("a11y.close", L"Close")); break;
    case Control::Info:
        info.name=Loc("a11y.info", L"Model information"); info.description=Loc("a11y.infoDesc", L"Show or hide Stats and Shading"); info.rect=InfoButtonRect(app);
        info.visible=model; info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.infoPanelVisible; break;
    case Control::InfoPanelClose:
        info.name=Loc("a11y.infoClose", L"Close model information"); info.description=Loc("a11y.infoCloseDesc", L"Close Stats and Shading");
        info.rect=InfoPanelCloseButtonRect(app); info.visible=model && app.infoPanelVisible; break;
    case Control::Zoom:
    {
        info.name=Loc("a11y.zoom", L"Zoom"); info.description=Loc("a11y.zoomDesc", L"Adjust camera zoom"); info.rect=ZoomTrackRect(app); info.visible=model; info.role=ROLE_SYSTEM_SLIDER;
        auto camera=app.renderThread.LockCamera(); info.value=LocFormat("a11y.percent", L"{0} percent", { std::to_wstring(static_cast<int>(std::lround(ZoomPercentFor(*camera)))) }); break;
    }
    case Control::Fullscreen:
        info.name=Loc("tooltip.fullscreen", L"Fullscreen"); info.description=Loc("a11y.fullscreenDesc", L"Enter or leave fullscreen"); info.rect=FullscreenButtonRect(app);
        info.visible=model; info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.isFullscreen; break;
    case Control::LightingStudio:
        info.name=Loc("a11y.studio", L"Studio lighting"); info.description=Loc("a11y.studioDesc", L"Neutral colorless lighting for evaluating PBR materials");
        info.rect=ComputeLightingToolbarLayout(app).studio;info.visible=model;info.role=ROLE_SYSTEM_RADIOBUTTON;
        info.checked=app.lightingMode==LightingMode::Studio;break;
    case Control::LightingClay:
        info.name=Loc("a11y.clay", L"Clay or solid shading");info.description=Loc("a11y.clayDesc", L"Matte gray material for inspecting geometry");
        info.rect=ComputeLightingToolbarLayout(app).clay;info.visible=model;info.role=ROLE_SYSTEM_RADIOBUTTON;
        info.checked=app.lightingMode==LightingMode::Clay;break;
    case Control::LightingDirectional:
        info.name=Loc("a11y.directional", L"Directional lighting");info.description=Loc("a11y.directionalDesc", L"Sharp rotatable light for inspecting surface detail");
        info.rect=ComputeLightingToolbarLayout(app).directional;info.visible=model;info.role=ROLE_SYSTEM_RADIOBUTTON;
        info.checked=app.lightingMode==LightingMode::Directional;break;
    case Control::DirectionalLightAngle:
    {
        // The visual slider is gone; the fragment anchors to the navigation
        // gizmo's outer ring, where the light is now rotated by dragging the
        // sun. Arrow keys still nudge it for keyboard users.
        float centerX=0.0f,centerY=0.0f,radius=0.0f;
        app.gizmo.RingBounds(centerX,centerY,radius);
        info.name=Loc("a11y.lightAngle", L"Directional light angle");
        info.description=Loc("a11y.lightAngleDesc", L"Rotate the inspection light by dragging its sun around the navigation gizmo");
        info.rect=RECT{static_cast<LONG>(centerX-radius),static_cast<LONG>(centerY-radius),
            static_cast<LONG>(centerX+radius),static_cast<LONG>(centerY+radius)};
        info.visible=model&&app.lightingMode==LightingMode::Directional;info.role=ROLE_SYSTEM_SLIDER;
        info.value=LocFormat("a11y.degrees", L"{0} degrees", { std::to_wstring(static_cast<int>(std::lround(app.directionalLightAngle*360.0f))) });break;
    }
    case Control::DirectionalLightElevation:
    {
        // Same gizmo anchor as the angle; drag the sun toward or away from the
        // center to raise/lower it, or nudge with the arrow keys.
        float centerX=0.0f,centerY=0.0f,radius=0.0f;
        app.gizmo.RingBounds(centerX,centerY,radius);
        info.name=Loc("a11y.lightElevation", L"Directional light elevation");
        info.description=Loc("a11y.lightElevationDesc", L"Raise or lower the inspection light by dragging its sun toward or away from the gizmo center");
        info.rect=RECT{static_cast<LONG>(centerX-radius),static_cast<LONG>(centerY-radius),
            static_cast<LONG>(centerX+radius),static_cast<LONG>(centerY+radius)};
        info.visible=model&&app.lightingMode==LightingMode::Directional;info.role=ROLE_SYSTEM_SLIDER;
        info.value=LocFormat("a11y.degrees", L"{0} degrees", { std::to_wstring(static_cast<int>(std::lround(app.directionalLightElevation*57.29577951308232f))) });break;
    }
    case Control::Wireframe:
        info.name=Loc("a11y.wireframe", L"Wireframe");info.description=Loc("a11y.wireframeDesc", L"Show only mesh edges with all triangle surfaces hidden");
        info.rect=ComputeLightingToolbarLayout(app).wireframe;info.visible=model;info.role=ROLE_SYSTEM_RADIOBUTTON;
        info.checked=app.lightingMode==LightingMode::Wireframe;break;
    case Control::SpeedSlider:
    {
        info.name=Loc("chrome.travelSpeed", L"Travel speed"); info.description=Loc("a11y.speedSliderDesc", L"Adjust flight speed"); info.rect=SpeedFlyoutTrackRect(app);
        info.visible=app.speedFlyoutOpen; info.role=ROLE_SYSTEM_SLIDER;
        auto camera=app.renderThread.LockCamera(); info.value=LocFormat("a11y.times", L"times {0}", { FormatMultiplier(camera->FlySpeedScale()) }); break;
    }
    case Control::OrbitSlider:
    {
        info.name=Loc("chrome.orbitSpeed", L"Orbit speed"); info.description=Loc("a11y.orbitSliderDesc", L"Adjust the auto-orbit speed");
        info.rect=OrbitFlyoutTrackRect(app); info.visible=OrbitSpeedFlyoutVisible(app); info.role=ROLE_SYSTEM_SLIDER;
        info.value=std::to_wstring(static_cast<int>(std::lround(app.orbitSpeedDegreesPerSecond)))+L"\u00B0/s"; break;
    }
    case Control::NativeOrientation:
        info.name=Loc("settings.nativeOrientation", L"Show model in its original orientation"); info.rect=SettingsToggleRowRect(app);
        info.visible=app.settingsPanelOpen; info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.showNativeOrientation; break;
    case Control::HideCursorWhileDragging:
        info.name=Loc("settings.hideCursor", L"Hide cursor while dragging"); info.description=Loc("a11y.hideCursorDesc", L"Hide the mouse pointer during viewport camera drags");
        info.rect=SettingsCursorToggleRowRect(app); info.visible=app.settingsPanelOpen;
        info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.hideCursorWhileDragging; break;
    case Control::HideModelUntilComplete:
        info.name=Loc("settings.hideUntilComplete", L"Show model only when fully loaded");
        info.description=Loc("a11y.hideUntilCompleteDesc", L"Keep the model hidden until loading and refinement complete");
        info.rect=SettingsHideUntilCompleteRowRect(app); info.visible=app.settingsPanelOpen;
        info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.hideModelUntilComplete; break;
    case Control::BackgroundMode:
        info.name=Loc("settings.background", L"Background");
        info.description=Loc("a11y.backgroundDesc", L"Choose the viewport backdrop; it does not change model lighting");
        info.value=BackgroundDisplayLabel(app);
        info.rect=SettingsBackgroundRowRect(app); info.visible=app.settingsPanelOpen;
        info.role=ROLE_SYSTEM_PUSHBUTTON; break;
    case Control::GroundHighlight:
        info.name=Loc("settings.groundHighlight", L"Ground highlight");
        info.description=Loc("a11y.groundHighlightDesc", L"Show a subtle illuminated area at the model base; contact shadows stay independent");
        info.rect=SettingsGroundHighlightRowRect(app); info.visible=app.settingsPanelOpen;
        info.role=ROLE_SYSTEM_CHECKBUTTON; info.checked=app.groundHighlightVisible; break;
    case Control::GizmoPositiveX: case Control::GizmoNegativeX: case Control::GizmoPositiveY:
    case Control::GizmoNegativeY: case Control::GizmoPositiveZ: case Control::GizmoNegativeZ:
    {
        struct AxisViewName { const char* key; const wchar_t* text; };
        static constexpr AxisViewName names[] = {
            { "a11y.viewPosX", L"View from positive X" }, { "a11y.viewNegX", L"View from negative X" },
            { "a11y.viewPosY", L"View from positive Y" }, { "a11y.viewNegY", L"View from negative Y" },
            { "a11y.viewPosZ", L"View from positive Z" }, { "a11y.viewNegZ", L"View from negative Z" } };
        const int index=static_cast<int>(control)-static_cast<int>(Control::GizmoPositiveX);
        info.name=Loc(names[index].key, names[index].text); info.description=Loc("a11y.snapAxisDesc", L"Snap the camera to this axis"); info.visible=model;
        DirectX::XMFLOAT4 orientation{}; { auto camera=app.renderThread.LockCamera(); orientation=camera->orientation; }
        const auto geometry=app.gizmo.ComputeDraw(DirectX::XMLoadFloat4(&orientation));
        const int axis=index/2; const bool positive=index%2==0;
        const auto node=positive?geometry.positive[axis]:geometry.negative[axis];
        const float radius=positive?geometry.nodeRadius:geometry.dotRadius;
        info.rect={LONG(geometry.centerX+node.x-radius),LONG(geometry.centerY+node.y-radius),
            LONG(geometry.centerX+node.x+radius),LONG(geometry.centerY+node.y+radius)};
        break;
    }
    case Control::GizmoRollCounterClockwise: case Control::GizmoRollClockwise:
    {
        info.name = control == Control::GizmoRollClockwise
            ? Loc("a11y.rollClockwise", L"Roll view clockwise 90 degrees")
            : Loc("a11y.rollCounterClockwise", L"Roll view counter-clockwise 90 degrees");
        info.description = Loc("a11y.rollDesc", L"Rotate the view 90 degrees in its own plane");

        DirectX::XMFLOAT4 orientation{}; { auto camera=app.renderThread.LockCamera(); orientation=camera->orientation; }
        const auto geometry=app.gizmo.ComputeDraw(DirectX::XMLoadFloat4(&orientation));
        info.visible = model && geometry.showRotationControls;
        const auto& glyph = control == Control::GizmoRollClockwise
            ? geometry.rollClockwise : geometry.rollCounterClockwise;
        const float pad = geometry.glyphStrokeWidth * 0.5f + 2.0f;
        float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
        if (glyph.pointCount > 0)
        {
            minX = maxX = glyph.points[0].x;
            minY = maxY = glyph.points[0].y;
            for (int point = 1; point < glyph.pointCount; ++point)
            {
                minX = std::min(minX, glyph.points[point].x);
                maxX = std::max(maxX, glyph.points[point].x);
                minY = std::min(minY, glyph.points[point].y);
                maxY = std::max(maxY, glyph.points[point].y);
            }
        }
        info.rect = { LONG(geometry.centerX + minX - pad), LONG(geometry.centerY + minY - pad),
            LONG(geometry.centerX + maxX + pad), LONG(geometry.centerY + maxY + pad) };
        break;
    }
    case Control::OpenWithTile0: case Control::OpenWithTile1: case Control::OpenWithTile2:
    case Control::OpenWithTile3: case Control::OpenWithTile4: case Control::OpenWithTile5:
    case Control::OpenWithTile6: case Control::OpenWithTile7:
    {
        const int slot = static_cast<int>(control) - static_cast<int>(Control::OpenWithTile0);
        const int appIndex = OpenWithAppForSlot(app, slot);
        info.visible = app.openWith.open && appIndex >= 0;
        if (info.visible)
        {
            const auto& item = app.openWith.apps[static_cast<std::size_t>(appIndex)];
            info.name = LocFormat("a11y.openWithApp", L"Open with {0}", { item.displayName });
            info.description = OpenWithGroupDescription(item.group);
            info.rect = app.openWith.layout.tiles[static_cast<std::size_t>(appIndex)];
            info.role = ROLE_SYSTEM_PUSHBUTTON;
        }
        break;
    }
    case Control::OpenWithMoreApps:
        info.name = Loc("a11y.openWithChooseAnother", L"Choose another app");
        info.rect = app.openWith.layout.fallback;
        info.visible = app.openWith.open;
        info.enabled = app.openWith.open;
        info.role = ROLE_SYSTEM_PUSHBUTTON;
        break;
    case Control::ErrorRetry: case Control::ErrorOpenAnother: case Control::ErrorCopyDetails:
    case Control::ErrorOpenAs:
    {
        HWND button=control==Control::ErrorRetry?app.retryButton:control==Control::ErrorOpenAnother?app.openAnotherButton
            :control==Control::ErrorOpenAs?app.openAsButton:app.copyButton;
        info.name=control==Control::ErrorRetry?Loc("action.retry", L"Retry")
            :control==Control::ErrorOpenAnother?Loc("action.openAnother", L"Open another")
            :control==Control::ErrorOpenAs?Loc("action.openAs", L"Open as\u2026")
            :Loc("action.copyDetails", L"Copy details");
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
    const int tileStart = static_cast<int>(viewer_accessibility::Control::OpenWithTile0);
    const int tileEnd = static_cast<int>(viewer_accessibility::Control::OpenWithTile7);
    const int controlValue = static_cast<int>(control);
    if (controlValue >= tileStart && controlValue <= tileEnd)
    {
        const int appIndex = OpenWithAppForSlot(app, controlValue - tileStart);
        if (appIndex >= 0)
        {
            app.openWith.focused = appIndex;
            app.openWith.scrollOffset = open_with_ui::ScrollToRevealTile(
                app.openWith.layout, app.openWith.scrollOffset, appIndex);
            UpdateOpenWithLayout(app);
        }
    }
    else if (control == viewer_accessibility::Control::OpenWithMoreApps)
    {
        app.openWith.focused = static_cast<int>(app.openWith.apps.size());
    }
    if (control==viewer_accessibility::Control::ErrorRetry) SetFocus(app.retryButton);
    else if (control==viewer_accessibility::Control::ErrorOpenAnother) SetFocus(app.openAnotherButton);
    else if (control==viewer_accessibility::Control::ErrorOpenAs) SetFocus(app.openAsButton);
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
    case Control::Orbit: ToggleOrbit(app); break;
    case Control::Fit: HandleCommand(app,ID_VIEW_FIT); break;
    case Control::Reset: HandleCommand(app,ID_VIEW_RESET); break;
    case Control::Share: DoShare(app); break;
    case Control::More: ShowMoreMenu(app); break;
    case Control::OpenWith: ToggleOpenWithPopup(app, true); break;
    case Control::OpenWithTile0: case Control::OpenWithTile1: case Control::OpenWithTile2:
    case Control::OpenWithTile3: case Control::OpenWithTile4: case Control::OpenWithTile5:
    case Control::OpenWithTile6: case Control::OpenWithTile7:
    {
        const int slot = static_cast<int>(control) - static_cast<int>(Control::OpenWithTile0);
        const int appIndex = OpenWithAppForSlot(app, slot);
        if (appIndex >= 0) InvokeOpenWithItem(app, appIndex);
        break;
    }
    case Control::OpenWithMoreApps:
        InvokeOpenWithItem(app, static_cast<int>(app.openWith.apps.size()));
        break;
    case Control::UpdateAvailable: BeginUpdateFromBadge(app); break;
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
    case Control::HideModelUntilComplete: ToggleHideModelUntilComplete(app); break;
    case Control::BackgroundMode: ShowBackgroundMenu(app); break;
    case Control::GroundHighlight: ToggleGroundHighlight(app); break;
    case Control::GizmoPositiveX: SnapViewCommand(app,ViewDir::Right); break;
    case Control::GizmoNegativeX: SnapViewCommand(app,ViewDir::Left); break;
    case Control::GizmoPositiveY: SnapViewCommand(app,ViewDir::Back); break;
    case Control::GizmoNegativeY: SnapViewCommand(app,ViewDir::Front); break;
    case Control::GizmoPositiveZ: SnapViewCommand(app,ViewDir::Top); break;
    case Control::GizmoNegativeZ: SnapViewCommand(app,ViewDir::Bottom); break;
    case Control::GizmoRollCounterClockwise: TurnViewCommand(app,ViewTurn::RollCounterClockwise); break;
    case Control::GizmoRollClockwise: TurnViewCommand(app,ViewTurn::RollClockwise); break;
    case Control::ErrorRetry: HandleCommand(app,ID_VIEW_RETRY); break;
    case Control::ErrorOpenAnother: HandleCommand(app,ID_VIEW_OPEN_ANOTHER); break;
    case Control::ErrorOpenAs: HandleCommand(app,ID_VIEW_OPEN_AS); break;
    case Control::ErrorCopyDetails: HandleCommand(app,ID_VIEW_COPY_DETAILS); break;
    default: break;
    }
}

std::wstring AccessibilityStatus(const ViewerApp& app)
{
    if (app.state==ViewerState::Loading) return LocFormat("status.loading", L"Loading {0}", { d3d12_import_bridge::SourceFormatLabel(app.diagnosticPath) });
    if (app.state==ViewerState::Failed) return app.errorSummary+L" "+app.errorDetails;
    if (!app.warning.empty()) return LocFormat("status.loadedWithWarnings", L"Model loaded with warnings. {0}", { app.warning });
    if (app.state==ViewerState::Ready) return app.filename.empty()?Loc("status.modelReady", L"Model ready"):LocFormat("status.fileReady", L"{0} ready", { app.filename });
    if (app.state==ViewerState::Partial) return Loc("status.previewCancelled", L"Preview cancelled; incomplete geometry remains visible");
    return Loc("status.noModel", L"No model open");
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
    if (app.keyboardControl==Control::OrbitSlider)
    {
        AdjustOrbitSpeed(app,direction*5.0); return true;
    }
    if (app.keyboardControl==Control::Orbit && app.orbitEnabled)
    {
        // The slider is hover-only; while the Orbit button itself has keyboard
        // focus, arrows still let a keyboard user tune the speed.
        AdjustOrbitSpeed(app,direction*5.0); return true;
    }
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

// Advances one UI fade toward its current target. Capped dt so returning from
// a modal or a suspended frame doesn't pop the alpha.
void UpdateFade(float& alpha, bool& fading, bool& initialized, float& target,
    double& lastSeconds, float newTarget, bool reduceMotion)
{
    constexpr double kFadeSeconds = 0.18;
    const double now = NowSeconds();

    // Start the fade at the frame the target flips, rather than measuring
    // from the last idle wake-up (which could be minutes ago and make the
    // first step jump most of the way).
    if (!initialized || newTarget != target)
    {
        initialized = true;
        target = newTarget;
        lastSeconds = now;
        if (reduceMotion) alpha = target;
        fading = std::abs(alpha - target) > 0.001f;
        return;
    }

    const double elapsed = std::clamp(now - lastSeconds, 0.0, 0.1);
    lastSeconds = now;
    if (reduceMotion)
    {
        alpha = target;
    }
    else
    {
        const float step = static_cast<float>(elapsed / kFadeSeconds);
        alpha = target > alpha
            ? std::min(target, alpha + step)
            : std::max(target, alpha - step);
    }
    fading = std::abs(alpha - target) > 0.001f;
}

// Fades the floating Reset button (visible while the camera is away from home,
// hidden at home).
void UpdateResetViewButtonFade(ViewerApp& app)
{
    const bool targetVisible = HasNavigableModel(app) && app.renderThread.LockCamera()->ViewMoved();
    UpdateFade(app.resetViewButtonAlpha, app.resetViewButtonFading,
        app.resetViewButtonFadeInitialized, app.resetViewButtonFadeTarget,
        app.lastResetViewFadeSeconds, targetVisible ? 1.0f : 0.0f, app.reduceMotion);
}

// Fades the gizmo's outer roll arrows with the face-view state, matching the
// Reset button's timing.
void UpdateRollGlyphFade(ViewerApp& app)
{
    auto camera = app.renderThread.LockCamera();
    const bool hasModel = HasNavigableModel(app);
    const bool currentFace = hasModel && IsFaceView(camera->Orientation());
    const bool targetFace = hasModel && IsFaceView(camera->TargetOrientation());
    // A gizmo-driven turn from one face view to another passes through
    // non-cardinal orientations on the way. If the arrows were already up
    // (previous target 1) and the glide is heading to another face, keep them
    // up for the whole transition instead of fading out and back in. A glide
    // from an oblique view only shows them once the face is actually reached.
    const bool targetVisible = currentFace
        || (targetFace && app.rollGlyphFadeTarget > 0.5f);
    UpdateFade(app.rollGlyphAlpha, app.rollGlyphFading, app.rollGlyphFadeInitialized,
        app.rollGlyphFadeTarget, app.lastRollGlyphFadeSeconds,
        targetVisible ? 1.0f : 0.0f, app.reduceMotion);

    // Drop a hover left over from the frame the arrows started fading out.
    if (app.rollGlyphFadeTarget == 0.0f
        && (app.gizmo.hover == NavGizmo::Part::RollCounterClockwise
            || app.gizmo.hover == NavGizmo::Part::RollClockwise))
    {
        app.gizmo.hover = NavGizmo::Part::None;
    }
}

// Everything in IsAnimating that is UI-owned state. The render thread ORs
// this with the camera's own motion, which it can see directly under the
// camera lock -- so the UI thread does not have to take that lock just to
// answer "should a frame happen".
bool IsAnimatingWithoutCamera(const ViewerApp& app)
{
    const double now = NowSeconds();
    return (app.state == ViewerState::Loading && !app.reduceMotion) || app.renderPresentationPending
        || HasNavigationInput(app) || app.resetViewPending || app.resetViewButtonFading
        || app.rollGlyphFading
        || now < app.speedHudUntil || now < app.modeHudUntil;
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
    case model_core::kStepPhasePreflight: return Loc("status.stepPreflight", L"• checking STEP text");
    case model_core::kStepPhaseRead: return Loc("status.stepRead", L"• reading STEP entities");
    case model_core::kStepPhaseTransfer: return Loc("status.stepTransfer", L"• building CAD model");
    case model_core::kStepPhasePlan: return Loc("status.stepPlan", L"• planning definitions");
    case model_core::kStepPhaseMesh:
        return total ? LocFormat("status.stepTessellatingOf", L"• tessellating {0} of {1}", { std::to_wstring(done), std::to_wstring(total) })
                     : Loc("status.stepTessellating", L"• tessellating shapes");
    case model_core::kStepPhaseEmit:
        return total ? LocFormat("status.stepPreparingOf", L"• preparing geometry {0} of {1}", { std::to_wstring(done), std::to_wstring(total) })
                     : Loc("status.stepPreparing", L"• preparing geometry");
    default: return {};
    }
}

// Builds the UI snapshot for the render thread's Direct2D chrome pass.
OverlayColor OverlayColorFromRgb(unsigned rgb, float alpha = 1.0f)
{
    return OverlayColor{
        static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
        static_cast<float>(rgb & 0xff) / 255.0f,
        alpha
    };
}

OverlayColor OverlayColorFromSystem(int index, float alpha = 1.0f)
{
    const COLORREF color = GetSysColor(index);
    return OverlayColor{ GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
        GetBValue(color) / 255.0f, alpha };
}

OpenWithPopupPalette BuildOpenWithPalette(const ViewerApp& app)
{
    OpenWithPopupPalette palette;
    if (app.highContrast)
    {
        palette.surface = OverlayColorFromSystem(COLOR_WINDOW, 0.98f);
        palette.border = OverlayColorFromSystem(COLOR_WINDOWTEXT);
        palette.text = OverlayColorFromSystem(COLOR_WINDOWTEXT);
        palette.secondary = OverlayColorFromSystem(COLOR_WINDOWTEXT);
        palette.hover = OverlayColorFromSystem(COLOR_HIGHLIGHT);
        palette.pressed = OverlayColorFromSystem(COLOR_HIGHLIGHT);
        palette.divider = OverlayColorFromSystem(COLOR_WINDOWTEXT);
        palette.focus = OverlayColorFromSystem(COLOR_HIGHLIGHT);
        palette.monogram = OverlayColorFromSystem(COLOR_WINDOW);
        palette.shadow = OverlayColor{ 0.0f, 0.0f, 0.0f, 0.0f };
        return palette;
    }

    // The window opts into immersive dark mode today. The light tokens are
    // kept here so the popup follows the window automatically when a light
    // chrome theme is added.
    constexpr bool windowDarkTheme = true;
    if (windowDarkTheme)
    {
        palette.surface = OverlayColorFromRgb(0x242426, 0.98f);
        palette.border = OverlayColorFromRgb(0x3A3A3C);
        palette.text = OverlayColorFromRgb(0xF5F5F7);
        palette.secondary = OverlayColorFromRgb(0xA1A1A6);
        palette.hover = OverlayColorFromRgb(0x3A3A3C);
        palette.pressed = OverlayColorFromRgb(0x3F3F42);
        palette.divider = OverlayColorFromRgb(0x3A3A3C);
        palette.focus = OverlayColorFromRgb(0x0A84FF);
        palette.monogram = OverlayColorFromRgb(0x3A3A3C);
        palette.shadow = OverlayColor{ 0.0f, 0.0f, 0.0f, 0.30f };
    }
    else
    {
        palette.surface = OverlayColorFromRgb(0xF9F9F9, 0.98f);
        palette.border = OverlayColorFromRgb(0xE2E2E2);
        palette.text = OverlayColorFromRgb(0x1A1A1A);
        palette.secondary = OverlayColorFromRgb(0x5F5F5F);
        palette.hover = OverlayColorFromRgb(0xEEEEEE);
        palette.pressed = OverlayColorFromRgb(0xE2E2E2);
        palette.divider = OverlayColorFromRgb(0xE2E2E2);
        palette.focus = OverlayColorFromRgb(0x0067C0);
        palette.monogram = OverlayColorFromRgb(0xE8E8E8);
        palette.shadow = OverlayColor{ 0.0f, 0.0f, 0.0f, 0.16f };
    }
    return palette;
}

OverlayInfo BuildOverlayInfo(ViewerApp& app)
{
    OverlayInfo overlay;
    overlay.state = app.state;
    overlay.filename = app.filename;
    d3d12_import_bridge::ImportResult failure; failure.errorStage = app.errorStage; failure.errorPhase = app.errorPhase;
    overlay.failureContext = d3d12_import_bridge::SourceFormatLabel(app.diagnosticPath) + L"  •  " + d3d12_import_bridge::FailurePhaseLabel(failure);
    overlay.loadingStatus = LocFormat("status.loading", L"Loading {0}", { d3d12_import_bridge::SourceFormatLabel(app.diagnosticPath) });
    if (app.state == ViewerState::Loading) {
        const auto stepPhase = app.stepProgressPhase.load(std::memory_order_relaxed);
        if (stepPhase != 0)
            overlay.loadingStatus = LocalizedJoin(overlay.loadingStatus, StepProgressText(stepPhase,
                app.stepProgressDone.load(std::memory_order_relaxed),
                app.stepProgressTotal.load(std::memory_order_relaxed)));
    }
    if (app.state == ViewerState::Partial) overlay.loadingStatus = Loc("status.previewCancelledShort", L"Preview cancelled • incomplete geometry");
    if (app.loadedModel && app.loadedModel->source.generationId == app.generation) {
        const auto flags = app.loadedModel->importStatus.flags;
        overlay.loadingStatus = LocalizedJoin(overlay.loadingStatus,
            (flags & model_core::kStatusPressure) ? Loc("status.waitingUploadCapacity", L"• waiting for upload capacity")
            : (flags & model_core::kStatusRefining) ? Loc("status.refiningTextures", L"• refining textures") : Loc("status.verifyingGeometry", L"• verifying complete geometry"));
    }
    overlay.errorSummary = app.errorSummary;
    overlay.errorDetails = app.errorDetails;
    overlay.warning = app.warning;
    overlay.warningButtonRect = WarningButtonRect(app);
    overlay.warningButtonHover = app.warningButtonHover;
    overlay.warningButtonPressed = app.warningButtonPressed;
    overlay.warningHasMissingAssets = !app.missingAssets.empty();
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
            *app.loadedModel, app.showNativeOrientation, app.groundAxis, app.activePresentation);
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
    overlay.presentationProfile=app.activePresentation;
    overlay.presentationPreset=app.activePreset;
    overlay.toneMap=app.toneMapMode;
    overlay.background=app.backgroundMode;
    overlay.backgroundColor=app.backgroundColor;
    overlay.groundHighlightVisible=app.groundHighlightVisible;
    overlay.exposure=app.exposure;
    overlay.studioIntensity=app.studioIntensity;
    overlay.keyLightScale=app.keyLightScale;
    overlay.fillLightScale=app.fillLightScale;
    overlay.rimLightScale=app.rimLightScale;
    overlay.directionalLightAngle=app.directionalLightAngle;
    overlay.directionalLightElevation=app.directionalLightElevation;
    overlay.lightDragging=app.pointerMode==PointerMode::LightDrag;
    overlay.hasModel = app.renderThread.HasModel();
    overlay.captureClean = app.captureClean;
    overlay.resetViewButtonAlpha = app.resetViewButtonAlpha;
    if (overlay.resetViewButtonAlpha > 0.001f)
    {
        overlay.resetViewButtonRect = ResetViewButtonRect(app);
        overlay.resetViewButtonHover = app.resetViewButtonHover;
        overlay.resetViewButtonPressed = app.resetViewButtonPressed;
    }
    overlay.rollGlyphAlpha = app.rollGlyphAlpha;
    overlay.gridVisible = app.gridVisible;
    overlay.shadowVisible = app.shadowVisible;
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
    overlay.orbitEnabled = app.orbitEnabled;
    overlay.orbitFlyoutOpen = OrbitSpeedFlyoutVisible(app);
    if (overlay.orbitFlyoutOpen)
    {
        overlay.orbitFlyoutRect = OrbitFlyoutRect(app);
        overlay.orbitFlyoutTrackRect = OrbitFlyoutTrackRect(app);
        overlay.orbitSliderT = static_cast<float>(OrbitSliderPositionFor(app.orbitSpeedDegreesPerSecond)) / static_cast<float>(kOrbitSliderMax);
        overlay.orbitValueText = std::to_wstring(static_cast<int>(std::lround(app.orbitSpeedDegreesPerSecond))) + L"\u00B0/s";
    }
    overlay.settingsPanelOpen = app.settingsPanelOpen;
    overlay.showNativeOrientation = app.showNativeOrientation;
    overlay.hideCursorWhileDragging = app.hideCursorWhileDragging;
    overlay.hideModelUntilComplete = app.hideModelUntilComplete;
    overlay.groundHighlightVisible = app.groundHighlightVisible;
    overlay.statsPanelTitle = Loc("info.statsShading", L"Stats & Shading");
    overlay.speedFlyoutTitle = Loc("chrome.travelSpeed", L"Travel speed");
    overlay.orbitFlyoutTitle = Loc("chrome.orbitSpeed", L"Orbit speed");
    overlay.nativeOrientationLabel = Loc("settings.nativeOrientation", L"Show model in its original orientation");
    overlay.hideCursorLabel = Loc("settings.hideCursor", L"Hide cursor while dragging");
    overlay.hideUntilCompleteLabel = Loc("settings.hideUntilComplete", L"Show model only when fully loaded");
    overlay.backgroundLabel = Loc("settings.background", L"Background");
    overlay.backgroundValue = BackgroundDisplayLabel(app);
    overlay.groundHighlightLabel = Loc("settings.groundHighlight", L"Ground highlight");
    overlay.languageLabel = Loc("settings.language", L"Language");
    overlay.languageValue = LocalizationDisplayName(LocalizationLanguage());
    overlay.emptyDropTitle = Loc("empty.dropTitle", L"Drop a 3D model here");
    overlay.emptySupportedFormats = Loc("empty.supportedFormats", L"Supported formats: GLB/GLTF, STL, PLY, OBJ/MTL, FBX, USD/Z, 3MF, STEP/STP");
    if (overlay.settingsPanelOpen)
    {
        overlay.settingsPanelRect = SettingsPanelRect(app);
        overlay.nativeOrientationRowRect = SettingsToggleRowRect(app);
        overlay.nativeOrientationSwitchRect = SettingsSwitchRect(app);
        overlay.hideCursorRowRect = SettingsCursorToggleRowRect(app);
        overlay.hideCursorSwitchRect = SettingsCursorSwitchRect(app);
        overlay.hideUntilCompleteRowRect = SettingsHideUntilCompleteRowRect(app);
        overlay.hideUntilCompleteSwitchRect = SettingsHideUntilCompleteSwitchRect(app);
        overlay.backgroundRowRect = SettingsBackgroundRowRect(app);
        overlay.groundHighlightRowRect = SettingsGroundHighlightRowRect(app);
        overlay.groundHighlightSwitchRect = SettingsGroundHighlightSwitchRect(app);
        overlay.languageRowRect = SettingsLanguageRowRect(app);
    }

    overlay.openWithOpen = app.openWith.open && HasNavigableModel(app) && !app.isFullscreen;
    if (overlay.openWithOpen)
    {
        const auto& layout = app.openWith.layout;
        overlay.openWithPanelRect = layout.panel;
        overlay.openWithStripRect = layout.strip;
        overlay.openWithFallbackRect = layout.fallback;
        overlay.openWithDividerRect = layout.divider;
        overlay.openWithScrollTrackRect = layout.scrollTrack;
        overlay.openWithScrollThumbRect = layout.scrollThumb;
        overlay.openWithScrollbarVisible = layout.scrollbarVisible;
        overlay.openWithStatusVisible = layout.statusVisible;
        overlay.openWithStatusLoading = app.openWith.loading;
        overlay.openWithStatusRect = layout.status;
        overlay.openWithPalette = BuildOpenWithPalette(app);
        overlay.openWithMoreAppsLabel = Loc("openWith.moreApps", L"More apps…");
        if (layout.statusVisible)
        {
            overlay.openWithStatusText = app.openWith.loading
                ? Loc("openWith.looking", L"Looking for installed apps…")
                : LocFormat("openWith.none", L"No other apps for {0}", { app.openWith.extension });
        }
        overlay.openWithTiles.reserve(app.openWith.apps.size() + 1);
        for (std::size_t index = 0; index < app.openWith.apps.size(); ++index)
        {
            const auto& item = app.openWith.apps[index];
            OpenWithTileOverlay tile;
            tile.label = item.displayName;
            tile.handlerKey = item.handlerKey;
            tile.icon = item.icon;
            tile.monogram = OpenWithMonogram(item.displayName);
            if (index < layout.tiles.size()) tile.rect = layout.tiles[index];
            tile.hovered = app.openWith.hovered == static_cast<int>(index);
            tile.pressed = app.openWith.pressed == static_cast<int>(index);
            tile.focused = app.openWith.focused == static_cast<int>(index);
            overlay.openWithTiles.push_back(std::move(tile));
        }
        OpenWithTileOverlay fallback;
        fallback.label = overlay.openWithMoreAppsLabel;
        fallback.rect = layout.fallback;
        fallback.fallback = true;
        fallback.hovered = app.openWith.hovered == static_cast<int>(app.openWith.apps.size());
        fallback.pressed = app.openWith.pressed == static_cast<int>(app.openWith.apps.size());
        fallback.focused = app.openWith.focused == static_cast<int>(app.openWith.apps.size());
        overlay.openWithTiles.push_back(std::move(fallback));
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
        const int control = static_cast<int>(app.keyboardControl);
        const int tileStart = static_cast<int>(viewer_accessibility::Control::OpenWithTile0);
        const int tileEnd = static_cast<int>(viewer_accessibility::Control::OpenWithTile7);
        const bool openWithControl = (control >= tileStart && control <= tileEnd) ||
            app.keyboardControl == viewer_accessibility::Control::OpenWithMoreApps;
        overlay.keyboardFocusVisible = !openWithControl && focused.visible && GetFocus() == app.window;
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
        ShowModeHud(*app, Loc("hud.appUnavailable", L"That app is no longer available"));
        return 0;
    case kOpenWithCatalogChangedMessage:
        RefreshOpenWithPopup(*app);
        return 0;
    case kOpenWithIconsReadyMessage:
        DrainOpenWithIcons(*app);
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
#if !defined(PREVIEW3D_ENABLE_TEST_CONTROL)
        // Shipping builds cannot be driven by local WM_APP messages.
        return 0;
#else
        if (!app->appSmoke || wParam > 240) return 0;
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
        // T20 deterministic capture controls. Fields 31/32/33/35/36 are free
        // after the existing smoke surface; lParam == -1 reads the current
        // canonical value. These remain inside the Debug-only block above.
        if (wParam == 31 || wParam == 32 || wParam == 33 || wParam == 35 || wParam == 36) {
            auto camera = app->renderThread.LockCamera();
            constexpr LPARAM kRead = -1;
            if (wParam == 31) {
                if (lParam != kRead)
                    camera->SetTestView(static_cast<float>(lParam) / 100.0f,
                        camera->TestPitchDegrees(), camera->TestDistanceScale());
                InvalidateRect(window, nullptr, FALSE);
                return static_cast<LRESULT>(std::lround(camera->TestYawDegrees() * 100.0f));
            }
            if (wParam == 32) {
                if (lParam != kRead)
                    camera->SetTestView(camera->TestYawDegrees(),
                        static_cast<float>(lParam) / 100.0f, camera->TestDistanceScale());
                InvalidateRect(window, nullptr, FALSE);
                return static_cast<LRESULT>(std::lround(camera->TestPitchDegrees() * 100.0f));
            }
            if (wParam == 33) {
                if (lParam != kRead)
                    camera->SetTestView(camera->TestYawDegrees(), camera->TestPitchDegrees(),
                        static_cast<double>(lParam) / 1000.0);
                InvalidateRect(window, nullptr, FALSE);
                return static_cast<LRESULT>(std::lround(camera->TestDistanceScale() * 1000.0));
            }
            if (wParam == 35) {
                if (lParam != kRead) {
                    if (lParam != 0 && lParam != 1) return static_cast<LRESULT>(camera->Projection());
                    camera->SetTestProjection(lParam ? ProjectionMode::Orthographic : ProjectionMode::Perspective);
                }
                InvalidateRect(window, nullptr, FALSE);
                return static_cast<LRESULT>(camera->Projection());
            }
            // 36: restore the captured first-open framing immediately.
            if (lParam == kRead) return 1;
            camera->ResetTestView(ViewportAspect(*app));
            InvalidateRect(window, nullptr, FALSE);
            return 1;
        }
        if (wParam == 105) {
            app->captureClean = lParam != 0;
            InvalidateRect(window, nullptr, FALSE);
            return app->captureClean ? 1 : 0;
        }
        if (wParam == 106) {
            // Force one render-thread pass and a synchronous WM_PAINT before a
            // capture reads the swap chain. Returns the presented-frame count.
            app->renderThread.Invalidate();
            InvalidateRect(window, nullptr, FALSE);
            UpdateWindow(window);
            return static_cast<LRESULT>(app->renderThread.SmokeValue(7));
        }
        if (wParam == 107) {
            // T20 capture capabilities: bit 0 canonical camera, bit 1
            // capture-clean viewport, bit 2 forced present.
            return 0x7;
        }
        if (wParam >= 108 && wParam <= 123) {
            // T20 evidence: the effective source-to-viewer transform in row-major
            // float order, one element per query. Recorded with every capture.
            if (!app->loadedModel) return 0;
            const DirectX::XMMATRIX transform = ActiveModelTransform(*app);
            DirectX::XMFLOAT4X4 value{};
            DirectX::XMStoreFloat4x4(&value, transform);
            const float* elements = &value._11;
            return static_cast<LRESULT>(std::bit_cast<uint32_t>(elements[wParam - 108]));
        }
        // T21 Debug material/light diagnostics. All of these live inside the
        // existing PREVIEW3D_ENABLE_TEST_CONTROL + --app-smoke guard, so a
        // shipped/Release build still rejects the message without reading any
        // path or adding a message authority surface.
        if (wParam == 124) {
            if (lParam >= 0 && lParam <= 20) app->renderThread.SetDebugChannel(static_cast<uint32_t>(lParam));
            return static_cast<LRESULT>(app->renderThread.DebugChannel());
        }
        if (wParam == 125) {
            const uint32_t mask = app->renderThread.DebugChannelMask();
            uint32_t supported = 0;
            for (uint32_t channel = 0; channel <= 20; ++channel)
                if ((mask & (1u << channel)) && RenderThread::DebugChannelSupported(channel))
                    supported |= 1u << channel;
            return static_cast<LRESULT>(supported);
        }
        if (wParam == 126) {
            app->debugSampleX0 = static_cast<int>(LOWORD(lParam));
            app->debugSampleY0 = static_cast<int>(HIWORD(lParam));
            return 1;
        }
        if (wParam == 127) {
            app->debugSampleX1 = static_cast<int>(LOWORD(lParam));
            app->debugSampleY1 = static_cast<int>(HIWORD(lParam));
            return 1;
        }
        if (wParam == 128) {
            app->renderThread.RequestDebugSample(app->debugSampleX0, app->debugSampleY0,
                                                 app->debugSampleX1, app->debugSampleY1);
            return 1;
        }
        if (wParam == 129) return static_cast<LRESULT>(app->renderThread.DebugSampleStatus());
        if (wParam >= 130 && wParam <= 133) {
            float rgba[4]{};
            if (!app->renderThread.DebugSample(rgba)) return 0;
            return static_cast<LRESULT>(std::bit_cast<uint32_t>(rgba[wParam - 130]));
        }
        if (wParam == 134) return static_cast<LRESULT>(app->renderThread.DebugMaterialCount());
        if (wParam == 135) {
            const uint32_t count = app->renderThread.DebugMaterialCount();
            if (lParam >= 0 && static_cast<uint32_t>(lParam) < count)
                app->debugMaterialIndex = static_cast<int>(lParam);
            return static_cast<LRESULT>(app->debugMaterialIndex);
        }
        if (wParam >= 136 && wParam <= 230) {
            DebugMaterialRecord record;
            if (!app->renderThread.DebugMaterial(static_cast<uint32_t>(app->debugMaterialIndex), record))
                return 0;
            auto f32 = [](float value) { return static_cast<LRESULT>(std::bit_cast<uint32_t>(value)); };
            switch (wParam) {
            case 136: return static_cast<LRESULT>(record.chunkId);
            case 137: return static_cast<LRESULT>(record.materialChunkId);
            case 138: return static_cast<LRESULT>(record.sourceMeshId);
            case 139: return static_cast<LRESULT>(record.sourceNodeId);
            case 140: return static_cast<LRESULT>(record.instanceId);
            case 141: return static_cast<LRESULT>(record.vertexCount);
            case 142: return static_cast<LRESULT>(record.indexCount);
            case 143: return static_cast<LRESULT>(record.bucket);
            case 144: return record.depthWrite ? 1 : 0;
            case 145: return static_cast<LRESULT>(record.cullMode);
            case 146: return record.hasMaterial ? 1 : 0;
            case 147: return static_cast<LRESULT>(record.alphaMode);
            case 148: return static_cast<LRESULT>(record.flags);
            case 149: return f32(record.baseColor[0]);
            case 150: return f32(record.baseColor[1]);
            case 151: return f32(record.baseColor[2]);
            case 152: return f32(record.baseColor[3]);
            case 153: return f32(record.metallic);
            case 154: return f32(record.roughness);
            case 155: return f32(record.alphaCutoff);
            case 156: return f32(record.emissive[0]);
            case 157: return f32(record.emissive[1]);
            case 158: return f32(record.emissive[2]);
            case 159: return f32(record.emissiveStrength);
            case 160: return f32(record.clearcoatFactor);
            case 161: return f32(record.clearcoatRoughness);
            case 162: return f32(record.occlusionStrength);
            case 163: return f32(record.sheenRoughness);
            case 164: return f32(record.sheen[0]);
            case 165: return f32(record.sheen[1]);
            case 166: return f32(record.sheen[2]);
            case 167: return f32(record.specularFactor);
            case 168: return f32(record.ior);
            case 169: return f32(record.specularColor[0]);
            case 170: return f32(record.specularColor[1]);
            case 171: return f32(record.specularColor[2]);
            case 172: return f32(record.transmissionFactor);
            case 173: return f32(record.uvOffset[0]);
            case 174: return f32(record.uvOffset[1]);
            case 175: return f32(record.uvScale[0]);
            case 176: return f32(record.uvScale[1]);
            case 177: return f32(record.uvRotation);
            case 178: {
                uint32_t packed = 0;
                for (int slot = 0; slot < 4; ++slot)
                    if (record.textureIndices[slot] >= 0)
                        packed |= uint32_t(record.textureIndices[slot] & 0xFF) << (slot * 8);
                return static_cast<LRESULT>(packed);
            }
            case 179: case 180: case 181: case 182:
                return static_cast<LRESULT>(record.textureChunkIds[wParam - 179]);
            case 183: case 184: case 185: case 186:
                return static_cast<LRESULT>(record.textureFormats[wParam - 183]);
            case 187: case 188: case 189: case 190:
                return static_cast<LRESULT>(record.textureColorSpaces[wParam - 187]);
            case 191: return static_cast<LRESULT>(record.mapMask);
            case 192: return record.points ? 1 : 0;
            case 193: return record.lines ? 1 : 0;
            case 194: return record.completeVertex ? 1 : 0;
            // T22 authored-input diagnostics. Slot 0 keeps the legacy uv fields
            // above; these add normal scale, per-slot metadata and slot 4.
            case 201: return f32(record.normalScale);
            case 202: case 203: case 204: case 205: case 206:
                return static_cast<LRESULT>(record.slotFlags[wParam - 202]);
            case 207: case 208: case 209: case 210:
                return f32(record.slotUvOffset[wParam - 207][0]);
            case 211: case 212: case 213: case 214:
                return f32(record.slotUvOffset[wParam - 211][1]);
            case 215: case 216: case 217: case 218:
                return f32(record.slotUvScale[wParam - 215][0]);
            case 219: case 220: case 221: case 222:
                return f32(record.slotUvScale[wParam - 219][1]);
            case 223: case 224: case 225: case 226:
                return f32(record.slotUvRotation[wParam - 223]);
            case 227: return static_cast<LRESULT>(record.textureIndices[4] >= 0
                ? uint32_t(record.textureIndices[4]) : 0u);
            case 228: return static_cast<LRESULT>(record.textureChunkIds[4]);
            case 229: return static_cast<LRESULT>(record.textureFormats[4]);
            case 230: return static_cast<LRESULT>(record.textureColorSpaces[4]);
            }
        }
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
        // T14 studio evidence: query (87) and select (88) the lighting mode
        // through the same SetLightingMode path the toolbar uses.
        if (wParam == 87) return static_cast<LRESULT>(app->lightingMode);
        if (wParam == 88) {
            if (lParam < static_cast<LPARAM>(LightingMode::Studio)
                || lParam > static_cast<LPARAM>(LightingMode::Wireframe)) return static_cast<LRESULT>(app->lightingMode);
            SetLightingMode(*app, static_cast<LightingMode>(lParam));
            return static_cast<LRESULT>(app->lightingMode);
        }
        // T15 presentation evidence: preset / exposure / background / tone map.
        if (wParam == 89) return static_cast<LRESULT>(app->activePreset);
        if (wParam == 90) {
            if (lParam >= 0 && lParam <= static_cast<LPARAM>(viewer_presentation::PresentationPreset::PointCloud)) {
                ApplyManualPresentationPreset(*app,
                    static_cast<viewer_presentation::PresentationPreset>(lParam));
                InvalidateRect(window, nullptr, FALSE);
            }
            return static_cast<LRESULT>(app->activePreset);
        }
        if (wParam == 91) return static_cast<LRESULT>(std::bit_cast<uint32_t>(app->exposure));
        if (wParam == 92) {
            const float value = std::bit_cast<float>(static_cast<uint32_t>(lParam));
            app->exposure = viewer_presentation::ClampExposure(value);
            app->exposureOverridden = true;
            InvalidateRect(window, nullptr, FALSE);
            return static_cast<LRESULT>(std::bit_cast<uint32_t>(app->exposure));
        }
        if (wParam == 93) return static_cast<LRESULT>(app->backgroundMode);
        if (wParam == 94) {
            if (lParam >= 0 && lParam <= static_cast<LPARAM>(viewer_presentation::BackgroundMode::Custom)) {
                app->backgroundMode = static_cast<viewer_presentation::BackgroundMode>(lParam);
                SaveViewerPreferences(*app);
                InvalidateRect(window, nullptr, FALSE);
            }
            return static_cast<LRESULT>(app->backgroundMode);
        }
        if (wParam == 95) return static_cast<LRESULT>(app->toneMapMode);
        if (wParam == 96) {
            if (lParam >= 0 && lParam <= static_cast<LPARAM>(viewer_presentation::ToneMapMode::None)) {
                app->toneMapMode = static_cast<viewer_presentation::ToneMapMode>(lParam);
                app->toneMapOverridden = true;
                InvalidateRect(window, nullptr, FALSE);
            }
            return static_cast<LRESULT>(app->toneMapMode);
        }
        if (wParam == 97) return static_cast<LRESULT>(std::bit_cast<uint32_t>(app->studioIntensity));
        if (wParam == 98) {
            const float value = std::bit_cast<float>(static_cast<uint32_t>(lParam));
            if (std::isfinite(value)) app->studioIntensity = std::clamp(value, 0.0f, 4.0f);
            InvalidateRect(window, nullptr, FALSE);
            return static_cast<LRESULT>(std::bit_cast<uint32_t>(app->studioIntensity));
        }
        if (wParam == 99) {
            const auto stats = app->renderThread.Stats();
            return static_cast<LRESULT>(std::bit_cast<uint64_t>(stats.meanMs));
        }
        if (wParam == 100) {
            const auto stats = app->renderThread.Stats();
            return static_cast<LRESULT>(std::bit_cast<uint64_t>(stats.p95Ms));
        }
        // T16 ground-shadow evidence: query (101) and set (102) the layer.
        if (wParam == 101) return static_cast<LRESULT>(app->shadowVisible ? 1 : 0);
        if (wParam == 102) {
            app->shadowVisible = lParam != 0;
            app->shadowOverridden = true;
            InvalidateRect(window, nullptr, FALSE);
            return static_cast<LRESULT>(app->shadowVisible ? 1 : 0);
        }
        // Grid independence evidence: query (103) and set (104).
        if (wParam == 103) return static_cast<LRESULT>(app->gridVisible ? 1 : 0);
        if (wParam == 104) {
            app->gridVisible = lParam != 0;
            app->presentationOverridden = true;
            InvalidateRect(window, nullptr, FALSE);
            return static_cast<LRESULT>(app->gridVisible ? 1 : 0);
        }
        // T21a appearance evidence: ground highlight query/set (195/196) and
        // custom background color query/set (197/198). 199 flushes the same
        // persisted state the Settings UI and the background shortcut write.
        if (wParam == 195) return static_cast<LRESULT>(app->groundHighlightVisible ? 1 : 0);
        if (wParam == 196) {
            app->groundHighlightVisible = lParam != 0;
            SaveViewerPreferences(*app);
            InvalidateRect(window, nullptr, FALSE);
            return static_cast<LRESULT>(app->groundHighlightVisible ? 1 : 0);
        }
        if (wParam == 197) return static_cast<LRESULT>(app->backgroundColor & 0x00FFFFFFu);
        if (wParam == 198) {
            if (lParam >= 0 && lParam <= 0x00FFFFFF) {
                app->backgroundColor = static_cast<uint32_t>(lParam) & 0x00FFFFFFu;
                SaveViewerPreferences(*app);
                InvalidateRect(window, nullptr, FALSE);
            }
            return static_cast<LRESULT>(app->backgroundColor & 0x00FFFFFFu);
        }
        if (wParam == 199) {
            SaveViewerPreferences(*app);
            return 1;
        }
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
#endif // PREVIEW3D_ENABLE_TEST_CONTROL
    case WM_COPYDATA:
    {
#if !defined(PREVIEW3D_ENABLE_TEST_CONTROL)
        // Shipping builds cannot be driven by local WM_COPYDATA messages.
        return 0;
#else
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
#endif // PREVIEW3D_ENABLE_TEST_CONTROL
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
            app->errorSummary = Loc("failure.graphics.summary", L"Graphics could not be started.");
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
            SetFailure(*app, Loc("failure.activationUnavailable.summary", L"Single-instance activation is unavailable."), activationError);
        if (!app->initialPath.empty() && app->rendererReady) BeginOpen(*app, app->initialPath, {}, app->forcedFormat);
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
        if (LOWORD(wParam) == ID_VIEW_OPEN_INITIAL) BeginOpen(*app, app->initialPath, {}, app->forcedFormat);
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
        if (app->openWith.open && wParam != SIZE_MINIMIZED) CloseOpenWithPopup(*app);
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
        if (app->openWith.open) CloseOpenWithPopup(*app);
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
            SetFailure(*app, Loc("failure.oneModelAtATime.summary", L"Open one model at a time."),
                Loc("failure.oneModelAtATime.details", L"Drop exactly one supported local 3D model into the viewer."));
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
            if (app->openWith.open && app->openWith.hovered >= 0)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
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
                || app->warningButtonHover || app->resetViewButtonHover || app->lightingButtonHover>=0
                || app->orbitSpeedFlyoutHover)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_MOUSELEAVE:
        if (app->openWith.hovered >= 0)
        {
            app->openWith.hovered = -1;
            InvalidateRect(window, nullptr, FALSE);
        }
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
        if (app->infoButtonHover || app->infoPanelCloseButtonHover || app->fullscreenButtonHover || app->warningButtonHover || app->lightingButtonHover>=0)
        {
            app->infoButtonHover = false;
            app->infoPanelCloseButtonHover = false;
            app->fullscreenButtonHover = false;
            app->warningButtonHover = false;
            app->lightingButtonHover = -1;
            InvalidateRect(window, nullptr, FALSE);
        }
        app->orbitSpeedFlyoutHover = false;
        app->resetViewButtonHover = false;
        UpdateTooltipTracking(*app);
        return 0;
    case WM_LBUTTONDOWN:
    {
        const POINT downPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (app->openWith.open)
        {
            const int hit = OpenWithIndexAtPoint(*app, downPoint);
            if (hit >= 0)
            {
                SetCapture(window);
                app->openWith.pressed = hit;
                app->openWith.hovered = hit;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            const RECT openWithButton = app->chrome.Button(Chrome::Part::OpenWith).rect;
            if (PtInRect(&openWithButton, downPoint))
            {
                CloseOpenWithPopup(*app);
                return 0;
            }
            if (OpenWithPointInPanel(*app, downPoint)) return 0;
            CloseOpenWithPopup(*app);
            // Outside clicks close the popup and still fall through to the
            // control under the pointer, matching the other flyouts.
        }
        if (OrbitSpeedFlyoutVisible(*app))
        {
            RECT hitTrack = OrbitFlyoutTrackRect(*app);
            InflateRect(&hitTrack, 0, Scale(*app, 8));
            if (PtInRect(&hitTrack, downPoint))
            {
                SetCapture(window);
                app->orbitSpeedSliderDragging = true;
                SetOrbitSpeedFromFlyoutX(*app, downPoint.x);
                InvalidateRect(window, nullptr, FALSE);
                UpdateTooltipTracking(*app);
                return 0;
            }
            // Consume clicks on the panel body so it never reaches the
            // viewport behind it and starts an orbit drag.
            const RECT panel = OrbitFlyoutRect(*app);
            if (PtInRect(&panel, downPoint)) return 0;
        }
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
            const RECT hideUntilCompleteSwitchRect = SettingsHideUntilCompleteSwitchRect(*app);
            const RECT backgroundRowRect = SettingsBackgroundRowRect(*app);
            const RECT highlightSwitchRect = SettingsGroundHighlightSwitchRect(*app);
            const RECT languageRowRect = SettingsLanguageRowRect(*app);
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
            if (PtInRect(&hideUntilCompleteSwitchRect, downPoint))
            {
                ToggleHideModelUntilComplete(*app);
                return 0;
            }
            if (PtInRect(&backgroundRowRect, downPoint))
            {
                ShowBackgroundMenu(*app);
                return 0;
            }
            if (PtInRect(&highlightSwitchRect, downPoint))
            {
                ToggleGroundHighlight(*app);
                return 0;
            }
            if (PtInRect(&languageRowRect, downPoint))
            {
                ShowLanguageMenu(*app);
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
        if (CanNavigate(*app) && app->renderThread.LockCamera()->ViewMoved())
        {
            const RECT resetButton = ResetViewButtonRect(*app);
            if (PtInRect(&resetButton, downPoint))
            {
                SetCapture(window);
                app->resetViewButtonPressed = true;
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
            const RECT warningBadge = WarningButtonRect(*app);
            if (!IsRectEmpty(&warningBadge) && PtInRect(&warningBadge, downPoint))
            {
                SetCapture(window);
                app->warningButtonPressed = true;
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
            ViewTurn turn{};
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
            else if (NavGizmo::TurnFor(part, turn))
            {
                // Outer roll glyphs: apply one exact 90-degree turn on press,
                // like an axis snap, so no drag capture is needed.
                TurnViewCommand(*app, turn);
            }
            else if (part != NavGizmo::Part::None)
            {
                // Axis node/stem: snap immediately on press. From another face
                // view this is one 90/180-degree turn about a single axis.
                auto camera = app->renderThread.LockCamera();
                camera->SnapToView(ViewOrientationFrom(camera->TargetOrientation(), app->gizmo.ViewFor(part)));
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
        if (app->openWith.open)
        {
            const int hit = OpenWithIndexAtPoint(*app, movePoint);
            if (hit != app->openWith.hovered)
            {
                app->openWith.hovered = hit;
                InvalidateRect(window, nullptr, FALSE);
            }
            if (OpenWithPointInPanel(*app, movePoint))
            {
                UpdateTooltipTracking(*app);
                return 0;
            }
        }
        if (app->speedSliderDragging)
        {
            if (GetCapture() == window) SetFlySpeedFromFlyoutX(*app, movePoint.x);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (app->orbitSpeedSliderDragging)
        {
            if (GetCapture() == window) SetOrbitSpeedFromFlyoutX(*app, movePoint.x);
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
            || app->warningButtonPressed || app->resetViewButtonPressed || app->lightingButtonPressed>=0) return 0;
        if (app->pointerMode == PointerMode::None)
        {
            // Orbit speed flyout: shown while hovering the Orbit button, and
            // kept open while the pointer moves down into the panel itself.
            RECT orbitHoverRect = OrbitFlyoutHoverRect(*app);
            const bool overOrbitFlyout = app->orbitEnabled && HasNavigableModel(*app)
                && PtInRect(&orbitHoverRect, movePoint) != FALSE;
            if (overOrbitFlyout != app->orbitSpeedFlyoutHover)
            {
                app->orbitSpeedFlyoutHover = overOrbitFlyout;
                if (overOrbitFlyout)
                {
                    TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                    TrackMouseEvent(&track);
                }
                InvalidateRect(window, nullptr, FALSE);
            }
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
                    // The two flyouts share the same column below the title
                    // bar; don't let the hover-only Orbit slider cover an
                    // explicitly opened Travel speed panel.
                    if (effective == Chrome::Part::Orbit && app->speedFlyoutOpen)
                        app->speedFlyoutOpen = false;
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
                // Warning badge hover (above the bottom bar, below the rest of
                // the viewport chrome). Tracked here so a click on it is never
                // mistaken for a viewport orbit.
                const RECT warningBadge = WarningButtonRect(*app);
                const bool overWarning = !IsRectEmpty(&warningBadge) && PtInRect(&warningBadge, movePoint) != FALSE;
                if (overWarning != app->warningButtonHover)
                {
                    app->warningButtonHover = overWarning;
                    if (overWarning)
                    {
                        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                        TrackMouseEvent(&track);
                    }
                    InvalidateRect(window, nullptr, FALSE);
                }
                // Floating Reset button: only hoverable while the camera is
                // actually away from home (matching its drawn visibility).
                const bool resetVisible = app->renderThread.LockCamera()->ViewMoved();
                const RECT resetButton = ResetViewButtonRect(*app);
                const bool overReset = resetVisible && PtInRect(&resetButton, movePoint) != FALSE;
                if (overReset != app->resetViewButtonHover)
                {
                    app->resetViewButtonHover = overReset;
                    if (overReset)
                    {
                        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                        TrackMouseEvent(&track);
                    }
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
        if (app->openWith.pressed >= 0)
        {
            const POINT upPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const int pressed = app->openWith.pressed;
            app->openWith.pressed = -1;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            if (OpenWithIndexAtPoint(*app, upPoint) == pressed)
                InvokeOpenWithItem(*app, pressed);
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->speedSliderDragging)
        {
            app->speedSliderDragging = false;
            if (GetCapture() == window) ReleaseCapture();
            UpdateTooltipTracking(*app);
            return 0;
        }
        if (app->orbitSpeedSliderDragging)
        {
            app->orbitSpeedSliderDragging = false;
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
        if (app->resetViewButtonPressed)
        {
            const POINT upPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            app->resetViewButtonPressed = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            const RECT resetButton = ResetViewButtonRect(*app);
            if (app->renderThread.LockCamera()->ViewMoved() && PtInRect(&resetButton, upPoint))
            {
                HandleCommand(*app, ID_VIEW_RESET);
            }
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
        if (app->warningButtonPressed)
        {
            const POINT upPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            app->warningButtonPressed = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            const RECT warningBadge = WarningButtonRect(*app);
            if (!IsRectEmpty(&warningBadge) && PtInRect(&warningBadge, upPoint)) ShowModelWarnings(*app);
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
        app->warningButtonPressed=false;
        app->resetViewButtonPressed=false;
        EndPointer(*app);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (PointInViewport(*app, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }) && CanNavigate(*app))
        {
            FrameSelectedOrAll(*app);
        }
        return 0;
    case WM_MOUSEHWHEEL:
    {
        POINT wheelPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };   // screen coords for this message
        ScreenToClient(window, &wheelPoint);
        if (app->openWith.open && OpenWithPointInPanel(*app, wheelPoint))
        {
            ScrollOpenWith(*app, GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1);
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL:
    {
        POINT wheelPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };   // screen coords for this message
        ScreenToClient(window, &wheelPoint);
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        if (app->openWith.open && OpenWithPointInPanel(*app, wheelPoint))
        {
            ScrollOpenWith(*app, GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
            return 0;
        }
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
        if (HandleOpenWithKey(*app, wParam)) return 0;
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
        if (wParam == VK_OEM_4) { AdjustExposure(*app, -0.1f); return 0; }
        if (wParam == VK_OEM_6) { AdjustExposure(*app, 0.1f); return 0; }
        if (wParam == VK_OEM_5) { CycleBackground(*app); return 0; }
        if (wParam == VK_OEM_3) { CycleToneMap(*app); return 0; }
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
        if (app->openWith.open) CloseOpenWithPopup(*app);
        if (app->speedFlyoutOpen) { app->speedFlyoutOpen = false; InvalidateRect(window, nullptr, FALSE); }
        if (app->settingsPanelOpen) { app->settingsPanelOpen = false; InvalidateRect(window, nullptr, FALSE); }
        app->orbitSpeedFlyoutHover = false;
        app->orbitSpeedSliderDragging = false;
        app->resetViewButtonHover = false;
        app->resetViewButtonPressed = false;
        return 0;
    case WM_CONTEXTMENU:
        return 0;
    case WM_SYSKEYDOWN:
        if (wParam == 'M' && (lParam & (1u << 29))) { ShowMoreMenu(*app); return 0; }
        break;
    case kUpdateCheckCompleteMessage:
    {
        std::unique_ptr<std::wstring> version(reinterpret_cast<std::wstring*>(lParam));
        if (!version || version->empty()) return 0;
        if (!preview3d_update::IsVersionNewer(*version, PREVIEW3D_VERSION)) return 0;
        if (app->updateVersion != *version)
        {
            app->updateVersion = *version;
            UpdateChromeLayout(*app);
            UpdateTooltipTracking(*app);
            InvalidateRect(window, nullptr, FALSE);
            viewer_accessibility::Announce(window, app->uiaAccessible,
                LocFormat("a11y.updateVersion", L"Update {0} is available", { L"v" + app->updateVersion }));
        }
        return 0;
    }
    case kUpdateDownloadCompleteMessage:
    {
        std::unique_ptr<UpdateDownloadResult> result(
            reinterpret_cast<UpdateDownloadResult*>(lParam));
        app->updateDownloading = false;
        if (!result) return 0;
        if (!result->ok)
        {
            MessageBoxW(window,
                Loc("update.downloadFailed",
                    L"The update could not be downloaded. Please try again later.").c_str(),
                Loc("update.caption", L"Preview 3D update").c_str(),
                MB_OK | MB_ICONERROR);
            return 0;
        }

        const std::wstring openPath = app->currentPath.empty()
            ? app->initialPath : app->currentPath;
        std::wstring handoffError;
        if (!preview3d_update::StartInstallerHandoff(
                result->installerPath, result->expectedSha256, openPath, handoffError))
        {
            MessageBoxW(window,
                Loc("update.applyFailed",
                    L"The update could not be started. Please try again later.").c_str(),
                Loc("update.caption", L"Preview 3D update").c_str(),
                MB_OK | MB_ICONERROR);
            return 0;
        }

        app->updateRestartPending = true;
        ShowModeHud(*app, LocFormat("hud.updateRestarting",
            L"Restarting to install update {0}...", { L"v" + app->updateVersion }));
        PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    }
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
        app->renderThread.FinishImport(complete->generation, complete->result.sourceIdentity, complete->result.missingAssets);
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
        SetFailure(*app, Loc("failure.graphics.summary", L"Graphics could not be started."),
                   failure ? failure->details : Loc("failure.renderInit.summary", L"The render thread stopped during initialization."));
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
            SetFailure(*app, Loc("failure.graphicsRecovery.summary", L"Graphics recovery failed."), recovery->details, recovery->path,
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
            if (!app->openFormatNotice.empty()) {
                if (!app->warning.empty()) app->warning += L"\n";
                app->warning += app->openFormatNotice;
            }
            app->missingAssets=uploaded->metadata->missingAssets;
        }
        // Apply the format catalog's presentation profile once, at the first
        // completed upload for a new document. Refinements, per-frame work and
        // reloads of the same path must not re-apply it; an explicit lighting
        // or grid change pins the session override.
        if (uploaded->metadata && uploaded->terminal && !uploaded->refinement
            && app->presentationSourcePath != uploaded->path)
        {
            app->presentationSourcePath = uploaded->path;
            if (!app->presentationOverridden)
            {
                if (const auto* descriptor =
                        model_core::FindBySourceFormatId(uploaded->metadata->source.format))
                {
                    const auto profile = model_core::ResolvePresentationProfile(*descriptor,
                        { uploaded->metadata->pointCount, uploaded->metadata->triangleCount });
                    ApplyPresentationProfile(*app, profile,
                        uploaded->hasMaterialSummary ? uploaded->materialSummary
                                                     : viewer_presentation::MaterialSummary{});
                }
            }
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
        app->updateCheckThread.request_stop();
        app->updateDownloadThread.request_stop();
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
    RECT windowBounds{ 0, 0, MulDiv(1200, dpi, 96), MulDiv(864, dpi, 96) };
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

// Runs exactly one release lookup off the UI thread. The worker only posts a
// validated version string back; it never touches app state, the window, or
// the renderer. Failure is silent by design: no toast, no modal, no retry loop.
void StartUpdateCheck(ViewerApp& app)
{
    if (!app.updateVersion.empty()) return;
    const HWND window = app.window;
    const std::shared_ptr<std::atomic_bool> alive = app.alive;
    app.updateCheckThread = std::jthread([window, alive](std::stop_token stop)
    {
        std::wstring version;
        std::wstring error;
        if (!preview3d_update::FetchLatestVersion(version, error)) return;
        if (stop.stop_requested() || !alive || !alive->load(std::memory_order_relaxed)) return;
        if (!preview3d_update::IsVersionNewer(version, PREVIEW3D_VERSION)) return;

        auto payload = std::make_unique<std::wstring>(std::move(version));
        if (!PostMessageW(window, kUpdateCheckCompleteMessage, 0,
                reinterpret_cast<LPARAM>(payload.get())))
        {
            return;
        }
        payload.release();
    });
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
    // ACG is withheld from the viewer until a GPU run validates the user-mode
    // D3D runtime's shader compilation path; the import children enable it.
    if (!platform::ApplyProcessMitigations(/*prohibitDynamicCode=*/false))
    {
        return 1;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT comResult = E_FAIL;

    ViewerApp app;
    app.instance = instance;
    app.benchmarkStartedUs = processStartedUs;
    const ViewerSettings settings = LoadSettings();
    // Localization first: every user-facing string below (including command-line
    // and security message boxes) resolves through the active language pack.
    LocalizationInitialize();
    LocalizationSetLanguage(settings.language);
    app.language = settings.language;
    app.showNativeOrientation = settings.showNativeOrientation;
    app.groundAxis = settings.groundAxis;
    app.groundAxisInverted = settings.groundAxisInverted;
    app.hideCursorWhileDragging = settings.hideCursorWhileDragging;
    app.hideModelUntilComplete = settings.hideModelUntilComplete;
    app.toneMapMode = static_cast<viewer_presentation::ToneMapMode>(settings.toneMapMode);
    app.backgroundMode = static_cast<viewer_presentation::BackgroundMode>(settings.backgroundMode);
    app.backgroundColor = settings.backgroundColor & 0x00FFFFFFu;
    app.groundHighlightVisible = settings.groundHighlightEnabled;
    app.exposure = viewer_presentation::ClampExposure(settings.exposure);
    app.shadowVisible = settings.groundShadowEnabled;
    app.shadowOverridden = settings.groundShadowOverridden;
    app.renderThread.SetHideModelUntilComplete(app.hideModelUntilComplete);
    bool commandLineInvalid = false;
    bool bypassSingleInstance = false;
    bool activationSmoke = false;
    bool listFormats = false;
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
            else if (_wcsicmp(arguments[i], L"--skip-content-check") == 0) app.skipContentCheck = true;
            else if (_wcsicmp(arguments[i], L"--list-formats") == 0) listFormats = true;
            else if (_wcsicmp(arguments[i], L"--format") == 0) {
                if (++i >= argumentCount) commandLineInvalid = true;
                else {
                    const auto option = open_options::ParseFormatName(arguments[i]);
                    if (!option.valid) commandLineInvalid = true;
                    else app.forcedFormat = option.descriptor;
                }
            }
            else if (_wcsnicmp(arguments[i], L"--format=", 9) == 0) {
                const auto option = open_options::ParseFormatName(arguments[i] + 9);
                if (!option.valid) commandLineInvalid = true;
                else app.forcedFormat = option.descriptor;
            }
            // The external test-control surface (--app-smoke plus the WM_APP+104
            // and WM_COPYDATA message handlers) is compiled only into Debug
            // developer/test builds. In a Release (shipping) binary these flags
            // fall through to the unknown-flag branch and are rejected with a
            // usage exit, so a local process cannot drive the product.
#if defined(PREVIEW3D_ENABLE_TEST_CONTROL)
            // Debug-only visual preview of the update badge: skips the live
            // check and presents a synthetic newer version so the icon,
            // tooltip, and click-through can be exercised offline.
            else if (_wcsicmp(arguments[i], L"--update-preview") == 0) app.updateVersion = L"1.2.3";
            else if (_wcsnicmp(arguments[i], L"--update-preview=", 17) == 0)
            {
                const std::wstring previewVersion =
                    preview3d_update::NormalizeVersion(arguments[i] + 17);
                if (previewVersion.empty() ||
                    !preview3d_update::IsVersionNewer(previewVersion, PREVIEW3D_VERSION))
                {
                    commandLineInvalid = true;
                }
                else app.updateVersion = previewVersion;
            }
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
#endif // PREVIEW3D_ENABLE_TEST_CONTROL
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
#if defined(PREVIEW3D_ENABLE_TEST_CONTROL)
            else if (_wcsicmp(arguments[i], L"--benchmark-worker-budget-failure") == 0) {
                app.appSmoke = true; app.faultForTesting = 6;
            }
#endif
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

    if (listFormats)
    {
        // Stable machine-readable listing: one
        // stable-name<TAB>extensions<TAB>display-name line per catalog format.
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        const std::wstring text = open_options::BuildFormatListText(
            [](const model_core::FormatDescriptor& descriptor) {
                return Loc(descriptor.displayNameKey, descriptor.displayName);
            });
        const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(required > 0 ? static_cast<std::size_t>(required) : 0, '\0');
        if (required > 0) {
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                utf8.data(), required, nullptr, nullptr);
        }
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        bool closeOutput = false;
        if (!output || output == INVALID_HANDLE_VALUE || GetFileType(output) == FILE_TYPE_UNKNOWN) {
            output = CreateFileW(L"CONOUT$", GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            closeOutput = true;
        }
        DWORD written = 0;
        if (output && output != INVALID_HANDLE_VALUE && !utf8.empty())
            WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        if (closeOutput && output && output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
        return 0;
    }

    if (!app.initialPath.empty())
    {
        std::wstring normalized, pathError;
        if (!active_instance::NormalizeForwardPath(app.initialPath, normalized, pathError)) commandLineInvalid = true;
        else app.initialPath = std::move(normalized);
    }
    if (commandLineInvalid)
    {
        // TODO: temporarily commented out for testing, undo at some point
        // MessageBoxW(nullptr, Loc("cli.usage", L"Usage: Preview3D.exe [model-path]\n       Preview3D.exe --open <model-path>").c_str(),
        //     kApplicationName, MB_OK | MB_ICONERROR);
        return 2;
    }

    if (app.benchmarkMode) {
        const bool validReference = app.benchmarkReference == L"performance" || app.benchmarkReference == L"compatibility";
        // A --benchmark-result target must be an app-owned .json file. This
        // rejects UNC/device paths, alternate data streams, traversal, and any
        // path outside %LOCALAPPDATA%\Binbuf\Preview 3D, so a shortcut or file
        // association cannot use the benchmark lane to truncate an arbitrary
        // user-writable file. An empty target keeps the console-output default.
        bool validResultPath = true;
        if (!app.benchmarkResultPath.empty()) {
            std::wstring appRoot;
            std::wstring pathError;
            validResultPath = preview3d::safeio::AppDataDirectory(appRoot) &&
                preview3d::safeio::IsSafeOutputPath(app.benchmarkResultPath, L".json", appRoot, pathError);
            if (validResultPath) {
                const std::size_t separator = app.benchmarkResultPath.find_last_of(L"\\/");
                if (separator != std::wstring::npos) {
                    CreateDirectoryW(appRoot.c_str(), nullptr);
                    CreateDirectoryW(app.benchmarkResultPath.substr(0, separator).c_str(), nullptr);
                }
            }
        }
        if (app.initialPath.empty() || app.benchmarkFrameLimit < 1 || app.benchmarkFrameLimit > 100'000
            || app.benchmarkRepeat < 1 || app.benchmarkRepeat > 100 || app.benchmarkDurationMs < 100
            || app.benchmarkDurationMs > 600'000 || !validReference || !validResultPath) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
            const char message[] = "Invalid --benchmark arguments (fixture, frames 1..100000, duration 100..600000 ms, repeat 1..100, reference performance|compatibility, result path under the app data directory with a .json extension).\n";
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
        // MessageBoxW(nullptr, instanceError.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
        return 5;
    }
    if (instanceRole == active_instance::Coordinator::Role::Secondary)
    {
        active_instance::Command command;
        command.type = app.initialPath.empty() ? active_instance::CommandType::Activate : active_instance::CommandType::Open;
        command.path = app.initialPath;
        if (app.activeInstance.Forward(command, instanceError)) return 0;
        // MessageBoxW(nullptr, instanceError.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
        return 3;
    }

    // Explain an unsigned engineering build up front on Smart App Control
    // machines. It cannot be bypassed per app, and it may later refuse a bundled
    // dependency (for example worker\zstd.dll with status 0xC0E90002). Skipped
    // for smoke/benchmark runs so automation is never interrupted by a dialog.
    if (!app.appSmoke && !app.benchmarkMode && app.benchFrames == 0 && IsSmartAppControlActive())
    {
        // MessageBoxW(nullptr,
        //     Loc("startup.smartAppControl",
        //         L"Smart App Control is active on this PC, and this Preview 3D build is unsigned.\n\n"
        //         L"Smart App Control has no per-app exception, so Windows may block the app or one of the DLLs bundled beside it (for example worker\\zstd.dll, error status 0xC0E90002).\n\n"
        //         L"If Preview 3D is blocked, turn off Smart App Control under Windows Security > App & browser control > Smart App Control settings.").c_str(),
        //     kApplicationName, MB_OK | MB_ICONWARNING);
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
    InitializeOpenWithCatalog(gMainWindow, OpenWithNotifications{
        kOpenWithLaunchFailedMessage, kOpenWithCatalogChangedMessage, kOpenWithIconsReadyMessage });
    // Automation and performance runs must stay deterministic and offline.
    if (!app.appSmoke && !app.benchmarkMode && app.benchFrames == 0) StartUpdateCheck(app);

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
            if (app.resetViewPending && !app.renderThread.LockCamera()->ViewMoved())
            {
                app.resetViewPending = false;
                // The button is fading out under a possibly stationary cursor;
                // drop its hover state so no stale tooltip pops up.
                if (app.resetViewButtonHover)
                {
                    app.resetViewButtonHover = false;
                    UpdateTooltipTracking(app);
                }
            }
            UpdateResetViewButtonFade(app);
            UpdateRollGlyphFade(app);
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

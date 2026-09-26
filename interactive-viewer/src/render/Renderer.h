#pragma once

#include "Chrome.h"
#include "GroundAxis.h"
#include "InfoPanel.h"
#include "Model.h"
#include "NavGizmo.h"

#include <DirectXMath.h>
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

// Vertical field of view shared by the camera math, framing, and picking.
constexpr float kVerticalFieldOfView = DirectX::XM_PIDIV4;

enum class ViewerState
{
    Empty,
    Loading,
    Ready,
    Failed,
    Partial // cancelled usable geometry; no verified complete catalog yet
};

enum class ProjectionMode
{
    Perspective,
    Orthographic
};

// Viewport shading is deliberately independent from the imported material
// catalog. Studio is the neutral/default material-review environment, Clay
// removes material distractions, and Directional exposes fine surface detail
// under a single user-rotatable raking light.
enum class LightingMode
{
    Studio,
    Clay,
    Directional,
    Wireframe
};

struct OverlayInfo
{
    ViewerState state = ViewerState::Empty;
    std::wstring filename;
    std::wstring errorSummary;
    std::wstring errorDetails;
    std::wstring warning;
    // Bottom-right warning badge hit rect (client px). Empty while no warning
    // badge is shown. Drawn from this same rect so drawing and hit-testing can
    // never disagree.
    RECT warningButtonRect{};
    bool warningButtonHover = false;
    bool warningButtonPressed = false;
    // True when the warning badge stands for unresolved sidecar assets, so the
    // activation path offers a folder picker instead of only an explanation.
    bool warningHasMissingAssets = false;
    std::wstring failureContext;
    std::wstring loadingStatus;
    // Set after the successful Present that includes the final visible
    // refinement for a newly opened file. This is the user-visible
    // load-to-render duration, formatted by the UI thread.
    std::wstring renderDurationText;
    // True while the load-to-render timer above is still running. The bottom
    // bar uses this to occupy the eventual duration slot with a spinner.
    bool renderTimerRunning = false;
    float animationPhase = 0.0f;
    float dpiScale = 1.0f;
    // Reserved viewport inset: how much of the client area the title/bottom
    // bars claim away from the 3D viewport (Preview3D.cpp's ViewportAspect,
    // and Renderer::Render's own viewportTop/bottomInset). Both collapse to 0
    // in Fullscreen so the viewport fills the whole monitor.
    int toolbarHeight = 40;
    int bottomBarHeight = 0;
    // The bars' own drawn/hit-tested height — always the real height
    // whenever the bar should be visible, Fullscreen included, where the two
    // bars become a floating toolbar overlaying the (now full-monitor)
    // viewport instead of pushing it down. Used by DrawTitleBar (via
    // Chrome::TitleBarRect, which is built from this same raw height) /
    // DrawBottomBar / DrawInfoPanel and the bottom-anchored HUD chrome —
    // never by the viewport sizing above.
    int barToolbarHeight = 40;
    int barBottomBarHeight = 0;
    int infoPanelWidth = 0;   // 0 when the Information panel is closed
    std::vector<InfoPanelSection> infoPanelSections;   // only meaningful while infoPanelWidth > 0
    float infoPanelScrollOffset = 0.0f;   // logical px scrolled down the section list, from WM_MOUSEWHEEL
    RECT infoPanelCloseButtonRect{};   // client px, right-aligned in the fixed panel header
    bool infoPanelCloseButtonHover = false;
    bool infoPanelCloseButtonPressed = false;
    float zoomPercent = 100.0f;   // 100 == the default Fit framing distance
    RECT zoomTrackRect{};         // client px, the D2D-drawn zoom slider's track, valid while barBottomBarHeight > 0
    float zoomSliderT = 0.0f;     // 0..1 normalized zoom-slider thumb position
    RECT infoButtonRect{};        // client px, docked at the far left of the bottom bar
    bool infoButtonHover = false;
    bool infoButtonPressed = false;
    RECT fullscreenButtonRect{};  // client px, bottom-bar Fullscreen toggle, right of the percent readout
    bool fullscreenButtonHover = false;
    bool fullscreenButtonPressed = false;
    bool isFullscreen = false;
    LightingMode lightingMode = LightingMode::Studio;
    float directionalLightAngle = 0.0f; // normalized 0..1 horizontal rotation
    float directionalLightElevation = 0.502f; // radians above the horizon, 0..~85deg
    bool lightDragging = false; // LMB held on the gizmo's sun (drag affordance)
    RECT lightingToolbarRect{};
    RECT studioButtonRect{};
    RECT clayButtonRect{};
    RECT directionalButtonRect{};
    RECT wireframeButtonRect{};
    bool studioButtonHover = false;
    bool clayButtonHover = false;
    bool directionalButtonHover = false;
    bool wireframeButtonHover = false;
    bool studioButtonPressed = false;
    bool clayButtonPressed = false;
    bool directionalButtonPressed = false;
    bool wireframeButtonPressed = false;
    bool hasModel = false;
    bool gridVisible = true;
    bool axisSnapEnabled = false;
    bool infoPanelVisible = false;
    bool speedFlyoutOpen = false;
    RECT speedFlyoutRect{};        // client px, valid only while speedFlyoutOpen
    RECT speedFlyoutTrackRect{};   // the draggable track within it
    float speedSliderT = 0.0f;     // 0..1 normalized thumb position
    std::wstring speedValueText;   // e.g. "×1.00"
    bool settingsPanelOpen = false;
    RECT settingsPanelRect{};        // client px, valid only while settingsPanelOpen
    RECT nativeOrientationRowRect{};
    RECT nativeOrientationSwitchRect{};
    RECT hideCursorRowRect{};
    RECT hideCursorSwitchRect{};
    bool showNativeOrientation = false;   // current value, for drawing the switch's on/off state
    bool hideCursorWhileDragging = true;
    GroundAxis groundAxis = GroundAxis::Automatic; // persisted selection used by rendering
    GroundAxis effectiveGroundAxis = GroundAxis::Z; // resolved X/Y/Z shown by the toolbar
    bool groundAxisInverted = false; // negative side of effectiveGroundAxis is up
    float selectionAmount = 0.0f;   // 0..1 mesh-selection highlight
    std::wstring speedHud;          // transient fly-speed readout
    float speedHudAlpha = 0.0f;
    std::wstring modeHud;           // transient mode readout (grid/projection)
    float modeHudAlpha = 0.0f;
    bool tooltipVisible = false;    // true once the hover-delay timer has elapsed
    RECT tooltipAnchorRect{};       // client px, the hovered button this tooltip describes
    std::wstring tooltipText;
    bool tooltipBelow = true;       // true: title-bar buttons (bubble drawn below); false: bottom-bar buttons (drawn above)
    bool highContrast = false;
    bool keyboardFocusVisible = false;
    RECT keyboardFocusRect{};
    // Root transform applied to the model draw only (never the grid). It maps
    // the selected model axis onto the app's fixed Z-up world; native
    // orientation bypasses it.
    DirectX::XMFLOAT4X4 modelTransform = []
    {
        DirectX::XMFLOAT4X4 identity{};
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        return identity;
    }();
};

// Per-frame navigation intents gathered from keyboard state. The camera eases
// toward these each frame, which is what makes flight feel weighty instead of
// toggling velocity on and off.
struct FlightInput
{
    float right = 0.0f;      // -1..1 strafe intent
    float up = 0.0f;        // -1..1 vertical intent
    float forward = 0.0f;  // -1..1 view-relative intent
    float roll = 0.0f;      // -1..1 roll intent
    float orbitX = 0.0f;    // pixels/second orbit intent (arrow keys)
    float orbitY = 0.0f;
    float panX = 0.0f;      // pixels/second pan intent (Shift+arrow keys)
    float panY = 0.0f;
    bool fast = false;      // Shift boost
    float viewportHeight = 0.0f;
};

// Turntable orbit camera. The orientation is a quaternion (world-from-camera
// rotation), which sidesteps Euler gimbal lock entirely: yaw composes about the
// world up axis and pitch about the camera right axis, both as axis-angle
// quaternion multiplies. The eye is always pivot + rotate((0, 0, distance), q).
struct Camera
{
    double targetX = 0.0;
    double targetY = 0.0;
    double targetZ = 0.0;
    double distance = 4.0;
    double targetDistance = 4.0;
    double sceneRadius = 1.0;
    DirectX::XMFLOAT4 orientation{ 0.0f, 0.0f, 0.0f, 1.0f };

    // Cached default framing captured when a model loads. "Reset View"
    // (Home / the toolbar button) interpolates the camera back to this state.
    double homeX = 0.0;
    double homeY = 0.0;
    double homeZ = 0.0;
    double homeDistance = 4.0;
    DirectX::XMFLOAT3 homeBoundsMin{};
    DirectX::XMFLOAT3 homeBoundsMax{};
    DirectX::XMFLOAT4 homeOrientation{ 0.0f, 0.0f, 0.0f, 1.0f };

    double desiredX = 0.0;
    double desiredY = 0.0;
    double desiredZ = 0.0;
    bool pivotAnimating = false;
    double pivotAnimElapsed = 0.0;
    double pivotFromX = 0.0;
    double pivotFromY = 0.0;
    double pivotFromZ = 0.0;
    DirectX::XMFLOAT4 desiredOrientation{ 0.0f, 0.0f, 0.0f, 1.0f };
    bool orientationAnimating = false;
    double orientationAnimElapsed = 0.0;
    DirectX::XMFLOAT4 orientationFrom{ 0.0f, 0.0f, 0.0f, 1.0f };
    bool reduceMotion = false;

    // Perspective/orthographic toggle. The orthographic half-height equals
    // distance * tan(fov/2), so switching projection preserves the framing at
    // the pivot plane and wheel dolly means the same thing in both modes.
    ProjectionMode projection = ProjectionMode::Perspective;
    double projectionBlend = 0.0;  // eased 0..1 cross-fade of the two matrices

    double velRight = 0.0;
    double velUp = 0.0;
    double velForward = 0.0;
    double rollRate = 0.0;
    double orbitRateX = 0.0;
    double orbitRateY = 0.0;
    double panRateX = 0.0;
    double panRateY = 0.0;
    double inertiaX = 0.0;
    double inertiaY = 0.0;
    double panInertiaX = 0.0;
    double panInertiaY = 0.0;
    double lastViewportHeight = 0.0;
    double flySpeedScale = 1.0;
    FlightInput input{};
    // Raw mouse-look deltas queued by WM_INPUT since the last Update(), so
    // look and WASD translation are integrated together on the same tick
    // instead of look snapping ahead of movement (see AccumulateLook).
    double pendingLookX = 0.0;
    double pendingLookY = 0.0;

    void SetBounds(const DirectX::XMFLOAT3& minimum, const DirectX::XMFLOAT3& maximum, float aspect);
    void Fit(float aspect);
    // Frames an arbitrary world-space box (used for "frame selected").
    void FrameBox(const DirectX::XMFLOAT3& minimum, const DirectX::XMFLOAT3& maximum, float aspect);
    void Reset(float aspect);
    void Orbit(float deltaX, float deltaY);
    void Look(float deltaX, float deltaY);
    // Queues a raw mouse-look delta to be applied on the next Update() tick,
    // rather than immediately, so a burst of WM_INPUT messages during a
    // fly-look drag rotates the camera in step with WASD translation instead
    // of ahead of it (which otherwise produces a blocky, chorded flight path).
    void AccumulateLook(float deltaX, float deltaY);
    void Pan(float deltaX, float deltaY, float viewportHeight);
    // Trucks along the world ground plane (flattened forward/right), optionally
    // snapped to the nearest world axis. Used by the MMB drag and Shift+arrows.
    void Truck(float deltaX, float deltaY, float viewportHeight, bool axisSnap);
    void Dolly(float wheelSteps);
    // Smooth continuous dolly driven by a Ctrl+MMB drag.
    void DollyDrag(float deltaY);
    void MoveLocal(float rightAmount, float upAmount, float forwardAmount);
    void Roll(float radians);
    void SetInput(const FlightInput& value);
    void Update(double deltaTime);
    bool HasMotion() const;
    void StopMotion();
    void SeedOrbitInertia(float velocityX, float velocityY);
    void SeedPanInertia(float velocityX, float velocityY);
    void CancelInertia();
    // Animates orientation to an axis-aligned view (shortest-arc slerp).
    void SnapToView(DirectX::XMVECTOR viewOrientation);
    void SetProjection(ProjectionMode mode);
    ProjectionMode Projection() const { return projection; }
    void SetFlySpeedScale(double scale);
    double FlySpeedScale() const { return flySpeedScale; }

    DirectX::XMVECTOR Orientation() const;
    DirectX::XMVECTOR EyePosition() const;
    DirectX::XMMATRIX ViewMatrix() const;
    // Blends perspective and orthographic according to projectionBlend.
    DirectX::XMMATRIX ProjectionMatrix(float aspect) const;

private:
    double FitDistance(float aspect) const;
    double FlightSpeed() const;
    void ApplyOrbitAngles(float yawAngle, float pitchAngle);
    void ShiftPivot(double x, double y, double z);
    double FarBound() const;
};

RECT CalculateErrorCardRect(int width, int height, int toolbarHeight, float dpiScale);

class Renderer
{
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Initialize(HWND window, std::wstring& error);
    bool Resize(int width, int height, std::wstring& error);
    bool UploadModel(const ModelData& model, std::wstring& error);
    // Rebuilds the ground-grid buffer from explicit bounds — decoupled from
    // UploadModel because the "show native orientation" toggle changes the
    // active/effective bounds (see Model.h's TransformBounds) without
    // re-uploading or re-parsing the model itself.
    bool RebuildGrid(const DirectX::XMFLOAT3& boundsMin, const DirectX::XMFLOAT3& boundsMax, std::wstring& error);
    void ClearModel();
    void Render(const Camera& camera, const OverlayInfo& overlay, const NavGizmo& gizmo, const Chrome& chrome);
    bool HasModel() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

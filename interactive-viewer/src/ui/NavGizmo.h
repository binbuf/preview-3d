#pragma once

#include <DirectXMath.h>

// Canonical axis-aligned view orientations for this viewer's own Z-up,
// right-handed world. A source format may use a different up axis at rest —
// glTF/GLB, for example, is Y-up by spec — but that is corrected on the way
// in (see Model.h's ModelData::upAxisCorrection) before anything reaches the
// camera, gizmo, or grid; this file only ever deals in the app's Z-up frame.
// "Front" looks along +Y at the model; the reverse views look from behind.
enum class ViewDir
{
    Front,
    Back,
    Left,
    Right,
    Top,
    Bottom
};

// Builds the camera orientation quaternion that looks along `forward` with
// `up` pointing up the screen. The rotation rows are (right, up, back), which
// matches this viewer's camera basis: eye = pivot + rotate((0, 0, distance), q),
// so the returned quaternion drops straight into Camera::orientation.
// A forward parallel to `up` (the pole case) is resolved with a fallback basis
// so the result is never degenerate.
DirectX::XMVECTOR OrientationFromForwardUp(DirectX::XMVECTOR forward, DirectX::XMVECTOR up);
DirectX::XMVECTOR CanonicalViewOrientation(ViewDir view);

// Blender-style navigation gizmo drawn in the top-right corner of the 3D
// viewport. This is pure layout/geometry state with no drawing or window
// dependencies: the app uses HitTest to route pointer input, and the renderer
// projects ComputeDraw output through Direct2D on top of the frame.
class NavGizmo
{
public:
    enum class Part
    {
        None,
        Ball,
        PosX,
        PosY,
        PosZ,
        NegX,
        NegY,
        NegZ,
        // The outer light ring (Directional mode only). Kept after the axis
        // parts so PartIndex's PosX-relative arithmetic is unaffected.
        Light
    };

    struct NodeGeometry
    {
        float x = 0.0f;      // screen-space offset from the gizmo center, pixels
        float y = 0.0f;
        float depth = 0.0f;  // view-space z of the node, pixels; > 0 is toward the viewer
    };

    struct DrawGeometry
    {
        float centerX = 0.0f;
        float centerY = 0.0f;
        float outerRadius = 0.0f;  // white light ring
        float ballRadius = 0.0f;   // dark disc inside it
        float nodeRadius = 0.0f;
        float dotRadius = 0.0f;
        float stemWidth = 0.0f;
        NodeGeometry positive[3]{};  // +X, +Y, +Z tips
        NodeGeometry negative[3]{};  // -X, -Y, -Z tails
        Part hover = Part::None;
    };

    // Screen-space placement of the directional-light sun marker. While the
    // Directional lighting mode is active the renderer draws a small sun on
    // the gizmo's outer ring here. The position is a camera-relative compass
    // bearing: the light's horizontal direction is measured against the
    // camera's own ground-plane heading, so the marker always sweeps the full
    // circle as the light rotates, no matter how the view is tilted. `depth`
    // is the view-space z of the true 3D light direction (positive means the
    // light is on the viewer's side of the model).
    struct SunGeometry
    {
        bool visible = false;
        float x = 0.0f;      // screen-space offset from the gizmo center, pixels
        float y = 0.0f;
        float depth = 0.0f;
    };

    // Repositions and resizes the gizmo for the current viewport. Coordinates
    // are client pixels; the top-right placement sits below the title bar and
    // clear of the bottom bar. `viewportWidth` is already narrowed by the
    // caller to exclude the Information panel when it's open, so the gizmo
    // never sits underneath it.
    void UpdateLayout(int viewportWidth, int viewportHeight, int topInset, int bottomInset, float dpiScale);

    // 2.5D raycast hit-test. The pointer spawns a view-space ray against the
    // six axis-node spheres and the center ball; the intersection nearest to
    // the viewer wins, which reproduces the occlusion the painter's-algorithm
    // drawing shows. Axis stems are tested as 2D segments as a fallback.
    //
    // `lightRing` is set by the caller while Directional lighting is active.
    // It carves an annulus around the outer radius out of the ball as
    // Part::Light (the drag target for rotating the light) so grabbing the
    // white outline never starts a camera orbit; the inner disc stays Ball.
    // When false the whole disc behaves exactly as before.
    Part HitTest(DirectX::XMVECTOR cameraOrientation, float pointerX, float pointerY,
        bool lightRing = false) const;

    // Projects the world axes through the inverse camera rotation into
    // gizmo-local screen space for this frame's drawing.
    DrawGeometry ComputeDraw(DirectX::XMVECTOR cameraOrientation) const;

    // Places the directional-light sun marker on the outer ring for the
    // current camera. `directionalLightAngle` is the app's normalized 0..1
    // horizontal rotation. The marker is a compass bearing relative to the
    // camera's own ground-plane heading, so it can always sweep the full ring
    // (the true 3D light direction's view depth is still reported through
    // `depth`).
    SunGeometry ComputeSun(DirectX::XMVECTOR cameraOrientation, float directionalLightAngle) const;

    // Inverts ComputeSun: given a pointer on the light ring, returns (via
    // `angle`) the normalized 0..1 directional-light angle whose sun marker
    // lands under that pointer, so dragging the ring makes the sun follow the
    // cursor. `currentAngle` is unused by the compass mapping but kept for
    // symmetry with ComputeSun. Returns false only when the pointer is at the
    // ring's center, in which case the caller should leave the angle unchanged.
    bool LightAngleForPoint(DirectX::XMVECTOR cameraOrientation, float pointerX, float pointerY,
        float currentAngle, float& angle) const;

    // The outer ring's client-pixel center and radius, so the light-angle
    // accessibility fragment can anchor to the control the user sees rather
    // than to the removed bottom-bar slider.
    void RingBounds(float& centerX, float& centerY, float& radius) const;

    // The orthographic view the camera should animate to when `part` is used.
    ViewDir ViewFor(Part part) const;

    Part hover = Part::None;

private:
    float centerX_ = 0.0f;
    float centerY_ = 0.0f;
    float outer_ = 0.0f;
    float ball_ = 0.0f;
    float stemLength_ = 0.0f;
    float node_ = 0.0f;
    float dot_ = 0.0f;
    float stemWidth_ = 0.0f;
    float hitSlop_ = 0.0f;
    float ringBand_ = 0.0f;
};

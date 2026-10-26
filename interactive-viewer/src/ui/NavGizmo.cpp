#include "NavGizmo.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

using namespace DirectX;

namespace
{
// Logical (96-DPI) geometry in pixels. The dark ball disc and the white light
// ring are sized independently: the ring is the drag target for the light and
// floats a few pixels outside the disc, so growing it never changes the size
// or centering of the ball, stems, and nodes inside.
constexpr float kBallLogical = 48.0f;
constexpr float kOuterLogical = 52.0f;
constexpr float kStemLogical = 30.0f;
constexpr float kNodeLogical = 11.5f;
constexpr float kDotLogical = 5.5f;
constexpr float kStemWidthLogical = 2.4f;
constexpr float kCornerMarginLogical = 14.0f;
constexpr float kHitSlopLogical = 3.0f;
// Sun-marker reach (the outer tip of its rays), used as the light drag handle.
constexpr float kSunOuterLogical = 7.3f;
// Half-width of the Part::Light annulus straddling the outer ring. Wide
// enough to grab comfortably, narrow enough that the inner disc still reads
// as the orbit target.
constexpr float kLightRingBandLogical = 6.0f;

// Roll glyphs, shown only in a face view. The artwork is a single left-hand
// arc arrow traced from the reference ViewCube, filled solid white over its
// grey hairline outline. Each arrow is scaled about its own centre and set
// level on a 70-pixel orbit, so the left arrow points exactly left at the top
// of the circle and the clockwise arrow is its mirror, pointing exactly down
// at the right. Both stay the same distance from the light ring, with their
// tails split on the upper-right diagonal.
constexpr float kRollArrowScale = 0.8f;          // logical pixels per SVG unit
constexpr float kRollArrowCenterRadius = 70.0f;  // orbit radius, from the centre
constexpr float kRollArrowCenterAngle = 72.0f;   // degrees, counter-clockwise arrow
constexpr float kRollArrowPivotX = 23.69f;       // SVG bounding-box centre
constexpr float kRollArrowPivotY = 15.49f;
// The arrows' outermost point, head barbs included, is 78.8 pixels from the
// centre with the outline; the reserved disc rounds that up.
constexpr float kGlyphReserveLogical = 79.0f;
constexpr float kGlyphStrokeLogical = 1.4f;
constexpr float kGlyphHitSlopLogical = 3.0f;
// A cardinal view is within about 1.8 degrees of a world axis. This loose
// tolerance is only for single-axis snap decisions; the roll arrows use the
// exact face-view test below.
constexpr float kCardinalTolerance = 0.9995f;
// About 0.08 degrees: close enough that only an exact snap (or floating-point
// round-off in one) counts as a face view.
constexpr float kFaceViewTolerance = 0.999999f;

// The left arrow's closed outline in SVG units (x right, y down), flattened
// from the asset's two cubic segments. The renderer fills it white and strokes
// the same loop grey.
constexpr float kRollArrowOutline[][2] = {
    { 0.990f, 14.340f },  // head tip
    { 13.640f, 1.690f },  // outer barb
    { 13.640f, 7.440f },  // barb notch
    { 16.613f, 7.781f },
    { 19.597f, 8.179f },
    { 22.575f, 8.635f },
    { 25.531f, 9.149f },
    { 28.447f, 9.724f },
    { 31.309f, 10.359f },
    { 34.098f, 11.055f },
    { 36.799f, 11.814f },
    { 39.395f, 12.636f },
    { 41.870f, 13.522f },
    { 44.207f, 14.473f },
    { 46.390f, 15.490f },  // tail, outer corner
    { 42.390f, 29.290f },  // tail, inner corner
    { 40.353f, 28.418f },
    { 38.201f, 27.511f },
    { 35.950f, 26.587f },
    { 33.614f, 25.660f },
    { 31.208f, 24.748f },
    { 28.746f, 23.865f },
    { 26.244f, 23.028f },
    { 23.716f, 22.253f },
    { 21.177f, 21.556f },
    { 18.641f, 20.952f },
    { 16.124f, 20.458f },
    { 13.640f, 20.090f },  // inner barb notch
    { 13.640f, 26.990f },  // inner barb
};

void AxisDirections(XMVECTOR cameraOrientation, XMVECTOR (&axes)[3])
{
    // World-to-view rotation: the camera orientation maps camera space into
    // world space, so the conjugate maps the world axes into view space.
    const XMVECTOR inverse = XMQuaternionConjugate(XMQuaternionNormalize(cameraOrientation));
    axes[0] = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), inverse);
    axes[1] = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), inverse);
    axes[2] = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), inverse);
}

// The camera's heading as two orthonormal world-space ground directions:
// `right` (screen-right) and `forward` (where the camera faces, flattened to
// the ground plane). Picking whichever of the camera's forward/right axes is
// more horizontal keeps the frame defined when the view looks straight down
// or the camera is rolled.
void HorizontalCameraFrame(XMVECTOR cameraOrientation, XMVECTOR& right, XMVECTOR& forward)
{
    const XMVECTOR up = XMVectorSet(0, 0, 1, 0);
    const XMVECTOR q = XMQuaternionNormalize(cameraOrientation);
    const XMVECTOR cameraForward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), q);
    const XMVECTOR cameraRight = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), q);
    const XMVECTOR forwardFlat = XMVectorSubtract(cameraForward,
        XMVectorMultiply(up, XMVectorSplatZ(cameraForward)));
    const XMVECTOR rightFlat = XMVectorSubtract(cameraRight,
        XMVectorMultiply(up, XMVectorSplatZ(cameraRight)));
    if (XMVectorGetX(XMVector3LengthSq(forwardFlat)) >= XMVectorGetX(XMVector3LengthSq(rightFlat)))
    {
        forward = XMVector3Normalize(forwardFlat);
        right = XMVector3Normalize(XMVector3Cross(forward, up));
    }
    else
    {
        right = XMVector3Normalize(rightFlat);
        forward = XMVector3Normalize(XMVector3Cross(up, right));
    }
}

int PartIndex(int axis, bool positive)
{
    return static_cast<int>(NavGizmo::Part::PosX) + axis + (positive ? 0 : 3);
}

using RollGlyph = NavGizmo::DrawGeometry::RotationGlyph;

// Places the traced arrow at the current DPI: scale about its SVG bounding-box
// centre, then translate it onto its spot on the orbit. The drawing is level,
// so the counter-clockwise arrow faces exactly left and the clockwise one is
// its mirror, facing exactly down, with both the same distance from the ring.
template <size_t Count>
void BuildRollGlyph(const float (&outline)[Count][2], bool mirror, float scale, RollGlyph& glyph)
{
    const float angle = XMConvertToRadians(kRollArrowCenterAngle);
    const float centerX = kRollArrowCenterRadius * std::cos(angle) * scale;
    const float centerY = -kRollArrowCenterRadius * std::sin(angle) * scale;
    const float unit = kRollArrowScale * scale;
    glyph.pointCount = 0;
    for (const auto& point : outline)
    {
        const float placedX = centerX + (point[0] - kRollArrowPivotX) * unit;
        const float placedY = centerY + (point[1] - kRollArrowPivotY) * unit;
        glyph.points[glyph.pointCount++] = mirror
            ? DirectX::XMFLOAT2{ -placedY, -placedX }
            : DirectX::XMFLOAT2{ placedX, placedY };
    }
}

// Even-odd containment for the arrow's solid body. The outline is a simple
// closed polygon even though the barb notches make it concave.
bool PointInGlyph(const RollGlyph& glyph, float x, float y)
{
    bool inside = false;
    for (int i = 0, j = glyph.pointCount - 1; i < glyph.pointCount; j = i++)
    {
        const DirectX::XMFLOAT2 a = glyph.points[i];
        const DirectX::XMFLOAT2 b = glyph.points[j];
        if (((a.y > y) != (b.y > y)) &&
            (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x))
        {
            inside = !inside;
        }
    }
    return inside;
}

// Squared distance from a point to the outline, so the stroke and its slop
// still grab clicks that land just outside the drawn band.
float GlyphEdgeDistanceSq(const RollGlyph& glyph, float x, float y)
{
    float best = std::numeric_limits<float>::infinity();
    for (int i = 0, j = glyph.pointCount - 1; i < glyph.pointCount; j = i++)
    {
        const DirectX::XMFLOAT2 a = glyph.points[j];
        const DirectX::XMFLOAT2 b = glyph.points[i];
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float lengthSq = dx * dx + dy * dy;
        const float t = lengthSq > 0.0f
            ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / lengthSq, 0.0f, 1.0f)
            : 0.0f;
        const float px = x - (a.x + dx * t);
        const float py = y - (a.y + dy * t);
        best = std::min(best, px * px + py * py);
    }
    return best;
}
}

XMVECTOR OrientationFromForwardUp(XMVECTOR forward, XMVECTOR up)
{
    const XMVECTOR back = XMVector3Normalize(XMVectorNegate(forward));
    XMVECTOR right = XMVector3Cross(up, back);
    if (XMVectorGetX(XMVector3LengthSq(right)) < 1e-8f)
    {
        // Pole case: the requested up is parallel to the view axis. Fall back
        // to the next world axis so the basis stays orthonormal.
        const XMVECTOR alternateUp = XMVectorSet(0, 1, 0, 0);
        right = XMVector3Cross(alternateUp, back);
        if (XMVectorGetX(XMVector3LengthSq(right)) < 1e-8f) right = XMVectorSet(1, 0, 0, 0);
        up = alternateUp;
    }
    right = XMVector3Normalize(right);
    const XMVECTOR trueUp = XMVector3Normalize(XMVector3Cross(back, right));
    // XMMATRIX rows are the images of the camera basis vectors under the
    // rotation (DirectXMath row-vector convention): camera +x -> world right,
    // +y -> world up, +z -> world back.
    const XMMATRIX basis(right, trueUp, back, XMVectorSet(0, 0, 0, 1));
    return XMQuaternionNormalize(XMQuaternionRotationMatrix(basis));
}

XMVECTOR CanonicalViewOrientation(ViewDir view)
{
    // Y and Z swap roles from the app's old Y-up convention: X stays
    // Left/Right, Y now takes Front/Back (was Z), and Z now takes Top/Bottom
    // (was Y), preserving the original invariant that forward = -axis and
    // the eye ends up on the +axis side for every ViewDir.
    const XMVECTOR worldUp = XMVectorSet(0, 0, 1, 0);
    const XMVECTOR worldBack = XMVectorSet(0, -1, 0, 0);
    switch (view)
    {
    case ViewDir::Front: return OrientationFromForwardUp(XMVectorSet(0, 1, 0, 0), worldUp);
    case ViewDir::Back: return OrientationFromForwardUp(worldBack, worldUp);
    case ViewDir::Right: return OrientationFromForwardUp(XMVectorSet(-1, 0, 0, 0), worldUp);
    case ViewDir::Left: return OrientationFromForwardUp(XMVectorSet(1, 0, 0, 0), worldUp);
    // Top keeps +Y up so the top view matches the conventional X-right /
    // Y-up framing (and the ViewCube screenshots); Bottom mirrors it.
    case ViewDir::Top: return OrientationFromForwardUp(XMVectorSet(0, 0, -1, 0), XMVectorSet(0, 1, 0, 0));
    case ViewDir::Bottom: return OrientationFromForwardUp(XMVectorSet(0, 0, 1, 0), XMVectorSet(0, -1, 0, 0));
    }
    return XMQuaternionIdentity();
}

bool IsCardinalView(XMVECTOR orientation)
{
    const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0),
        XMQuaternionNormalize(orientation));
    const float ax = std::abs(XMVectorGetX(forward));
    const float ay = std::abs(XMVectorGetY(forward));
    const float az = std::abs(XMVectorGetZ(forward));
    return std::max(ax, std::max(ay, az)) >= kCardinalTolerance;
}

bool IsFaceView(XMVECTOR orientation)
{
    const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0),
        XMQuaternionNormalize(orientation));
    const float ax = std::abs(XMVectorGetX(forward));
    const float ay = std::abs(XMVectorGetY(forward));
    const float az = std::abs(XMVectorGetZ(forward));
    return std::max(ax, std::max(ay, az)) >= kFaceViewTolerance;
}

XMVECTOR ViewOrientationFrom(XMVECTOR currentOrientation, ViewDir view)
{
    const XMVECTOR canonical = CanonicalViewOrientation(view);
    if (!IsCardinalView(currentOrientation)) return canonical;

    const XMVECTOR current = XMQuaternionNormalize(currentOrientation);
    const XMVECTOR currentForward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), current);
    const XMVECTOR targetForward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), canonical);
    const float alignment = XMVectorGetX(XMVector3Dot(currentForward, targetForward));

    if (alignment > 0.9999f)
    {
        // Already looking at the requested face. Settling any roll difference
        // is a single rotation about the view axis.
        return canonical;
    }

    XMVECTOR axis;
    float angle;
    if (alignment < -0.9999f)
    {
        // Opposite face: a 180-degree turn about X flips top/bottom, while a
        // turn about Z flips the side views. Both keep the canonical upright
        // framing and still use one axis.
        axis = std::abs(XMVectorGetZ(currentForward)) > 0.9999f
            ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 0, 1, 0);
        angle = XM_PI;
    }
    else
    {
        // Adjacent face: the cross product is one world axis, and a +90-degree
        // turn about it maps the current forward direction onto the target.
        axis = XMVector3Normalize(XMVector3Cross(currentForward, targetForward));
        angle = XM_PIDIV2;
    }
    return XMQuaternionNormalize(XMQuaternionMultiply(
        current, XMQuaternionRotationAxis(axis, angle)));
}

XMVECTOR TurnViewOrientation(XMVECTOR orientation, ViewTurn turn)
{
    // The view axis is camera +z (the back of the camera). Rolling about it by
    // +90 degrees turns the world content clockwise on screen while leaving
    // the face being looked at unchanged.
    const XMVECTOR base = XMQuaternionNormalize(orientation);
    XMVECTOR axis = XMVectorSet(0, 0, 1, 0);
    float angle = 0.0f;
    switch (turn)
    {
    case ViewTurn::RollCounterClockwise: axis = XMVectorSet(0, 0, 1, 0); angle = -XM_PIDIV2; break;
    case ViewTurn::RollClockwise: axis = XMVectorSet(0, 0, 1, 0); angle = XM_PIDIV2; break;
    }
    // XMQuaternionMultiply(Q1, Q2) concatenates Q1 then Q2, so passing the
    // local rotation first applies it before the camera's world orientation.
    return XMQuaternionNormalize(XMQuaternionMultiply(
        XMQuaternionRotationAxis(axis, angle), base));
}

void NavGizmo::UpdateLayout(int viewportWidth, int viewportHeight, int topInset, int bottomInset, float dpiScale)
{
    const float scale = std::max(0.75f, dpiScale);
    ball_ = kBallLogical * scale;
    outer_ = kOuterLogical * scale;
    stemLength_ = kStemLogical * scale;
    node_ = kNodeLogical * scale;
    dot_ = kDotLogical * scale;
    stemWidth_ = kStemWidthLogical * scale;
    hitSlop_ = kHitSlopLogical * scale;
    ringBand_ = kLightRingBandLogical * scale;
    sun_ = kSunOuterLogical * scale;
    glyphReserve_ = kGlyphReserveLogical * scale;
    glyphStroke_ = kGlyphStrokeLogical * scale;
    glyphHitSlop_ = kGlyphHitSlopLogical * scale;
    BuildRollGlyph(kRollArrowOutline, false, scale, rollCounterClockwiseGlyph_);
    BuildRollGlyph(kRollArrowOutline, true, scale, rollClockwiseGlyph_);
    const float margin = kCornerMarginLogical * scale;
    const float width = std::max(1.0f, static_cast<float>(viewportWidth));
    const float top = static_cast<float>(topInset);
    const float bottom = std::max(top + 1.0f, static_cast<float>(viewportHeight) - static_cast<float>(bottomInset));
    // Reserve room for the outer rotation glyphs, not just the light ring, so
    // the new controls never clip against the viewport edge. On a viewport too
    // narrow for the full footprint, keep the ball on screen and let the empty
    // part of the reserved circle fall off the left edge instead of clipping
    // the arrows.
    const float reserved = std::max(outer_, glyphReserve_);
    centerX_ = std::max(margin + outer_, width - margin - reserved);
    // Keep the gizmo inside short viewports rather than overlapping the
    // status area, falling back to the top-bar/bottom-bar edges if space
    // runs out.
    const float highest = top + margin + reserved;
    const float lowest = std::max(top + reserved, bottom - margin - reserved);
    centerY_ = std::min(highest, lowest);
    hover = Part::None;
}

NavGizmo::Part NavGizmo::GlyphAt(float gx, float gy, XMVECTOR cameraOrientation) const
{
    if (!IsFaceView(cameraOrientation)) return Part::None;

    // The arrows are solid closed outlines, so any point inside either one is
    // a hit; a small slop around the stroked edge makes fine outlines easy to
    // grab without turning the surrounding empty space into a control.
    const float edgeSlop = glyphStroke_ * 0.5f + glyphHitSlop_;
    const float edgeSlopSq = edgeSlop * edgeSlop;
    const Part parts[] = { Part::RollCounterClockwise, Part::RollClockwise };
    const RollGlyph* glyphs[] = { &rollCounterClockwiseGlyph_, &rollClockwiseGlyph_ };
    for (int i = 0; i < 2; ++i)
    {
        if (glyphs[i]->pointCount < 3) continue;
        if (PointInGlyph(*glyphs[i], gx, gy)) return parts[i];
        if (GlyphEdgeDistanceSq(*glyphs[i], gx, gy) <= edgeSlopSq) return parts[i];
    }
    return Part::None;
}

NavGizmo::Part NavGizmo::HitTest(XMVECTOR cameraOrientation, float pointerX, float pointerY,
    bool lightRing, float directionalLightAngle, float directionalLightElevation) const
{
    const float gx = pointerX - centerX_;
    const float gy = pointerY - centerY_;
    const float distanceSq = gx * gx + gy * gy;
    const float ringReach = outer_ + (lightRing ? std::max(hitSlop_, ringBand_) : hitSlop_);
    const float reach = std::max(ringReach, glyphReserve_ + glyphHitSlop_);
    if (distanceSq > reach * reach) return Part::None;

    // The sun marker is its own drag handle wherever elevation has moved it to,
    // so pulling it toward the center to raise the light is a direct grab rather
    // than a ring-only gesture. It wins over an axis node it may overlap because
    // it is the only handle for the light.
    if (lightRing)
    {
        const SunGeometry sun = ComputeSun(cameraOrientation, directionalLightAngle, directionalLightElevation);
        const float sunDx = gx - sun.x;
        const float sunDy = gy - sun.y;
        const float sunReach = sun_ + hitSlop_;
        if (sunDx * sunDx + sunDy * sunDy <= sunReach * sunReach) return Part::Light;
    }

    // Rotation glyphs win over the ball and the nodes so their whole body is
    // grabbable; the sun above wins over both.
    const Part glyph = GlyphAt(gx, gy, cameraOrientation);
    if (glyph != Part::None) return glyph;

    XMVECTOR axes[3]{};
    AxisDirections(cameraOrientation, axes);

    // Orthographic view ray: origin (gx, gy, eyeZ), direction (0, 0, -1).
    // Sphere entry t = eyeZ - cz - sqrt(r^2 - dx^2 - dy^2); smallest t wins.
    const float eyeZ = outer_ * 1.5f;
    float bestT = std::numeric_limits<float>::infinity();
    Part best = Part::None;

    for (int axis = 0; axis < 3; ++axis)
    {
        const float ax = XMVectorGetX(axes[axis]);
        const float ay = XMVectorGetY(axes[axis]);
        const float az = XMVectorGetZ(axes[axis]);
        for (int side = 0; side < 2; ++side)
        {
            const float sign = side == 0 ? 1.0f : -1.0f;
            // Screen y grows downward, so the view-space y axis flips.
            const float sx = ax * sign * stemLength_;
            const float sy = -ay * sign * stemLength_;
            const float sz = az * sign * stemLength_;
            const float radius = (side == 0 ? node_ : dot_) + hitSlop_;
            const float dx = gx - sx;
            const float dy = gy - sy;
            const float squared = radius * radius - dx * dx - dy * dy;
            if (squared < 0.0f) continue;
            const float t = eyeZ - sz - std::sqrt(squared);
            if (t < bestT)
            {
                bestT = t;
                best = static_cast<Part>(PartIndex(axis, side == 0));
            }
        }
    }

    // A node/dot hit always wins over the ball: the renderer (DrawGizmo)
    // always paints every node on top of the ball's flat disc regardless of
    // depth, so the ball must never out-rank a node here either. Arbitrating
    // ball-vs-node by the same painter's-algorithm depth as the node-vs-node
    // comparison above (as this used to do) is wrong: the ball is a full
    // sphere of radius `outer_`, which bulges toward the viewer enough at a
    // node's screen offset to beat any node whose axis is closer to
    // edge-on (small view-space depth) — even though that node is clearly
    // visible and unoccluded on screen. That mismatch made clicks on
    // legitimately-visible nodes (e.g. an axis near-perpendicular to the
    // current view) silently grab the ball instead of snapping the view.
    if (best != Part::None) return best;

    const float radialDistance = std::sqrt(distanceSq);
    // The outer light ring owns an annulus straddling the white outline, so
    // grabbing the outline rotates the light instead of orbiting the camera.
    // The inner disc below stays the orbit target.
    if (lightRing && std::abs(radialDistance - outer_) <= ringBand_) return Part::Light;

    // The ball: drag anywhere else on the disc to orbit.
    {
        const float radius = ball_;
        const float squared = radius * radius - gx * gx - gy * gy;
        if (squared >= 0.0f) return Part::Ball;
    }

    // Axis stems as 2D segments from the center to each positive tip.
    float bestDistance = std::numeric_limits<float>::infinity();
    Part stem = Part::None;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float tipX = XMVectorGetX(axes[axis]) * stemLength_;
        const float tipY = -XMVectorGetY(axes[axis]) * stemLength_;
        const float lengthSq = tipX * tipX + tipY * tipY;
        const float t = lengthSq > 0.0f
            ? std::clamp((gx * tipX + gy * tipY) / lengthSq, 0.0f, 1.0f)
            : 0.0f;
        const float px = gx - tipX * t;
        const float py = gy - tipY * t;
        const float distance = px * px + py * py;
        if (distance < bestDistance)
        {
            bestDistance = distance;
            stem = static_cast<Part>(PartIndex(axis, true));
        }
    }
    const float stemReach = stemWidth_ * 2.0f + hitSlop_;
    if (stem != Part::None && bestDistance <= stemReach * stemReach) return stem;
    return Part::None;
}

NavGizmo::DrawGeometry NavGizmo::ComputeDraw(XMVECTOR cameraOrientation) const
{
    DrawGeometry geometry;
    geometry.centerX = centerX_;
    geometry.centerY = centerY_;
    geometry.outerRadius = outer_;
    geometry.ballRadius = ball_;
    geometry.nodeRadius = node_;
    geometry.dotRadius = dot_;
    geometry.stemWidth = stemWidth_;
    geometry.glyphStrokeWidth = glyphStroke_;
    geometry.glyphReserveRadius = glyphReserve_;
    geometry.rollCounterClockwise = rollCounterClockwiseGlyph_;
    geometry.rollClockwise = rollClockwiseGlyph_;
    geometry.showRotationControls = IsFaceView(cameraOrientation);
    geometry.hover = hover;

    XMVECTOR axes[3]{};
    AxisDirections(cameraOrientation, axes);
    for (int axis = 0; axis < 3; ++axis)
    {
        const float ax = XMVectorGetX(axes[axis]);
        const float ay = XMVectorGetY(axes[axis]);
        const float az = XMVectorGetZ(axes[axis]);
        geometry.positive[axis] = { ax * stemLength_, -ay * stemLength_, az * stemLength_ };
        geometry.negative[axis] = { -ax * stemLength_, ay * stemLength_, -az * stemLength_ };
    }
    return geometry;
}

NavGizmo::SunGeometry NavGizmo::ComputeSun(XMVECTOR cameraOrientation, float directionalLightAngle,
    float directionalLightElevation) const
{
    SunGeometry sun;
    const float azimuth = directionalLightAngle * XM_2PI;
    const float elevation = std::clamp(directionalLightElevation, 0.0f, 1.4835f);
    const float cosine = std::cos(azimuth);
    const float sine = std::sin(azimuth);

    // Compass bearing: measure the light's horizontal direction against the
    // camera's own ground-plane heading rather than projecting the 3D vector.
    // The projection collapses to a line whenever the view is roughly level
    // with the light's rotation plane (which is the common side-on view), and
    // then the sun could never reach every angle. A yaw-only frame always
    // spans the full ring for any camera pitch or roll.
    XMVECTOR right = XMVectorZero();
    XMVECTOR forward = XMVectorZero();
    HorizontalCameraFrame(cameraOrientation, right, forward);
    const XMVECTOR light = XMVectorSet(cosine, sine, 0.0f, 0.0f);
    const float along = XMVectorGetX(XMVector3Dot(light, right));   // screen +x
    const float ahead = XMVectorGetX(XMVector3Dot(light, forward)); // screen -y

    // Elevation is encoded radially: on the horizon the sun rides the outer
    // ring, and it converges on the center as it climbs overhead. The bearing
    // still covers every angle because it comes from the yaw-only frame above.
    const float radius = outer_ * std::cos(elevation);
    sun.x = along * radius;
    sun.y = -ahead * radius;

    // Keep a true 3D depth so the renderer can tell when the light is on the
    // far side of the model.
    const XMVECTOR light3D = XMVector3Normalize(XMVectorSet(
        cosine * std::cos(elevation), sine * std::cos(elevation), std::sin(elevation), 0.0f));
    const XMVECTOR inverse = XMQuaternionConjugate(XMQuaternionNormalize(cameraOrientation));
    sun.depth = XMVectorGetZ(XMVector3Rotate(light3D, inverse));
    sun.visible = true;
    return sun;
}

bool NavGizmo::LightAngleForPoint(XMVECTOR cameraOrientation, float pointerX, float pointerY,
    float /*currentAngle*/, float& angle) const
{
    const float gx = pointerX - centerX_;
    const float gy = pointerY - centerY_;
    const float length = std::sqrt(gx * gx + gy * gy);
    if (length < 1e-3f) return false;

    XMVECTOR right = XMVectorZero();
    XMVECTOR forward = XMVectorZero();
    HorizontalCameraFrame(cameraOrientation, right, forward);

    // Undo ComputeSun's screen mapping (screen +x is `right`, screen -y is
    // `ahead`) and read the light's world azimuth back off the ground frame.
    const float along = gx / length;
    const float ahead = -gy / length;
    const XMVECTOR light = XMVectorAdd(XMVectorScale(right, along), XMVectorScale(forward, ahead));

    float normalized = std::atan2(XMVectorGetY(light), XMVectorGetX(light)) / XM_2PI;
    normalized -= std::floor(normalized);
    angle = normalized;
    return true;
}

bool NavGizmo::LightElevationForPoint(float pointerX, float pointerY, float& elevation) const
{
    if (outer_ <= 0.0f) return false;
    const float gx = pointerX - centerX_;
    const float gy = pointerY - centerY_;
    const float radial = std::sqrt(gx * gx + gy * gy);
    // Inverse of ComputeSun's radius = outer * cos(elevation).
    elevation = std::acos(std::clamp(radial / outer_, 0.0f, 1.0f));
    return true;
}

void NavGizmo::RingBounds(float& centerX, float& centerY, float& radius) const
{
    centerX = centerX_;
    centerY = centerY_;
    radius = outer_;
}

ViewDir NavGizmo::ViewFor(Part part) const
{
    // Clicking a node snaps the camera to that side of the model: the +X node
    // parks the eye on +X looking down -X (Right), and so on. Z is this
    // app's up axis, so +Z/-Z now map to Top/Bottom (Y took over Front/Back).
    switch (part)
    {
    case Part::PosX: return ViewDir::Right;
    case Part::NegX: return ViewDir::Left;
    case Part::PosY: return ViewDir::Back;
    case Part::NegY: return ViewDir::Front;
    case Part::PosZ: return ViewDir::Top;
    case Part::NegZ: return ViewDir::Bottom;
    default: return ViewDir::Front;
    }
}

bool NavGizmo::TurnFor(Part part, ViewTurn& turn)
{
    switch (part)
    {
    case Part::RollCounterClockwise: turn = ViewTurn::RollCounterClockwise; return true;
    case Part::RollClockwise: turn = ViewTurn::RollClockwise; return true;
    default: return false;
    }
}

#include "framework.h"
#include "Renderer.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <sstream>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
XMVECTOR LoadOrientation(const Camera& camera)
{
    return XMQuaternionNormalize(XMLoadFloat4(&camera.orientation));
}

ComPtr<ID3DBlob> CompileShader(const char* source, const char* entry, const char* target, std::wstring& error)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> messages;
    const HRESULT result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target,
        flags, 0, &shader, &messages);
    if (FAILED(result))
    {
        error = L"The built-in graphics shader could not be compiled.";
        if (messages)
        {
            const char* text = static_cast<const char*>(messages->GetBufferPointer());
            const int needed = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(messages->GetBufferSize()), nullptr, 0);
            if (needed > 0)
            {
                std::wstring details(needed, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(messages->GetBufferSize()), details.data(), needed);
                error += L" " + details;
            }
        }
        return nullptr;
    }
    return shader;
}

const char* kVertexShader = R"(
cbuffer Frame : register(b0)
{
    row_major float4x4 viewProjection;
    row_major float4x4 modelTransform;
    float4 cameraPosition;
    float4 options;
};

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 color : COLOR;
};

VertexOutput main(VertexInput input)
{
    VertexOutput output;
    float4 worldPos = mul(float4(input.position, 1.0), modelTransform);
    output.position = mul(worldPos, viewProjection);
    output.worldPosition = worldPos.xyz;
    // modelTransform is always a pure rotation (identity or a fixed axis
    // correction, never scale/shear), so the raw 3x3 part transforms
    // normals exactly without an inverse-transpose.
    output.normal = mul(input.normal, (float3x3)modelTransform);
    output.color = input.color;
    return output;
}
)";

const char* kPixelShader = R"(
cbuffer Frame : register(b0)
{
    row_major float4x4 viewProjection;
    row_major float4x4 modelTransform;
    float4 cameraPosition;
    float4 options;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 color : COLOR;
};

float4 main(PixelInput input) : SV_TARGET
{
    float3 normal = normalize(input.normal);
    float3 viewDirection = normalize(cameraPosition.xyz - input.worldPosition);
    float3 key = normalize(float3(-0.45, -0.35, 0.82));
    float3 fill = normalize(float3(0.65, 0.70, 0.25));
    float keyLight = saturate(dot(normal, key));
    float fillLight = saturate(dot(normal, fill));
    float hemisphere = lerp(0.18, 0.42, normal.z * 0.5 + 0.5);
    float rim = pow(1.0 - saturate(dot(normal, viewDirection)), 3.0) * 0.12;
    float3 lit = input.color.rgb * (hemisphere + keyLight * 0.72 + fillLight * 0.18) + rim;
    // Mesh-selection highlight: a cool fresnel lift driven by options.x.
    float outline = pow(1.0 - saturate(dot(normal, viewDirection)), 2.0);
    lit += options.x * (outline * 0.45 * float3(0.36, 0.62, 1.0) + 0.03);
    return float4(lit, input.color.a);
}
)";

struct FrameConstants
{
    XMFLOAT4X4 viewProjection{};
    XMFLOAT4X4 modelTransform{};
    XMFLOAT4 cameraPosition{};
    XMFLOAT4 options{};
};
}

namespace
{
constexpr float kOrbitPixelsToRadians = 0.008f;
constexpr float kLookPixelsToRadians = 0.006f;
constexpr double kFlightAccelSeconds = 0.13;
constexpr double kRollAccelSeconds = 0.10;
constexpr double kRollBaseSpeed = 1.2;
constexpr double kBoostMultiplier = 2.0;
constexpr double kInertiaDecaySeconds = 0.5;
constexpr double kInertiaMinPixelsPerSecond = 12.0;
constexpr double kInertiaMaxPixelsPerSecond = 420.0;
constexpr double kZoomEaseSeconds = 0.09;
constexpr double kPivotAnimSeconds = 0.5;
constexpr double kOrientationAnimSeconds = 0.55;
// Perspective/orthographic cross-fade duration.
constexpr double kProjectionBlendSeconds = 0.16;
// Ctrl+MMB drag: log-distance change per dragged pixel.
constexpr double kDollyDragPerPixel = 0.0045;
// Orbit and look pitch clamp: |forward . worldUp| may not exceed this, which
// keeps the turntable and freelook from crossing the up-axis pole and
// flipping the horizon. Yaw is never restricted.
constexpr float kPitchPoleLimit = 0.99998f;
constexpr double kFlySpeedMin = 0.05;
constexpr double kFlySpeedMax = 40.0;

double EaseFactor(double deltaSeconds, double tau)
{
    return deltaSeconds > 0.0 ? 1.0 - std::exp(-deltaSeconds / tau) : 0.0;
}

XMFLOAT4 DefaultOrientation()
{
    // Home view direction (eye - pivot, normalized), captured from a manually
    // tuned camera placement and reproduced exactly rather than composed from
    // round azimuth/elevation angles: +X, -Y and +Z, looking back toward
    // -X/+Y/-Z. The eye is always pivot + rotate((0, 0, distance), q), so
    // this offset is exactly the local +Z axis in world space; the rest of
    // the basis (right/up) is filled in via cross products against the
    // world-up axis, which reproduces the captured placement exactly (it was
    // roll-free — its up vector already matched world-up x back).
    const XMVECTOR worldUp = XMVectorSet(0, 0, 1, 0);
    const XMVECTOR back = XMVector3Normalize(XMVectorSet(0.423293f, -0.83207f, 0.358444f, 0.0f));
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, back));
    const XMVECTOR up = XMVector3Cross(back, right);
    const XMMATRIX basis(right, up, back, XMVectorSet(0, 0, 0, 1));
    XMFLOAT4 result{};
    XMStoreFloat4(&result, XMQuaternionNormalize(XMQuaternionRotationMatrix(basis)));
    return result;
}
}

void Camera::SetBounds(const XMFLOAT3& minimum, const XMFLOAT3& maximum, float aspect)
{
    homeX = (static_cast<double>(minimum.x) + maximum.x) * 0.5;
    homeY = (static_cast<double>(minimum.y) + maximum.y) * 0.5;
    homeZ = (static_cast<double>(minimum.z) + maximum.z) * 0.5;
    const double extentX = static_cast<double>(maximum.x) - minimum.x;
    const double extentY = static_cast<double>(maximum.y) - minimum.y;
    const double extentZ = static_cast<double>(maximum.z) - minimum.z;
    sceneRadius = std::max(0.0001, std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ) * 0.5);
    targetX = homeX;
    targetY = homeY;
    targetZ = homeZ;
    homeOrientation = DefaultOrientation();
    orientation = homeOrientation;
    desiredOrientation = homeOrientation;
    homeBoundsMin = minimum;
    homeBoundsMax = maximum;
    distance = FitDistance(aspect);
    targetDistance = distance;
    homeDistance = distance;  // cached default framing for Reset View
    pivotAnimating = false;
    orientationAnimating = false;
    StopMotion();
}

double Camera::FitDistance(float aspect) const
{
    const double vertical = kVerticalFieldOfView;
    const double horizontal = 2.0 * std::atan(std::tan(vertical * 0.5) * std::max(0.1f, aspect));
    const double limitingFov = std::min(vertical, horizontal);
    const double value = sceneRadius / std::max(0.05, std::sin(limitingFov * 0.5)) / 0.93;
    return std::clamp(value, sceneRadius * 0.05, sceneRadius * 250.0);
}

void Camera::Fit(float aspect)
{
    FrameBox(homeBoundsMin, homeBoundsMax, aspect);
}

void Camera::FrameBox(const XMFLOAT3& minimum, const XMFLOAT3& maximum, float aspect)
{
    desiredX = (static_cast<double>(minimum.x) + maximum.x) * 0.5;
    desiredY = (static_cast<double>(minimum.y) + maximum.y) * 0.5;
    desiredZ = (static_cast<double>(minimum.z) + maximum.z) * 0.5;
    const double extentX = static_cast<double>(maximum.x) - minimum.x;
    const double extentY = static_cast<double>(maximum.y) - minimum.y;
    const double extentZ = static_cast<double>(maximum.z) - minimum.z;
    const double radius = std::max(0.0001, std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ) * 0.5);
    const double vertical = kVerticalFieldOfView;
    const double horizontal = 2.0 * std::atan(std::tan(vertical * 0.5) * std::max(0.1f, aspect));
    const double limiting = std::min(vertical, horizontal);
    targetDistance = std::clamp(radius / std::max(0.05, std::sin(limiting * 0.5)) / 0.93,
        radius * 0.05, radius * 250.0);
    pivotFromX = targetX;
    pivotFromY = targetY;
    pivotFromZ = targetZ;
    pivotAnimElapsed = 0.0;
    pivotAnimating = true;
    if (reduceMotion)
    {
        targetX = desiredX; targetY = desiredY; targetZ = desiredZ;
        distance = targetDistance;
        pivotAnimating = false;
    }
    CancelInertia();
}

void Camera::Reset(float aspect)
{
    Fit(aspect);
    desiredOrientation = homeOrientation;
    orientationFrom = orientation;
    orientationAnimElapsed = 0.0;
    orientationAnimating = true;
    if (reduceMotion)
    {
        orientation = desiredOrientation;
        orientationAnimating = false;
    }
    // Reset also restores the cached default projection mode.
    SetProjection(ProjectionMode::Perspective);
}

void Camera::SnapToView(XMVECTOR viewOrientation)
{
    XMStoreFloat4(&desiredOrientation, XMQuaternionNormalize(viewOrientation));
    orientationFrom = orientation;
    orientationAnimElapsed = 0.0;
    orientationAnimating = true;
    if (reduceMotion)
    {
        orientation = desiredOrientation;
        orientationAnimating = false;
    }
    CancelInertia();
}

void Camera::SetProjection(ProjectionMode mode)
{
    projection = mode;
    if (reduceMotion) projectionBlend = projection == ProjectionMode::Orthographic ? 1.0 : 0.0;
}

void Camera::SetFlySpeedScale(double scale)
{
    flySpeedScale = std::clamp(scale, kFlySpeedMin, kFlySpeedMax);
}

void Camera::ApplyOrbitAngles(float yawAngle, float pitchAngle)
{
    XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR worldUp = XMVectorSet(0, 0, 1, 0);
    // Turntable orbit: yaw about the world up axis, pitch about the camera
    // right axis. Composed as quaternion multiplies there is no Euler order
    // and no gimbal lock; the pole clamp below only keeps the horizon from
    // flipping when the view would cross straight over the top.
    current = XMQuaternionNormalize(XMQuaternionMultiply(current,
        XMQuaternionRotationAxis(worldUp, yawAngle)));
    if (pitchAngle != 0.0f)
    {
        const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current);
        const XMVECTOR pitched = XMQuaternionNormalize(XMQuaternionMultiply(current,
            XMQuaternionRotationAxis(right, pitchAngle)));
        const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), pitched);
        if (std::abs(XMVectorGetX(XMVector3Dot(forward, worldUp))) <= kPitchPoleLimit)
        {
            current = pitched;
        }
    }
    XMStoreFloat4(&orientation, current);
    orientationAnimating = false;
}

void Camera::Orbit(float deltaX, float deltaY)
{
    ApplyOrbitAngles(-deltaX * kOrbitPixelsToRadians, -deltaY * kOrbitPixelsToRadians);
}

void Camera::Look(float deltaX, float deltaY)
{
    XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR target = XMVectorSet(static_cast<float>(targetX), static_cast<float>(targetY),
        static_cast<float>(targetZ), 1.0f);
    const XMVECTOR eye = target + XMVector3Rotate(XMVectorSet(0, 0, static_cast<float>(distance), 0), current);

    const XMVECTOR worldUp = XMVectorSet(0, 0, 1, 0);
    // Yaw about world up, not the camera's local up: once the camera is
    // pitched, its local up axis is tilted, and yawing about it banks the
    // horizon instead of turning level. Unreal's editor free-look (and our
    // own orbit drag, see ApplyOrbitAngles) always yaws about world up so
    // the camera stays level to the ground plane through any pitch.
    const XMVECTOR yaw = XMQuaternionRotationAxis(worldUp, -deltaX * kLookPixelsToRadians);
    current = XMQuaternionNormalize(XMQuaternionMultiply(current, yaw));
    const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current);
    const XMVECTOR pitched = XMQuaternionNormalize(XMQuaternionMultiply(current,
        XMQuaternionRotationAxis(right, -deltaY * kLookPixelsToRadians)));
    const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), pitched);
    // Unreal-style freelook: yaw freely, but clamp pitch at the horizon pole.
    current = std::abs(XMVectorGetX(XMVector3Dot(forward, worldUp))) > kPitchPoleLimit
        ? current : pitched;

    const XMVECTOR newTarget = eye - XMVector3Rotate(
        XMVectorSet(0, 0, static_cast<float>(distance), 0), current);
    XMFLOAT3 targetValue{};
    XMStoreFloat3(&targetValue, newTarget);
    targetX = targetValue.x;
    targetY = targetValue.y;
    targetZ = targetValue.z;
    pivotAnimating = false;
    orientationAnimating = false;
    XMStoreFloat4(&orientation, current);
}

void Camera::AccumulateLook(float deltaX, float deltaY)
{
    pendingLookX += deltaX;
    pendingLookY += deltaY;
}

void Camera::ShiftPivot(double x, double y, double z)
{
    targetX += x;
    targetY += y;
    targetZ += z;
    if (pivotAnimating)
    {
        pivotFromX += x;
        pivotFromY += y;
        pivotFromZ += z;
        desiredX += x;
        desiredY += y;
        desiredZ += z;
    }
}

void Camera::Pan(float deltaX, float deltaY, float viewportHeight)
{
    if (viewportHeight <= 1.0f) return;
    if (viewportHeight > 1.0f) lastViewportHeight = viewportHeight;
    const XMVECTOR current = LoadOrientation(*this);
    XMFLOAT3 right{};
    XMFLOAT3 up{};
    XMStoreFloat3(&right, XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current));
    XMStoreFloat3(&up, XMVector3Rotate(XMVectorSet(0, 1, 0, 0), current));
    const double unitsPerPixel = 2.0 * distance * std::tan(kVerticalFieldOfView * 0.5) / viewportHeight;
    ShiftPivot((-right.x * deltaX + up.x * deltaY) * unitsPerPixel,
        (-right.y * deltaX + up.y * deltaY) * unitsPerPixel,
        (-right.z * deltaX + up.z * deltaY) * unitsPerPixel);
}

void Camera::Truck(float deltaX, float deltaY, float viewportHeight, bool axisSnap)
{
    if (viewportHeight <= 1.0f) return;
    lastViewportHeight = viewportHeight;
    const XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR worldUp = XMVectorSet(0, 0, 1, 0);
    const XMVECTOR forward3 = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), current);
    const XMVECTOR right3 = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current);
    XMFLOAT3 forwardValue{};
    XMFLOAT3 rightValue{};
    XMStoreFloat3(&forwardValue, forward3);
    XMStoreFloat3(&rightValue, right3);

    XMVECTOR flatForward = XMVectorSet(forwardValue.x, forwardValue.y, 0, 0);
    if (XMVectorGetX(XMVector3LengthSq(flatForward)) < 1e-6f)
    {
        // Looking straight up/down: right stays horizontal (pitch is applied
        // about it), so derive forward from it instead of leaving it
        // undefined, keeping truck direction continuous through the pole.
        flatForward = XMVector3Cross(worldUp, XMVectorSet(rightValue.x, rightValue.y, 0, 0));
    }
    flatForward = XMVector3Normalize(flatForward);
    XMVECTOR flatRight = XMVector3Normalize(XMVectorSet(rightValue.x, rightValue.y, 0, 0));
    if (axisSnap)
    {
        const float yaw = std::atan2(XMVectorGetX(flatForward), XMVectorGetY(flatForward));
        const float snapped = std::round(yaw / XM_PIDIV2) * XM_PIDIV2;
        flatForward = XMVectorSet(std::sin(snapped), std::cos(snapped), 0, 0);
        flatRight = XMVector3Cross(flatForward, worldUp);
    }

    XMFLOAT3 forwardFlat{};
    XMFLOAT3 rightFlat{};
    XMStoreFloat3(&forwardFlat, flatForward);
    XMStoreFloat3(&rightFlat, flatRight);
    const double unitsPerPixel = 2.0 * distance * std::tan(kVerticalFieldOfView * 0.5) / viewportHeight;
    ShiftPivot((-rightFlat.x * deltaX + forwardFlat.x * deltaY) * unitsPerPixel,
        (-rightFlat.y * deltaX + forwardFlat.y * deltaY) * unitsPerPixel, 0.0);
}

void Camera::Dolly(float wheelSteps)
{
    targetDistance *= std::exp(-static_cast<double>(wheelSteps) * 0.16);
    targetDistance = std::clamp(targetDistance, sceneRadius * 0.025, sceneRadius * 250.0);
}

void Camera::DollyDrag(float deltaY)
{
    // Ctrl+MMB drag: pull up to close in, push down to pull back. Exponential
    // in the drag distance, so the response feels proportional at any scale.
    targetDistance = std::clamp(targetDistance * std::exp(static_cast<double>(deltaY) * kDollyDragPerPixel),
        sceneRadius * 0.025, sceneRadius * 250.0);
}

void Camera::MoveLocal(float rightAmount, float upAmount, float forwardAmount)
{
    const XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current);
    const XMVECTOR up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), current);
    const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), current);
    XMFLOAT3 movement{};
    XMStoreFloat3(&movement, right * rightAmount + up * upAmount + forward * forwardAmount);
    ShiftPivot(movement.x, movement.y, movement.z);
}

void Camera::Roll(float radians)
{
    XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), current);
    const XMVECTOR roll = XMQuaternionRotationAxis(forward, radians);
    current = XMQuaternionNormalize(XMQuaternionMultiply(current, roll));
    XMStoreFloat4(&orientation, current);
    orientationAnimating = false;
}

void Camera::SetInput(const FlightInput& value)
{
    input = value;
}

void Camera::SeedOrbitInertia(float velocityX, float velocityY)
{
    inertiaX = std::clamp(static_cast<double>(velocityX), -kInertiaMaxPixelsPerSecond, kInertiaMaxPixelsPerSecond);
    inertiaY = std::clamp(static_cast<double>(velocityY), -kInertiaMaxPixelsPerSecond, kInertiaMaxPixelsPerSecond);
}

void Camera::SeedPanInertia(float velocityX, float velocityY)
{
    panInertiaX = std::clamp(static_cast<double>(velocityX), -kInertiaMaxPixelsPerSecond, kInertiaMaxPixelsPerSecond);
    panInertiaY = std::clamp(static_cast<double>(velocityY), -kInertiaMaxPixelsPerSecond, kInertiaMaxPixelsPerSecond);
}

void Camera::CancelInertia()
{
    inertiaX = 0.0;
    inertiaY = 0.0;
    panInertiaX = 0.0;
    panInertiaY = 0.0;
}

void Camera::StopMotion()
{
    velRight = 0.0;
    velUp = 0.0;
    velForward = 0.0;
    rollRate = 0.0;
    orbitRateX = 0.0;
    orbitRateY = 0.0;
    panRateX = 0.0;
    panRateY = 0.0;
    CancelInertia();
    // A stopped camera keeps its projection blend settled where it is; the
    // blend continues to ease toward the selected mode via Update().
}

double Camera::FlightSpeed() const
{
    // Fly at a rate that scales with the scene, and slow down as the camera
    // closes in on the subject for precision, faster when pulled back.
    const double base = std::max(sceneRadius * 1.25, 1e-9);
    const double zoomScale = std::clamp(distance / std::max(1e-9, sceneRadius * 2.0), 0.22, 2.8);
    return base * zoomScale * (input.fast ? kBoostMultiplier : 1.0) * flySpeedScale;
}

void Camera::Update(double deltaTime)
{
    // Apply any queued raw mouse-look first, so this tick's WASD translation
    // (below) moves along the orientation the player is looking at *now*
    // rather than the stale one from before this frame's mouse deltas.
    if (pendingLookX != 0.0 || pendingLookY != 0.0)
    {
        Look(static_cast<float>(pendingLookX), static_cast<float>(pendingLookY));
        pendingLookX = 0.0;
        pendingLookY = 0.0;
    }

    if (reduceMotion)
    {
        CancelInertia();
        projectionBlend = projection == ProjectionMode::Orthographic ? 1.0 : 0.0;
        distance = targetDistance;
        if (pivotAnimating) { targetX=desiredX; targetY=desiredY; targetZ=desiredZ; pivotAnimating=false; }
        if (orientationAnimating) { orientation=desiredOrientation; orientationAnimating=false; }
    }

    const double flightEase = EaseFactor(deltaTime, kFlightAccelSeconds);

    // Smoothed keyboard flight: velocity eases toward the intent instead of
    // toggling, so motion ramps up on press and settles on release.
    double directionRight = input.right;
    double directionUp = input.up;
    double directionForward = input.forward;
    const double directionLength = std::sqrt(directionRight * directionRight + directionUp * directionUp +
        directionForward * directionForward);
    if (directionLength > 1.0)
    {
        directionRight /= directionLength;
        directionUp /= directionLength;
        directionForward /= directionLength;
    }
    const double speed = FlightSpeed();
    const double stopEpsilon = std::max(speed * 0.01, 1e-9);
    velRight += (directionRight * speed - velRight) * flightEase;
    velUp += (directionUp * speed - velUp) * flightEase;
    velForward += (directionForward * speed - velForward) * flightEase;
    if (std::abs(velRight) < stopEpsilon) velRight = 0.0;
    if (std::abs(velUp) < stopEpsilon) velUp = 0.0;
    if (std::abs(velForward) < stopEpsilon) velForward = 0.0;
    if (velRight != 0.0 || velUp != 0.0 || velForward != 0.0)
    {
        MoveLocal(static_cast<float>(velRight * deltaTime), static_cast<float>(velUp * deltaTime),
            static_cast<float>(velForward * deltaTime));
    }

    const double rollTarget = input.roll * kRollBaseSpeed * (input.fast ? 2.0 : 1.0);
    rollRate += (rollTarget - rollRate) * EaseFactor(deltaTime, kRollAccelSeconds);
    if (std::abs(rollRate) < 0.01) rollRate = 0.0;
    if (rollRate != 0.0) Roll(static_cast<float>(rollRate * deltaTime));

    // Continuous, eased arrow-key orbit and Shift+arrow pan.
    orbitRateX += (static_cast<double>(input.orbitX) - orbitRateX) * flightEase;
    orbitRateY += (static_cast<double>(input.orbitY) - orbitRateY) * flightEase;
    if (std::abs(orbitRateX) < 25.0) orbitRateX = 0.0;
    if (std::abs(orbitRateY) < 25.0) orbitRateY = 0.0;
    if (orbitRateX != 0.0 || orbitRateY != 0.0)
    {
        ApplyOrbitAngles(-static_cast<float>(orbitRateX * deltaTime) * kOrbitPixelsToRadians,
            -static_cast<float>(orbitRateY * deltaTime) * kOrbitPixelsToRadians);
    }

    panRateX += (static_cast<double>(input.panX) - panRateX) * flightEase;
    panRateY += (static_cast<double>(input.panY) - panRateY) * flightEase;
    if (std::abs(panRateX) < 25.0) panRateX = 0.0;
    if (std::abs(panRateY) < 25.0) panRateY = 0.0;
    if (panRateX != 0.0 || panRateY != 0.0)
    {
        Truck(static_cast<float>(panRateX * deltaTime), static_cast<float>(panRateY * deltaTime), input.viewportHeight,
            /*axisSnap=*/false);
    }

    // Post-drag orbit inertia.
    if (inertiaX != 0.0 || inertiaY != 0.0)
    {
        ApplyOrbitAngles(-static_cast<float>(inertiaX * deltaTime) * kOrbitPixelsToRadians,
            -static_cast<float>(inertiaY * deltaTime) * kOrbitPixelsToRadians);
        const double decay = deltaTime > 0.0 ? std::exp(-deltaTime / kInertiaDecaySeconds) : 0.0;
        inertiaX *= decay;
        inertiaY *= decay;
        if (std::abs(inertiaX) < kInertiaMinPixelsPerSecond && std::abs(inertiaY) < kInertiaMinPixelsPerSecond)
        {
            CancelInertia();
        }
    }

    // Post-drag pan inertia, exponentially damped the same way.
    if (panInertiaX != 0.0 || panInertiaY != 0.0)
    {
        const double height = lastViewportHeight;
        if (height > 1.0)
        {
            Truck(static_cast<float>(panInertiaX * deltaTime), static_cast<float>(panInertiaY * deltaTime),
                static_cast<float>(height), /*axisSnap=*/false);
        }
        const double decay = deltaTime > 0.0 ? std::exp(-deltaTime / kInertiaDecaySeconds) : 0.0;
        panInertiaX *= decay;
        panInertiaY *= decay;
        if (std::abs(panInertiaX) < kInertiaMinPixelsPerSecond && std::abs(panInertiaY) < kInertiaMinPixelsPerSecond)
        {
            panInertiaX = 0.0;
            panInertiaY = 0.0;
        }
    }

    // Perspective <-> orthographic cross-fade. Blending the two projection
    // matrices per element reads as a short dolly-zoom instead of a hard cut.
    {
        const double projectionTarget = projection == ProjectionMode::Orthographic ? 1.0 : 0.0;
        if (projectionBlend != projectionTarget)
        {
            projectionBlend += (projectionTarget - projectionBlend) * EaseFactor(deltaTime, kProjectionBlendSeconds);
            if (std::abs(projectionBlend - projectionTarget) < 0.002) projectionBlend = projectionTarget;
        }
    }

    // Eased wheel zoom toward the target distance.
    if (distance != targetDistance)
    {
        distance += (targetDistance - distance) * EaseFactor(deltaTime, kZoomEaseSeconds);
        if (std::fabs(std::log(distance / targetDistance)) < 1e-3) distance = targetDistance;
    }

    // Fit glide: ease the orbit pivot back to the model center.
    if (pivotAnimating)
    {
        pivotAnimElapsed += deltaTime;
        const double t = std::min(1.0, pivotAnimElapsed / kPivotAnimSeconds);
        const double smooth = t * t * (3.0 - 2.0 * t);
        targetX = pivotFromX + (desiredX - pivotFromX) * smooth;
        targetY = pivotFromY + (desiredY - pivotFromY) * smooth;
        targetZ = pivotFromZ + (desiredZ - pivotFromZ) * smooth;
        if (t >= 1.0)
        {
            targetX = desiredX;
            targetY = desiredY;
            targetZ = desiredZ;
            pivotAnimating = false;
        }
    }

    // Reset glide: slerp the orientation back to the home view.
    if (orientationAnimating)
    {
        orientationAnimElapsed += deltaTime;
        const double t = std::min(1.0, orientationAnimElapsed / kOrientationAnimSeconds);
        const float smooth = static_cast<float>(t * t * (3.0 - 2.0 * t));
        const XMVECTOR from = XMLoadFloat4(&orientationFrom);
        const XMVECTOR to = XMQuaternionNormalize(XMLoadFloat4(&desiredOrientation));
        // Take the shortest arc: flip the target when its sign points away.
        const float alignment = XMVectorGetX(XMVector4Dot(from, to));
        const XMVECTOR shortest = alignment < 0.0f ? XMVectorNegate(to) : to;
        XMVECTOR next = XMQuaternionNormalize(XMQuaternionSlerp(from, shortest, smooth));
        if (t >= 1.0)
        {
            next = to;
            orientationAnimating = false;
        }
        XMStoreFloat4(&orientation, next);
    }
}

bool Camera::HasMotion() const
{
    if (pendingLookX != 0.0 || pendingLookY != 0.0) return true;
    if (velRight != 0.0 || velUp != 0.0 || velForward != 0.0) return true;
    if (rollRate != 0.0) return true;
    if (orbitRateX != 0.0 || orbitRateY != 0.0) return true;
    if (panRateX != 0.0 || panRateY != 0.0) return true;
    if (inertiaX != 0.0 || inertiaY != 0.0) return true;
    if (panInertiaX != 0.0 || panInertiaY != 0.0) return true;
    if (pivotAnimating || orientationAnimating) return true;
    if (distance != targetDistance) return true;
    if (projectionBlend != (projection == ProjectionMode::Orthographic ? 1.0 : 0.0)) return true;
    return false;
}

XMVECTOR Camera::Orientation() const
{
    return LoadOrientation(*this);
}

XMVECTOR Camera::EyePosition() const
{
    const XMVECTOR pivot = XMVectorSet(static_cast<float>(targetX), static_cast<float>(targetY),
        static_cast<float>(targetZ), 1.0f);
    return pivot + XMVector3Rotate(XMVectorSet(0, 0, static_cast<float>(distance), 0), Orientation());
}

double Camera::FarBound() const
{
    // Robust far plane for panned/flown-away cameras: the eye-to-scene-center
    // distance is bounded by the pivot distance plus the pivot's drift from
    // the scene's cached home center.
    const double driftX = targetX - homeX;
    const double driftY = targetY - homeY;
    const double driftZ = targetZ - homeZ;
    const double drift = std::sqrt(driftX * driftX + driftY * driftY + driftZ * driftZ);
    return distance + drift + sceneRadius * 8.0;
}

XMMATRIX Camera::ViewMatrix() const
{
    const XMVECTOR eye = EyePosition();
    const XMVECTOR pivot = XMVectorSet(static_cast<float>(targetX), static_cast<float>(targetY),
        static_cast<float>(targetZ), 1.0f);
    // The camera's own up is always perpendicular to its forward by
    // construction (yaw/pitch compose about the camera's own axes), so the
    // look-at never degenerates; the guard covers bit-level edge cases only.
    XMVECTOR up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), Orientation());
    const XMVECTOR viewAxis = XMVector3Normalize(XMVectorSubtract(eye, pivot));
    if (std::abs(XMVectorGetX(XMVector3Dot(XMVector3Normalize(up), viewAxis))) > 0.999999f)
    {
        up = XMVectorSet(0, 0, 1, 0);
    }
    return XMMatrixLookAtRH(eye, pivot, up);
}

XMMATRIX Camera::ProjectionMatrix(float aspect) const
{
    aspect = std::max(0.05f, aspect);
    const float nearPlane = static_cast<float>(std::max(sceneRadius * 0.00001, distance * 0.0005));
    const float farPlane = static_cast<float>(std::max(FarBound(), static_cast<double>(nearPlane) * 100.0));
    const XMMATRIX perspective = XMMatrixPerspectiveFovRH(kVerticalFieldOfView, aspect, nearPlane, farPlane);
    if (projectionBlend <= 0.0) return perspective;
    // Orthographic half-height matches the perspective frustum height at the
    // pivot distance, so the toggle preserves framing and dolly stays unified.
    const float halfHeight = static_cast<float>(distance * std::tan(kVerticalFieldOfView * 0.5));
    const XMMATRIX orthographic = XMMatrixOrthographicRH(halfHeight * aspect * 2.0f, halfHeight * 2.0f,
        nearPlane, farPlane);
    if (projectionBlend >= 1.0) return orthographic;
    const float t = static_cast<float>(projectionBlend);
    const float smooth = t * t * (3.0f - 2.0f * t);
    XMMATRIX blended{};
    for (int row = 0; row < 4; ++row)
    {
        blended.r[row] = XMVectorLerp(perspective.r[row], orthographic.r[row], smooth);
    }
    return blended;
}

struct Renderer::Impl
{
    HWND window = nullptr;
    int width = 0;
    int height = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> renderTarget;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> constantBuffer;
    ComPtr<ID3D11RasterizerState> rasterizerState;
    ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11BlendState> blendState;
    ComPtr<ID3D11Buffer> vertexBuffer;
    ComPtr<ID3D11Buffer> indexBuffer;
    ComPtr<ID3D11Buffer> gridBuffer;
    UINT indexCount = 0;
    UINT gridVertexCount = 0;

    bool CreateTargets(std::wstring& error)
    {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT result = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(result) || FAILED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget)))
        {
            error = L"The window render target could not be created.";
            return false;
        }

        D3D11_TEXTURE2D_DESC depthDescription{};
        depthDescription.Width = static_cast<UINT>(std::max(1, width));
        depthDescription.Height = static_cast<UINT>(std::max(1, height));
        depthDescription.MipLevels = 1;
        depthDescription.ArraySize = 1;
        depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDescription.SampleDesc.Count = 1;
        depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(device->CreateTexture2D(&depthDescription, nullptr, &depthTexture)) ||
            FAILED(device->CreateDepthStencilView(depthTexture.Get(), nullptr, &depthView)))
        {
            error = L"The depth buffer could not be created.";
            return false;
        }

        return true;
    }

    // Takes explicit (already-effective, post-up-axis-correction) bounds
    // rather than a whole ModelData, since the "show native orientation"
    // toggle changes which bounds are active without re-uploading the model.
    bool BuildGrid(const XMFLOAT3& boundsMin, const XMFLOAT3& boundsMax, std::wstring& error)
    {
        std::vector<ModelVertex> vertices;
        const float centerX = (boundsMin.x + boundsMax.x) * 0.5f;
        const float centerY = (boundsMin.y + boundsMax.y) * 0.5f;
        const float extentX = boundsMax.x - boundsMin.x;
        const float extentY = boundsMax.y - boundsMin.y;
        const float extentZ = boundsMax.z - boundsMin.z;
        const float radius = std::max(0.001f, std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ) * 0.5f);
        const float span = radius * 2.2f;
        // The ground plane is X-Y (Z is this app's up axis).
        const float ground = boundsMin.z - radius * 0.012f;
        constexpr int divisions = 20;
        vertices.reserve((divisions + 1) * 4);
        for (int line = 0; line <= divisions; ++line)
        {
            const float t = static_cast<float>(line) / divisions;
            const float offset = -span + 2.0f * span * t;
            const bool major = line == divisions / 2 || line % 5 == 0;
            const XMFLOAT4 color = major ? XMFLOAT4(0.22f, 0.25f, 0.30f, 0.52f) : XMFLOAT4(0.16f, 0.18f, 0.22f, 0.36f);
            vertices.push_back({ XMFLOAT3(centerX - span, centerY + offset, ground), XMFLOAT3(0, 0, 1), color });
            vertices.push_back({ XMFLOAT3(centerX + span, centerY + offset, ground), XMFLOAT3(0, 0, 1), color });
            vertices.push_back({ XMFLOAT3(centerX + offset, centerY - span, ground), XMFLOAT3(0, 0, 1), color });
            vertices.push_back({ XMFLOAT3(centerX + offset, centerY + span, ground), XMFLOAT3(0, 0, 1), color });
        }
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(ModelVertex));
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{ vertices.data(), 0, 0 };
        if (FAILED(device->CreateBuffer(&description, &initial, &gridBuffer)))
        {
            error = L"The ground grid could not be uploaded.";
            return false;
        }
        gridVertexCount = static_cast<UINT>(vertices.size());
        return true;
    }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

bool Renderer::Initialize(HWND window, std::wstring& error)
{
    impl_->window = window;
    RECT client{};
    GetClientRect(window, &client);
    impl_->width = std::max(1L, client.right - client.left);
    impl_->height = std::max(1L, client.bottom - client.top);

    DXGI_SWAP_CHAIN_DESC swapDescription{};
    swapDescription.BufferDesc.Width = impl_->width;
    swapDescription.BufferDesc.Height = impl_->height;
    swapDescription.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDescription.BufferDesc.RefreshRate.Numerator = 60;
    swapDescription.BufferDesc.RefreshRate.Denominator = 1;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.BufferCount = 2;
    swapDescription.OutputWindow = window;
    swapDescription.Windowed = TRUE;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL createdLevel{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
        &swapDescription, &impl_->swapChain, &impl_->device, &createdLevel, &impl_->context);
    if (FAILED(result))
    {
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
            &swapDescription, &impl_->swapChain, &impl_->device, &createdLevel, &impl_->context);
    }
    if (FAILED(result))
    {
        error = L"A Direct3D 11 device could not be created.";
        return false;
    }

    ComPtr<ID3DBlob> vertexCode = CompileShader(kVertexShader, "main", "vs_5_0", error);
    ComPtr<ID3DBlob> pixelCode = CompileShader(kPixelShader, "main", "ps_5_0", error);
    if (!vertexCode || !pixelCode) return false;
    if (FAILED(impl_->device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
        &impl_->vertexShader)) || FAILED(impl_->device->CreatePixelShader(pixelCode->GetBufferPointer(),
            pixelCode->GetBufferSize(), nullptr, &impl_->pixelShader)))
    {
        error = L"The built-in graphics pipeline could not be created.";
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(ModelVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(ModelVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(ModelVertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    if (FAILED(impl_->device->CreateInputLayout(layout, ARRAYSIZE(layout), vertexCode->GetBufferPointer(),
        vertexCode->GetBufferSize(), &impl_->inputLayout)))
    {
        error = L"The model vertex layout could not be created.";
        return false;
    }

    D3D11_BUFFER_DESC constantDescription{};
    constantDescription.ByteWidth = sizeof(FrameConstants);
    constantDescription.Usage = D3D11_USAGE_DEFAULT;
    constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(impl_->device->CreateBuffer(&constantDescription, nullptr, &impl_->constantBuffer)))
    {
        error = L"The camera buffer could not be created.";
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    impl_->device->CreateRasterizerState(&rasterizer, &impl_->rasterizerState);

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    impl_->device->CreateDepthStencilState(&depth, &impl_->depthState);

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    impl_->device->CreateBlendState(&blend, &impl_->blendState);

    return impl_->CreateTargets(error);
}

bool Renderer::Resize(int width, int height, std::wstring& error)
{
    if (!impl_->swapChain || width <= 0 || height <= 0) return true;
    if (width == impl_->width && height == impl_->height && impl_->renderTarget) return true;
    impl_->context->OMSetRenderTargets(0, nullptr, nullptr);
    impl_->renderTarget.Reset();
    impl_->depthView.Reset();
    impl_->depthTexture.Reset();
    impl_->context->Flush();
    const HRESULT result = impl_->swapChain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result))
    {
        error = L"The 3D viewport could not be resized.";
        return false;
    }
    impl_->width = width;
    impl_->height = height;
    return impl_->CreateTargets(error);
}

bool Renderer::UploadModel(const ModelData& model, std::wstring& error)
{
    if (!impl_->device || model.vertices.empty() || model.indices.empty())
    {
        error = L"The imported model has no renderable triangle data.";
        return false;
    }
    if (model.vertices.size() * sizeof(ModelVertex) > std::numeric_limits<UINT>::max() ||
        model.indices.size() * sizeof(std::uint32_t) > std::numeric_limits<UINT>::max())
    {
        error = L"The model is too large for a single preview buffer.";
        return false;
    }

    ComPtr<ID3D11Buffer> vertexBuffer;
    D3D11_BUFFER_DESC vertexDescription{};
    vertexDescription.ByteWidth = static_cast<UINT>(model.vertices.size() * sizeof(ModelVertex));
    vertexDescription.Usage = D3D11_USAGE_IMMUTABLE;
    vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertexData{ model.vertices.data(), 0, 0 };
    if (FAILED(impl_->device->CreateBuffer(&vertexDescription, &vertexData, &vertexBuffer)))
    {
        error = L"There was not enough graphics memory to upload this model.";
        return false;
    }

    ComPtr<ID3D11Buffer> indexBuffer;
    D3D11_BUFFER_DESC indexDescription{};
    indexDescription.ByteWidth = static_cast<UINT>(model.indices.size() * sizeof(std::uint32_t));
    indexDescription.Usage = D3D11_USAGE_IMMUTABLE;
    indexDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA indexData{ model.indices.data(), 0, 0 };
    if (FAILED(impl_->device->CreateBuffer(&indexDescription, &indexData, &indexBuffer)))
    {
        error = L"There was not enough graphics memory to upload this model.";
        return false;
    }

    impl_->vertexBuffer = std::move(vertexBuffer);
    impl_->indexBuffer = std::move(indexBuffer);
    impl_->indexCount = static_cast<UINT>(model.indices.size());
    return true;
}

bool Renderer::RebuildGrid(const XMFLOAT3& boundsMin, const XMFLOAT3& boundsMax, std::wstring& error)
{
    if (!impl_->device) return false;
    impl_->gridBuffer.Reset();
    impl_->gridVertexCount = 0;
    return impl_->BuildGrid(boundsMin, boundsMax, error);
}

void Renderer::ClearModel()
{
    impl_->vertexBuffer.Reset();
    impl_->indexBuffer.Reset();
    impl_->gridBuffer.Reset();
    impl_->indexCount = 0;
    impl_->gridVertexCount = 0;
}

bool Renderer::HasModel() const
{
    return impl_->vertexBuffer && impl_->indexBuffer && impl_->indexCount > 0;
}

void Renderer::Render(const Camera& camera, const OverlayInfo& overlay, const NavGizmo&, const Chrome&)
{
    if (!impl_->context || !impl_->renderTarget || impl_->width <= 0 || impl_->height <= 0) return;
    ID3D11RenderTargetView* renderTarget = impl_->renderTarget.Get();
    impl_->context->OMSetRenderTargets(1, &renderTarget, impl_->depthView.Get());
    const float background[] = { 28.0f / 255.0f, 28.0f / 255.0f, 30.0f / 255.0f, 1.0f };
    impl_->context->ClearRenderTargetView(impl_->renderTarget.Get(), background);
    impl_->context->ClearDepthStencilView(impl_->depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    const bool loading = overlay.state == ViewerState::Loading;
    const float viewportTop = loading ? 0.0f : static_cast<float>(overlay.toolbarHeight);
    const float bottomInset = loading ? 0.0f : static_cast<float>(overlay.bottomBarHeight);
    const float panelInset = loading ? 0.0f : static_cast<float>(overlay.infoPanelWidth);
    const float viewportWidth = std::max(1.0f, static_cast<float>(impl_->width) - panelInset);
    const float viewportHeight = std::max(1.0f, static_cast<float>(impl_->height) - viewportTop - bottomInset);
    D3D11_VIEWPORT viewport{ 0.0f, viewportTop, viewportWidth, viewportHeight, 0.0f, 1.0f };
    impl_->context->RSSetViewports(1, &viewport);
    impl_->context->RSSetState(impl_->rasterizerState.Get());
    impl_->context->OMSetDepthStencilState(impl_->depthState.Get(), 0);
    const float blendFactor[] = { 0, 0, 0, 0 };
    impl_->context->OMSetBlendState(impl_->blendState.Get(), blendFactor, 0xffffffff);
    impl_->context->IASetInputLayout(impl_->inputLayout.Get());
    impl_->context->VSSetShader(impl_->vertexShader.Get(), nullptr, 0);
    impl_->context->PSSetShader(impl_->pixelShader.Get(), nullptr, 0);

    const XMVECTOR eye = camera.EyePosition();
    const XMMATRIX view = camera.ViewMatrix();
    const float aspect = viewportWidth / viewportHeight;
    const XMMATRIX projection = camera.ProjectionMatrix(aspect);
    FrameConstants constants;
    XMStoreFloat4x4(&constants.viewProjection, view * projection);
    XMStoreFloat4(&constants.cameraPosition, eye);
    constants.options = XMFLOAT4(overlay.selectionAmount, 0.0f, 0.0f, 0.0f);
    ID3D11Buffer* constantBuffer = impl_->constantBuffer.Get();
    impl_->context->VSSetConstantBuffers(0, 1, &constantBuffer);
    impl_->context->PSSetConstantBuffers(0, 1, &constantBuffer);

    const UINT stride = sizeof(ModelVertex);
    const UINT offsetBytes = 0;
    if (!loading && impl_->gridBuffer && overlay.gridVisible)
    {
        // The grid always stays in the app's fixed Z-up world, regardless of
        // the model's own up-axis correction/toggle.
        XMStoreFloat4x4(&constants.modelTransform, XMMatrixIdentity());
        impl_->context->UpdateSubresource(impl_->constantBuffer.Get(), 0, nullptr, &constants, 0, 0);
        ID3D11Buffer* grid = impl_->gridBuffer.Get();
        impl_->context->IASetVertexBuffers(0, 1, &grid, &stride, &offsetBytes);
        impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        impl_->context->Draw(impl_->gridVertexCount, 0);
    }
    if (!loading && impl_->vertexBuffer && impl_->indexBuffer)
    {
        constants.modelTransform = overlay.modelTransform;
        impl_->context->UpdateSubresource(impl_->constantBuffer.Get(), 0, nullptr, &constants, 0, 0);
        ID3D11Buffer* vertices = impl_->vertexBuffer.Get();
        impl_->context->IASetVertexBuffers(0, 1, &vertices, &stride, &offsetBytes);
        impl_->context->IASetIndexBuffer(impl_->indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        impl_->context->DrawIndexed(impl_->indexCount, 0, 0);
    }

    impl_->swapChain->Present(1, 0);
}

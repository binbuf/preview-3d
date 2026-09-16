#pragma once
#include <model_core/WireFormat.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

// Uses only broker-verified bounds, with the same double-origin subtraction
// and up-axis correction as drawing. Conservative at the near plane.
inline float DetailViewPriority(const model_core::ChunkDescriptor& geometry,
    const double sceneOrigin[3], const double cameraTarget[3],
    const DirectX::XMFLOAT4X4& modelTransform,
    const DirectX::XMFLOAT4X4& viewProjection)
{
    const auto vp = DirectX::XMLoadFloat4x4(&viewProjection);
    bool outside[6] = {true,true,true,true,true,true};
    float minX=1, maxX=-1, minY=1, maxY=-1;
    for (unsigned corner=0; corner<8; ++corner) {
        double p[3];
        for (unsigned axis=0; axis<3; ++axis)
            p[axis] = geometry.origin[axis]-sceneOrigin[axis]
                + ((corner & (1u<<axis)) ? geometry.localMax[axis] : geometry.localMin[axis]);
        const double transformed[3] = {
            p[0]*modelTransform._11 + p[1]*modelTransform._21 + p[2]*modelTransform._31,
            p[0]*modelTransform._12 + p[1]*modelTransform._22 + p[2]*modelTransform._32,
            p[0]*modelTransform._13 + p[1]*modelTransform._23 + p[2]*modelTransform._33,
        };
        const auto clip = DirectX::XMVector4Transform(DirectX::XMVectorSet(
            float(transformed[0]-cameraTarget[0]),float(transformed[1]-cameraTarget[1]),
            float(transformed[2]-cameraTarget[2]),1),vp);
        DirectX::XMFLOAT4 c; DirectX::XMStoreFloat4(&c,clip);
        if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(c.w)) return 1;
        outside[0] &= c.x < -c.w; outside[1] &= c.x > c.w;
        outside[2] &= c.y < -c.w; outside[3] &= c.y > c.w;
        outside[4] &= c.z < 0; outside[5] &= c.z > c.w;
        if (c.w > 1e-6f) {
            minX=(std::min)(minX,c.x/c.w); maxX=(std::max)(maxX,c.x/c.w);
            minY=(std::min)(minY,c.y/c.w); maxY=(std::max)(maxY,c.y/c.w);
        } else { minX=minY=-1; maxX=maxY=1; }
    }
    for (bool value:outside) if (value) return 0;
    const float extent=(std::max)(maxX-minX,maxY-minY);
    const float center=std::abs((maxX+minX)*0.5f)+std::abs((maxY+minY)*0.5f);
    return (0.001f+(std::min)(extent,4.0f))/(1.0f+center);
}

#pragma once
#include <model_core/WireFormat.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

struct CameraRelativeInstanceTransform {
    DirectX::XMFLOAT4X4 localToCamera{};
    DirectX::XMFLOAT4X4 normalToCamera{};
};

// All absolute translation and camera-origin subtraction happens in double.
// Only the small camera-relative result and the linear terms cross to float.
inline CameraRelativeInstanceTransform BuildCameraRelativeInstanceTransform(
    const double instance[16], const double origin[3], const double sceneOrigin[3],
    const double cameraTarget[3], const DirectX::XMFLOAT4X4& axis)
{
    CameraRelativeInstanceTransform result{};
    auto& out=result.localToCamera;
    const double a[3][3]={{axis._11,axis._12,axis._13},{axis._21,axis._22,axis._23},{axis._31,axis._32,axis._33}};
    float* values=&out._11;
    for(unsigned row=0;row<3;++row)for(unsigned column=0;column<3;++column){double value=0;
        for(unsigned k=0;k<3;++k)value+=instance[row*4+k]*a[k][column];values[row*4+column]=float(value);}
    out._14=out._24=out._34=0;out._44=1;
    double transformedOrigin[3]{};
    for(unsigned i=0;i<3;++i)transformedOrigin[i]=origin[0]*instance[i]+origin[1]*instance[4+i]
        +origin[2]*instance[8+i]+instance[12+i]-sceneOrigin[i];
    out._41=float(transformedOrigin[0]*a[0][0]+transformedOrigin[1]*a[1][0]+transformedOrigin[2]*a[2][0]-cameraTarget[0]);
    out._42=float(transformedOrigin[0]*a[0][1]+transformedOrigin[1]*a[1][1]+transformedOrigin[2]*a[2][1]-cameraTarget[1]);
    out._43=float(transformedOrigin[0]*a[0][2]+transformedOrigin[1]*a[1][2]+transformedOrigin[2]*a[2][2]-cameraTarget[2]);
    const auto inverse=DirectX::XMMatrixInverse(nullptr,DirectX::XMLoadFloat4x4(&out));
    DirectX::XMStoreFloat4x4(&result.normalToCamera,DirectX::XMMatrixTranspose(inverse));
    result.normalToCamera._14=result.normalToCamera._24=result.normalToCamera._34=0;
    result.normalToCamera._41=result.normalToCamera._42=result.normalToCamera._43=0;result.normalToCamera._44=1;
    return result;
}

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

inline float InstanceViewPriority(const double boundsMin[3], const double boundsMax[3],
    const double sceneOrigin[3], const double cameraTarget[3],
    const DirectX::XMFLOAT4X4& modelTransform, const DirectX::XMFLOAT4X4& viewProjection)
{
    const auto vp = DirectX::XMLoadFloat4x4(&viewProjection);
    bool outside[6] = {true,true,true,true,true,true};
    float minX=1, maxX=-1, minY=1, maxY=-1;
    for (unsigned corner=0; corner<8; ++corner) {
        double p[3];
        for (unsigned axis=0; axis<3; ++axis)
            p[axis] = ((corner & (1u<<axis)) ? boundsMax[axis] : boundsMin[axis]) - sceneOrigin[axis];
        const double transformed[3] = {
            p[0]*modelTransform._11 + p[1]*modelTransform._21 + p[2]*modelTransform._31,
            p[0]*modelTransform._12 + p[1]*modelTransform._22 + p[2]*modelTransform._32,
            p[0]*modelTransform._13 + p[1]*modelTransform._23 + p[2]*modelTransform._33,
        };
        DirectX::XMFLOAT4 c; DirectX::XMStoreFloat4(&c,DirectX::XMVector4Transform(DirectX::XMVectorSet(
            float(transformed[0]-cameraTarget[0]),float(transformed[1]-cameraTarget[1]),
            float(transformed[2]-cameraTarget[2]),1),vp));
        if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(c.w)) return 1;
        outside[0]&=c.x < -c.w; outside[1]&=c.x > c.w; outside[2]&=c.y < -c.w;
        outside[3]&=c.y > c.w; outside[4]&=c.z < 0; outside[5]&=c.z > c.w;
        if (c.w>1e-6f) { minX=(std::min)(minX,c.x/c.w);maxX=(std::max)(maxX,c.x/c.w);
            minY=(std::min)(minY,c.y/c.w);maxY=(std::max)(maxY,c.y/c.w); }
        else { minX=minY=-1;maxX=maxY=1; }
    }
    for (bool value:outside) if (value) return 0;
    const float extent=(std::max)(maxX-minX,maxY-minY);
    const float center=std::abs((maxX+minX)*0.5f)+std::abs((maxY+minY)*0.5f);
    return (0.001f+(std::min)(extent,4.0f))/(1.0f+center);
}

#pragma once

#include <model_core/WireFormat.h>

#include <DirectXMath.h>

#include <cstdint>

// The source/model axis that is treated as vertical and mapped onto the
// viewer's fixed Z-up world/ground plane. Automatic preserves the historical
// behavior: declared Y-up sources (glTF) use Y; sources without an up-axis use
// Z. Once the user presses the toolbar button, the preference is explicit.
enum class GroundAxis : std::uint8_t
{
    Automatic = 0,
    X = 1,
    Y = 2,
    Z = 3,
};

inline GroundAxis ResolveGroundAxis(GroundAxis selected, model_core::UpAxisId sourceUpAxis)
{
    if (selected != GroundAxis::Automatic) return selected;
    return sourceUpAxis == model_core::UpAxisId::Y ? GroundAxis::Y : GroundAxis::Z;
}

// The requested cycle is deliberately Z -> Y -> X -> Z. Automatic first
// resolves against the current document, so the first press always advances
// from what the user is actually seeing.
inline GroundAxis NextGroundAxis(GroundAxis selected, model_core::UpAxisId sourceUpAxis)
{
    switch (ResolveGroundAxis(selected, sourceUpAxis))
    {
    case GroundAxis::Z: return GroundAxis::Y;
    case GroundAxis::Y: return GroundAxis::X;
    case GroundAxis::X: return GroundAxis::Z;
    default: return GroundAxis::Z;
    }
}

inline const wchar_t* GroundAxisName(GroundAxis axis)
{
    switch (axis)
    {
    case GroundAxis::X: return L"X";
    case GroundAxis::Y: return L"Y";
    case GroundAxis::Z: return L"Z";
    default: return L"Automatic";
    }
}

// Maps the selected source axis onto the viewer's positive Z axis. When the
// direction is inverted, the negative side of that same source axis becomes
// up, allowing an upside-down/inside-out authored model to be grounded on its
// opposite end without changing the selected X/Y/Z axis.
// Exact signed permutations avoid trigonometric residue in bounds and retain
// the existing glTF Y-up correction byte-for-byte.
inline DirectX::XMMATRIX GroundAxisTransform(
    GroundAxis selected, model_core::UpAxisId sourceUpAxis, bool showNativeOrientation,
    bool groundAxisInverted = false)
{
    if (showNativeOrientation) return DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX transform;
    switch (ResolveGroundAxis(selected, sourceUpAxis))
    {
    case GroundAxis::X:
        transform = DirectX::XMMatrixSet(
            0, 0, 1, 0,
            0, 1, 0, 0,
           -1, 0, 0, 0,
            0, 0, 0, 1);
        break;
    case GroundAxis::Y:
        transform = DirectX::XMMatrixSet(
            1, 0, 0, 0,
            0, 0, 1, 0,
            0,-1, 0, 0,
            0, 0, 0, 1);
        break;
    case GroundAxis::Z:
    default:
        transform = DirectX::XMMatrixIdentity();
        break;
    }
    if (!groundAxisInverted) return transform;
    // Rotate 180 degrees around world X after the ordinary axis correction:
    // output Z changes sign while the matrix remains a proper rotation.
    const DirectX::XMMATRIX flipWorldUp = DirectX::XMMatrixSet(
        1, 0, 0, 0,
        0,-1, 0, 0,
        0, 0,-1, 0,
        0, 0, 0, 1);
    return DirectX::XMMatrixMultiply(transform, flipWorldUp);
}

inline void PermuteGroundedDimensions(double dimensions[3], GroundAxis selected,
    model_core::UpAxisId sourceUpAxis, bool showNativeOrientation)
{
    if (showNativeOrientation) return;
    const double x = dimensions[0];
    const double y = dimensions[1];
    const double z = dimensions[2];
    switch (ResolveGroundAxis(selected, sourceUpAxis))
    {
    case GroundAxis::X:
        dimensions[0] = z; dimensions[1] = y; dimensions[2] = x;
        break;
    case GroundAxis::Y:
        dimensions[0] = x; dimensions[1] = z; dimensions[2] = y;
        break;
    default:
        break;
    }
}

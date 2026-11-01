#pragma once

// Pure per-format up-axis/unit policy for SceneMetadata.
//
// The trusted broker applies exactly this predicate to every section before it
// copies a chunk, and the unit tests can exercise the same rule without
// launching an import worker. It is intentionally a small, dependency-free
// function: no parser, no filesystem and no platform headers.

#include "model_core/WireFormat.h"

namespace model_core {

// Returns true when the reported source up axis and metre factor are allowed
// for the wire format. A zero metre factor means "units unspecified"; it is
// never treated as metres by assumption.
constexpr bool SceneUnitsAndUpAxisAllowed(
    SourceFormatId format, UpAxisId upAxis, double metersPerUnit) noexcept
{
    switch (format) {
    case SourceFormatId::Gltf:
    case SourceFormatId::Glb:
        // glTF mandates Y-up and metres.
        return upAxis == UpAxisId::Y && metersPerUnit == 1.0;
    case SourceFormatId::Stl:
    case SourceFormatId::Ply:
    case SourceFormatId::AsciiStl:
    case SourceFormatId::AsciiPly:
        // No standard up axis or authored length unit.
        return upAxis == UpAxisId::Unknown && metersPerUnit == 0.0;
    case SourceFormatId::Obj:
        // OBJ has no standard up axis. The product default is the ecosystem
        // Y-up convention, but an older/third-party worker reporting Unknown
        // is still accepted; authored units never exist.
        return (upAxis == UpAxisId::Y || upAxis == UpAxisId::Unknown)
            && metersPerUnit == 0.0;
    case SourceFormatId::Fbx:
        // Geometry is normalized to Y-up/metres by the adapter; the authored
        // axes and unit scale are not carried on the wire.
        return upAxis == UpAxisId::Y && metersPerUnit == 1.0;
    case SourceFormatId::Usda:
    case SourceFormatId::Usdc:
    case SourceFormatId::Usdz:
        return upAxis != UpAxisId::Unknown && metersPerUnit > 0.0;
    case SourceFormatId::Step:
        // STEP does not define a universal display-up axis, but the authored
        // length unit must convert to a verified positive metre factor.
        return upAxis == UpAxisId::Unknown && metersPerUnit > 0.0;
    case SourceFormatId::Iges:
        // IGES has no universal display-up axis. OCCT normalizes the
        // transferred geometry to its system millimetre, so the verified
        // post-transfer metre factor must be positive. A missing global unit
        // flag is a documented millimetre assumption with a warning.
        return upAxis == UpAxisId::Unknown && metersPerUnit > 0.0;
    case SourceFormatId::Brep:
        // OCCT BREP carries topology only: no display-up axis and no authored
        // length unit. Zero is the explicit "unspecified" value, never a
        // guessed millimetre scale.
        return upAxis == UpAxisId::Unknown && metersPerUnit == 0.0;
    case SourceFormatId::Xbf:
        // Binary XCAF documents carry no universal display-up axis. The XCAF
        // length unit is reported when present and is always a positive metre
        // factor; absence is resolved to the documented OCCT millimetre
        // default with a compatibility warning, never left ambiguous on wire.
        return upAxis == UpAxisId::Unknown && metersPerUnit > 0.0;
    case SourceFormatId::Fcstd:
        // FreeCAD stores evaluated Part geometry in millimetres, so the
        // adapter reports a positive verified metre factor and no display-up
        // axis; the ground-axis control chooses the orientation.
        return upAxis == UpAxisId::Unknown && metersPerUnit > 0.0;
    case SourceFormatId::Dae:
    case SourceFormatId::DirectX:
    case SourceFormatId::Off:
    case SourceFormatId::X3d:
        // Assimp compatibility formats carry no authored length unit on the
        // wire. The declared up axis is the exporter family convention and is
        // presentation metadata only; Unknown remains accepted for an older
        // worker, but a different authored axis is a protocol violation.
        return metersPerUnit == 0.0
            && (upAxis == UpAxisId::Y || upAxis == UpAxisId::Unknown);
    case SourceFormatId::ThreeDs:
    case SourceFormatId::Dxf:
    case SourceFormatId::Amf:
        return metersPerUnit == 0.0
            && (upAxis == UpAxisId::Z || upAxis == UpAxisId::Unknown);
    default:
        return true;
    }
}

} // namespace model_core

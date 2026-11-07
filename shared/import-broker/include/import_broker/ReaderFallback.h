#pragma once

// T08 closed compatibility-reader fallback policy.
//
// A native glTF/OBJ/FBX/3MF reader may fail on a valid file for a
// representational reason that the bounded Assimp compatibility reader can
// still read. This header owns the *closed* product decision:
//
//   - which four families have a shipped compatibility reader;
//   - which typed failures are eligible;
//   - the stable reader names used in diagnostics.
//
// It deliberately contains no process, path or parser code. The broker applies
// the decision (one retry, before any batch is published, in the same sandbox
// with the same budgets); the worker only ever receives the existing closed
// `AssimpFormat` hint.

#include "model_core/ControlProtocol.h"
#include "model_core/ImportError.h"

#include <cstdint>

namespace import_broker {

// Closed route identity. It names a native family and its shipped
// compatibility reader; it is never a free-form reader or parser hint.
enum class ReaderFallbackRoute : uint32_t {
    None = 0,
    GltfAssimp = 1,
    ObjAssimp = 2,
    FbxAssimp = 3,
    ThreeMfAssimp = 4,
};

constexpr bool IsKnownReaderFallbackRoute(ReaderFallbackRoute route) noexcept
{
    return route == ReaderFallbackRoute::None
        || route == ReaderFallbackRoute::GltfAssimp
        || route == ReaderFallbackRoute::ObjAssimp
        || route == ReaderFallbackRoute::FbxAssimp
        || route == ReaderFallbackRoute::ThreeMfAssimp;
}

// The closed eligible set. These are representational gaps: the native parser
// could not accept the file's container/features or found no displayable
// primitive, but the bytes are not known to be unsafe and no resource policy
// was reached. Everything else is terminal and must never be retried.
constexpr bool IsReaderFallbackEligibleError(model_core::ImportErrorCode code) noexcept
{
    using E = model_core::ImportErrorCode;
    return code == E::UnsupportedFormat
        || code == E::UnsupportedRequiredFeature
        || code == E::EmptyGeometry;
}

struct ReaderFallbackNames {
    const char* nativeReader = nullptr;
    const char* compatibilityReader = nullptr;
};

constexpr ReaderFallbackNames ReaderFallbackNamesFor(ReaderFallbackRoute route) noexcept
{
    switch (route) {
    case ReaderFallbackRoute::GltfAssimp:
        return { "gltf", "gltf-assimp" };
    case ReaderFallbackRoute::ObjAssimp:
        return { "obj", "obj-assimp" };
    case ReaderFallbackRoute::FbxAssimp:
        return { "fbx", "fbx-assimp" };
    case ReaderFallbackRoute::ThreeMfAssimp:
        return { "3mf", "3mf-assimp" };
    case ReaderFallbackRoute::None:
        break;
    }
    return {};
}

// The worker hint the compatibility attempt uses. `Dae` is the inert default
// for `None`; callers must validate the route before dispatch.
constexpr model_core::AssimpFormat AssimpFormatForReaderFallback(
    ReaderFallbackRoute route) noexcept
{
    switch (route) {
    case ReaderFallbackRoute::GltfAssimp: return model_core::AssimpFormat::Gltf;
    case ReaderFallbackRoute::ObjAssimp: return model_core::AssimpFormat::Obj;
    case ReaderFallbackRoute::FbxAssimp: return model_core::AssimpFormat::Fbx;
    case ReaderFallbackRoute::ThreeMfAssimp: return model_core::AssimpFormat::ThreeMf;
    case ReaderFallbackRoute::None: break;
    }
    return model_core::AssimpFormat::Dae;
}

} // namespace import_broker

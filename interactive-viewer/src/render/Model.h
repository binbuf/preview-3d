#pragma once

#include "model_core/FileIdentity.h"

#include <DirectXMath.h>
#include <model_core/WireFormat.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ModelVertex
{
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 normal{};
    DirectX::XMFLOAT4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

// Scanned (not rendered) glTF/GLB metadata for the Information side panel. All of
// this comes from cheap lookups against the already-parsed glTF JSON — none of it
// requires decoding texture pixels or implementing animation/skinning playback,
// which stay deliberately out of scope for this preview slice (see README.md).
struct ModelStats
{
    // Mesh Data
    bool hasUv0 = false;
    bool hasUv1 = false;
    bool hasVertexColors = false;
    int materialCount = 0;

    // Texture Data: how many materials reference each PBR texture slot. glTF
    // packs metallic (B) and roughness (G) into one `metallicRoughnessTexture`,
    // so that single count stands in for both Specular/Metallic and
    // Gloss/Roughness. `hasConstant*` flags a factor-only (untextured) value
    // for the fields that have no natural "count" of their own.
    int albedoTextureCount = 0;
    int normalTextureCount = 0;
    int specularMetallicTextureCount = 0;
    int occlusionTextureCount = 0;
    int emissiveTextureCount = 0;
    bool hasConstantBaseColor = false;
    bool hasConstantEmissiveColor = false;
    bool hasConstantSpecularColor = false;
    bool hasTransparency = false;   // alphaMode != OPAQUE, or a base color factor alpha < 1

    // Animation Data
    int animationCount = 0;
    int skinCount = 0;
    int boneCount = 0;   // total joints across all skins (glTF has no separate "bone" concept)

    // Performance Data
    int drawCallCount = 0;

    // Scene Data
    int nodeCount = 0;
    int meshCount = 0;
};

// A source format's up axis at rest, as reported by its importer. Used only
// to pick/derive ModelData::upAxisCorrection; nothing downstream branches on
// this directly.
enum class SourceUpAxis
{
    Unknown,
    X,
    Y,   // glTF/GLB's mandated up axis.
    Z,   // The ecosystem-norm up axis for STL/3MF/etc. (no importer yet).
};

struct ModelData
{
    // Legacy loader storage. The progressive app leaves both vectors empty;
    // its selection queries a bounded GPU pixel instead of retaining vertices.
    // Baked node transforms, exactly as the source format authored them
    // (glTF is Y-up) — never re-baked to the app's own Z-up convention.
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    // Progressive bounds are relative to sceneOrigin in native/source axes.
    // Float boxes feed unchanged camera math; double boxes feed UI dimensions.
    DirectX::XMFLOAT3 boundsMin{};
    DirectX::XMFLOAT3 boundsMax{};
    std::uint64_t triangleCount = 0;
    std::uint64_t vertexCount = 0;
    std::uint64_t pointCount = 0;
    model_core::SceneMetadata source{};
    model_core::FileIdentity sourceIdentity{};
    double sceneOrigin[3]{};
    double relativeMin[3]{};
    double relativeMax[3]{};
    bool boundsVerified = false;
    std::wstring warning;
    // Authored relative references whose sidecar assets could not be found,
    // deduplicated and in request order. The viewer lists these and offers a
    // folder picker so the user can point at where the assets live.
    std::vector<std::wstring> missingAssets;
    model_core::ImportStatusPayload importStatus{};
    ModelStats stats;

    // Root transform mapping this model's native/source axes into the app's
    // own Z-up world (NavGizmo.h, Renderer.cpp's Camera). Applied only at
    // render/pick/bounds time — never baked into `vertices` above — so a
    // live "show native orientation" toggle can swap it for identity
    // instantly, with no re-parse and no vertex-buffer rebuild.
    SourceUpAxis sourceUpAxis = SourceUpAxis::Unknown;
    DirectX::XMFLOAT4X4 upAxisCorrection = []
    {
        DirectX::XMFLOAT4X4 identity{};
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        return identity;
    }();
};

struct LoadResult
{
    bool succeeded = false;
    bool cancelled = false;
    std::shared_ptr<ModelData> model;
    std::wstring summary;
    std::wstring details;
};

using LoadProgressCallback = std::function<void(const wchar_t*)>;

LoadResult LoadGlb(
    const std::wstring& path,
    const std::shared_ptr<std::atomic_bool>& cancel,
    const LoadProgressCallback& progress);

// Ray-versus-mesh intersection for click selection. The importer bakes node
// transforms, so vertices are already in the model's native/source space —
// callers must transform the ray into that space first (undo
// ModelData::upAxisCorrection, i.e. use its inverse) before calling this.
// `direction` does not need to be normalized. Returns true and sets
// `hitDistance` to the entry distance along the normalized direction.
bool PickMesh(
    const ModelData& model,
    const DirectX::XMFLOAT3& origin,
    const DirectX::XMFLOAT3& direction,
    float& hitDistance);

// Transforms an axis-aligned box's 8 corners by `transform` and returns the
// new axis-aligned box enclosing them. Used to re-derive display/camera
// bounds whenever the active root transform (ModelData::upAxisCorrection vs.
// identity) changes, without touching the baked vertex data.
void TransformBounds(
    const DirectX::XMFLOAT3& minimum, const DirectX::XMFLOAT3& maximum,
    DirectX::FXMMATRIX transform,
    DirectX::XMFLOAT3& outMinimum, DirectX::XMFLOAT3& outMaximum);

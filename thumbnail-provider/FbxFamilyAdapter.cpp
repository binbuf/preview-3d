// T31 FBX family adapter implementation (see FbxFamilyAdapter.h).

#include "FbxFamilyAdapter.h"

#include "Deadline.h"
#include "ProviderLimits.h"

#include "ufbx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace preview3d::provider {
namespace {

// Per-face triangulation ceiling and deadline checkpoint interval, matching the
// T24 OBJ adapter's bounded-polygon policy.
constexpr std::uint64_t kMaxTrianglesPerFace = 65'536;
constexpr std::uint64_t kCheckpointFaces = 1'024;
constexpr std::uint64_t kCheckpointTriangles = 4'096;

// One embedded image may hold at most this many encoded bytes before it is
// discarded unread. Independent of the aggregate decoded-pixel budget.
constexpr std::uint64_t kEmbeddedImageEncodedMaxBytes = 32ull * 1024 * 1024;

bool Finite(double value) noexcept { return std::isfinite(value); }

bool Finite3(ufbx_vec3 value) noexcept
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

float Saturate(double value, float fallback = 1.0f) noexcept
{
    if (!Finite(value)) {
        return fallback;
    }
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

float SafeFloat(double value, float fallback = 0.0f) noexcept
{
    if (!Finite(value) || value < -(std::numeric_limits<float>::max)()
        || value > (std::numeric_limits<float>::max)()) {
        return fallback;
    }
    return static_cast<float>(value);
}

void NormalizeInto(ufbx_vec3 value, float out[3]) noexcept
{
    const double lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
    if (!Finite(lengthSquared) || lengthSquared <= 1e-24) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    const double inverse = 1.0 / std::sqrt(lengthSquared);
    out[0] = static_cast<float>(value.x * inverse);
    out[1] = static_cast<float>(value.y * inverse);
    out[2] = static_cast<float>(value.z * inverse);
}

bool InheritedVisible(const ufbx_node* node) noexcept
{
    for (const ufbx_node* current = node; current != nullptr; current = current->parent) {
        if (!current->visible) {
            return false;
        }
    }
    return true;
}

// Deny every implicit external file (geometry cache, texture sidecar). The
// provider has no filesystem capability and a stream-only FBX must never open a
// path. `user` points at FbxAdapter::externalFileDetected_ so a test can observe
// that a reference was denied.
bool DenyExternalFile(void* user, ufbx_stream*, const char*, size_t,
                      const ufbx_open_file_info*) noexcept
{
    if (user != nullptr) {
        *static_cast<bool*>(user) = true;
    }
    return false;
}

// Cooperative deadline hook: ufbx polls this at `progress_interval_hint` bytes.
ufbx_progress_result CheckProgress(void* user, const ufbx_progress*) noexcept
{
    const auto* deadline = static_cast<const Deadline*>(user);
    return (deadline != nullptr && !deadline->Checkpoint()) ? UFBX_PROGRESS_CANCEL
                                                            : UFBX_PROGRESS_CONTINUE;
}

ErrorCode MapLoadError(const ufbx_error& error) noexcept
{
    switch (error.type) {
        case UFBX_ERROR_EMPTY_FILE:
            return ErrorCode::EmptyGeometry;
        case UFBX_ERROR_OUT_OF_MEMORY:
            return ErrorCode::OutOfMemory;
        case UFBX_ERROR_MEMORY_LIMIT:
        case UFBX_ERROR_ALLOCATION_LIMIT:
        case UFBX_ERROR_NODE_DEPTH_LIMIT:
            return ErrorCode::ResourceLimit;
        case UFBX_ERROR_CANCELLED:
            return ErrorCode::Cancelled;
        case UFBX_ERROR_UNSUPPORTED_VERSION:
            return ErrorCode::UnsupportedFormat;
        default:
            return ErrorCode::MalformedData;
    }
}

model_core::MaterialPayload ConvertMaterial(const ufbx_material& material) noexcept
{
    using namespace model_core;
    MaterialPayload result = NeutralMaterial();

    const bool haveBase = material.pbr.base_color.has_value
        || material.fbx.diffuse_color.has_value;
    if (haveBase) {
        const bool usePbrBase = material.pbr.base_color.has_value;
        const ufbx_vec4 base = usePbrBase ? material.pbr.base_color.value_vec4
                                          : material.fbx.diffuse_color.value_vec4;
        const double baseFactor = material.pbr.base_factor.has_value
            ? material.pbr.base_factor.value_real
            : (material.fbx.diffuse_factor.has_value
                   ? material.fbx.diffuse_factor.value_real
                   : 1.0);
        result.baseColorFactor[0] = Saturate(base.x * baseFactor);
        result.baseColorFactor[1] = Saturate(base.y * baseFactor);
        result.baseColorFactor[2] = Saturate(base.z * baseFactor);
    }

    double opacity = 1.0;
    bool haveOpacity = false;
    if (material.pbr.opacity.has_value) {
        opacity = material.pbr.opacity.value_real;
        haveOpacity = true;
    } else if (material.fbx.transparency_factor.has_value) {
        opacity = 1.0 - material.fbx.transparency_factor.value_real;
        haveOpacity = true;
    }
    if (haveOpacity) {
        result.baseColorFactor[3] = Saturate(opacity);
    }

    if (material.pbr.metalness.has_value) {
        result.metallicFactor = Saturate(material.pbr.metalness.value_real, 0.0f);
    }
    if (material.pbr.roughness.has_value) {
        result.roughnessFactor = Saturate(material.pbr.roughness.value_real, 1.0f);
    }

    const bool haveEmission = material.pbr.emission_color.has_value
        || material.fbx.emission_color.has_value;
    if (haveEmission) {
        const bool usePbrEmission = material.pbr.emission_color.has_value;
        const ufbx_vec3 emission = usePbrEmission ? material.pbr.emission_color.value_vec3
                                                  : material.fbx.emission_color.value_vec3;
        const double emissionFactor = material.pbr.emission_factor.has_value
            ? material.pbr.emission_factor.value_real
            : (material.fbx.emission_factor.has_value
                   ? material.fbx.emission_factor.value_real
                   : 1.0);
        result.emissiveFactor[0] = SafeFloat(emission.x * emissionFactor);
        result.emissiveFactor[1] = SafeFloat(emission.y * emissionFactor);
        result.emissiveFactor[2] = SafeFloat(emission.z * emissionFactor);
    }

    result.alphaMode = result.baseColorFactor[3] < 0.999f
        ? static_cast<std::uint32_t>(AlphaModeId::Blend)
        : static_cast<std::uint32_t>(AlphaModeId::Opaque);

    // The preview path samples no texture, so FlipV is intentionally omitted.
    // FBX surfaces are frequently authored single-sided; keep the preview
    // double-sided unless the source explicitly culls back faces.
    result.flags = kMaterialFlagDoubleSided;
    if (material.features.double_sided.is_explicit && !material.features.double_sided.enabled) {
        result.flags &= ~kMaterialFlagDoubleSided;
    }
    if (material.features.unlit.is_explicit && material.features.unlit.enabled) {
        result.flags |= kMaterialFlagUnlit;
    }
    return result;
}

// --- Allowlisted embedded-image structural validation -----------------------

enum class ImageFormat { None, Png, Jpeg, Gif, Bmp, Webp };

struct ImageInfo {
    ImageFormat format = ImageFormat::None;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

std::uint32_t ReadBe32(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 24)
        | (static_cast<std::uint32_t>(p[1]) << 16)
        | (static_cast<std::uint32_t>(p[2]) << 8)
        | static_cast<std::uint32_t>(p[3]);
}

std::uint32_t ReadBe16(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 8) | static_cast<std::uint32_t>(p[1]);
}

std::uint32_t ReadLe16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
}

std::uint32_t ReadLe32(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
        | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool IsPng(const std::uint8_t* b, std::size_t n) noexcept
{
    static constexpr std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return n >= 24 && std::memcmp(b, sig, sizeof(sig)) == 0;
}

ImageInfo SniffPng(const std::uint8_t* b) noexcept
{
    ImageInfo info;
    info.format = ImageFormat::Png;
    info.width = ReadBe32(b + 16);
    info.height = ReadBe32(b + 20);
    return info;
}

bool IsGif(const std::uint8_t* b, std::size_t n) noexcept
{
    return n >= 10
        && (std::memcmp(b, "GIF87a", 6) == 0 || std::memcmp(b, "GIF89a", 6) == 0);
}

ImageInfo SniffGif(const std::uint8_t* b) noexcept
{
    ImageInfo info;
    info.format = ImageFormat::Gif;
    info.width = ReadLe16(b + 6);
    info.height = ReadLe16(b + 8);
    return info;
}

bool IsBmp(const std::uint8_t* b, std::size_t n) noexcept
{
    return n >= 26 && b[0] == 'B' && b[1] == 'M';
}

ImageInfo SniffBmp(const std::uint8_t* b) noexcept
{
    ImageInfo info;
    info.format = ImageFormat::Bmp;
    const std::int32_t width = static_cast<std::int32_t>(ReadLe32(b + 18));
    const std::int32_t height = static_cast<std::int32_t>(ReadLe32(b + 22));
    if (width <= 0 || height == 0) {
        return info;
    }
    info.width = static_cast<std::uint32_t>(width);
    info.height = static_cast<std::uint32_t>(height < 0 ? -height : height);
    return info;
}

bool IsWebp(const std::uint8_t* b, std::size_t n) noexcept
{
    return n >= 16 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0;
}

ImageInfo SniffWebp(const std::uint8_t* b, std::size_t n) noexcept
{
    ImageInfo info;
    info.format = ImageFormat::Webp;
    if (std::memcmp(b + 12, "VP8X", 4) == 0 && n >= 30) {
        info.width = (ReadLe32(b + 24) & 0x00FFFFFFu) + 1u;
        info.height = (ReadLe32(b + 27) & 0x00FFFFFFu) + 1u;
    } else if (std::memcmp(b + 12, "VP8L", 4) == 0 && n >= 25) {
        const std::uint32_t bits = ReadLe32(b + 21);
        info.width = (bits & 0x3FFFu) + 1u;
        info.height = ((bits >> 14) & 0x3FFFu) + 1u;
    } else if (std::memcmp(b + 12, "VP8 ", 4) == 0 && n >= 30) {
        info.width = ReadLe16(b + 26) & 0x3FFFu;
        info.height = ReadLe16(b + 28) & 0x3FFFu;
    }
    return info;
}

bool IsJpeg(const std::uint8_t* b, std::size_t n) noexcept
{
    return n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
}

ImageInfo SniffJpeg(const std::uint8_t* b, std::size_t n) noexcept
{
    ImageInfo info;
    info.format = ImageFormat::Jpeg;
    std::size_t pos = 2;
    while (pos + 3 < n) {
        if (b[pos] != 0xFF) {
            break;
        }
        const std::uint8_t marker = b[pos + 1];
        pos += 2;
        if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (pos + 1 >= n) {
            break;
        }
        const std::uint32_t length = ReadBe16(b + pos);
        if (length < 2 || pos + length > n) {
            break;
        }
        const bool isSof = (marker >= 0xC0 && marker <= 0xC3)
            || (marker >= 0xC5 && marker <= 0xC7)
            || (marker >= 0xC9 && marker <= 0xCB)
            || (marker >= 0xCD && marker <= 0xCF);
        if (isSof && length >= 7) {
            info.height = ReadBe16(b + pos + 3);
            info.width = ReadBe16(b + pos + 5);
            break;
        }
        pos += length;
    }
    return info;
}

ImageInfo SniffImage(std::span<const std::byte> bytes) noexcept
{
    const auto* b = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::size_t n = bytes.size();
    if (IsPng(b, n)) {
        return SniffPng(b);
    }
    if (IsJpeg(b, n)) {
        return SniffJpeg(b, n);
    }
    if (IsGif(b, n)) {
        return SniffGif(b);
    }
    if (IsBmp(b, n)) {
        return SniffBmp(b);
    }
    if (IsWebp(b, n)) {
        return SniffWebp(b, n);
    }
    return ImageInfo{};
}

} // namespace

FbxAdapter::~FbxAdapter() noexcept { FreeScene(); }

void FbxAdapter::FreeScene() noexcept
{
    if (scene_ != nullptr) {
        ufbx_free_scene(scene_);
        scene_ = nullptr;
    }
}

void FbxAdapter::Reset() noexcept
{
    FreeScene();
    input_ = AdapterInput{};
    parsed_ = false;
    evaluationRan_ = false;
    externalFileDetected_ = false;
    omittedUnsupportedGeometry_ = false;
    hasVertexColors_ = false;
    evaluatedTriangles_ = 0;
    embeddedImageCount_ = 0;
    embeddedImagePixels_ = 0;
    bytes_ = {};
    ownedBytes_.clear();
    ownedBytes_.shrink_to_fit();
    bytesReservation_.reset();
    materials_.clear();
    materialIndex_.clear();
    whiteMaterialIndex_ = 0;
    haveOrigin_ = false;
    origin_[0] = origin_[1] = origin_[2] = 0.0;
}

ErrorCode FbxAdapter::Initialize(const AdapterInput& input) noexcept
{
    Reset();
    if (input.source == nullptr || input.limits == nullptr || input.deadline == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    input_ = input;
    return ErrorCode::None;
}

ErrorCode FbxAdapter::SourceReadFailure() const noexcept
{
    if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    return ErrorCode::MalformedData;
}

ErrorCode FbxAdapter::LoadSourceBytes() noexcept
{
    bytes_ = input_.source->ContiguousView();
    if (!bytes_.empty()) {
        return ErrorCode::None;
    }
    const std::uint64_t size = input_.source->Size();
    if (size == 0) {
        return ErrorCode::MalformedData;
    }
    if (size > ProviderLimits::kContiguousBackingMaxBytes) {
        return ErrorCode::ResourceLimit;
    }
    if (input_.ledger != nullptr) {
        auto reservation = input_.ledger->ReserveScoped(size);
        if (!reservation.has_value()) {
            return ErrorCode::ResourceLimit;
        }
        bytesReservation_ = std::move(reservation);
    }
    ownedBytes_.resize(static_cast<std::size_t>(size));
    if (!input_.source->ReadAt(0, ownedBytes_)) {
        ownedBytes_.clear();
        return SourceReadFailure();
    }
    bytes_ = std::span<const std::byte>(ownedBytes_.data(), ownedBytes_.size());
    return ErrorCode::None;
}

ErrorCode FbxAdapter::Preflight() noexcept
{
    if (scene_->nodes.count > ProviderLimits::kNodesMax
        || scene_->meshes.count > ProviderLimits::kNodesMax
        || scene_->materials.count > ProviderLimits::kMaterialsMax
        || scene_->anim_stacks.count > ProviderLimits::kNodesMax
        || scene_->skin_deformers.count > ProviderLimits::kNodesMax
        || scene_->bones.count > ProviderLimits::kNodesMax) {
        return ErrorCode::ResourceLimit;
    }

    std::uint64_t triangles = 0;
    std::uint64_t vertices = 0;
    bool hasSupportedGeometry = false;
    bool hasSubdivision = false;
    for (const ufbx_mesh* mesh : scene_->meshes) {
        if (mesh == nullptr) {
            continue;
        }
        if (mesh->num_triangles > ProviderLimits::kTrianglesInspectedMax) {
            return ErrorCode::ResourceLimit;
        }
        const auto nextTriangles = CheckedAdd(triangles, mesh->num_triangles);
        const auto nextVertices = CheckedAdd(vertices, mesh->num_vertices);
        if (!nextTriangles.has_value() || !nextVertices.has_value()) {
            return ErrorCode::ResourceLimit;
        }
        triangles = *nextTriangles;
        vertices = *nextVertices;
        if (triangles > ProviderLimits::kTrianglesInspectedMax
            || vertices > ProviderLimits::kPointsInspectedMax) {
            return ErrorCode::ResourceLimit;
        }
        if (mesh->faces.count != 0 && mesh->vertex_position.exists) {
            hasSupportedGeometry = true;
        }
        hasSubdivision |= mesh->subdivision_preview_levels != 0
            || mesh->subdivision_render_levels != 0 || mesh->subdivision_evaluated
            || mesh->from_tessellated_nurbs;
    }
    evaluatedTriangles_ = triangles;

    const bool hasCaches = scene_->cache_deformers.count != 0
        || scene_->cache_files.count != 0;
    const bool hasNurbs = scene_->nurbs_curves.count != 0
        || scene_->nurbs_surfaces.count != 0
        || scene_->nurbs_trim_surfaces.count != 0
        || scene_->nurbs_trim_boundaries.count != 0;
    const bool hasProcedural = scene_->procedural_geometries.count != 0;
    const bool hasUnsupported = hasCaches || hasNurbs || hasSubdivision || hasProcedural;

    if (!hasSupportedGeometry) {
        // NURBS/subdivision/procedural/cache-only content has no supported
        // polygon to render; Explorer's generic icon is the documented result.
        return hasUnsupported ? ErrorCode::UnsupportedRequiredFeature
                              : ErrorCode::EmptyGeometry;
    }
    omittedUnsupportedGeometry_ = hasUnsupported;
    return ErrorCode::None;
}

ErrorCode FbxAdapter::EvaluateStaticPose() noexcept
{
    const ufbx_anim* animation = scene_->anim;
    double time = 0.0;
    if (scene_->anim_stacks.count != 0) {
        const ufbx_anim_stack* first = scene_->anim_stacks.data[0];
        if (first == nullptr || first->anim == nullptr || !Finite(first->time_begin)) {
            return ErrorCode::MalformedData;
        }
        animation = first->anim;
        time = first->time_begin;
    }
    if (animation == nullptr) {
        return ErrorCode::MalformedData;
    }

    const std::uint64_t scratch = ProviderLimits::kAccountedScratchMaxBytes;
    ufbx_evaluate_opts options{};
    options.temp_allocator.memory_limit = static_cast<std::size_t>(scratch / 2);
    options.result_allocator.memory_limit = static_cast<std::size_t>(scratch / 2);
    options.temp_allocator.allocation_limit = 1'000'000;
    options.result_allocator.allocation_limit = 1'000'000;
    options.evaluate_skinning = true;
    options.evaluate_caches = false;
    options.evaluate_flags = 0;
    // ufbx 0.23.0 has no evaluation progress callback. The call is bounded by
    // the pre-evaluation count checks above plus these explicit allocator limits.
    options.load_external_files = false;
    options.open_file_cb.fn = DenyExternalFile;
    options.open_file_cb.user = &externalFileDetected_;

    ufbx_error error{};
    ufbx_scene* evaluated = ufbx_evaluate_scene(scene_, animation, time, &options, &error);
    if (evaluated == nullptr) {
        return MapLoadError(error);
    }
    ufbx_free_scene(scene_);
    scene_ = evaluated;
    evaluationRan_ = true;
    return ErrorCode::None;
}

void FbxAdapter::BuildMaterials() noexcept
{
    materials_.clear();
    materialIndex_.clear();
    whiteMaterialIndex_ = 0;
    hasVertexColors_ = false;
    for (const ufbx_node* node : scene_->nodes) {
        if (node != nullptr && node->mesh != nullptr && node->mesh->vertex_color.exists) {
            hasVertexColors_ = true;
            break;
        }
    }
    if (hasVertexColors_) {
        model_core::MaterialPayload white = NeutralMaterial();
        white.baseColorFactor[0] = 1.0f;
        white.baseColorFactor[1] = 1.0f;
        white.baseColorFactor[2] = 1.0f;
        white.baseColorFactor[3] = 1.0f;
        materials_.push_back(white);
        whiteMaterialIndex_ = 1;
    }
    for (const ufbx_material* material : scene_->materials) {
        if (material == nullptr) {
            continue;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(materials_.size()) + 1u;
        materials_.push_back(ConvertMaterial(*material));
        materialIndex_.emplace(material, index);
    }
}

ErrorCode FbxAdapter::ValidateEmbeddedImages() noexcept
{
    std::uint64_t remaining = ProviderLimits::kDecodedTexturePixelsMax;
    for (const ufbx_texture* texture : scene_->textures) {
        if (texture == nullptr) {
            continue;
        }
        const ufbx_blob& blob = texture->content;
        if (blob.data == nullptr || blob.size == 0) {
            continue; // external texture reference: neutral fallback
        }
        if (blob.size > kEmbeddedImageEncodedMaxBytes) {
            continue;
        }
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(blob.data), blob.size);
        const ImageInfo info = SniffImage(bytes);
        if (info.format == ImageFormat::None || info.width == 0 || info.height == 0) {
            continue;
        }
        const auto pixels = CheckedMultiply(static_cast<std::uint64_t>(info.width),
                                            static_cast<std::uint64_t>(info.height));
        if (!pixels.has_value() || *pixels == 0 || *pixels > remaining) {
            continue;
        }
        remaining -= *pixels;
        embeddedImagePixels_ += *pixels;
        ++embeddedImageCount_;
    }
    return ErrorCode::None;
}

ErrorCode FbxAdapter::Parse() noexcept
{
    if (input_.deadline == nullptr || !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    parsed_ = false;
    FreeScene();
    evaluationRan_ = false;
    externalFileDetected_ = false;
    omittedUnsupportedGeometry_ = false;
    hasVertexColors_ = false;
    evaluatedTriangles_ = 0;
    embeddedImageCount_ = 0;
    embeddedImagePixels_ = 0;
    materials_.clear();
    materialIndex_.clear();
    whiteMaterialIndex_ = 0;
    haveOrigin_ = false;
    origin_[0] = origin_[1] = origin_[2] = 0.0;

    ErrorCode load = LoadSourceBytes();
    if (load != ErrorCode::None) {
        return load;
    }
    if (bytes_.size() > ProviderLimits::kStreamMaxBytes) {
        return ErrorCode::ResourceLimit;
    }

    const std::uint64_t scratch = ProviderLimits::kAccountedScratchMaxBytes;
    ufbx_load_opts options{};
    // FBX has both an ASCII and a binary encoding; the provider knows the family
    // from the CLSID and lets ufbx detect the encoding from the stream content
    // only. The synthesized filename never reaches the filesystem.
    options.file_format = UFBX_FILE_FORMAT_UNKNOWN;
    options.no_format_from_content = false;
    options.no_format_from_extension = true;
    options.filename = ufbx_string{"document.fbx", 12};
    options.ignore_animation = false;
    options.ignore_embedded = false;
    options.evaluate_skinning = true;
    options.evaluate_caches = false;
    options.load_external_files = false;
    options.ignore_missing_external_files = true;
    options.skip_skin_vertices = false;
    options.clean_skin_weights = true;
    options.force_single_thread_ascii_parsing = true;
    options.generate_missing_normals = true;
    options.normalize_normals = true;
    options.normalize_tangents = true;
    options.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    options.unicode_error_handling = UFBX_UNICODE_ERROR_HANDLING_ABORT_LOADING;
    options.node_depth_limit = 256;
    options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_PRESERVE;
    options.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
    options.pivot_handling = UFBX_PIVOT_HANDLING_RETAIN;
    options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0;
    options.temp_allocator.memory_limit = static_cast<std::size_t>(scratch / 2);
    options.result_allocator.memory_limit = static_cast<std::size_t>(scratch / 2);
    options.temp_allocator.allocation_limit = 1'000'000;
    options.result_allocator.allocation_limit = 1'000'000;
    options.open_file_cb.fn = DenyExternalFile;
    options.open_file_cb.user = &externalFileDetected_;
    options.progress_cb.fn = CheckProgress;
    options.progress_cb.user = input_.deadline;
    options.progress_interval_hint = 1u << 20;

    ufbx_error error{};
    scene_ = ufbx_load_memory(bytes_.data(), bytes_.size(), &options, &error);
    if (scene_ == nullptr) {
        return MapLoadError(error);
    }
    // Do not let a mis-routed non-FBX stream parse as another grammar.
    if (scene_->metadata.file_format != UFBX_FILE_FORMAT_FBX) {
        FreeScene();
        return ErrorCode::UnsupportedFormat;
    }
    if (!input_.deadline->Checkpoint()) {
        FreeScene();
        return ErrorCode::Cancelled;
    }

    ErrorCode preflight = Preflight();
    if (preflight != ErrorCode::None) {
        FreeScene();
        return preflight;
    }
    if (!input_.deadline->Checkpoint()) {
        FreeScene();
        return ErrorCode::Cancelled;
    }

    ErrorCode evaluated = EvaluateStaticPose();
    if (evaluated != ErrorCode::None) {
        FreeScene();
        return evaluated;
    }
    if (!input_.deadline->Checkpoint()) {
        FreeScene();
        return ErrorCode::Cancelled;
    }

    ErrorCode images = ValidateEmbeddedImages();
    if (images != ErrorCode::None) {
        FreeScene();
        return images;
    }

    BuildMaterials();
    parsed_ = true;
    return ErrorCode::None;
}

ErrorCode FbxAdapter::EnumerateMaterials(IMaterialSink& sink) noexcept
{
    if (!parsed_) {
        return ErrorCode::InternalImporterFailure;
    }
    for (std::size_t i = 0; i < materials_.size(); ++i) {
        if (!sink.OnMaterial(static_cast<std::uint32_t>(i) + 1u, materials_[i])) {
            return ErrorCode::None;
        }
    }
    return ErrorCode::None;
}

ErrorCode FbxAdapter::EnumerateGeometry(IGeometrySink& sink) noexcept
{
    if (!parsed_ || scene_ == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }

    std::vector<std::uint32_t> triangles;
    std::uint64_t faceCounter = 0;
    std::uint64_t triangleCounter = 0;

    for (const ufbx_node* node : scene_->nodes) {
        if (node == nullptr || node->mesh == nullptr || !InheritedVisible(node)) {
            continue;
        }
        const ufbx_mesh& mesh = *node->mesh;
        if (mesh.faces.count == 0 || !mesh.skinned_position.exists || mesh.num_indices == 0) {
            continue;
        }

        // Evaluated attributes are authoritative (FBX-001). When
        // `skinned_is_local` the vertex is in geometry space and must be moved
        // to world by `geometry_to_world`; otherwise it is already world space.
        ufbx_matrix transform{};
        if (mesh.skinned_is_local) {
            transform = node->geometry_to_world;
        } else {
            transform.m00 = 1.0;
            transform.m11 = 1.0;
            transform.m22 = 1.0;
        }
        const ufbx_matrix normalTransform = ufbx_matrix_for_normals(&transform);
        const bool hasNormals = mesh.skinned_normal.exists;
        const bool hasColors = mesh.vertex_color.exists;

        for (std::size_t faceIndex = 0; faceIndex < mesh.faces.count; ++faceIndex) {
            if (((++faceCounter) & (kCheckpointFaces - 1)) == 0
                && !input_.deadline->Checkpoint()) {
                return ErrorCode::Cancelled;
            }
            const ufbx_face face = mesh.faces.data[faceIndex];
            if (face.num_indices < 3) {
                continue;
            }
            const std::uint64_t perFaceTriangles =
                static_cast<std::uint64_t>(face.num_indices) - 2;
            if (perFaceTriangles > kMaxTrianglesPerFace) {
                return ErrorCode::ResourceLimit;
            }
            const std::size_t required = static_cast<std::size_t>(perFaceTriangles) * 3;
            if (triangles.size() < required) {
                triangles.resize(required);
            }
            const std::uint32_t produced =
                static_cast<std::uint32_t>(ufbx_triangulate_face(triangles.data(), required,
                                                                 &mesh, face));
            if (produced != perFaceTriangles) {
                return ErrorCode::MalformedData;
            }

            std::uint32_t faceMaterial = 0;
            if (mesh.face_material.data != nullptr && faceIndex < mesh.face_material.count) {
                faceMaterial = mesh.face_material.data[faceIndex];
            }
            const ufbx_material* material = nullptr;
            if (node->materials.data != nullptr && faceMaterial < node->materials.count) {
                material = node->materials.data[faceMaterial];
            } else if (mesh.materials.data != nullptr && faceMaterial < mesh.materials.count) {
                material = mesh.materials.data[faceMaterial];
            }
            std::uint32_t materialIndex = 0;
            const auto it = materialIndex_.find(material);
            if (material != nullptr && it != materialIndex_.end()) {
                materialIndex = it->second;
            } else if (hasColors && whiteMaterialIndex_ != 0) {
                materialIndex = whiteMaterialIndex_;
            }

            for (std::uint32_t triangle = 0; triangle < produced; ++triangle) {
                if (((++triangleCounter) & (kCheckpointTriangles - 1)) == 0
                    && !input_.deadline->Checkpoint()) {
                    return ErrorCode::Cancelled;
                }
                TriangleSample sample{};
                const int cornerOrder[3] = {0, mesh.reversed_winding ? 2 : 1,
                                            mesh.reversed_winding ? 1 : 2};
                bool finite = true;
                for (int corner = 0; corner < 3; ++corner) {
                    const std::uint32_t index =
                        triangles[static_cast<std::size_t>(triangle) * 3
                                  + static_cast<std::size_t>(corner)];
                    if (index >= mesh.num_indices) {
                        finite = false;
                        break;
                    }
                    const ufbx_vec3 position = ufbx_transform_position(
                        &transform, ufbx_get_vertex_vec3(&mesh.skinned_position, index));
                    if (!Finite3(position)) {
                        finite = false;
                        break;
                    }
                    VertexSample& vertex = sample.vertices[cornerOrder[corner]];
                    if (!haveOrigin_) {
                        origin_[0] = position.x;
                        origin_[1] = position.y;
                        origin_[2] = position.z;
                        haveOrigin_ = true;
                    }
                    vertex.position[0] = static_cast<float>(position.x - origin_[0]);
                    vertex.position[1] = static_cast<float>(position.y - origin_[1]);
                    vertex.position[2] = static_cast<float>(position.z - origin_[2]);
                    if (hasNormals) {
                        const ufbx_vec3 normal = ufbx_transform_direction(
                            &normalTransform,
                            ufbx_get_vertex_vec3(&mesh.skinned_normal, index));
                        NormalizeInto(normal, vertex.normal);
                    }
                    if (hasColors) {
                        const ufbx_vec4 color = ufbx_get_vertex_vec4(&mesh.vertex_color, index);
                        vertex.color[0] = Saturate(color.x);
                        vertex.color[1] = Saturate(color.y);
                        vertex.color[2] = Saturate(color.z);
                        vertex.color[3] = Saturate(color.w);
                    }
                }
                if (!finite) {
                    continue; // degenerate/non-finite triangle dropped locally
                }
                for (int axis = 0; axis < 3; ++axis) {
                    sample.origin[axis] = origin_[axis];
                }
                sample.materialIndex = materialIndex;
                if (!sink.OnTriangle(sample)) {
                    return ErrorCode::None; // sampler inspect-cap stop, not a failure
                }
            }
        }
    }
    return ErrorCode::None;
}

} // namespace preview3d::provider
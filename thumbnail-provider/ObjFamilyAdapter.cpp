// T24 OBJ family adapter implementation (see ObjFamilyAdapter.h).

#include "ObjFamilyAdapter.h"

#include "ContainmentStage.h"
#include "Deadline.h"
#include "ProviderLimits.h"

#include "ufbx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace preview3d::provider {
namespace {

// Per-face triangulation ceiling. OBJ n-gons are bounded well below this in
// practice; above it the source is treated as a resource limit rather than
// allocating an unbounded scratch polygon. The T14 sampler's inspect cap still
// ends enumeration on the total triangle count.
constexpr std::uint64_t kMaxTrianglesPerFace = 65'536;

// Per-face deadline checkpoint interval.
constexpr std::uint64_t kCheckpointFaces = 1024;

bool Finite(double value) noexcept { return std::isfinite(value); }

bool Finite3(ufbx_vec3 value) noexcept
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

float Saturate(double value) noexcept
{
    if (!Finite(value)) {
        return 1.0f;
    }
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
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

// Deny every implicit external file (`.mtl`, texture, geometry cache). The
// provider has no filesystem capability and a stream-only OBJ must never open a
// path. `user` points at ObjAdapter::externalFileDenied_ so a test can observe
// that a reference was denied instead of silently ignored.
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
            return ErrorCode::ResourceLimit;
        case UFBX_ERROR_CANCELLED:
            return ErrorCode::Cancelled;
        case UFBX_ERROR_NODE_DEPTH_LIMIT:
            return ErrorCode::ResourceLimit;
        default:
            return ErrorCode::MalformedData;
    }
}

} // namespace

ObjAdapter::~ObjAdapter() noexcept { FreeScene(); }

void ObjAdapter::FreeScene() noexcept
{
    if (scene_ != nullptr) {
        ufbx_free_scene(scene_);
        scene_ = nullptr;
    }
}

void ObjAdapter::Reset() noexcept
{
    FreeScene();
    input_ = AdapterInput{};
    parsed_ = false;
    externalFileDenied_ = false;
    hasVertexColors_ = false;
    haveOrigin_ = false;
    origin_[0] = origin_[1] = origin_[2] = 0.0;
    bytes_ = {};
    ownedBytes_.clear();
    ownedBytes_.shrink_to_fit();
    bytesReservation_.reset();
}

ErrorCode ObjAdapter::Initialize(const AdapterInput& input) noexcept
{
    return RunContainedStageMember([this, &input]() { return InitializeImpl(input); },
                                   DiagnosticStage::AdapterInitialize);
}

ErrorCode ObjAdapter::InitializeImpl(const AdapterInput& input)
{
    Reset();
    if (input.source == nullptr || input.limits == nullptr || input.deadline == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    input_ = input;
    return ErrorCode::None;
}

ErrorCode ObjAdapter::SourceReadFailure() const noexcept
{
    if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    return ErrorCode::MalformedData;
}

ErrorCode ObjAdapter::Parse() noexcept
{
    return RunContainedStageMember([this]() { return ParseImpl(); }, DiagnosticStage::Parse);
}

ErrorCode ObjAdapter::ParseImpl()
{
    if (!input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    parsed_ = false;
    FreeScene();
    hasVertexColors_ = false;
    externalFileDenied_ = false;
    haveOrigin_ = false;

    // OBJ is a text grammar ufbx parses in one contiguous pass. Use the frozen
    // contiguous view when the Shell stream exposed one; otherwise copy into a
    // checked, ledger-charged backing buffer bounded by the 128 MiB cap.
    bytes_ = input_.source->ContiguousView();
    if (bytes_.empty()) {
        const std::uint64_t size = input_.source->Size();
        if (size == 0) {
            return ErrorCode::MalformedData;
        }
        if (size > ProviderLimits::kContiguousBackingMaxBytes) {
            return ErrorCode::ResourceLimit;
        }
        std::optional<AllocationReservation> reservation;
        if (input_.ledger != nullptr) {
            reservation = input_.ledger->ReserveScoped(size);
            if (!reservation.has_value()) {
                return ErrorCode::ResourceLimit;
            }
        }
        ownedBytes_.resize(static_cast<std::size_t>(size));
        if (!input_.source->ReadAt(0, ownedBytes_)) {
            ownedBytes_.clear();
            return SourceReadFailure();
        }
        bytes_ = std::span<const std::byte>(ownedBytes_.data(), ownedBytes_.size());
        bytesReservation_ = std::move(reservation);
    }
    if (bytes_.size() > ProviderLimits::kStreamMaxBytes) {
        return ErrorCode::ResourceLimit;
    }

    const std::uint64_t scratch = ProviderLimits::kAccountedScratchMaxBytes;
    ufbx_load_opts options{};
    options.temp_allocator.memory_limit = static_cast<std::size_t>(scratch / 2);
    options.result_allocator.memory_limit = static_cast<std::size_t>(scratch / 2);
    options.temp_allocator.allocation_limit = 1'000'000;
    options.result_allocator.allocation_limit = 1'000'000;
    options.file_format = UFBX_FILE_FORMAT_OBJ;
    options.no_format_from_content = true;
    options.no_format_from_extension = true;
    options.filename = ufbx_string{"document.obj", 12};
    // The provider never resolves a sidecar: mtllib and textures are ignored
    // and the deny hook is the second line of defence.
    options.load_external_files = false;
    options.ignore_missing_external_files = true;
    options.ignore_animation = true;
    options.ignore_embedded = true;
    options.generate_missing_normals = true;
    options.normalize_normals = true;
    options.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    options.unicode_error_handling = UFBX_UNICODE_ERROR_HANDLING_ABORT_LOADING;
    options.node_depth_limit = 256;
    options.open_file_cb.fn = DenyExternalFile;
    options.open_file_cb.user = &externalFileDenied_;
    options.progress_cb.fn = CheckProgress;
    options.progress_cb.user = input_.deadline;
    options.progress_interval_hint = 1u << 20;

    ufbx_error error{};
    scene_ = ufbx_load_memory(bytes_.data(), bytes_.size(), &options, &error);
    if (scene_ == nullptr) {
        return MapLoadError(error);
    }
    if (!input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }

    if (scene_->nodes.count > ProviderLimits::kNodesMax ||
        scene_->meshes.count > ProviderLimits::kNodesMax ||
        scene_->materials.count > ProviderLimits::kMaterialsMax) {
        return ErrorCode::ResourceLimit;
    }

    bool hasGeometry = false;
    std::uint64_t triangleCount = 0;
    for (std::size_t i = 0; i < scene_->meshes.count; ++i) {
        const ufbx_mesh* mesh = scene_->meshes.data[i];
        if (mesh == nullptr) {
            continue;
        }
        if (mesh->vertex_color.exists) {
            hasVertexColors_ = true;
        }
        if (mesh->vertex_position.exists && mesh->num_indices != 0 && mesh->num_triangles != 0) {
            hasGeometry = true;
            triangleCount += mesh->num_triangles;
        }
    }
    if (!hasGeometry || triangleCount == 0) {
        return ErrorCode::EmptyGeometry;
    }

    parsed_ = true;
    return ErrorCode::None;
}

ErrorCode ObjAdapter::EnumerateMaterials(IMaterialSink& sink) noexcept
{
    return RunContainedStageMember([this, &sink]() { return EnumerateMaterialsImpl(sink); },
                                   DiagnosticStage::Materials);
}

ErrorCode ObjAdapter::EnumerateMaterialsImpl(IMaterialSink& sink)
{
    // OBJ material records and their `.mtl` are deliberately ignored: the
    // thumbnail path renders the neutral palette. When the geometry carries
    // vertex colors the base color is forced white so `vertexColor * baseColor`
    // preserves the source (CpuRasterizer.h, "Material resolution").
    model_core::MaterialPayload material = NeutralMaterial();
    if (hasVertexColors_) {
        material.baseColorFactor[0] = 1.0f;
        material.baseColorFactor[1] = 1.0f;
        material.baseColorFactor[2] = 1.0f;
        material.baseColorFactor[3] = 1.0f;
    }
    sink.OnMaterial(1, material);
    return ErrorCode::None;
}

ErrorCode ObjAdapter::EnumerateGeometry(IGeometrySink& sink) noexcept
{
    return RunContainedStageMember([this, &sink]() { return EnumerateGeometryImpl(sink); },
                                   DiagnosticStage::Geometry);
}

ErrorCode ObjAdapter::EnumerateGeometryImpl(IGeometrySink& sink)
{
    if (!parsed_ || scene_ == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }

    std::vector<std::uint32_t> triangles;
    std::uint64_t faceCounter = 0;

    for (std::size_t nodeIndex = 0; nodeIndex < scene_->nodes.count; ++nodeIndex) {
        const ufbx_node* node = scene_->nodes.data[nodeIndex];
        if (node == nullptr || node->mesh == nullptr || !node->visible) {
            continue;
        }
        const ufbx_mesh& mesh = *node->mesh;
        if (!mesh.vertex_position.exists || mesh.num_indices == 0) {
            continue;
        }
        const ufbx_matrix transform = node->geometry_to_world;
        const ufbx_matrix normalTransform = ufbx_matrix_for_normals(&transform);
        const bool hasNormals = mesh.vertex_normal.exists;
        const bool hasColors = mesh.vertex_color.exists;

        for (std::size_t faceIndex = 0; faceIndex < mesh.faces.count; ++faceIndex) {
            if (((++faceCounter) & (kCheckpointFaces - 1)) == 0 && !input_.deadline->Checkpoint()) {
                return ErrorCode::Cancelled;
            }
            const ufbx_face face = mesh.faces.data[faceIndex];
            if (face.num_indices < 3) {
                continue;
            }
            const std::uint64_t triangleCount = static_cast<std::uint64_t>(face.num_indices) - 2;
            if (triangleCount > kMaxTrianglesPerFace) {
                return ErrorCode::ResourceLimit;
            }
            const std::size_t required = static_cast<std::size_t>(triangleCount) * 3;
            if (triangles.size() < required) {
                triangles.resize(required);
            }
            const std::uint32_t produced =
                ufbx_triangulate_face(triangles.data(), required, &mesh, face);
            if (produced != triangleCount) {
                return ErrorCode::MalformedData;
            }

            for (std::uint32_t triangle = 0; triangle < produced; ++triangle) {
                TriangleSample sample{};
                bool finite = true;
                for (int corner = 0; corner < 3; ++corner) {
                    const std::uint32_t index =
                        triangles[static_cast<std::size_t>(triangle) * 3 + corner];
                    if (index >= mesh.num_indices) {
                        finite = false;
                        break;
                    }
                    const ufbx_vec3 position = ufbx_transform_position(
                        &transform, ufbx_get_vertex_vec3(&mesh.vertex_position, index));
                    if (!Finite3(position)) {
                        finite = false;
                        break;
                    }
                    if (!haveOrigin_) {
                        origin_[0] = position.x;
                        origin_[1] = position.y;
                        origin_[2] = position.z;
                        haveOrigin_ = true;
                    }
                    sample.vertices[corner].position[0] =
                        static_cast<float>(position.x - origin_[0]);
                    sample.vertices[corner].position[1] =
                        static_cast<float>(position.y - origin_[1]);
                    sample.vertices[corner].position[2] =
                        static_cast<float>(position.z - origin_[2]);
                    for (int axis = 0; axis < 3; ++axis) {
                        sample.origin[axis] = origin_[axis];
                    }
                    if (hasNormals) {
                        const ufbx_vec3 normal = ufbx_transform_direction(
                            &normalTransform, ufbx_get_vertex_vec3(&mesh.vertex_normal, index));
                        NormalizeInto(normal, sample.vertices[corner].normal);
                    }
                    if (hasColors) {
                        const ufbx_vec4 color = ufbx_get_vertex_vec4(&mesh.vertex_color, index);
                        sample.vertices[corner].color[0] = Saturate(color.x);
                        sample.vertices[corner].color[1] = Saturate(color.y);
                        sample.vertices[corner].color[2] = Saturate(color.z);
                        sample.vertices[corner].color[3] = Saturate(color.w);
                    }
                }
                if (!finite) {
                    continue; // degenerate/non-finite triangle dropped locally
                }
                sample.materialIndex = 1;
                if (!sink.OnTriangle(sample)) {
                    return ErrorCode::None; // sampler inspect-cap stop, not a failure
                }
            }
        }
    }
    return ErrorCode::None;
}

} // namespace preview3d::provider
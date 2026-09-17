#include "import_broker/SharedSectionValidator.h"
#include "model_core/WireFormat.h"
#include "ufbx.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string_view>

namespace {

constexpr uint32_t kEnvelopeMagic = 0x5a584246; // "FBXZ"
constexpr size_t kMemoryLimit = 64u * 1024u * 1024u;
constexpr size_t kInputLimit = 16u * 1024u * 1024u;
constexpr size_t kSidecarLimit = 1u * 1024u * 1024u;
constexpr size_t kElementWalkLimit = 1u * 1024u * 1024u;

#pragma pack(push, 1)
struct Envelope {
    uint32_t magic;
    uint8_t flags;
    uint8_t cancelAfter;
    uint16_t pathBytes;
    uint32_t primaryBytes;
};
#pragma pack(pop)
static_assert(sizeof(Envelope) == 12);

enum : uint8_t {
    kProtocol = 1u << 0,
    kExternalFiles = 1u << 1,
    kEvaluate = 1u << 2,
    kCancel = 1u << 3,
};

struct Input {
    uint8_t flags = kEvaluate;
    uint8_t cancelAfter = 0;
    std::span<const std::byte> primary;
    std::string_view sidecarPath;
    std::span<const std::byte> sidecar;
};

Input Decode(std::span<const std::byte> bytes)
{
    Input result{};
    if (bytes.size() < sizeof(Envelope)) {
        result.primary = bytes;
        return result;
    }
    Envelope envelope{};
    std::memcpy(&envelope, bytes.data(), sizeof(envelope));
    if (envelope.magic != kEnvelopeMagic) {
        result.primary = bytes;
        return result;
    }
    result.flags = envelope.flags;
    result.cancelAfter = envelope.cancelAfter;
    size_t offset = sizeof(envelope);
    const size_t primaryBytes = (std::min)({size_t(envelope.primaryBytes),
                                            bytes.size() - offset, kInputLimit});
    result.primary = bytes.subspan(offset, primaryBytes);
    offset += primaryBytes;
    const size_t pathBytes = (std::min)(size_t(envelope.pathBytes), bytes.size() - offset);
    result.sidecarPath = {reinterpret_cast<const char*>(bytes.data() + offset), pathBytes};
    offset += pathBytes;
    result.sidecar = bytes.subspan(offset, (std::min)(bytes.size() - offset, kSidecarLimit));
    return result;
}

struct Stream {
    std::span<const std::byte> bytes;
    size_t offset = 0;
};

size_t Read(void* user, void* destination, size_t size)
{
    auto& stream = *static_cast<Stream*>(user);
    const size_t count = (std::min)(size, stream.bytes.size() - stream.offset);
    if (count) std::memcpy(destination, stream.bytes.data() + stream.offset, count);
    stream.offset += count;
    return count;
}

bool Skip(void* user, size_t size)
{
    auto& stream = *static_cast<Stream*>(user);
    if (size > stream.bytes.size() - stream.offset) return false;
    stream.offset += size;
    return true;
}

uint64_t Size(void* user) { return static_cast<Stream*>(user)->bytes.size(); }
void Close(void* user) { delete static_cast<Stream*>(user); }

struct Callbacks {
    Input input;
    uint32_t progressCalls = 0;
};

bool OpenVirtual(void* user, ufbx_stream* stream, const char* path, size_t pathLength,
                 const ufbx_open_file_info*)
{
    auto& callbacks = *static_cast<Callbacks*>(user);
    if (!path || pathLength != callbacks.input.sidecarPath.size()
        || std::string_view(path, pathLength) != callbacks.input.sidecarPath
        || callbacks.input.sidecar.empty()) return false;
    auto* source = new (std::nothrow) Stream{callbacks.input.sidecar, 0};
    if (!source) return false;
    stream->read_fn = Read;
    stream->skip_fn = Skip;
    stream->size_fn = Size;
    stream->close_fn = Close;
    stream->user = source;
    return true;
}

ufbx_progress_result Progress(void* user, const ufbx_progress*)
{
    auto& callbacks = *static_cast<Callbacks*>(user);
    ++callbacks.progressCalls;
    return (callbacks.input.flags & kCancel)
        && callbacks.progressCalls > callbacks.input.cancelAfter
        ? UFBX_PROGRESS_CANCEL : UFBX_PROGRESS_CONTINUE;
}

struct SceneDeleter {
    void operator()(ufbx_scene* scene) const noexcept { ufbx_free_scene(scene); }
};
using Scene = std::unique_ptr<ufbx_scene, SceneDeleter>;

uint64_t Hash(uint64_t hash, double value)
{
    if (!std::isfinite(value) || std::abs(value) > 1.0e30) return hash ^ UINT64_MAX;
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (hash ^ bits) * 1099511628211ull;
}

void WalkNormalized(const ufbx_scene& scene)
{
    uint64_t hash = 1469598103934665603ull;
    size_t walked = 0;
    for (const ufbx_node* node : scene.nodes) {
        if (!node || ++walked > kElementWalkLimit) break;
        for (double value : node->node_to_world.v) hash = Hash(hash, value);
        if (!node->mesh) continue;
        const ufbx_mesh& mesh = *node->mesh;
        // ufbx vertex attributes are indexed by polygon-corner index, not by
        // control-point count. This deliberately mirrors the product's
        // triangulated source-index walk.
        const size_t vertices = (std::min)({mesh.num_indices,
            mesh.skinned_position.indices.count, kElementWalkLimit - walked});
        for (size_t index = 0; index < vertices; ++index) {
            const ufbx_vec3 position = ufbx_get_vertex_vec3(&mesh.skinned_position, index);
            hash = Hash(Hash(Hash(hash, position.x), position.y), position.z);
        }
        walked += vertices;
        for (const ufbx_face& face : mesh.faces) {
            if (++walked > kElementWalkLimit) break;
            if (face.num_indices < 3 || face.num_indices > 4096) continue;
            uint32_t triangles[3 * 4094]{};
            (void)ufbx_triangulate_face(triangles, std::size(triangles), &mesh, face);
        }
    }
    // Keep the normalization walk observable without imposing a crash oracle.
    volatile uint64_t sink = hash;
    (void)sink;
}

void FuzzFbx(const Input& input)
{
    if (input.primary.empty() || input.primary.size() > kInputLimit) return;
    Callbacks callbacks{input};
    ufbx_load_opts options{};
    options.file_format = UFBX_FILE_FORMAT_FBX;
    options.no_format_from_content = true;
    options.no_format_from_extension = true;
    options.filename = {"document.fbx", 12};
    options.strict = true;
    options.evaluate_skinning = true;
    options.evaluate_caches = false;
    options.load_external_files = (input.flags & kExternalFiles) != 0;
    options.ignore_missing_external_files = true;
    options.node_depth_limit = model_core::kMaxSceneHierarchyDepth;
    options.temp_allocator.memory_limit = kMemoryLimit / 2;
    options.result_allocator.memory_limit = kMemoryLimit / 2;
    options.temp_allocator.allocation_limit = 100'000;
    options.result_allocator.allocation_limit = 100'000;
    options.open_file_cb.fn = OpenVirtual;
    options.open_file_cb.user = &callbacks;
    options.progress_cb.fn = Progress;
    options.progress_cb.user = &callbacks;
    options.progress_interval_hint = 4096;
    ufbx_error error{};
    Scene loaded(ufbx_load_memory(input.primary.data(), input.primary.size(), &options, &error));
    if (!loaded) return;
    WalkNormalized(*loaded);
    if (!(input.flags & kEvaluate)) return;
    const ufbx_anim* animation = loaded->anim;
    double time = 0.0;
    if (loaded->anim_stacks.count && loaded->anim_stacks.data[0]) {
        animation = loaded->anim_stacks.data[0]->anim;
        time = loaded->anim_stacks.data[0]->time_begin;
    }
    if (!animation || !std::isfinite(time)) return;
    ufbx_evaluate_opts evaluate{};
    evaluate.evaluate_skinning = true;
    evaluate.evaluate_caches = false;
    evaluate.load_external_files = false;
    evaluate.temp_allocator.memory_limit = kMemoryLimit / 2;
    evaluate.result_allocator.memory_limit = kMemoryLimit / 2;
    evaluate.temp_allocator.allocation_limit = 100'000;
    evaluate.result_allocator.allocation_limit = 100'000;
    Scene evaluated(ufbx_evaluate_scene(loaded.get(), animation, time, &evaluate, &error));
    if (evaluated) WalkNormalized(*evaluated);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!data || size > kInputLimit + kSidecarLimit + 4096) return 0;
    const Input input = Decode({reinterpret_cast<const std::byte*>(data), size});
    if (input.flags & kProtocol) {
        (void)import_broker::ValidateAndCopySection(input.primary, 0, 4096);
    } else {
        FuzzFbx(input);
    }
    return 0;
}

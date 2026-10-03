#ifndef NOMINMAX
#define NOMINMAX
#endif

// Standalone, no-GPU sanitizer target for the Wavefront OBJ/MTL fast path.
//
// OBJ/MTL is parsed by the pinned `ufbx` library, the same parser
// `import_worker::ImportObj` drives. `ObjFuzz` loads the bounded primary
// source with ufbx in OBJ mode (with a bounded, in-memory virtual MTL sidecar
// served through the open-file callback) and, in a second envelope domain, in
// MTL mode directly -- so OBJ tokenization, MTL tokenization, missing
// sidecars, hostile external references, material conversion, triangulation
// and a bounded normalized scene walk all run in-process. It creates no
// window, GPU device, mapped file, filesystem-derived resolver, network
// client, or child process: every external open is answered from the
// single-entry in-memory sidecar or denied. The adapter's wire-emission and
// image-decode stages are deliberately not instrumented here (compressed
// codecs are SEC-16; the real provider/worker lanes cover them). The 2 MiB
// primary / 1 MiB sidecar caps keep a libFuzzer unit bounded without the real
// AppContainer/Job.

#include "model_core/WireFormat.h"
#include "ufbx.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

namespace {

constexpr uint32_t kEnvelopeMagic = 0x5a4a424f; // "OBJZ"
constexpr size_t kMemoryLimit = 64u * 1024u * 1024u;
constexpr size_t kInputLimit = 2u * 1024u * 1024u;
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
    kMtlPrimary = 1u << 0,
    kExternalFiles = 1u << 1,
    kCancel = 1u << 3,
};

struct Input {
    uint8_t flags = kExternalFiles;
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
    // Serve exactly the one bounded in-memory sidecar; deny every other
    // reference (missing sidecar, traversal, absolute, network, UNC) without
    // ever touching the filesystem.
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
        const size_t vertices = (std::min)({mesh.num_indices, kElementWalkLimit - walked});
        for (size_t index = 0; index < vertices; ++index) {
            const ufbx_vec3 position = ufbx_get_vertex_vec3(&mesh.vertex_position, index);
            hash = Hash(Hash(Hash(hash, position.x), position.y), position.z);
            if (mesh.vertex_normal.exists) {
                const ufbx_vec3 normal = ufbx_get_vertex_vec3(&mesh.vertex_normal, index);
                hash = Hash(Hash(Hash(hash, normal.x), normal.y), normal.z);
            }
            if (mesh.vertex_uv.exists) {
                const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, index);
                hash = Hash(Hash(hash, uv.x), uv.y);
            }
        }
        walked += vertices;
        for (const ufbx_face& face : mesh.faces) {
            if (++walked > kElementWalkLimit) break;
            if (face.num_indices < 3 || face.num_indices > 4096) continue;
            uint32_t triangles[3 * 4094]{};
            (void)ufbx_triangulate_face(triangles, std::size(triangles), &mesh, face);
        }
    }
    // MTL mode has no geometry; still make material parsing observable.
    for (size_t index = 0; index < scene.materials.count; ++index) {
        const ufbx_material* material = scene.materials.data[index];
        if (!material) continue;
        hash = Hash(hash, double(material->name.length));
        hash = Hash(hash, material->pbr.base_color.value_vec4.x);
        hash = Hash(hash, material->pbr.base_color.value_vec4.y);
        hash = Hash(hash, material->pbr.base_color.value_vec4.z);
    }
    volatile uint64_t sink = hash;
    (void)sink;
}

void FuzzObj(const Input& input)
{
    if (input.primary.empty() || input.primary.size() > kInputLimit) return;
    const bool mtl = (input.flags & kMtlPrimary) != 0;
    Callbacks callbacks{input};
    ufbx_load_opts options{};
    options.file_format = mtl ? UFBX_FILE_FORMAT_MTL : UFBX_FILE_FORMAT_OBJ;
    options.no_format_from_content = true;
    options.no_format_from_extension = true;
    options.filename = mtl ? ufbx_string{"document.mtl", 12} : ufbx_string{"document.obj", 12};
    options.strict = true;
    options.load_external_files = (input.flags & kExternalFiles) != 0;
    options.ignore_missing_external_files = true;
    options.generate_missing_normals = true;
    options.normalize_normals = true;
    options.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    options.unicode_error_handling = UFBX_UNICODE_ERROR_HANDLING_ABORT_LOADING;
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
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!data || size > kInputLimit + kSidecarLimit + 4096) return 0;
    const Input input = Decode({reinterpret_cast<const std::byte*>(data), size});
    FuzzObj(input);
    return 0;
}
// FBX-001 is deliberately a non-product spike. This file links the pinned
// ufbx directly into the test executable; it does not add an FBX control
// opcode, worker dispatcher route, viewer filter, or registration surface.

#include "ufbx.h"
#include "FbxSpikeWorker.h"
#include "SandboxTestSupport.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "import_broker/WorkerPool.h"
#include "model_core/ControlProtocol.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr size_t kLoadTempLimit = 64u * 1024u * 1024u;
constexpr size_t kLoadResultLimit = 64u * 1024u * 1024u;
constexpr size_t kEvaluateTempLimit = 64u * 1024u * 1024u;
constexpr size_t kEvaluateResultLimit = 64u * 1024u * 1024u;
constexpr size_t kAllocationLimit = 1'000'000;
constexpr double kHashQuantum = 1.0e-6;
static_assert(UFBX_VERSION == ufbx_pack_version(0, 23, 0),
              "FBX-001 measurements are valid only for pinned ufbx 0.23.0");

struct AllocationTracker {
    size_t current = 0;
    size_t peak = 0;
    size_t allocations = 0;
};

void* TrackAllocate(void* user, size_t size)
{
    auto& tracker = *static_cast<AllocationTracker*>(user);
    void* pointer = std::malloc(size);
    if (pointer) {
        tracker.current += size;
        tracker.peak = (std::max)(tracker.peak, tracker.current);
        ++tracker.allocations;
    }
    return pointer;
}

void* TrackReallocate(void* user, void* oldPointer, size_t oldSize, size_t newSize)
{
    auto& tracker = *static_cast<AllocationTracker*>(user);
    void* pointer = std::realloc(oldPointer, newSize);
    if (pointer || newSize == 0) {
        tracker.current -= oldSize;
        tracker.current += newSize;
        tracker.peak = (std::max)(tracker.peak, tracker.current);
        if (newSize) ++tracker.allocations;
    }
    return pointer;
}

void TrackFree(void* user, void* pointer, size_t size)
{
    auto& tracker = *static_cast<AllocationTracker*>(user);
    std::free(pointer);
    tracker.current -= size;
}

ufbx_allocator_opts AllocatorOptions(AllocationTracker& tracker, size_t memoryLimit,
                                     size_t allocationLimit = kAllocationLimit)
{
    ufbx_allocator_opts options{};
    options.allocator.alloc_fn = TrackAllocate;
    options.allocator.realloc_fn = TrackReallocate;
    options.allocator.free_fn = TrackFree;
    options.allocator.user = &tracker;
    options.memory_limit = memoryLimit;
    options.allocation_limit = allocationLimit;
    return options;
}

struct LoadedScene {
    AllocationTracker temp;
    AllocationTracker result;
    ufbx_scene* scene = nullptr;
    ufbx_error error{};

    ~LoadedScene()
    {
        if (scene) ufbx_free_scene(scene);
    }
};

struct EvaluatedScene {
    AllocationTracker temp;
    AllocationTracker result;
    ufbx_scene* scene = nullptr;
    ufbx_error error{};

    ~EvaluatedScene()
    {
        if (scene) ufbx_free_scene(scene);
    }
};

struct ProgressState {
    size_t calls = 0;
    bool cancel = false;
};

ufbx_progress_result Progress(void* user, const ufbx_progress*)
{
    auto& state = *static_cast<ProgressState*>(user);
    ++state.calls;
    return state.cancel ? UFBX_PROGRESS_CANCEL : UFBX_PROGRESS_CONTINUE;
}

std::filesystem::path FixturePath(std::string_view name)
{
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" / "fbx-spike" / name;
}

std::vector<std::byte> ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    REQUIRE(length >= 0);
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<size_t>(length));
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    REQUIRE(input.good());
    return bytes;
}

int Base64Digit(char value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

std::vector<std::byte> DecodeBase64Fixture(std::string_view name)
{
    const auto encodedBytes = ReadFile(FixturePath(name));
    std::vector<std::byte> decoded;
    decoded.reserve(encodedBytes.size() * 3 / 4);
    uint32_t accumulator = 0;
    unsigned bits = 0;
    for (const std::byte raw : encodedBytes) {
        const char character = static_cast<char>(raw);
        if (character == '=') break;
        const int digit = Base64Digit(character);
        if (digit < 0) {
            REQUIRE((character == '\r' || character == '\n' || character == ' ' || character == '\t'));
            continue;
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<std::byte>((accumulator >> bits) & 0xffu));
        }
    }
    REQUIRE_FALSE(decoded.empty());
    return decoded;
}

std::vector<std::byte> LoadFixture(std::string_view name)
{
    if (name.ends_with(".base64")) return DecodeBase64Fixture(name);
    return ReadFile(FixturePath(name));
}

std::unique_ptr<LoadedScene> Load(std::span<const std::byte> bytes,
                                  size_t tempLimit = kLoadTempLimit,
                                  size_t resultLimit = kLoadResultLimit,
                                  uint32_t depthLimit = 256,
                                  ProgressState* progress = nullptr,
                                  bool reverseWinding = false)
{
    auto loaded = std::make_unique<LoadedScene>();
    ufbx_load_opts options{};
    options.temp_allocator = AllocatorOptions(loaded->temp, tempLimit);
    options.result_allocator = AllocatorOptions(loaded->result, resultLimit);
    options.evaluate_skinning = true;
    options.evaluate_caches = false;
    options.load_external_files = false;
    options.ignore_missing_external_files = true;
    options.clean_skin_weights = true;
    options.generate_missing_normals = true;
    options.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    options.unicode_error_handling = UFBX_UNICODE_ERROR_HANDLING_ABORT_LOADING;
    options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_PRESERVE;
    options.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
    options.pivot_handling = UFBX_PIVOT_HANDLING_RETAIN;
    options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0;
    options.reverse_winding = reverseWinding;
    options.node_depth_limit = depthLimit;
    options.filename = { "fixture.fbx", 11 };
    if (progress) {
        options.progress_cb.fn = Progress;
        options.progress_cb.user = progress;
        options.progress_interval_hint = 1;
    }
    loaded->scene = ufbx_load_memory(bytes.data(), bytes.size(), &options, &loaded->error);
    return loaded;
}

std::unique_ptr<EvaluatedScene> Evaluate(const ufbx_scene& source, const ufbx_anim* animation,
                                         double time)
{
    auto evaluated = std::make_unique<EvaluatedScene>();
    ufbx_evaluate_opts options{};
    options.temp_allocator = AllocatorOptions(evaluated->temp, kEvaluateTempLimit);
    options.result_allocator = AllocatorOptions(evaluated->result, kEvaluateResultLimit);
    options.evaluate_skinning = true;
    options.evaluate_caches = false;
    options.load_external_files = false;
    evaluated->scene = ufbx_evaluate_scene(&source, animation, time, &options, &evaluated->error);
    return evaluated;
}

std::unique_ptr<EvaluatedScene> EvaluateIntendedPose(const ufbx_scene& source)
{
    if (source.anim_stacks.count) {
        const ufbx_anim_stack* first = source.anim_stacks.data[0];
        return Evaluate(source, first->anim, first->time_begin);
    }
    return Evaluate(source, source.anim, 0.0);
}

class StableHash {
public:
    void AddUint(uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value_ ^= static_cast<uint8_t>(value >> shift);
            value_ *= 1099511628211ull;
        }
    }

    bool AddFinite(double value)
    {
        if (!std::isfinite(value) || std::abs(value) > 1.0e12) return false;
        const double scaled = std::nearbyint(value / kHashQuantum);
        if (scaled < static_cast<double>((std::numeric_limits<int64_t>::min)())
            || scaled > static_cast<double>((std::numeric_limits<int64_t>::max)())) return false;
        AddUint(static_cast<uint64_t>(static_cast<int64_t>(scaled)));
        return true;
    }

    uint64_t Value() const noexcept { return value_; }

private:
    uint64_t value_ = 1469598103934665603ull;
};

struct Snapshot {
    uint64_t hash = 0;
    uint64_t normalizedBytes = 0;
    bool finite = true;
    size_t meshInstances = 0;
};

Snapshot HashNormalizedPose(const ufbx_scene& scene)
{
    StableHash hash;
    Snapshot snapshot{};
    hash.AddUint(scene.nodes.count);
    hash.AddUint(scene.meshes.count);
    for (size_t nodeIndex = 0; nodeIndex < scene.nodes.count; ++nodeIndex) {
        const ufbx_node* node = scene.nodes.data[nodeIndex];
        hash.AddUint(node->typed_id);
        hash.AddUint(node->parent ? node->parent->typed_id : UINT32_MAX);
        hash.AddUint(node->mesh ? node->mesh->typed_id : UINT32_MAX);
        for (const double value : node->geometry_to_world.v)
            snapshot.finite &= hash.AddFinite(value);
        snapshot.normalizedBytes += sizeof(float) * 12;
        hash.AddUint(node->materials.count);
        for (const ufbx_material* material : node->materials)
            hash.AddUint(material ? material->typed_id : UINT32_MAX);
        if (!node->mesh) continue;
        ++snapshot.meshInstances;
        const ufbx_mesh& mesh = *node->mesh;
        hash.AddUint(mesh.reversed_winding ? 1u : 0u);
        const ufbx_matrix normalMatrix = ufbx_matrix_for_normals(&node->geometry_to_world);
        for (size_t index = 0; index < mesh.num_indices; ++index) {
            ufbx_vec3 position = ufbx_get_vertex_vec3(&mesh.skinned_position, index);
            ufbx_vec3 normal = ufbx_get_vertex_vec3(&mesh.skinned_normal, index);
            if (mesh.skinned_is_local) {
                position = ufbx_transform_position(&node->geometry_to_world, position);
                normal = ufbx_transform_direction(&normalMatrix, normal);
            }
            snapshot.finite &= hash.AddFinite(position.x);
            snapshot.finite &= hash.AddFinite(position.y);
            snapshot.finite &= hash.AddFinite(position.z);
            snapshot.finite &= hash.AddFinite(normal.x);
            snapshot.finite &= hash.AddFinite(normal.y);
            snapshot.finite &= hash.AddFinite(normal.z);
            snapshot.normalizedBytes += sizeof(float) * 6;
        }
    }
    snapshot.hash = hash.Value();
    return snapshot;
}

std::string Text(ufbx_string value)
{
    return std::string(value.data, value.length);
}

void PrintMemory(std::string_view fixture, const LoadedScene& loaded,
                 const EvaluatedScene* evaluated, const Snapshot& snapshot)
{
    std::cout << "FBX-001 " << fixture
              << " load-temp-peak=" << loaded.temp.peak
              << " load-result-peak=" << loaded.result.peak;
    if (evaluated) {
        std::cout << " eval-temp-peak=" << evaluated->temp.peak
                  << " eval-result-peak=" << evaluated->result.peak;
    }
    std::cout << " normalized-bytes=" << snapshot.normalizedBytes
              << " hash=0x" << std::hex << snapshot.hash << std::dec << '\n';
}

} // namespace

TEST_CASE("FBX-001 pinned ufbx loads ASCII and binary fixtures within explicit allocator caps",
          "[fbx-spike][allocator]")
{
    struct Fixture { const char* name; bool ascii; };
    const Fixture fixtures[] = {
        { "hierarchy-instances-pivots-ascii.fbx", true },
        { "cube-binary.fbx.base64", false },
    };
    for (const auto& fixture : fixtures) {
        const auto bytes = LoadFixture(fixture.name);
        const auto loaded = Load(bytes);
        CAPTURE(fixture.name, loaded->error.type, loaded->error.description.data);
        REQUIRE(loaded->scene);
        CHECK(loaded->scene->metadata.ascii == fixture.ascii);
        if (!fixture.ascii) CHECK(loaded->scene->anim_stacks.count == 0);
        CHECK(loaded->temp.peak <= kLoadTempLimit);
        CHECK(loaded->result.peak <= kLoadResultLimit);
        const auto evaluated = EvaluateIntendedPose(*loaded->scene);
        REQUIRE(evaluated->scene);
        CHECK(evaluated->temp.peak <= kEvaluateTempLimit);
        CHECK(evaluated->result.peak <= kEvaluateResultLimit);
        const auto snapshot = HashNormalizedPose(*evaluated->scene);
        CHECK(snapshot.finite);
        PrintMemory(fixture.name, *loaded, evaluated.get(), snapshot);
    }
}

TEST_CASE("FBX-001 pose rule selects first authored stack at its start time",
          "[fbx-spike][pose]")
{
    const auto multipleBytes = LoadFixture("multiple-stacks-ascii.fbx");
    const auto multiple = Load(multipleBytes);
    REQUIRE(multiple->scene);
    REQUIRE(multiple->scene->anim_stacks.count == 3);
    CHECK(Text(multiple->scene->anim_stacks.data[0]->name) == "X");
    CHECK(Text(multiple->scene->anim_stacks.data[1]->name) == "Y");
    CHECK(Text(multiple->scene->anim_stacks.data[2]->name) == "Z");

    const auto nonZeroBytes = LoadFixture("dual-quaternion-ascii.fbx");
    const auto nonZero = Load(nonZeroBytes);
    REQUIRE(nonZero->scene);
    REQUIRE(nonZero->scene->anim_stacks.count >= 1);
    const ufbx_anim_stack* first = nonZero->scene->anim_stacks.data[0];
    CHECK(first->time_begin > 0.0);
    const auto intended = EvaluateIntendedPose(*nonZero->scene);
    const auto explicitStart = Evaluate(*nonZero->scene, first->anim, first->time_begin);
    REQUIRE(intended->scene);
    REQUIRE(explicitStart->scene);
    CHECK(HashNormalizedPose(*intended->scene).hash == HashNormalizedPose(*explicitStart->scene).hash);
}

TEST_CASE("FBX-001 evaluated skin and blend attributes are finite and deterministic",
          "[fbx-spike][deformation][determinism]")
{
    struct Fixture {
        const char* name;
        bool expectSkin;
        ufbx_skinning_method skinning;
        bool expectBlend;
    };
    const Fixture fixtures[] = {
        { "linear-skin-binary.fbx.base64", true, UFBX_SKINNING_METHOD_LINEAR, false },
        { "dual-quaternion-ascii.fbx", true, UFBX_SKINNING_METHOD_DUAL_QUATERNION, false },
        { "blended-skin-binary.fbx.base64", true, UFBX_SKINNING_METHOD_BLENDED_DQ_LINEAR, false },
        { "shape-animation-binary.fbx.base64", false, UFBX_SKINNING_METHOD_LINEAR, true },
        { "combined-skin-blend-ascii.fbx", true, UFBX_SKINNING_METHOD_LINEAR, true },
    };
    for (const auto& fixture : fixtures) {
        const auto bytes = LoadFixture(fixture.name);
        uint64_t referenceHash = 0;
        for (unsigned run = 0; run < 8; ++run) {
            const auto loaded = Load(bytes);
            CAPTURE(fixture.name, run, loaded->error.type);
            REQUIRE(loaded->scene);
            if (fixture.expectBlend)
                REQUIRE(loaded->scene->blend_deformers.count > 0);
            if (fixture.expectSkin) {
                REQUIRE(loaded->scene->skin_deformers.count > 0);
                CHECK(loaded->scene->skin_deformers.data[0]->skinning_method == fixture.skinning);
            }
            const auto evaluated = EvaluateIntendedPose(*loaded->scene);
            REQUIRE(evaluated->scene);
            const auto snapshot = HashNormalizedPose(*evaluated->scene);
            REQUIRE(snapshot.finite);
            REQUIRE(snapshot.normalizedBytes > 0);
            if (run == 0) referenceHash = snapshot.hash;
            else CHECK(snapshot.hash == referenceHash);
            if (run == 0) PrintMemory(fixture.name, *loaded, evaluated.get(), snapshot);
        }
    }
}

TEST_CASE("FBX-001 instance relationships remain shared while node transforms remain distinct",
          "[fbx-spike][instances]")
{
    const auto bytes = LoadFixture("hierarchy-instances-pivots-ascii.fbx");
    const auto loaded = Load(bytes);
    REQUIRE(loaded->scene);
    bool foundSharedMesh = false;
    for (const ufbx_mesh* mesh : loaded->scene->meshes) {
        if (mesh->instances.count < 2) continue;
        foundSharedMesh = true;
        CHECK(mesh->instances.data[0]->mesh == mesh->instances.data[1]->mesh);
        bool transformDiffers = false;
        for (size_t component = 0; component < 12; ++component)
            transformDiffers |= mesh->instances.data[0]->geometry_to_world.v[component]
                != mesh->instances.data[1]->geometry_to_world.v[component];
        CHECK(transformDiffers);
    }
    CHECK(foundSharedMesh);
}

TEST_CASE("FBX-001 authored scale units and axes survive deterministic conversion",
          "[fbx-spike][transforms][units][axes]")
{
    const auto negative = Load(LoadFixture("negative-scale-pivots-ascii.fbx"));
    REQUIRE(negative->scene);
    bool foundNegative = false;
    for (const ufbx_node* node : negative->scene->nodes) {
        const ufbx_vec3 scale = node->local_transform.scale;
        foundNegative |= scale.x < 0.0 || scale.y < 0.0 || scale.z < 0.0;
    }
    CHECK(foundNegative);

    const auto nonuniform = Load(LoadFixture("nonuniform-scale-pivots-ascii.fbx"));
    REQUIRE(nonuniform->scene);
    bool foundNonuniform = false;
    for (const ufbx_node* node : nonuniform->scene->nodes) {
        const ufbx_vec3 scale = node->local_transform.scale;
        foundNonuniform |= std::abs(scale.x - scale.y) > 1.0e-9
            || std::abs(scale.y - scale.z) > 1.0e-9;
    }
    CHECK(foundNonuniform);
    CHECK(std::abs(nonuniform->scene->settings.original_unit_meters - 0.01) < 1.0e-12);
    CHECK(std::abs(nonuniform->scene->metadata.geometry_scale - 0.01) < 1.0e-12);

    const auto zUp = Load(LoadFixture("z-up-binary.fbx.base64"));
    REQUIRE(zUp->scene);
    CHECK(zUp->scene->settings.axes.up == UFBX_COORDINATE_AXIS_POSITIVE_Z);
    CHECK(zUp->scene->metadata.space_conversion == UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY);
    const ufbx_quat rotation = zUp->scene->metadata.root_rotation;
    CHECK((std::abs(rotation.x) > 1.0e-9 || std::abs(rotation.y) > 1.0e-9
           || std::abs(rotation.z) > 1.0e-9 || std::abs(rotation.w - 1.0) > 1.0e-9));
    CHECK(HashNormalizedPose(*zUp->scene).finite);
}

TEST_CASE("FBX-001 normals use inverse transpose and winding reversal is observable",
          "[fbx-spike][normals][winding]")
{
    ufbx_matrix transform{};
    transform.m00 = -2.0;
    transform.m11 = 3.0;
    transform.m22 = 0.5;
    const ufbx_matrix normalMatrix = ufbx_matrix_for_normals(&transform);
    const ufbx_vec3 tangent{ 1.0, 1.0, 0.0 };
    const ufbx_vec3 normal{ 1.0, -1.0, 0.0 };
    const ufbx_vec3 worldTangent = ufbx_transform_direction(&transform, tangent);
    const ufbx_vec3 worldNormal = ufbx_transform_direction(&normalMatrix, normal);
    CHECK(std::abs(worldTangent.x * worldNormal.x + worldTangent.y * worldNormal.y
                   + worldTangent.z * worldNormal.z) < 1.0e-12);

    const auto bytes = LoadFixture("cube-binary.fbx.base64");
    const auto ordinary = Load(bytes, kLoadTempLimit, kLoadResultLimit, 256, nullptr, false);
    const auto reversed = Load(bytes, kLoadTempLimit, kLoadResultLimit, 256, nullptr, true);
    REQUIRE(ordinary->scene);
    REQUIRE(reversed->scene);
    REQUIRE(ordinary->scene->meshes.count == reversed->scene->meshes.count);
    REQUIRE(ordinary->scene->meshes.count > 0);
    CHECK(ordinary->scene->meshes.data[0]->reversed_winding
          != reversed->scene->meshes.data[0]->reversed_winding);
}

TEST_CASE("FBX-001 load cancellation and allocator exhaustion are controlled and recoverable",
          "[fbx-spike][cancellation][allocator][recovery]")
{
    const auto bytes = LoadFixture("shape-animation-binary.fbx.base64");

    ProgressState progress{};
    progress.cancel = true;
    const auto cancelled = Load(bytes, kLoadTempLimit, kLoadResultLimit, 256, &progress);
    CHECK_FALSE(cancelled->scene);
    CHECK(cancelled->error.type == UFBX_ERROR_CANCELLED);
    CHECK(progress.calls > 0);

    const auto exhaustedTemp = Load(bytes, 1024, kLoadResultLimit);
    CHECK_FALSE(exhaustedTemp->scene);
    CHECK((exhaustedTemp->error.type == UFBX_ERROR_MEMORY_LIMIT
           || exhaustedTemp->error.type == UFBX_ERROR_ALLOCATION_LIMIT));

    const auto exhaustedResult = Load(bytes, kLoadTempLimit, 1024);
    CHECK_FALSE(exhaustedResult->scene);
    CHECK((exhaustedResult->error.type == UFBX_ERROR_MEMORY_LIMIT
           || exhaustedResult->error.type == UFBX_ERROR_ALLOCATION_LIMIT));

    const auto recovered = Load(bytes);
    REQUIRE(recovered->scene);
    const auto evaluated = EvaluateIntendedPose(*recovered->scene);
    REQUIRE(evaluated->scene);
    CHECK(HashNormalizedPose(*evaluated->scene).finite);
}

TEST_CASE("FBX-001 malformed truncation and depth limits fail without poisoning the next load",
          "[fbx-spike][malformed][limits][recovery]")
{
    auto bytes = LoadFixture("cube-binary.fbx.base64");
    bytes.resize(bytes.size() / 2);
    const auto truncated = Load(bytes);
    CHECK_FALSE(truncated->scene);

    const auto hierarchyBytes = LoadFixture("nested-hierarchy-binary.fbx.base64");
    const auto tooDeep = Load(hierarchyBytes, kLoadTempLimit, kLoadResultLimit, 1);
    CHECK_FALSE(tooDeep->scene);
    CHECK(tooDeep->error.type == UFBX_ERROR_NODE_DEPTH_LIMIT);

    const auto countSource = LoadFixture("multiple-stacks-ascii.fbx");
    std::string excessiveCount(reinterpret_cast<const char*>(countSource.data()), countSource.size());
    const size_t countOffset = excessiveCount.find("\t\tVertices: *24 {");
    const size_t countEnd = excessiveCount.find("\n\t\t}", countOffset);
    REQUIRE(countOffset != std::string::npos);
    REQUIRE(countEnd != std::string::npos);
    constexpr size_t kDeclaredReals = 12'000'000;
    std::string values(kDeclaredReals * 2 - 1, ',');
    for (size_t index = 0; index < values.size(); index += 2) values[index] = '0';
    std::string replacement = "\t\tVertices: *" + std::to_string(kDeclaredReals)
        + " {\n\t\t\ta: " + values;
    excessiveCount.replace(countOffset, countEnd - countOffset, replacement);
    const auto excessive = Load(std::as_bytes(std::span(excessiveCount)));
    CHECK_FALSE(excessive->scene);
    CHECK((excessive->error.type == UFBX_ERROR_MEMORY_LIMIT
           || excessive->error.type == UFBX_ERROR_ALLOCATION_LIMIT));
    CHECK(excessive->temp.peak <= kLoadTempLimit);
    CHECK(excessive->result.peak <= kLoadResultLimit);

    const auto recovered = Load(hierarchyBytes);
    REQUIRE(recovered->scene);
    CHECK(HashNormalizedPose(*recovered->scene).finite);
}

TEST_CASE("FBX-001 unsupported feature inventory is visible through the pinned API",
          "[fbx-spike][feature-policy]")
{
    const auto bytes = LoadFixture("shape-animation-binary.fbx.base64");
    const auto loaded = Load(bytes);
    REQUIRE(loaded->scene);
    // Compile-time/runtime probes for every family whose product policy FBX-001
    // must freeze. Counts are data, not inferred from warning strings.
    CHECK(loaded->scene->cache_deformers.count == 0);
    CHECK(loaded->scene->constraints.count == 0);
    CHECK(loaded->scene->nurbs_curves.count == 0);
    CHECK(loaded->scene->nurbs_surfaces.count == 0);
    CHECK(loaded->scene->procedural_geometries.count == 0);
    for (const ufbx_mesh* mesh : loaded->scene->meshes)
        CHECK(mesh->subdivision_preview_levels == 0);
    for (const ufbx_texture* texture : loaded->scene->textures) {
        static_cast<void>(texture->content);
        static_cast<void>(texture->filename);
        static_cast<void>(texture->absolute_filename);
        static_cast<void>(texture->relative_filename);
    }
}

TEST_CASE("FBX-001 evaluation cancellation replaces the real sandboxed pooled worker",
          "[fbx-spike][cancellation][sandbox][worker-pool]")
{
    sandbox_test_support::SandboxFixture fixture;
    import_broker::WorkerPool pool;
    import_broker::SandboxLimits limits{};
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(fixture.sid), limits, 1,
                                      L"--fbx-spike-pool", error));
    auto index = pool.AcquireIdle();
    REQUIRE(index.has_value());
    const DWORD originalProcessId = pool.ProcessId(*index);

    const auto source = LoadFixture("combined-skin-blend-ascii.fbx");
    const SIZE_T sectionSize = sizeof(import_worker::FbxSpikePayloadHeader) + source.size();
    auto section = import_broker::CreateSharedSection(sectionSize);
    REQUIRE(section);
    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ | FILE_MAP_WRITE, sectionSize);
    REQUIRE(view);
    auto* header = reinterpret_cast<import_worker::FbxSpikePayloadHeader*>(view.bytes().data());
    header->magic = import_worker::kFbxSpikePayloadMagic;
    header->sourceByteLength = source.size();
    header->evaluationState = 0;
    header->reserved = 0;
    std::memcpy(view.bytes().data() + sizeof(*header), source.data(), source.size());

    platform::Win32Handle cancellationEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    REQUIRE(cancellationEvent);
    const auto workerEvent = pool.DuplicateSectionIntoWorker(*index, cancellationEvent.get());
    REQUIRE(workerEvent.has_value());
    header->cancellationEventHandleValue = *workerEvent;
    const auto workerSection = pool.DuplicateSectionIntoWorker(*index, section.get());
    REQUIRE(workerSection.has_value());

    model_core::StartGenerationRequest request{};
    request.generationId = 0xfb001;
    request.sceneVariant = import_worker::kFbxSpikeEvaluationSceneVariant;
    request.sectionHandleValue = *workerSection;
    request.sectionByteCapacity = sectionSize;
    request.maxChunkCount = 1;
    REQUIRE(pool.SendRequest(*index, model_core::ControlOpcode::StartGeneration,
                             &request, sizeof(request)));

    const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (InterlockedCompareExchange(&header->evaluationState, 0, 0) != 2
           && std::chrono::steady_clock::now() < enteredDeadline) Sleep(1);
    REQUIRE(InterlockedCompareExchange(&header->evaluationState, 0, 0) == 2);

    HANDLE processCopyRaw = nullptr;
    REQUIRE(DuplicateHandle(GetCurrentProcess(), pool.ProcessHandle(*index), GetCurrentProcess(),
                            &processCopyRaw, SYNCHRONIZE, FALSE, 0));
    platform::Win32Handle processCopy(processCopyRaw);
    const auto cancelStart = std::chrono::steady_clock::now();
    REQUIRE(SetEvent(cancellationEvent.get()));
    model_core::ReceivedControlMessage reply{};
    CHECK(pool.WaitForReply(*index, 500, reply) == import_broker::WaitReplyOutcome::TimedOut);
    REQUIRE(pool.TerminateAndReplace(*index, error));
    const auto replacedAt = std::chrono::steady_clock::now();
    CHECK(WaitForSingleObject(processCopy.get(), 2000) == WAIT_OBJECT_0);
    CHECK(pool.ProcessId(*index) != originalProcessId);
    const auto interruption = std::chrono::duration_cast<std::chrono::milliseconds>(
        replacedAt - cancelStart);
    CHECK(interruption >= std::chrono::milliseconds(450));
    CHECK(interruption < std::chrono::milliseconds(2500));
    std::cout << "FBX-001 evaluation-noninterruptible-ms=" << interruption.count() << '\n';

    // TerminateAndReplace installs an idle replacement in the same slot. It
    // must immediately serve a normal valid request, proving the stuck ufbx
    // evaluation did not poison the pool or its control channel.
    auto replacement = pool.AcquireIdle();
    REQUIRE(replacement.has_value());
    REQUIRE(*replacement == *index);
    auto output = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(output);
    const auto workerOutput = pool.DuplicateSectionIntoWorker(*replacement, output.get());
    REQUIRE(workerOutput.has_value());
    model_core::StartGenerationRequest valid{};
    valid.generationId = 0xfb002;
    valid.sceneVariant = model_core::kSceneVariant_CubeAndPointCluster;
    valid.sectionHandleValue = *workerOutput;
    valid.sectionByteCapacity = import_broker::kSyntheticSectionBytes;
    valid.maxChunkCount = 8;
    REQUIRE(pool.SendRequest(*replacement, model_core::ControlOpcode::StartGeneration,
                             &valid, sizeof(valid)));
    REQUIRE(pool.WaitForReply(*replacement, 5000, reply) == import_broker::WaitReplyOutcome::Ready);
    CHECK(reply.header.opcode == uint32_t(model_core::ControlOpcode::ChunksReady));
    pool.Release(*replacement);

    const auto gltfPath = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
        / "interactive-viewer" / "test-assets" / "tri_tight.glb";
    import_broker::ImportSessionRequest importRequest{};
    importRequest.workerExePath = sandbox_test_support::WorkerExePath();
    importRequest.sourcePath = gltfPath.wstring();
    importRequest.format = import_broker::ImportFormat::Gltf;
    importRequest.generationId = 0xfb003;
    importRequest.sectionByteCapacity = 4u * 1024u * 1024u;
    importRequest.maxChunkCount = 64;
    importRequest.maxChunkBatchesPerGeneration = 4;
    importRequest.maxSidecarRequestsPerGeneration = 8;
    importRequest.maxSidecarFileBytes = 16u * 1024u * 1024u;
    const auto validImport = import_broker::RunImportSession(importRequest);
    CAPTURE(validImport.stage, validImport.errorCode);
    CHECK(validImport.ok);
}

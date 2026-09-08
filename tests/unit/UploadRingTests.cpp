// Gate 2 workstream B, slice 3: upload ring + copy queue/fence
// integration + fence-complete publication, exercised end-to-end against
// a synthetic in-process streaming source. See
// .docs/design/04-rendering-and-streaming.md ("Upload ring",
// "Fence-complete publication") and .docs/design/10-delivery-plan.md's
// Gate 2 exit criteria (ring wrap property, forced-delay/previous-LOD
// behavior).

#include "D3D12Device.h"
#include "D3D12UploadRing.h"
#include "SceneSnapshot.h"
#include "SyntheticStreamingSource.h"

#include <model_core/VertexLayouts.h>
#include <platform/Generation.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

namespace {

D3D12Device& SharedDevice()
{
    static D3D12Device device = [] {
        D3D12Device d;
        auto result = d.Initialize();
        REQUIRE(result.success);
        return d;
    }();
    return device;
}

Microsoft::WRL::ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device& device, uint64_t size,
                                                    D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = heapType;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    REQUIRE(SUCCEEDED(device.CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState,
                                                       nullptr, IID_PPV_ARGS(&resource))));
    return resource;
}

Microsoft::WRL::ComPtr<ID3D12Resource> MakeDefaultBuffer(ID3D12Device& device, uint64_t size)
{
    return MakeBuffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
}

// Copies `destination`'s content into a READBACK buffer over the ring's
// own copy queue (a second, independently-allocated command list on the
// same queue -- legitimate, queues just serialize submitted lists) and
// returns it as a byte vector for comparison. Blocks (bounded) until the
// readback copy itself completes.
std::vector<std::byte> ReadBack(D3D12Device& device, D3D12UploadRing& ring, ID3D12Resource& destination,
                                 uint64_t size)
{
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    REQUIRE(SUCCEEDED(device.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                                IID_PPV_ARGS(&allocator))));
    REQUIRE(SUCCEEDED(device.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(),
                                                           nullptr, IID_PPV_ARGS(&commandList))));

    auto readback = MakeBuffer(*device.Device(), size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(readback.Get(), 0, &destination, 0, size);
    REQUIRE(SUCCEEDED(commandList->Close()));

    ID3D12CommandList* lists[] = { commandList.Get() };
    ring.CopyQueue().Queue()->ExecuteCommandLists(1, lists);
    uint64_t fenceValue = ring.CopyQueue().SignalNext();
    REQUIRE(ring.CopyQueue().WaitForValue(fenceValue, 5000) == D3D12CommandQueue::WaitResult::Signaled);

    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(size) };
    void* mapped = nullptr;
    REQUIRE(SUCCEEDED(readback->Map(0, &readRange, &mapped)));
    std::vector<std::byte> result(size);
    std::memcpy(result.data(), mapped, static_cast<size_t>(size));
    D3D12_RANGE writtenRange{ 0, 0 };
    readback->Unmap(0, &writtenRange);
    return result;
}

SceneSnapshotPtr DrainUntil(D3D12UploadRing& ring, const platform::GenerationSource& generation,
                             size_t expectedCount, int maxAttempts = 2000)
{
    SceneSnapshotPtr snapshot;
    for (int i = 0; i < maxAttempts; ++i) {
        snapshot = ring.DrainCompletedPublications(generation);
        if (snapshot->ReadyResources().size() >= expectedCount) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return snapshot;
}

} // namespace

TEST_CASE("Upload copies bytes through the ring to a DEFAULT buffer, verified via readback after "
          "fence-complete publication",
          "[graphics]")
{
    D3D12UploadRing ring;
    REQUIRE(ring.Initialize(SharedDevice()));

    std::vector<std::byte> source(4096);
    for (size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<std::byte>(i % 256);
    }

    auto destination = MakeDefaultBuffer(*SharedDevice().Device(), source.size());

    platform::GenerationSource generation;
    D3D12UploadRing::UploadRequest request;
    request.sourceBytes = source;
    request.destination = destination.Get();
    request.generation = generation.Snapshot();
    request.clusterId = 0;
    request.lodLevel = 0;

    REQUIRE(ring.Upload(request) == D3D12UploadRing::UploadResult::Uploaded);
    ring.FlushBatch();

    SceneSnapshotPtr snapshot = DrainUntil(ring, generation, 1);
    REQUIRE(snapshot->ReadyResources().size() == 1);
    CHECK(snapshot->ReadyResources()[0].resource == destination.Get());
    CHECK(snapshot->ReadyResources()[0].generationValue == generation.Current());

    std::vector<std::byte> readBack = ReadBack(SharedDevice(), ring, *destination.Get(), source.size());
    CHECK(std::memcmp(readBack.data(), source.data(), source.size()) == 0);
}

TEST_CASE("Ring wrap-around never overwrites an unretired allocation", "[graphics]")
{
    D3D12UploadRing::CreateOptions options;
    options.initialCapacityBytes = 4096;
    options.growthIncrementBytes = 0; // disable growth so this genuinely exercises wrap, not just expansion
    options.maxCapacityBytes = 4096;
    options.maxBatchBytes = 512;

    D3D12UploadRing ring;
    REQUIRE(ring.Initialize(SharedDevice(), options));

    platform::GenerationSource generation;
    constexpr size_t kChunkSize = 64;
    constexpr uint32_t kChunkCount = 200; // 12800 bytes total -- ~3x the 4096-byte ring capacity

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> destinations;
    destinations.reserve(kChunkCount);

    for (uint32_t i = 0; i < kChunkCount; ++i) {
        std::vector<std::byte> source(kChunkSize, static_cast<std::byte>(i % 256));
        auto destination = MakeDefaultBuffer(*SharedDevice().Device(), kChunkSize);

        D3D12UploadRing::UploadRequest request;
        request.sourceBytes = source;
        request.destination = destination.Get();
        request.generation = generation.Snapshot();
        request.clusterId = i;
        request.lodLevel = 0;

        REQUIRE(ring.Upload(request) == D3D12UploadRing::UploadResult::Uploaded);
        // The core wrap-safety invariant: the ring must never believe it
        // occupies more than it physically has room for.
        CHECK(ring.UsedBytes() <= ring.CapacityBytes());

        destinations.push_back(destination);
        if (i % 8 == 0) {
            ring.ReclaimCompleted();
        }
    }
    ring.FlushBatch();

    SceneSnapshotPtr snapshot = DrainUntil(ring, generation, kChunkCount);
    REQUIRE(snapshot->ReadyResources().size() == kChunkCount);

    // Content correctness for a representative sample (first, middle,
    // last) proves a wrap never silently overwrote live, not-yet-copied
    // data -- corruption here would show up as wrong bytes, not a crash.
    for (uint32_t index : { 0u, kChunkCount / 2, kChunkCount - 1 }) {
        std::vector<std::byte> expected(kChunkSize, static_cast<std::byte>(index % 256));
        std::vector<std::byte> actual = ReadBack(SharedDevice(), ring, *destinations[index].Get(), kChunkSize);
        CHECK(actual == expected);
    }
}

TEST_CASE("An allocation that doesn't fit triggers ring growth up to the configured cap", "[graphics]")
{
    D3D12UploadRing::CreateOptions options;
    options.initialCapacityBytes = 1024;
    options.growthIncrementBytes = 1024;
    options.maxCapacityBytes = 4096;
    options.maxBatchBytes = 64ull * 1024 * 1024;

    D3D12UploadRing ring;
    REQUIRE(ring.Initialize(SharedDevice(), options));
    CHECK(ring.CapacityBytes() == 1024);

    platform::GenerationSource generation;
    std::vector<std::byte> source(3000, std::byte{ 0xAB });
    auto destination = MakeDefaultBuffer(*SharedDevice().Device(), source.size());

    D3D12UploadRing::UploadRequest request;
    request.sourceBytes = source;
    request.destination = destination.Get();
    request.generation = generation.Snapshot();

    // 3000 bytes doesn't fit in the initial 1024-byte ring -- this must
    // grow (1024 -> 2048 -> 3072, capped at 4096) rather than block
    // forever on an empty ring with nothing to reclaim.
    REQUIRE(ring.Upload(request) == D3D12UploadRing::UploadResult::Uploaded);
    CHECK(ring.CapacityBytes() > 1024);
    CHECK(ring.CapacityBytes() <= 4096);

    ring.FlushBatch();
    SceneSnapshotPtr snapshot = DrainUntil(ring, generation, 1);
    REQUIRE(snapshot->ReadyResources().size() == 1);

    std::vector<std::byte> readBack = ReadBack(SharedDevice(), ring, *destination.Get(), source.size());
    CHECK(readBack == source);
}

TEST_CASE("Synthetic clusters publish into a growing SceneSnapshot only as their copies complete, "
          "and a stale-generation upload is dropped rather than published",
          "[graphics]")
{
    D3D12UploadRing ring;
    REQUIRE(ring.Initialize(SharedDevice()));

    platform::GenerationSource generation;

    // Snapshotted before the Advance() below -- deliberately stale by the
    // time DrainCompletedPublications checks it against `generation`.
    platform::GenerationToken staleToken = generation.Snapshot();
    generation.Advance();

    // "The direct queue... keeps drawing the previous proxy/LOD": before
    // any upload has even been recorded, the current snapshot must be the
    // untouched initial empty one.
    SceneSnapshotPtr before = ring.CurrentSnapshot();
    CHECK(before->ReadyResources().empty());

    auto clusters = GenerateSyntheticClusters(/*clusterCount=*/3, /*seed=*/1);
    REQUIRE(clusters.size() == 3);

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> destinations;
    size_t expectedReadyCount = 0;
    for (const auto& cluster : clusters) {
        REQUIRE(cluster.lods.size() == 3);
        for (const auto& level : cluster.lods) {
            uint64_t byteSize =
                level.vertices.size() * sizeof(model_core::VertexPositionNormalUv0F32);
            auto destination = MakeDefaultBuffer(*SharedDevice().Device(), byteSize);

            D3D12UploadRing::UploadRequest request;
            request.sourceBytes = std::as_bytes(std::span(level.vertices));
            request.destination = destination.Get();
            request.generation = generation.Snapshot(); // current -- taken after Advance() above
            request.clusterId = cluster.clusterId;
            request.lodLevel = static_cast<uint32_t>(level.lod);

            REQUIRE(ring.Upload(request) == D3D12UploadRing::UploadResult::Uploaded);
            destinations.push_back(destination);
            ++expectedReadyCount;
        }
    }
    // Successively finer LODs of the same cluster must carry more
    // vertices -- proves GenerateSyntheticClusters actually produced
    // "bounded clusters/proxy/three LODs," not three identically-sized
    // levels.
    CHECK(clusters[0].lods[0].vertices.size() < clusters[0].lods[1].vertices.size());
    CHECK(clusters[0].lods[1].vertices.size() < clusters[0].lods[2].vertices.size());

    // One extra upload deliberately tagged with the pre-Advance() token.
    auto staleDestination = MakeDefaultBuffer(*SharedDevice().Device(), 64);
    std::vector<std::byte> staleSource(64, std::byte{ 1 });
    D3D12UploadRing::UploadRequest staleRequest;
    staleRequest.sourceBytes = staleSource;
    staleRequest.destination = staleDestination.Get();
    staleRequest.generation = staleToken;
    staleRequest.clusterId = 999u;
    staleRequest.lodLevel = 0;
    REQUIRE(ring.Upload(staleRequest) == D3D12UploadRing::UploadResult::Uploaded);

    ring.FlushBatch();

    SceneSnapshotPtr snapshot = DrainUntil(ring, generation, expectedReadyCount);
    REQUIRE(snapshot->ReadyResources().size() == expectedReadyCount);
    for (const auto& info : snapshot->ReadyResources()) {
        CHECK(info.clusterId != 999u); // the stale-generation upload never appears
    }

    // The earlier snapshot object is a distinct, still-empty instance --
    // epoch/handle swap, not in-place mutation of a shared snapshot.
    CHECK(before->ReadyResources().empty());
}

// Gate 2 fault injection: "delayed copy fences" -- filling a gap the
// existing tests above only covered incidentally (via the empty-before-
// any-upload check), not as a deliberate held-open scenario.
TEST_CASE("A delayed copy fence leaves CurrentSnapshot unchanged until it actually completes", "[graphics]")
{
    D3D12UploadRing ring;
    REQUIRE(ring.Initialize(SharedDevice()));

    platform::GenerationSource generation;
    SceneSnapshotPtr before = ring.CurrentSnapshot();
    REQUIRE(before->ReadyResources().empty());

    std::vector<std::byte> source(256, std::byte{ 0xCD });
    auto destination = MakeDefaultBuffer(*SharedDevice().Device(), source.size());

    D3D12UploadRing::UploadRequest request;
    request.sourceBytes = source;
    request.destination = destination.Get();
    request.generation = generation.Snapshot();
    request.clusterId = 42;
    request.lodLevel = 0;

    REQUIRE(ring.Upload(request) == D3D12UploadRing::UploadResult::Uploaded);
    // Deliberately not flushed/drained yet -- the batch (and therefore its
    // fence) hasn't even been submitted, simulating a delayed-copy-fence
    // fault: nothing has completed, so nothing should be visible yet.
    SceneSnapshotPtr stillPending = ring.CurrentSnapshot();
    CHECK(stillPending == before); // the exact same snapshot object -- no premature swap
    CHECK(stillPending->ReadyResources().empty());

    // Now let it actually complete, proving the delay was real (this
    // wasn't an API that simply never updates) and the snapshot changes
    // once the fence genuinely retires.
    ring.FlushBatch();
    SceneSnapshotPtr after = DrainUntil(ring, generation, 1);
    REQUIRE(after->ReadyResources().size() == 1);
    CHECK(after != before);
}

// Gate 2 fault injection: "OOM" -- a request too large for the ring to
// ever satisfy, even after growing to its configured cap, must fail
// cleanly rather than crash or corrupt ring state.
TEST_CASE("A single allocation larger than the ring's configured cap is rejected cleanly, not attempted",
          "[graphics]")
{
    D3D12UploadRing::CreateOptions options;
    options.initialCapacityBytes = 1024;
    options.growthIncrementBytes = 1024;
    options.maxCapacityBytes = 4096; // hard ceiling this ring can ever grow to

    D3D12UploadRing ring;
    REQUIRE(ring.Initialize(SharedDevice(), options));

    platform::GenerationSource generation;
    std::vector<std::byte> tooLarge(options.maxCapacityBytes + 1, std::byte{ 0x01 });
    auto destination = MakeDefaultBuffer(*SharedDevice().Device(), tooLarge.size());

    D3D12UploadRing::UploadRequest request;
    request.sourceBytes = tooLarge;
    request.destination = destination.Get();
    request.generation = generation.Snapshot();

    CHECK(ring.Upload(request) == D3D12UploadRing::UploadResult::Failed);

    // The ring itself is unaffected -- still usable for a properly-sized
    // request afterward, proving this was a clean rejection, not a
    // corrupted/wedged ring.
    std::vector<std::byte> smallPayload(64, std::byte{ 0x02 });
    auto smallDestination = MakeDefaultBuffer(*SharedDevice().Device(), smallPayload.size());
    D3D12UploadRing::UploadRequest smallRequest;
    smallRequest.sourceBytes = smallPayload;
    smallRequest.destination = smallDestination.Get();
    smallRequest.generation = generation.Snapshot();
    CHECK(ring.Upload(smallRequest) == D3D12UploadRing::UploadResult::Uploaded);
}

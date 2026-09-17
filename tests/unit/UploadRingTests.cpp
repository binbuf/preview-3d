// Gate 2 workstream B, slice 3: upload ring + copy queue/fence
// integration + fence-complete publication, exercised end-to-end against
// a synthetic in-process streaming source. See
// .docs/design/04-rendering-and-streaming.md ("Upload ring",
// "Fence-complete publication") and .docs/design/10-delivery-plan.md's
// Gate 2 exit criteria (ring wrap property, forced-delay/previous-LOD
// behavior).

#include "D3D12Device.h"
#include "D3D12UploadRing.h"
#include "D3D12ViewerPath.h"
#include "DetailView.h"
#include "WicImageDecodeAdapter.h"
#include "TextureTranscodeAdapter.h"
#include <filesystem>
#include <fstream>
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

// A DEFAULT-heap Texture2D created in COMMON, which is what
// UploadTextureRequest requires: the copy queue implicitly promotes
// COMMON -> COPY_DEST and the state decays back to COMMON afterwards, so
// the direct queue can promote it again without a barrier the copy queue
// could not have recorded.
Microsoft::WRL::ComPtr<ID3D12Resource> MakeDefaultTexture(ID3D12Device& device, uint32_t width,
                                                            uint32_t height, DXGI_FORMAT format, uint16_t levels=1)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = levels;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    REQUIRE(SUCCEEDED(device.CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&resource))));
    return resource;
}

// Reads a mip-0 Texture2D back through the ring's copy queue and returns
// its rows re-tightened (padding removed), so a comparison against the
// tightly-packed source is direct rather than pitch-aware at every call
// site.
std::vector<std::byte> ReadBackTexture(D3D12Device& device, D3D12UploadRing& ring,
                                        ID3D12Resource& texture, uint32_t width, uint32_t height,
                                        DXGI_FORMAT format, uint32_t subresource=0)
{
    const auto desc=texture.GetDesc();
    (void)width;(void)height;(void)format;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    device.Device()->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    auto readback
        = MakeBuffer(*device.Device(), totalBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    REQUIRE(SUCCEEDED(device.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                                IID_PPV_ARGS(&allocator))));
    REQUIRE(SUCCEEDED(device.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(),
                                                           nullptr, IID_PPV_ARGS(&commandList))));

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = &texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = subresource;

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;

    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    REQUIRE(SUCCEEDED(commandList->Close()));

    ID3D12CommandList* lists[] = { commandList.Get() };
    ring.CopyQueue().Queue()->ExecuteCommandLists(1, lists);
    uint64_t fenceValue = ring.CopyQueue().SignalNext();
    REQUIRE(ring.CopyQueue().WaitForValue(fenceValue, 5000) == D3D12CommandQueue::WaitResult::Signaled);

    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalBytes) };
    void* mapped = nullptr;
    REQUIRE(SUCCEEDED(readback->Map(0, &readRange, &mapped)));
    std::vector<std::byte> tight(static_cast<size_t>(numRows) * static_cast<size_t>(rowSizeInBytes));
    const auto* padded = static_cast<const std::byte*>(mapped);
    for (UINT row = 0; row < numRows; ++row) {
        std::memcpy(tight.data() + static_cast<size_t>(row) * rowSizeInBytes,
                     padded + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                     static_cast<size_t>(rowSizeInBytes));
    }
    D3D12_RANGE writtenRange{ 0, 0 };
    readback->Unmap(0, &writtenRange);
    return tight;
}

// A deterministic tightly-packed RGBA8 source -- every byte distinct
// enough that a row misalignment shows up as a mismatch rather than
// coincidentally matching.
std::vector<std::byte> MakeTightRgba8(uint32_t width, uint32_t height)
{
    std::vector<std::byte> bytes(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>((i * 7 + 13) & 0xFF);
    }
    return bytes;
}

// Counts D3D12 debug-layer messages at ERROR or CORRUPTION severity.
// Returns -1 when the info queue is unavailable (Release, or a machine
// without the Graphics Tools feature) -- reported, not failed.
int DebugLayerErrorCount(bool& queueAvailable)
{
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(SharedDevice().Device()->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
        queueAvailable = false;
        return -1;
    }
    queueAvailable = true;

    int errors = 0;
    const UINT64 count = infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T length = 0;
        if (FAILED(infoQueue->GetMessage(i, nullptr, &length))) continue;
        std::vector<std::byte> storage(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(infoQueue->GetMessage(i, message, &length))) continue;
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR
            || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
            ++errors;
        }
    }
    return errors;
}

} // namespace

TEST_CASE("A texture uploads through the ring's copy queue and reads back byte-exact", "[graphics]")
{
    // The question this settles: PROGRESS.md recorded implicit COMMON-state
    // promotion as confirmed for buffers on the copy queue but explicitly
    // "not yet exercised for textures". A texture created in COMMON must
    // promote to COPY_DEST for CopyTextureRegion and decay back afterwards,
    // with no barrier -- a copy queue cannot record one.
    D3D12UploadRing ring;
    REQUIRE(ring.Initialize(SharedDevice(), D3D12UploadRing::CreateOptions{}));

    constexpr uint32_t kWidth = 8;
    constexpr uint32_t kHeight = 8;
    auto source = MakeTightRgba8(kWidth, kHeight);
    auto texture = MakeDefaultTexture(*SharedDevice().Device(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM);

    platform::GenerationSource generation;
    D3D12UploadRing::UploadTextureRequest request;
    request.sourceBytes = std::span<const std::byte>(source);
    request.destination = texture.Get();
    request.width = kWidth;
    request.height = kHeight;
    request.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    request.generation = generation.Snapshot();
    request.clusterId = 1;

    REQUIRE(ring.UploadTexture(request) == D3D12UploadRing::UploadResult::Uploaded);
    ring.FlushBatch();

    auto snapshot = DrainUntil(ring, generation, 1);
    REQUIRE(snapshot->ReadyResources().size() == 1);
    CHECK(snapshot->ReadyResources()[0].resource == texture.Get());

    auto readBack
        = ReadBackTexture(SharedDevice(), ring, *texture.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    REQUIRE(readBack.size() == source.size());
    CHECK(std::memcmp(readBack.data(), source.data(), source.size()) == 0);
}

TEST_CASE("A texture whose packed row pitch is not 256-aligned is repitched correctly", "[graphics]")
{
    // 3 px * 4 bytes = 12 bytes per source row against a 256-byte staging
    // pitch. This is precisely the case a bulk memcpy of the wire format's
    // tightly-packed bytes gets silently wrong, so a byte-exact readback
    // here is the whole point of the test.
    D3D12UploadRing ring;
    REQUIRE(ring.Initialize(SharedDevice(), D3D12UploadRing::CreateOptions{}));

    constexpr uint32_t kWidth = 3;
    constexpr uint32_t kHeight = 5;
    auto source = MakeTightRgba8(kWidth, kHeight);
    auto texture = MakeDefaultTexture(*SharedDevice().Device(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM);

    platform::GenerationSource generation;
    D3D12UploadRing::UploadTextureRequest request;
    request.sourceBytes = std::span<const std::byte>(source);
    request.destination = texture.Get();
    request.width = kWidth;
    request.height = kHeight;
    request.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    request.generation = generation.Snapshot();

    REQUIRE(ring.UploadTexture(request) == D3D12UploadRing::UploadResult::Uploaded);
    ring.FlushBatch();
    DrainUntil(ring, generation, 1);

    auto readBack
        = ReadBackTexture(SharedDevice(), ring, *texture.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    REQUIRE(readBack.size() == source.size());
    CHECK(std::memcmp(readBack.data(), source.data(), source.size()) == 0);
}

TEST_CASE("A texture upload following an unaligned buffer upload still lands aligned", "[graphics]")
{
    // A buffer upload of a deliberately odd size leaves the ring's write
    // cursor off any 512-byte boundary. CopyTextureRegion requires its
    // placed-footprint offset be D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
    // aligned, so without the allocator's alignment support this produces a
    // debug-layer error and wrong pixels -- both of which the readback and
    // the debug-layer case below would catch.
    D3D12UploadRing ring;
    REQUIRE(ring.Initialize(SharedDevice(), D3D12UploadRing::CreateOptions{}));

    platform::GenerationSource generation;

    std::vector<std::byte> bufferSource(37, std::byte{ 0xAB });
    auto buffer = MakeDefaultBuffer(*SharedDevice().Device(), bufferSource.size());
    D3D12UploadRing::UploadRequest bufferRequest;
    bufferRequest.sourceBytes = std::span<const std::byte>(bufferSource);
    bufferRequest.destination = buffer.Get();
    bufferRequest.generation = generation.Snapshot();
    bufferRequest.clusterId = 1;
    REQUIRE(ring.Upload(bufferRequest) == D3D12UploadRing::UploadResult::Uploaded);

    constexpr uint32_t kWidth = 4;
    constexpr uint32_t kHeight = 4;
    auto textureSource = MakeTightRgba8(kWidth, kHeight);
    auto texture = MakeDefaultTexture(*SharedDevice().Device(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12UploadRing::UploadTextureRequest textureRequest;
    textureRequest.sourceBytes = std::span<const std::byte>(textureSource);
    textureRequest.destination = texture.Get();
    textureRequest.width = kWidth;
    textureRequest.height = kHeight;
    textureRequest.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureRequest.generation = generation.Snapshot();
    textureRequest.clusterId = 2;
    REQUIRE(ring.UploadTexture(textureRequest) == D3D12UploadRing::UploadResult::Uploaded);

    ring.FlushBatch();
    auto snapshot = DrainUntil(ring, generation, 2);
    // Geometry and textures share one lane and one fence, so they become
    // ready in the same snapshot rather than racing each other.
    CHECK(snapshot->ReadyResources().size() == 2);

    auto bufferBack = ReadBack(SharedDevice(), ring, *buffer.Get(), bufferSource.size());
    CHECK(std::memcmp(bufferBack.data(), bufferSource.data(), bufferSource.size()) == 0);
    auto textureBack
        = ReadBackTexture(SharedDevice(), ring, *texture.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    REQUIRE(textureBack.size() == textureSource.size());
    CHECK(std::memcmp(textureBack.data(), textureSource.data(), textureSource.size()) == 0);
}

TEST_CASE("A whole model's upload never touches the direct queue", "[graphics]")
{
    // Gate 2's exit criterion is "the direct queue records no ordinary
    // load-time wait on the copy fence". The product path used to submit
    // staging copies to the direct queue and block on them, which
    // contradicts it outright.
    //
    // Asserted through fence values rather than timing: a D3D12 fence only
    // advances on an explicit Signal, so a direct queue whose next and
    // completed values are unmoved across a full upload cycle provably had
    // nothing submitted to it and waited on nothing.
    D3D12CommandQueue directQueue;
    REQUIRE(directQueue.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"Direct"));

    D3D12UploadRing ring;
    REQUIRE(ring.Initialize(SharedDevice(), D3D12UploadRing::CreateOptions{}));

    // The lane itself: uploads must be on a COPY-typed queue, not a second
    // direct one, or "off the direct queue" would be true only by accident.
    CHECK(ring.CopyQueue().Queue()->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COPY);

    const uint64_t directCompletedBefore = directQueue.CompletedValue();

    platform::GenerationSource generation;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> keepAlive;

    // A model's worth of work: several mesh buffer pairs plus a texture,
    // which is what BeginUploadModel queues.
    for (uint32_t i = 0; i < 3; ++i) {
        std::vector<std::byte> vertices(4096, std::byte{ 0x11 });
        std::vector<std::byte> indices(1024, std::byte{ 0x22 });
        for (const auto* payload : { &vertices, &indices }) {
            auto buffer = MakeDefaultBuffer(*SharedDevice().Device(), payload->size());
            D3D12UploadRing::UploadRequest request;
            request.sourceBytes = std::span<const std::byte>(*payload);
            request.destination = buffer.Get();
            request.generation = generation.Snapshot();
            request.clusterId = static_cast<uint32_t>(keepAlive.size());
            REQUIRE(ring.Upload(request) == D3D12UploadRing::UploadResult::Uploaded);
            keepAlive.push_back(buffer);
        }
    }

    auto textureSource = MakeTightRgba8(16, 16);
    auto texture = MakeDefaultTexture(*SharedDevice().Device(), 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12UploadRing::UploadTextureRequest textureRequest;
    textureRequest.sourceBytes = std::span<const std::byte>(textureSource);
    textureRequest.destination = texture.Get();
    textureRequest.width = 16;
    textureRequest.height = 16;
    textureRequest.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureRequest.generation = generation.Snapshot();
    textureRequest.clusterId = 1000;
    REQUIRE(ring.UploadTexture(textureRequest) == D3D12UploadRing::UploadResult::Uploaded);
    keepAlive.push_back(texture);

    ring.FlushBatch();
    auto snapshot = DrainUntil(ring, generation, 7);
    REQUIRE(snapshot->ReadyResources().size() == 7);
    ring.ReclaimCompleted();

    // The whole model reached the GPU and the direct queue never moved.
    CHECK(directQueue.CompletedValue() == directCompletedBefore);
}

TEST_CASE("A mixed buffer/texture upload batch raises no debug-layer error", "[graphics]")
{
    // Passing tests are not evidence on their own here: an illegal resource
    // state or a misaligned footprint reports through OutputDebugString and
    // fails nothing unless the info queue is inspected. This is what
    // actually confirms the implicit COMMON -> COPY_DEST -> COMMON promotion
    // for textures on a copy queue is legal rather than merely working by
    // accident on this driver.
    bool queueAvailable = false;
    const int before = DebugLayerErrorCount(queueAvailable);
    if (!queueAvailable) {
        WARN("D3D12 info queue unavailable (Release build or no Graphics Tools feature) -- "
             "debug-layer assertion skipped");
        return;
    }

    D3D12UploadRing ring;
    REQUIRE(ring.Initialize(SharedDevice(), D3D12UploadRing::CreateOptions{}));

    platform::GenerationSource generation;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> keepAlive;

    for (uint32_t i = 0; i < 4; ++i) {
        std::vector<std::byte> bufferSource(64 + i * 17, std::byte{ 0x5A });
        auto buffer = MakeDefaultBuffer(*SharedDevice().Device(), bufferSource.size());
        D3D12UploadRing::UploadRequest bufferRequest;
        bufferRequest.sourceBytes = std::span<const std::byte>(bufferSource);
        bufferRequest.destination = buffer.Get();
        bufferRequest.generation = generation.Snapshot();
        bufferRequest.clusterId = i * 2;
        REQUIRE(ring.Upload(bufferRequest) == D3D12UploadRing::UploadResult::Uploaded);
        keepAlive.push_back(buffer);

        const uint32_t dimension = 2 + i;
        auto textureSource = MakeTightRgba8(dimension, dimension);
        auto texture
            = MakeDefaultTexture(*SharedDevice().Device(), dimension, dimension, DXGI_FORMAT_R8G8B8A8_UNORM);
        D3D12UploadRing::UploadTextureRequest textureRequest;
        textureRequest.sourceBytes = std::span<const std::byte>(textureSource);
        textureRequest.destination = texture.Get();
        textureRequest.width = dimension;
        textureRequest.height = dimension;
        textureRequest.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureRequest.generation = generation.Snapshot();
        textureRequest.clusterId = i * 2 + 1;
        REQUIRE(ring.UploadTexture(textureRequest) == D3D12UploadRing::UploadResult::Uploaded);
        keepAlive.push_back(texture);
    }

    ring.FlushBatch();
    auto snapshot = DrainUntil(ring, generation, 8);
    CHECK(snapshot->ReadyResources().size() == 8);

    const int after = DebugLayerErrorCount(queueAvailable);
    CHECK(after == before);
}

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

TEST_CASE("Texture chains copy each padded subresource and publish only after the last immutable mip", "[graphics][texture-upload]")
{
    bool debug=false;const int before=DebugLayerErrorCount(debug);
    for (DXGI_FORMAT format:{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_BC7_UNORM_SRGB}) {
        D3D12UploadRing ring;D3D12UploadRing::CreateOptions options;
        options.initialCapacityBytes=8192;options.maxCapacityBytes=8192;options.growthIncrementBytes=0;options.maxBatchBytes=1;
        REQUIRE(ring.Initialize(SharedDevice(),options));
        auto texture=MakeDefaultTexture(*SharedDevice().Device(),7,5,format,3);
        platform::GenerationSource generation;
        std::vector<std::vector<std::byte>> levels(3);
        for (uint32_t mip=3;mip-->0;) {
            const uint32_t w=7u>>mip,h=5u>>mip;
            const uint32_t row=format==DXGI_FORMAT_BC7_UNORM_SRGB ? ((w+3)/4)*16 : w*4;
            const uint32_t rows=format==DXGI_FORMAT_BC7_UNORM_SRGB ? (h+3)/4 : h;
            levels[mip].resize(size_t(row)*rows,std::byte(uint8_t(40+mip)));
            D3D12UploadRing::UploadTextureRequest request;
            request.sourceBytes=levels[mip];request.destination=texture.Get();request.width=w;request.height=h;
            request.destinationSubresource=mip;request.format=format;request.generation=generation.Snapshot();
            request.publishResource=mip==0;
            REQUIRE(ring.UploadTexture(request)==D3D12UploadRing::UploadResult::Uploaded);
            REQUIRE(ring.CopyQueue().WaitForValue(ring.LastSubmittedFenceValue(),5000)==D3D12CommandQueue::WaitResult::Signaled);
            auto snapshot=ring.DrainCompletedPublications(generation);
            CHECK(snapshot->ReadyResources().size()==(mip==0 ? 1u : 0u));
        }
        for (uint32_t mip=0;mip<3;++mip) CHECK(ReadBackTexture(SharedDevice(),ring,*texture.Get(),7u>>mip,5u>>mip,format,mip)==levels[mip]);
        D3D12UploadRing::UploadTextureRequest bad;
        bad.sourceBytes=levels[0];bad.destination=texture.Get();bad.width=7;bad.height=5;bad.format=format;
        bad.destinationSubresource=3;CHECK(ring.UploadTexture(bad)==D3D12UploadRing::UploadResult::Failed);
        bad.destinationSubresource=1;CHECK(ring.UploadTexture(bad)==D3D12UploadRing::UploadResult::Failed);
        bad.destinationSubresource=0;levels[0].push_back(std::byte{0});bad.sourceBytes=levels[0];
        CHECK(ring.UploadTexture(bad)==D3D12UploadRing::UploadResult::Failed);
    }
    if (debug) { bool available=false;CHECK(DebugLayerErrorCount(available)==before); }
}

TEST_CASE("Coarse regions share bounded GPU buffers and publish only after every slice copy completes", "[graphics][coarse-proxy]")
{
    using namespace model_core;
    D3D12ViewerPath uploader; uploader.device.AttachForUpload(SharedDevice().Device());
    REQUIRE(uploader.uploadRing.Initialize(uploader.device));
    Microsoft::WRL::ComPtr<ID3D12Fence> gate;
    REQUIRE(SUCCEEDED(uploader.device.Device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate))));
    REQUIRE(SUCCEEDED(uploader.uploadRing.CopyQueue().Queue()->Wait(gate.Get(),1)));
    std::vector<d3d12_import_bridge::ImportedMesh> meshes(2);
    std::vector<std::byte> expectedVertices,expectedIndices;
    for (unsigned i=0;i<2;++i) {
        auto& mesh=meshes[i]; mesh.chunkId=kCoarseIdentity|(i+1); mesh.vertexCount=3; mesh.indexCount=3;
        mesh.topology=ChunkTopology::TriangleList; mesh.vertexLayoutId=VertexLayoutId::PositionOnly_F32;
        mesh.geometry.lodLevel=kCoarseLod;
        const VertexPositionOnlyF32 vertices[]{{float(i*10),0,0},{float(i*10+1),0,0},{float(i*10),1,0}};
        const uint32_t indices[]{0,1,2};
        auto vb=std::as_bytes(std::span(vertices)); auto ib=std::as_bytes(std::span(indices));
        mesh.payload.assign(vb.begin(),vb.end()); mesh.payload.insert(mesh.payload.end(),ib.begin(),ib.end());
        expectedVertices.insert(expectedVertices.end(),vb.begin(),vb.end()); expectedIndices.insert(expectedIndices.end(),ib.begin(),ib.end());
    }
    std::wstring error; REQUIRE(uploader.BeginUploadModel(meshes,{}, {},error));
    CHECK_FALSE(uploader.PollUploads());
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!uploader.PollUploads() && std::chrono::steady_clock::now()<deadline) Sleep(1);
    REQUIRE(uploader.hasModel); REQUIRE(uploader.model.meshes.size()==2);
    const auto& first=uploader.model.meshes[0]; const auto& second=uploader.model.meshes[1];
    CHECK(first.vertexBuffer==second.vertexBuffer); CHECK(first.indexBuffer==second.indexBuffer);
    CHECK(second.vbv.BufferLocation-first.vbv.BufferLocation==36);
    CHECK(second.ibv.BufferLocation-first.ibv.BufferLocation==12);
    CHECK(ReadBack(uploader.device,uploader.uploadRing,*first.vertexBuffer.Get(),expectedVertices.size())==expectedVertices);
    CHECK(ReadBack(uploader.device,uploader.uploadRing,*first.indexBuffer.Get(),expectedIndices.size())==expectedIndices);
    const auto vd=first.vertexBuffer->GetDesc(),id=first.indexBuffer->GetDesc();
    const auto va=uploader.device.Device()->GetResourceAllocationInfo(0,1,&vd),ia=uploader.device.Device()->GetResourceAllocationInfo(0,1,&id);
    CHECK(va.SizeInBytes+ia.SizeInBytes==131072);
    uploader.WaitForIdle();
}

TEST_CASE("Product texture uploader validates complete payloads and preserves immutable color-space mip chains", "[graphics][texture-upload]")
{
    D3D12ViewerPath uploader;uploader.device.AttachForUpload(SharedDevice().Device());
    D3D12UploadRing::CreateOptions options;options.initialCapacityBytes=8192;options.maxCapacityBytes=8192;options.growthIncrementBytes=0;
    REQUIRE(uploader.uploadRing.Initialize(uploader.device,options));
    d3d12_import_bridge::ImportedImage image;image.chunkId=3;image.logicalChunkId=1;
    image.pixelFormat=model_core::PixelFormatId::RGBA8_UNORM;image.colorSpace=model_core::ColorSpaceId::Srgb;
    image.width=7;image.height=5;image.mipLevels=3;
    std::vector<std::vector<std::byte>> levels;
    for (uint32_t level=0;level<3;++level) {
        levels.push_back(MakeTightRgba8(7u>>level,5u>>level));
        image.pixelBytes.insert(image.pixelBytes.end(),levels.back().begin(),levels.back().end());
    }
    std::wstring error;
    REQUIRE(uploader.BeginUploadModel({}, {}, {image},error));
    for (unsigned i=0;i<2000 && !uploader.PollUploads();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(uploader.model.textures.size()==1);
    auto& texture=uploader.model.textures.front();CHECK(texture.chunkId==1);
    CHECK(texture.resource->GetDesc().Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    CHECK(texture.resource->GetDesc().MipLevels==3);
    for (uint32_t level=0;level<3;++level)
        CHECK(ReadBackTexture(SharedDevice(),uploader.uploadRing,*texture.resource.Get(),7u>>level,5u>>level,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,level)==levels[level]);
    image.pixelBytes.push_back(std::byte{0});CHECK_FALSE(uploader.BeginUploadModel({}, {}, {image},error));
    image.pixelBytes.pop_back();image.colorSpace=model_core::ColorSpaceId::Linear;
    REQUIRE(uploader.BeginUploadModel({}, {}, {image},error));
    for (unsigned i=0;i<2000 && !uploader.PollUploads();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(uploader.model.textures.size()==1);CHECK(uploader.model.textures[0].resource->GetDesc().Format==DXGI_FORMAT_R8G8B8A8_UNORM);
    uploader.uploadIsCancelled=[] {return true;};CHECK_FALSE(uploader.BeginUploadModel({}, {}, {image},error));
    uploader.WaitForIdle();
}

TEST_CASE("One uploaded geometry allocation backs many stable hierarchical instance draws", "[graphics][fbx-002][instances]")
{
    const double identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const double preciseOrigin[3]={1.0e12+0.25,-2.0e12+0.5,3.0e12+0.75};
    const double preciseScene[3]={1.0e12,-2.0e12,3.0e12};const double preciseCamera[3]={0.125,0.25,0.5};
    DirectX::XMFLOAT4X4 identityAxis;DirectX::XMStoreFloat4x4(&identityAxis,DirectX::XMMatrixIdentity());
    const auto relative=BuildCameraRelativeInstanceTransform(identity,preciseOrigin,preciseScene,preciseCamera,identityAxis);
    CHECK(relative.localToCamera._41==0.125f);CHECK(relative.localToCamera._42==0.25f);CHECK(relative.localToCamera._43==0.25f);
    D3D12ViewerPath uploader;uploader.device.AttachForUpload(SharedDevice().Device());
    D3D12UploadRing::CreateOptions options;options.initialCapacityBytes=64*1024;options.maxCapacityBytes=64*1024;options.growthIncrementBytes=0;
    REQUIRE(uploader.uploadRing.Initialize(uploader.device,options));
    d3d12_import_bridge::ImportedMesh mesh;mesh.chunkId=1;mesh.topology=model_core::ChunkTopology::TriangleList;
    mesh.vertexLayoutId=model_core::VertexLayoutId::PositionOnly_F32;mesh.vertexCount=3;mesh.indexCount=3;
    mesh.geometry.chunkId=1;mesh.geometry.topology=mesh.topology;mesh.geometry.vertexCount=3;mesh.geometry.indexCount=3;
    mesh.geometry.vertexLayoutId=uint32_t(mesh.vertexLayoutId);mesh.geometry.origin[0]=1.0e12;
    const model_core::VertexPositionOnlyF32 vertices[3]={{0,0,0},{1,0,0},{0,1,0}};const uint32_t indices[3]={0,1,2};
    const auto vb=std::as_bytes(std::span(vertices));const auto ib=std::as_bytes(std::span(indices));
    mesh.payload.assign(vb.begin(),vb.end());mesh.payload.insert(mesh.payload.end(),ib.begin(),ib.end());
    std::vector<d3d12_import_bridge::ImportedNode> nodes(32);
    std::vector<d3d12_import_bridge::ImportedInstance> instances(32);
    for(uint32_t i=0;i<32;++i){auto& node=nodes[i].data;node.nodeId=100+i;node.parentNodeId=i?100:0;
        node.flags=model_core::kSceneRecordVisible;node.localTransform[0]=i==31?-1:1;
        node.localTransform[5]=node.localTransform[10]=node.localTransform[15]=1;node.localTransform[12]=i?double(i)*8:1.0e12;
        auto& instance=instances[i].data;instance.instanceId=1000+i;instance.nodeId=node.nodeId;
        instance.geometryChunkId=1;instance.flags=model_core::kSceneRecordVisible;
        instance.worldMin[0]=1.0e12;instance.worldMax[0]=1.0e12+1;instance.worldMin[1]=0;instance.worldMax[1]=1;
    }
    std::wstring error;REQUIRE(uploader.BeginUploadModel({mesh},{},{},error,nodes,instances));
    REQUIRE(uploader.pendingModel.meshes.size()==32);CHECK(uploader.pendingResourceCount==2);
    auto* sharedVertex=uploader.pendingModel.meshes[0].vertexBuffer.Get();auto* sharedIndex=uploader.pendingModel.meshes[0].indexBuffer.Get();
    for(uint32_t i=0;i<32;++i){const auto& draw=uploader.pendingModel.meshes[i];
        CHECK(draw.vertexBuffer.Get()==sharedVertex);CHECK(draw.indexBuffer.Get()==sharedIndex);
        CHECK(draw.instanceId==1000+i);CHECK(draw.instanceTransform[12]>=1.0e12);}
    CHECK(uploader.pendingModel.meshes.back().mirrored);
    uploader.WaitForIdle();
}

TEST_CASE("PNG JPEG and Basis decode-to-product-upload pixels match frozen goldens at every mip", "[graphics][texture-upload]")
{
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct ComGuard { HRESULT h;~ComGuard(){if (SUCCEEDED(h))CoUninitialize();} } comGuard{com};
    for (const wchar_t* name:{L"textures/gray.png",L"textures/gray.jpg",L"basisu_sample.ktx2"}) {
        std::ifstream file(std::filesystem::path(PREVIEW3D_TEST_ASSETS_DIR)/name,std::ios::binary|std::ios::ate);
        REQUIRE(file.good());const auto size=file.tellg();REQUIRE(size>0);file.seekg(0);
        std::vector<std::byte> encoded(static_cast<size_t>(size));file.read(reinterpret_cast<char*>(encoded.data()),size);REQUIRE(file.good());
        d3d12_import_bridge::ImportedImage image;image.chunkId=1;
        const bool basis=std::wstring(name)==L"basisu_sample.ktx2";
        image.colorSpace=basis ? model_core::ColorSpaceId::Linear : model_core::ColorSpaceId::Srgb;
        if (basis) {
            import_worker::TextureDecodeOptions options;options.semantic=import_worker::TextureSemantic::Data;
            auto decoded=import_worker::TranscodeKtx2BasisImage(encoded,options);REQUIRE(decoded);
            image.pixelFormat=decoded->pixelFormat;image.width=decoded->width;image.height=decoded->height;
            image.mipLevels=decoded->mipLevels;image.pixelBytes=std::move(decoded->pixelBytes);
        } else {
            auto decoded=import_worker::DecodeRasterImageWic(encoded,image.colorSpace);REQUIRE(decoded);
            image.pixelFormat=decoded->pixelFormat;image.width=decoded->width;image.height=decoded->height;
            image.mipLevels=decoded->mipLevels;image.pixelBytes=std::move(decoded->pixelBytes);
        }
        REQUIRE(image.pixelFormat==model_core::PixelFormatId::RGBA8_UNORM);
        D3D12ViewerPath uploader;uploader.device.AttachForUpload(SharedDevice().Device());
        D3D12UploadRing::CreateOptions options;options.initialCapacityBytes=8192;options.maxCapacityBytes=8192;options.growthIncrementBytes=0;
        REQUIRE(uploader.uploadRing.Initialize(uploader.device,options));std::wstring error;
        REQUIRE(uploader.BeginUploadModel({}, {}, {image},error));
        for (unsigned i=0;i<2000 && !uploader.PollUploads();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(uploader.model.textures.size()==1);
        auto& texture=*uploader.model.textures[0].resource.Get();
        for (uint32_t mip=0;mip<image.mipLevels;++mip) {
            const uint32_t w=image.width>>mip,h=image.height>>mip;
            const auto pixels=ReadBackTexture(SharedDevice(),uploader.uploadRing,texture,w,h,texture.GetDesc().Format,mip);
            for (uint32_t y=0;y<h;++y)for (uint32_t x=0;x<w;++x) {
                const size_t at=(size_t(y)*w+x)*4;
                for (unsigned c=0;c<3;++c) {
                    const int expected=basis && c<2 ? ((c==0 ? x : y)<4 ? 220 : 40) : 128;
                    CHECK(std::abs(int(std::to_integer<uint8_t>(pixels[at+c]))-expected)<=(basis ? 8 : 2));
                }
                CHECK(pixels[at+3]==std::byte{255});
            }
        }
        uploader.WaitForIdle();
    }
}

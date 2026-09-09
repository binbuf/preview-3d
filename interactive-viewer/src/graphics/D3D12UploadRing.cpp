#include "D3D12UploadRing.h"

#include "D3D12Device.h"

#include <platform/CheckedMath.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

bool D3D12UploadRing::CreateUploadResource(uint64_t sizeBytes, ComPtr<ID3D12Resource>& outResource,
                                            std::byte*& outMapped)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = device_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        return false;
    }

    // The CPU only ever writes into this heap -- an empty read range is
    // the documented way to tell the runtime/debug layer this mapping is
    // never read back, matching D3D12's persistent-upload-mapping pattern.
    void* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    hr = resource->Map(0, &noRead, &mapped);
    if (FAILED(hr)) {
        return false;
    }

    outResource = resource;
    outMapped = static_cast<std::byte*>(mapped);
    return true;
}

bool D3D12UploadRing::Initialize(D3D12Device& device, const CreateOptions& options)
{
    device_ = device.Device();
    options_ = options;

    if (!copyQueue_.Initialize(*device_, D3D12_COMMAND_LIST_TYPE_COPY, L"Upload")) {
        return false;
    }

    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&commandAllocator_)))) {
        return false;
    }
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, commandAllocator_.Get(), nullptr,
                                           IID_PPV_ARGS(&commandList_)))) {
        return false;
    }
    // CreateCommandList returns it open; close before the first Reset(),
    // matching the FrameRecorder convention in SwapChainTests.cpp.
    commandList_->Close();

    ComPtr<ID3D12Resource> resource;
    std::byte* mapped = nullptr;
    if (!CreateUploadResource(options_.initialCapacityBytes, resource, mapped)) {
        return false;
    }
    ringResource_ = resource;
    mapped_ = mapped;
    capacity_ = options_.initialCapacityBytes;
    writeOffset_ = 0;
    usedBytes_ = 0;
    return true;
}

bool D3D12UploadRing::BeginBatch()
{
    if (lastSubmittedFenceValue_ != 0) {
        if (copyQueue_.WaitForValue(lastSubmittedFenceValue_, /*timeoutMs=*/10000)
            != D3D12CommandQueue::WaitResult::Signaled) {
            return false;
        }
    }
    if (FAILED(commandAllocator_->Reset())) {
        return false;
    }
    if (FAILED(commandList_->Reset(commandAllocator_.Get(), nullptr))) {
        return false;
    }
    commandListOpen_ = true;
    return true;
}

bool D3D12UploadRing::Grow()
{
    // Any CopyBufferRegion already recorded in the currently-open,
    // not-yet-submitted batch reads from the resource about to be
    // retired below. Flush first so lastSubmittedFenceValue_ genuinely
    // covers that work -- otherwise ReclaimCompleted() could release the
    // old resource (it's just a ComPtr; D3D12 command lists do not keep
    // resources alive on your behalf) while a still-unexecuted command
    // list still references it.
    if (commandListOpen_ && batchBytes_ > 0) {
        FlushBatch();
    }

    // Deliberately not std::min: <windows.h> (pulled in transitively via
    // d3d12.h) defines min/max function-like macros unless NOMINMAX is
    // set, which this repo doesn't do globally -- a manual ternary avoids
    // that landmine rather than relying on include-order luck.
    auto grownOpt = platform::CheckedAdd(capacity_, options_.growthIncrementBytes);
    uint64_t newCapacity = 0;
    if (grownOpt) {
        newCapacity = (*grownOpt < options_.maxCapacityBytes) ? *grownOpt : options_.maxCapacityBytes;
    } else {
        newCapacity = options_.maxCapacityBytes;
    }
    if (newCapacity <= capacity_) {
        return false;
    }

    ComPtr<ID3D12Resource> newResource;
    std::byte* newMapped = nullptr;
    if (!CreateUploadResource(newCapacity, newResource, newMapped)) {
        return false;
    }

    // The ring is pure transient staging -- growth has no content to
    // migrate, only a lifetime to respect: any CopyBufferRegion already
    // recorded against the old resource must finish before it's released,
    // so the whole old resource retires as one unit behind its own last
    // submitted fence rather than being tracked allocation-by-allocation
    // any further.
    if (ringResource_) {
        retiredResources_.push_back(RetiredResource{ ringResource_, lastSubmittedFenceValue_ });
    }
    pendingRingAllocations_.clear();

    ringResource_ = newResource;
    mapped_ = newMapped;
    capacity_ = newCapacity;
    writeOffset_ = 0;
    usedBytes_ = 0;
    return true;
}

bool D3D12UploadRing::TryAllocateInternal(uint64_t size, uint64_t alignment, uint64_t& outOffset,
                                           uint64_t& outOccupiedBytes)
{
    if (alignment == 0) {
        alignment = 1;
    }
    if (size > capacity_) {
        return false; // cannot ever fit at the current capacity, regardless of wrap -- caller must grow
    }

    // Align the cursor up first, then decide whether what remains of the
    // tail can still hold the allocation. With alignment == 1 this reduces
    // exactly to the original tail-room test, so buffer uploads are
    // unaffected.
    auto alignedOpt = platform::CheckedAdd(writeOffset_, alignment - 1);
    if (!alignedOpt) {
        return false;
    }
    uint64_t alignedOffset = (*alignedOpt / alignment) * alignment;

    uint64_t start = 0;
    uint64_t padding = 0;
    if (alignedOffset <= capacity_ && size <= capacity_ - alignedOffset) {
        start = alignedOffset;
        padding = alignedOffset - writeOffset_;
    } else {
        // Not enough contiguous tail room: wrap, wasting the tail. Offset 0
        // satisfies any alignment used here -- a committed buffer resource's
        // base is at least D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT (64
        // KiB), far above the 512-byte texture placement requirement.
        start = 0;
        padding = capacity_ - writeOffset_;
    }

    auto requiredOpt = platform::CheckedAdd(size, padding);
    if (!requiredOpt || *requiredOpt > (capacity_ - usedBytes_)) {
        return false; // not enough free space right now -- caller reclaims/grows/backpressures
    }

    usedBytes_ += padding; // wrap/align only into proven-free space, past the wasted bytes
    outOffset = start;
    outOccupiedBytes = size + padding;
    writeOffset_ = (start + size) % capacity_;
    usedBytes_ += size;
    return true;
}

bool D3D12UploadRing::EnsureSpace(uint64_t size, uint64_t alignment, uint64_t& outOffset,
                                   uint64_t& outOccupiedBytes)
{
    if (TryAllocateInternal(size, alignment, outOffset, outOccupiedBytes)) {
        return true;
    }

    // Reclaim whatever's already fence-complete -- free real work, not a
    // stall.
    ReclaimCompleted();
    if (TryAllocateInternal(size, alignment, outOffset, outOccupiedBytes)) {
        return true;
    }

    // Still short: grow rather than block, up to the configured cap --
    // "allowed to grow in 64 MiB increments up to the lesser of 512 MiB
    // and its CPU/GPU memory budget" (the live budget query is a later
    // slice; only the configured cap applies here).
    while (capacity_ < options_.maxCapacityBytes) {
        if (!Grow()) {
            break;
        }
        if (TryAllocateInternal(size, alignment, outOffset, outOccupiedBytes)) {
            return true;
        }
    }

    // At capacity and still short: submit whatever's already recorded so
    // its fence can actually retire, then wait -- bounded -- for the
    // oldest outstanding allocation. This is the "only the upload
    // coordinator may wait on the copy-fence event" backpressure path.
    if (commandListOpen_ && batchBytes_ > 0) {
        FlushBatch();
    }
    while (!pendingRingAllocations_.empty()) {
        uint64_t waitFor = pendingRingAllocations_.front().fenceValue;
        if (copyQueue_.WaitForValue(waitFor, /*timeoutMs=*/10000) != D3D12CommandQueue::WaitResult::Signaled) {
            return false;
        }
        ReclaimCompleted();
        if (TryAllocateInternal(size, alignment, outOffset, outOccupiedBytes)) {
            return true;
        }
    }
    return false; // nothing left to reclaim and still no room
}

D3D12UploadRing::UploadResult D3D12UploadRing::Upload(const UploadRequest& request)
{
    if (request.sourceBytes.empty() || request.destination == nullptr) {
        return UploadResult::Failed;
    }
    uint64_t size = static_cast<uint64_t>(request.sourceBytes.size());
    if (size > options_.maxCapacityBytes) {
        return UploadResult::Failed; // caller must split into bounded chunks first
    }

    uint64_t offset = 0;
    uint64_t occupiedBytes = 0;
    if (!EnsureSpace(size, /*alignment=*/1, offset, occupiedBytes)) {
        return UploadResult::Backpressured;
    }

    if (!commandListOpen_ && !BeginBatch()) {
        return UploadResult::Failed;
    }

    std::memcpy(mapped_ + offset, request.sourceBytes.data(), static_cast<size_t>(size));

    commandList_->CopyBufferRegion(request.destination, request.destinationOffset, ringResource_.Get(), offset,
                                    size);

    RecordAllocationAndPublication(size, occupiedBytes, request.destination, request.generation,
                                    request.clusterId, request.lodLevel, /*approximateBytes=*/size);
    return UploadResult::Uploaded;
}

D3D12UploadRing::UploadResult D3D12UploadRing::UploadTexture(const UploadTextureRequest& request)
{
    if (request.sourceBytes.empty() || request.destination == nullptr || request.width == 0
        || request.height == 0 || request.format == DXGI_FORMAT_UNKNOWN) {
        return UploadResult::Failed;
    }

    // Ask the device for the real staging layout rather than deriving a
    // pitch by hand -- "never guess" applies to row pitch exactly as it
    // does to the wire format's own byte math.
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = request.width;
    desc.Height = request.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = request.format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
    if (numRows == 0 || rowSizeInBytes == 0 || totalBytes == 0) {
        return UploadResult::Failed;
    }
    if (totalBytes > options_.maxCapacityBytes) {
        return UploadResult::Failed; // caller must split; a single mip cannot exceed the ring
    }

    // The source is tightly packed, so it only has to carry numRows *
    // rowSizeInBytes -- markedly less than the padded staging total.
    auto requiredSourceOpt
        = platform::CheckedMultiply(static_cast<uint64_t>(numRows), static_cast<uint64_t>(rowSizeInBytes));
    if (!requiredSourceOpt || request.sourceBytes.size() < *requiredSourceOpt) {
        return UploadResult::Failed;
    }

    uint64_t offset = 0;
    uint64_t occupiedBytes = 0;
    if (!EnsureSpace(totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, offset, occupiedBytes)) {
        return UploadResult::Backpressured;
    }

    if (!commandListOpen_ && !BeginBatch()) {
        return UploadResult::Failed;
    }

    const auto* src = reinterpret_cast<const std::byte*>(request.sourceBytes.data());
    std::byte* dst = mapped_ + offset;
    for (UINT row = 0; row < numRows; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                     src + static_cast<size_t>(row) * rowSizeInBytes, static_cast<size_t>(rowSizeInBytes));
    }

    // GetCopyableFootprints was called with a zero base offset, so its
    // Offset is 0; rebase it onto where the ring actually placed the rows.
    footprint.Offset = offset;

    D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
    destinationLocation.pResource = request.destination;
    destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destinationLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource = ringResource_.Get();
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sourceLocation.PlacedFootprint = footprint;

    commandList_->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);

    RecordAllocationAndPublication(totalBytes, occupiedBytes, request.destination, request.generation,
                                    request.clusterId, request.lodLevel, /*approximateBytes=*/totalBytes);
    return UploadResult::Uploaded;
}

void D3D12UploadRing::RecordAllocationAndPublication(uint64_t size, uint64_t occupiedBytes,
                                                      ID3D12Resource* destination,
                                                      const platform::GenerationToken& generation,
                                                      uint32_t clusterId, uint32_t lodLevel,
                                                      uint64_t approximateBytes)
{
    batchRingAllocations_.push_back(RingAllocation{ occupiedBytes, /*fenceValue=*/0 });

    ReadyResourceInfo info{};
    info.resource = destination;
    info.generationValue = generation.Value();
    info.clusterId = clusterId;
    info.lodLevel = lodLevel;
    info.approximateBytes = approximateBytes;
    batchPublications_.push_back(PendingPublication{ info, generation, /*fenceValue=*/0 });

    batchBytes_ += size;
    if (batchBytes_ >= options_.maxBatchBytes) {
        FlushBatch();
    }
}

void D3D12UploadRing::FlushBatch()
{
    if (!commandListOpen_) {
        return;
    }

    commandList_->Close();
    commandListOpen_ = false;

    if (batchRingAllocations_.empty()) {
        return; // nothing recorded this batch; just needed the list closed for the next Reset()
    }

    ID3D12CommandList* lists[] = { commandList_.Get() };
    copyQueue_.Queue()->ExecuteCommandLists(1, lists);
    uint64_t fenceValue = copyQueue_.SignalNext();

    for (auto& alloc : batchRingAllocations_) {
        alloc.fenceValue = fenceValue;
        pendingRingAllocations_.push_back(alloc);
    }
    batchRingAllocations_.clear();

    for (auto& pub : batchPublications_) {
        pub.fenceValue = fenceValue;
        pendingPublications_.push_back(pub);
    }
    batchPublications_.clear();

    lastSubmittedFenceValue_ = fenceValue;
    batchBytes_ = 0;
}

void D3D12UploadRing::ReclaimCompleted()
{
    uint64_t completed = copyQueue_.CompletedValue();

    while (!pendingRingAllocations_.empty() && pendingRingAllocations_.front().fenceValue <= completed) {
        usedBytes_ -= pendingRingAllocations_.front().occupiedBytes;
        pendingRingAllocations_.pop_front();
    }

    for (auto it = retiredResources_.begin(); it != retiredResources_.end();) {
        if (it->lastFenceValue <= completed) {
            it->resource->Unmap(0, nullptr);
            it = retiredResources_.erase(it);
        } else {
            ++it;
        }
    }
}

SceneSnapshotPtr D3D12UploadRing::DrainCompletedPublications(const platform::GenerationSource& currentGeneration)
{
    uint64_t completed = copyQueue_.CompletedValue();

    std::vector<ReadyResourceInfo> newlyReady;
    while (!pendingPublications_.empty() && pendingPublications_.front().fenceValue <= completed) {
        PendingPublication pub = std::move(pendingPublications_.front());
        pendingPublications_.pop_front();
        if (pub.generation.IsCurrent(currentGeneration)) {
            newlyReady.push_back(pub.info);
        }
        // else: stale generation -- silently dropped, never promoted.
    }
    return publisher_.Publish(std::move(newlyReady));
}

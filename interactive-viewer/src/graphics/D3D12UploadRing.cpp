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

bool D3D12UploadRing::TryAllocateInternal(uint64_t size, uint64_t& outOffset, uint64_t& outOccupiedBytes)
{
    if (size > capacity_) {
        return false; // cannot ever fit at the current capacity, regardless of wrap -- caller must grow
    }

    uint64_t tailRoom = capacity_ - writeOffset_;
    uint64_t padding = (size <= tailRoom) ? 0 : tailRoom;

    auto requiredOpt = platform::CheckedAdd(size, padding);
    if (!requiredOpt || *requiredOpt > (capacity_ - usedBytes_)) {
        return false; // not enough free space right now -- caller reclaims/grows/backpressures
    }

    if (padding > 0) {
        usedBytes_ += padding; // wrap only into proven-free space, past the wasted tail bytes
        writeOffset_ = 0;
    }
    outOffset = writeOffset_;
    outOccupiedBytes = size + padding;
    writeOffset_ = (writeOffset_ + size) % capacity_;
    usedBytes_ += size;
    return true;
}

bool D3D12UploadRing::EnsureSpace(uint64_t size, uint64_t& outOffset, uint64_t& outOccupiedBytes)
{
    if (TryAllocateInternal(size, outOffset, outOccupiedBytes)) {
        return true;
    }

    // Reclaim whatever's already fence-complete -- free real work, not a
    // stall.
    ReclaimCompleted();
    if (TryAllocateInternal(size, outOffset, outOccupiedBytes)) {
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
        if (TryAllocateInternal(size, outOffset, outOccupiedBytes)) {
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
        if (TryAllocateInternal(size, outOffset, outOccupiedBytes)) {
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
    if (!EnsureSpace(size, offset, occupiedBytes)) {
        return UploadResult::Backpressured;
    }

    if (!commandListOpen_ && !BeginBatch()) {
        return UploadResult::Failed;
    }

    std::memcpy(mapped_ + offset, request.sourceBytes.data(), static_cast<size_t>(size));

    commandList_->CopyBufferRegion(request.destination, request.destinationOffset, ringResource_.Get(), offset,
                                    size);

    batchRingAllocations_.push_back(RingAllocation{ occupiedBytes, /*fenceValue=*/0 });
    batchPublications_.push_back(PendingPublication{
        ReadyResourceInfo{ request.destination, request.generation.Value(), request.clusterId, request.lodLevel },
        request.generation, /*fenceValue=*/0 });

    batchBytes_ += size;
    if (batchBytes_ >= options_.maxBatchBytes) {
        FlushBatch();
    }

    return UploadResult::Uploaded;
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

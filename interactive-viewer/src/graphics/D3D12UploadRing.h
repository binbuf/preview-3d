#pragma once

// The upload lane: a persistently-mapped UPLOAD-heap ring buffer plus the
// copy queue/fence that retires it, and the fence-complete publication
// path that promotes a copied resource from Uploading to Ready. Implements
// .docs/design/04-rendering-and-streaming.md's "Upload ring" and
// "Fence-complete publication" sections. Owns its own copy-typed
// D3D12CommandQueue -- composition, not new fence/queue plumbing, per
// that class's own header comment already anticipating this exact
// caller ("when full, only the upload coordinator may wait on the
// copy-fence event").
//
// Deliberate simplification for this slice: a single command
// allocator/list is reused across batches, waiting (bounded) for the
// previous batch's fence before each Reset() -- the same fence-wait-
// before-reset discipline SwapChainTests.cpp's FrameRecorder already
// established. A rotating multi-allocator scheme would remove that stall
// and is a natural follow-up if measured batch cadence throughput ever
// requires it; it is not needed to prove the ring/publication semantics
// this slice targets.
//
// Also out of scope here (later Gate 2 workstream B slices): the DXGI
// budget monitor (growth is capped by CreateOptions::maxCapacityBytes
// alone, not a live budget query) and any fault injection.

#include "D3D12CommandQueue.h"
#include "SceneSnapshot.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <platform/Generation.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

class D3D12Device;

class D3D12UploadRing
{
public:
    struct CreateOptions
    {
        uint64_t initialCapacityBytes = 256ull * 1024 * 1024;
        uint64_t growthIncrementBytes = 64ull * 1024 * 1024;
        uint64_t maxCapacityBytes = 512ull * 1024 * 1024;
        // Batch flush threshold in bytes. The design doc also specifies a
        // ~2ms time threshold ("whichever comes first") -- deferred here
        // since it needs a real caller-driven frame cadence to mean
        // anything; this slice's callers flush explicitly.
        uint64_t maxBatchBytes = 64ull * 1024 * 1024;
    };

    // A single bounded chunk to upload. The caller is responsible for
    // splitting a larger source into chunks no larger than
    // CreateOptions::maxCapacityBytes before calling Upload() --
    // "split input into bounded chunks rather than waiting for one huge
    // contiguous segment."
    struct UploadRequest
    {
        std::span<const std::byte> sourceBytes;
        ID3D12Resource* destination = nullptr; // non-owning; a DEFAULT-heap buffer the caller created in COMMON
        uint64_t destinationOffset = 0;
        platform::GenerationToken generation;
        uint32_t clusterId = 0;
        uint32_t lodLevel = 0;
    };

    enum class UploadResult { Uploaded, Backpressured, Failed };

    // Neither copyable nor movable -- it owns a SceneSnapshotPublisher,
    // which owns a std::mutex. Nothing in this slice needs to relocate a
    // live ring; construct it in place (a local, a member, or behind a
    // unique_ptr if a caller ever needs indirection).
    D3D12UploadRing() = default;
    D3D12UploadRing(const D3D12UploadRing&) = delete;
    D3D12UploadRing& operator=(const D3D12UploadRing&) = delete;
    D3D12UploadRing(D3D12UploadRing&&) = delete;
    D3D12UploadRing& operator=(D3D12UploadRing&&) = delete;

    bool Initialize(D3D12Device& device, const CreateOptions& options = {});

    // Copies sourceBytes into the ring and records a CopyBufferRegion
    // into `destination`. Returns Backpressured only if the ring is at
    // its capacity cap and even a bounded wait for the oldest
    // outstanding allocation's fence didn't free enough room in time --
    // ordinary streaming should never see this if callers periodically
    // call ReclaimCompleted()/DrainCompletedPublications().
    UploadResult Upload(const UploadRequest& request);

    // Closes and submits whatever's currently recorded, even if under
    // the batch threshold. The ring never auto-flushes off a timer by
    // itself -- there's no background thread here; a caller (eventually
    // the real upload coordinator's frame-driven loop) controls cadence.
    void FlushBatch();

    // Reclaims ring space, and drops fully-retired grown-past resources,
    // whose fence has completed. Safe and cheap to call every frame.
    void ReclaimCompleted();

    // Pops every publication whose copy fence has completed. A
    // still-current one is folded into a new immutable SceneSnapshot; a
    // stale-generation one is silently dropped rather than promoted --
    // "Graphics validates that its generation is still current." Always
    // returns the current snapshot, even if nothing new became ready
    // this call, so a caller can unconditionally treat the return value
    // as "what to draw now."
    SceneSnapshotPtr DrainCompletedPublications(const platform::GenerationSource& currentGeneration);

    // The latest published snapshot without draining -- "the direct
    // queue... keeps drawing the previous proxy/LOD" until an explicit
    // Drain call actually promotes something new.
    SceneSnapshotPtr CurrentSnapshot() const { return publisher_.Current(); }

    D3D12CommandQueue& CopyQueue() noexcept { return copyQueue_; }
    uint64_t CapacityBytes() const noexcept { return capacity_; }
    uint64_t UsedBytes() const noexcept { return usedBytes_; }
    size_t PendingRingAllocationCount() const noexcept { return pendingRingAllocations_.size(); }
    size_t PendingPublicationCount() const noexcept { return pendingPublications_.size(); }

private:
    struct RingAllocation
    {
        uint64_t occupiedBytes = 0; // size + any wrap padding this allocation forced
        uint64_t fenceValue = 0;    // assigned once the batch containing it is submitted
    };
    struct PendingPublication
    {
        ReadyResourceInfo info;
        platform::GenerationToken generation;
        uint64_t fenceValue = 0;
    };
    struct RetiredResource
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint64_t lastFenceValue = 0;
    };

    bool CreateUploadResource(uint64_t sizeBytes, Microsoft::WRL::ComPtr<ID3D12Resource>& outResource,
                               std::byte*& outMapped);
    bool BeginBatch();
    bool Grow();
    bool TryAllocateInternal(uint64_t size, uint64_t& outOffset, uint64_t& outOccupiedBytes);
    bool EnsureSpace(uint64_t size, uint64_t& outOffset, uint64_t& outOccupiedBytes);

    ID3D12Device* device_ = nullptr; // non-owning
    D3D12CommandQueue copyQueue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    bool commandListOpen_ = false;
    uint64_t lastSubmittedFenceValue_ = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> ringResource_;
    std::byte* mapped_ = nullptr;
    uint64_t capacity_ = 0;
    uint64_t writeOffset_ = 0;
    uint64_t usedBytes_ = 0;
    CreateOptions options_;

    std::vector<RingAllocation> batchRingAllocations_;
    std::vector<PendingPublication> batchPublications_;
    uint64_t batchBytes_ = 0;

    std::deque<RingAllocation> pendingRingAllocations_;
    std::deque<PendingPublication> pendingPublications_;
    std::vector<RetiredResource> retiredResources_;

    SceneSnapshotPublisher publisher_;
};

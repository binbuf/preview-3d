#pragma once

// The first concrete definition of the immutable SceneSnapshot the design
// doc describes (.docs/design/04-rendering-and-streaming.md,
// "Fence-complete publication": "A new immutable SceneSnapshot references
// the ready mesh/texture descriptor"; ADR-004 in
// .docs/design/11-decisions-and-risks.md: "This is an epoch/handle swap,
// not an unowned atomic raw pointer. Old snapshots remain alive until the
// last direct fence that used them completes."). Scoped minimally for
// Gate 2 workstream B slice 3 -- enough to prove the upload ring's
// publication path end-to-end, not the full renderer's eventual data
// model (materials/textures/culling are out of scope here).

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

struct ID3D12Resource;

struct ReadyResourceInfo {
    ID3D12Resource* resource = nullptr; // non-owning
    uint64_t generationValue = 0;
    uint32_t clusterId = 0;
    uint32_t lodLevel = 0;
    // Both added for Gate 2's DXGI budget monitor / eviction planner
    // (interactive-viewer/src/graphics/DxgiBudgetMonitor.h) -- appended at
    // the end, both defaulted, so every existing positional-aggregate-init
    // call site (D3D12UploadRing.cpp, UploadRingTests.cpp) keeps compiling
    // unchanged. approximateBytes is the resource's approximate GPU-visible
    // size, used by the eviction planner's budget-fit math; lastVisibleFrame
    // is a caller-supplied recency signal (no real frame loop exists yet to
    // populate it from, so it defaults to 0 -- see DxgiBudgetMonitor.h's own
    // scoping note on why this isn't camera/frustum-aware).
    uint64_t approximateBytes = 0;
    uint32_t lastVisibleFrame = 0;
};

class SceneSnapshot {
public:
    explicit SceneSnapshot(std::vector<ReadyResourceInfo> ready) : ready_(std::move(ready)) {}

    const std::vector<ReadyResourceInfo>& ReadyResources() const noexcept { return ready_; }

private:
    const std::vector<ReadyResourceInfo> ready_;
};

using SceneSnapshotPtr = std::shared_ptr<const SceneSnapshot>;

// Owns the "latest published snapshot" epoch/handle-swap slot. Publish()
// merges newly fence-complete resources into the previous snapshot's set
// (a resource re-published under the same cluster/LOD replaces the
// earlier entry) and swaps in a new immutable SceneSnapshot -- the
// previous one stays alive through any shared_ptr already held
// (e.g. by an in-flight render-thread frame). Thread-safe: the render
// thread calls Current(), the upload coordinator calls Publish() --
// different lanes per the design doc's ownership model.
class SceneSnapshotPublisher {
public:
    SceneSnapshotPublisher()
        : current_(std::make_shared<const SceneSnapshot>(std::vector<ReadyResourceInfo>{}))
    {
    }

    SceneSnapshotPtr Current() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }

    // A no-op call (nothing newly ready) returns the current snapshot
    // unchanged rather than allocating a needless new one -- this is what
    // keeps a caller's "poll and drain" loop from ever observing a
    // premature or half-updated snapshot.
    SceneSnapshotPtr Publish(std::vector<ReadyResourceInfo> newlyReady)
    {
        if (newlyReady.empty()) {
            return Current();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ReadyResourceInfo> merged = current_->ReadyResources();
        for (const ReadyResourceInfo& info : newlyReady) {
            auto it = std::find_if(merged.begin(), merged.end(), [&](const ReadyResourceInfo& existing) {
                return existing.clusterId == info.clusterId && existing.lodLevel == info.lodLevel;
            });
            if (it != merged.end()) {
                *it = info;
            } else {
                merged.push_back(info);
            }
        }
        current_ = std::make_shared<const SceneSnapshot>(std::move(merged));
        return current_;
    }

private:
    mutable std::mutex mutex_;
    SceneSnapshotPtr current_;
};

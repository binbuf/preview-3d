#pragma once

// One ID3D12CommandQueue and its owned, monotonically increasing
// ID3D12Fence. Implements .docs/design/04-rendering-and-streaming.md's
// fence semantics: the ordinary path is polling (GetCompletedValue), with
// a bounded event-wait mode also supported since the upload coordinator
// will need it later ("when full, only the upload coordinator may wait on
// the copy-fence event"). This class doesn't decide which mode a caller
// uses -- both are real, spec'd paths. No swap chain, no command lists, no
// rendering -- see D3D12Device.h for the same scope note.

#include <d3d12.h>
#include <wrl/client.h>

#include <platform/Win32Handle.h>

#include <cstdint>

class D3D12CommandQueue
{
public:
    enum class WaitResult { Signaled, TimedOut, Failed };

    D3D12CommandQueue() = default;
    D3D12CommandQueue(const D3D12CommandQueue&) = delete;
    D3D12CommandQueue& operator=(const D3D12CommandQueue&) = delete;
    D3D12CommandQueue(D3D12CommandQueue&&) = default;
    D3D12CommandQueue& operator=(D3D12CommandQueue&&) = default;

    bool Initialize(ID3D12Device& device, D3D12_COMMAND_LIST_TYPE type, const wchar_t* debugName);

    // Advances the internal counter and calls ID3D12CommandQueue::Signal.
    // Returns the newly-submitted target value, or 0 on failure (check
    // LastError()). Values start at 1 -- 0 is reserved as "never
    // signaled," matching CreateFence's initial value of 0.
    uint64_t SignalNext();

    // ID3D12Fence::GetCompletedValue() -- the ordinary, non-blocking poll
    // path.
    uint64_t CompletedValue() const noexcept;

    // Bounded event-based wait. Never INFINITE -- matches this repo's
    // established finite-timeout convention.
    WaitResult WaitForValue(uint64_t value, DWORD timeoutMs) const;

    ID3D12CommandQueue* Queue() const noexcept { return queue_.Get(); }
    ID3D12Fence* Fence() const noexcept { return fence_.Get(); }
    HRESULT LastError() const noexcept { return lastError_; }

private:
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    platform::Win32Handle fenceEvent_;
    uint64_t nextValue_ = 1;
    HRESULT lastError_ = S_OK;
};

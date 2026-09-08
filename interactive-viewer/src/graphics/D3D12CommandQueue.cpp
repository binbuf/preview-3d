#include "D3D12CommandQueue.h"

using Microsoft::WRL::ComPtr;

bool D3D12CommandQueue::Initialize(ID3D12Device& device, D3D12_COMMAND_LIST_TYPE type,
                                    const wchar_t* debugName)
{
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    lastError_ = device.CreateCommandQueue(&desc, IID_PPV_ARGS(&queue_));
    if (FAILED(lastError_)) {
        return false;
    }
    if (debugName != nullptr) {
        queue_->SetName(debugName);
    }

    lastError_ = device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(lastError_)) {
        return false;
    }

    fenceEvent_.reset(CreateEventW(nullptr, /*bManualReset=*/FALSE, /*bInitialState=*/FALSE, nullptr));
    if (!fenceEvent_) {
        lastError_ = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }

    nextValue_ = 1;
    return true;
}

uint64_t D3D12CommandQueue::SignalNext()
{
    uint64_t target = nextValue_;
    lastError_ = queue_->Signal(fence_.Get(), target);
    if (FAILED(lastError_)) {
        return 0;
    }
    ++nextValue_;
    return target;
}

uint64_t D3D12CommandQueue::CompletedValue() const noexcept
{
    return fence_->GetCompletedValue();
}

D3D12CommandQueue::WaitResult D3D12CommandQueue::WaitForValue(uint64_t value, DWORD timeoutMs) const
{
    if (fence_->GetCompletedValue() >= value) {
        return WaitResult::Signaled;
    }

    HRESULT hr = fence_->SetEventOnCompletion(value, fenceEvent_.get());
    if (FAILED(hr)) {
        return WaitResult::Failed;
    }

    DWORD waitResult = WaitForSingleObject(fenceEvent_.get(), timeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        return WaitResult::Signaled;
    }
    if (waitResult == WAIT_TIMEOUT) {
        return WaitResult::TimedOut;
    }
    return WaitResult::Failed;
}

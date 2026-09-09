#include "D3D12ViewerPath.h"

#include <windows.h>

#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
constexpr DWORD kIdleWaitTimeoutMs = 5000;
}

bool D3D12ViewerPath::Initialize(HWND window, std::wstring& error)
{
    auto deviceResult = device.Initialize();
    if (!deviceResult.success) {
        error = L"The D3D12 device could not be created (HRESULT " + std::to_wstring(deviceResult.hr) + L").";
        return false;
    }

    if (!directQueue.Initialize(*device.Device(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"Direct")) {
        error = L"The D3D12 direct command queue could not be created.";
        return false;
    }

    RECT clientRect{};
    GetClientRect(window, &clientRect);
    D3D12SwapChain::CreateOptions swapChainOptions;
    swapChainOptions.width = static_cast<UINT>(clientRect.right - clientRect.left);
    swapChainOptions.height = static_cast<UINT>(clientRect.bottom - clientRect.top);
    auto swapChainResult = swapChain.Initialize(device, directQueue, window, swapChainOptions);
    if (!swapChainResult.success) {
        error = L"The D3D12 swap chain could not be created (HRESULT " + std::to_wstring(swapChainResult.hr) + L").";
        return false;
    }

    if (FAILED(device.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&commandAllocator)))) {
        error = L"The D3D12 command allocator could not be created.";
        return false;
    }
    if (FAILED(device.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(),
                                                    nullptr, IID_PPV_ARGS(&commandList)))) {
        error = L"The D3D12 command list could not be created.";
        return false;
    }
    // CreateCommandList returns it open; close before the first Reset(),
    // matching the FrameRecorder convention SwapChainTests.cpp established.
    commandList->Close();

    return true;
}

void D3D12ViewerPath::WaitForIdle()
{
    if (lastFrameFence != 0) {
        directQueue.WaitForValue(lastFrameFence, kIdleWaitTimeoutMs);
    }
}

bool D3D12ViewerPath::Resize(int width, int height, std::wstring& error)
{
    WaitForIdle();
    return swapChain.Resize(static_cast<UINT>(width), static_cast<UINT>(height), error);
}

void D3D12ViewerPath::RenderClearFrame()
{
    WaitForIdle();

    if (FAILED(commandAllocator->Reset())) {
        return;
    }
    if (FAILED(commandList->Reset(commandAllocator.Get(), nullptr))) {
        return;
    }

    UINT index = swapChain.CurrentBackBufferIndex();
    ID3D12Resource* backBuffer = swapChain.BackBuffer(index);

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource = backBuffer;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &toRenderTarget);

    // .docs/design/10-delivery-plan.md's Gate 1 "immediate #1C1C1E background".
    const float clearColor[4] = { 0x1C / 255.0f, 0x1C / 255.0f, 0x1E / 255.0f, 1.0f };
    commandList->ClearRenderTargetView(swapChain.BackBufferRtv(index), clearColor, 0, nullptr);

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    std::swap(toPresent.Transition.StateBefore, toPresent.Transition.StateAfter);
    commandList->ResourceBarrier(1, &toPresent);

    if (FAILED(commandList->Close())) {
        return;
    }
    ID3D12CommandList* lists[] = { commandList.Get() };
    directQueue.Queue()->ExecuteCommandLists(1, lists);

    swapChain.Present();
    lastFrameFence = directQueue.SignalNext();
}

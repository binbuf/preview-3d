#pragma once

// First slice of wiring D3D12 into the real Preview3D.exe: a bare clear-and-
// present loop over the actual chrome-managed HWND, gated entirely behind the
// opt-in --d3d12 command-line flag (see Preview3D.cpp's wWinMain argument
// scan and the WM_CREATE/WM_SIZE/WM_PAINT/WM_DESTROY branches). The existing
// D3D11 Renderer stays the untouched default -- these two paths are mutually
// exclusive per window (only one swap chain can own presentation for a given
// HWND). No camera, no model, no D2D/D3D11On12 chrome overlay, no device-loss
// recovery yet -- all deliberately deferred to later slices of this same
// effort; this one only proves the D3D12Device/D3D12CommandQueue/
// D3D12SwapChain primitives (already proven in isolation via Tests.Unit)
// actually drive a real window's frame loop.

#include "D3D12CommandQueue.h"
#include "D3D12Device.h"
#include "D3D12SwapChain.h"

#include <wrl/client.h>

#include <cstdint>
#include <string>

struct D3D12ViewerPath
{
    D3D12Device device;
    D3D12CommandQueue directQueue;
    D3D12SwapChain swapChain;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    uint64_t lastFrameFence = 0;

    // Creates the device, direct queue, swap chain (sized to `window`'s
    // current client rect), and the single command allocator/list this bare
    // path reuses every frame.
    bool Initialize(HWND window, std::wstring& error);

    // Precondition enforced internally via WaitForIdle() first -- matches
    // D3D12SwapChain::Resize's own documented precondition ("every command
    // list referencing the current back buffers must have already
    // retired"); this struct is the "whichever caller drives a real frame
    // loop" D3D12SwapChain's header comment defers that to.
    bool Resize(int width, int height, std::wstring& error);

    // Clears to the design doc's Gate 1 "immediate #1C1C1E" idle background
    // and presents. Waits for the previous frame's fence before reusing the
    // single command allocator, same fence-wait-before-reset discipline
    // SwapChainTests.cpp's FrameRecorder already established for this exact
    // recorder shape.
    void RenderClearFrame();

    // Bounded wait on lastFrameFence. Called before Resize and on
    // WM_DESTROY -- GPU work must be known-idle before this struct's
    // destructor releases the D3D12 objects; RAII alone doesn't order that.
    void WaitForIdle();
};

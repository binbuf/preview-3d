// Gate 2 workstream B, slice 2: swap chain + a real (hidden, test-owned)
// window + one full clear-and-present cycle, plus resize. Still no
// shaders, no geometry, no Direct2D overlay -- that question (ADR-010) is
// explicitly out of scope here. See
// .docs/design/04-rendering-and-streaming.md ("Device and presentation").

#include "D3D12CommandQueue.h"
#include "D3D12Device.h"
#include "D3D12SwapChain.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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

// A minimal, real, never-shown HWND to host a swap chain for the duration
// of one test case. Not HWND_MESSAGE -- message-only windows are a
// narrower, murkier case for DXGI flip-model compatibility; a plain hidden
// top-level window already satisfies "real HWND, no visible surface"
// without introducing that extra unknown.
struct TestWindow {
    ATOM classAtom = 0;
    HWND hwnd = nullptr;

    TestWindow()
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"D3D12SwapChainTestWindow";
        classAtom = RegisterClassExW(&wc);
        REQUIRE(classAtom != 0);

        hwnd = CreateWindowExW(0, wc.lpszClassName, L"D3D12SwapChainTest", WS_OVERLAPPEDWINDOW, 0,
                                0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
        REQUIRE(hwnd != nullptr);
        // Deliberately never ShowWindow()'d.
    }

    ~TestWindow()
    {
        if (hwnd != nullptr) {
            DestroyWindow(hwnd);
        }
        if (classAtom != 0) {
            UnregisterClassW(MAKEINTATOM(classAtom), GetModuleHandleW(nullptr));
        }
    }

    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;
};

// Command allocator/list ownership deliberately stays out of D3D12SwapChain
// (see its header comment) -- this is the render-thread-owned piece per
// the design's ownership model, kept local to the test here since no real
// render thread exists yet.
struct FrameRecorder {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;

    explicit FrameRecorder(ID3D12Device& device)
    {
        REQUIRE(SUCCEEDED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&allocator))));
        REQUIRE(SUCCEEDED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     allocator.Get(), nullptr,
                                                     IID_PPV_ARGS(&commandList))));
        // CreateCommandList returns it open; close before the first Reset().
        commandList->Close();
    }

    // Records a clear-and-present-ready frame against the swap chain's
    // current back buffer and executes it on directQueue. Does not itself
    // call Present() or signal the fence -- the caller does that, since
    // this class only records/executes.
    void RecordAndExecute(D3D12SwapChain& swapChain, D3D12CommandQueue& directQueue)
    {
        REQUIRE(SUCCEEDED(allocator->Reset()));
        REQUIRE(SUCCEEDED(commandList->Reset(allocator.Get(), nullptr)));

        UINT index = swapChain.CurrentBackBufferIndex();
        ID3D12Resource* backBuffer = swapChain.BackBuffer(index);

        D3D12_RESOURCE_BARRIER toRenderTarget{};
        toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRenderTarget.Transition.pResource = backBuffer;
        toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &toRenderTarget);

        const float clearColor[4] = { 0.1f, 0.2f, 0.4f, 1.0f };
        commandList->ClearRenderTargetView(swapChain.BackBufferRtv(index), clearColor, 0, nullptr);

        D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
        std::swap(toPresent.Transition.StateBefore, toPresent.Transition.StateAfter);
        commandList->ResourceBarrier(1, &toPresent);

        REQUIRE(SUCCEEDED(commandList->Close()));
        ID3D12CommandList* lists[] = { commandList.Get() };
        directQueue.Queue()->ExecuteCommandLists(1, lists);
    }
};

} // namespace

TEST_CASE("D3D12SwapChain creates against a real window with the spec'd three-buffer "
          "flip-discard format",
          "[graphics]")
{
    TestWindow window;
    D3D12CommandQueue directQueue;
    REQUIRE(directQueue.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    L"Direct"));

    D3D12SwapChain swapChain;
    D3D12SwapChain::CreateOptions options;
    options.width = 640;
    options.height = 480;
    auto result = swapChain.Initialize(SharedDevice(), directQueue, window.hwnd, options);

    REQUIRE(result.success);
    CHECK(result.hr == S_OK);
    CHECK(swapChain.Width() == 640);
    CHECK(swapChain.Height() == 480);
    for (UINT i = 0; i < D3D12SwapChain::kBufferCount; ++i) {
        CHECK(swapChain.BackBuffer(i) != nullptr);
    }
    CHECK(swapChain.FrameLatencyWaitableHandle() != nullptr);
}

TEST_CASE("One frame can be cleared and presented", "[graphics]")
{
    TestWindow window;
    D3D12CommandQueue directQueue;
    REQUIRE(directQueue.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    L"Direct"));

    D3D12SwapChain swapChain;
    D3D12SwapChain::CreateOptions options;
    options.width = 640;
    options.height = 480;
    REQUIRE(swapChain.Initialize(SharedDevice(), directQueue, window.hwnd, options).success);

    FrameRecorder recorder(*SharedDevice().Device());
    recorder.RecordAndExecute(swapChain, directQueue);

    HRESULT presentHr = swapChain.Present();
    uint64_t fenceValue = directQueue.SignalNext();

    CHECK(SUCCEEDED(presentHr));
    // DXGI_STATUS_OCCLUDED is a legitimate, non-error outcome for a window
    // with no visible presentation surface -- informational only, matching
    // the debug-layer-availability non-hard-assert precedent in
    // GraphicsDeviceTests.cpp.
    INFO("Present() HRESULT: " << presentHr << " (DXGI_STATUS_OCCLUDED = " << DXGI_STATUS_OCCLUDED
                                << ")");

    CHECK(directQueue.WaitForValue(fenceValue, 5000) == D3D12CommandQueue::WaitResult::Signaled);
}

TEST_CASE("Presenting five frames in a row does not deadlock or leak", "[graphics]")
{
    TestWindow window;
    D3D12CommandQueue directQueue;
    REQUIRE(directQueue.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    L"Direct"));

    D3D12SwapChain swapChain;
    D3D12SwapChain::CreateOptions options;
    options.width = 640;
    options.height = 480;
    REQUIRE(swapChain.Initialize(SharedDevice(), directQueue, window.hwnd, options).success);

    FrameRecorder recorder(*SharedDevice().Device());
    uint64_t previousFenceValue = 0;

    for (int frame = 0; frame < 5; ++frame) {
        if (previousFenceValue != 0) {
            REQUIRE(directQueue.WaitForValue(previousFenceValue, 5000)
                    == D3D12CommandQueue::WaitResult::Signaled);
        }

        recorder.RecordAndExecute(swapChain, directQueue);
        HRESULT presentHr = swapChain.Present();
        CHECK(SUCCEEDED(presentHr));

        previousFenceValue = directQueue.SignalNext();
    }

    CHECK(directQueue.WaitForValue(previousFenceValue, 5000)
          == D3D12CommandQueue::WaitResult::Signaled);
}

TEST_CASE("A per-frame allocator ring waits on its own slot, not the previous frame", "[graphics]")
{
    // The pacing shape D3D12ViewerPath::frames now uses. The case above
    // waits on the immediately previous frame with one allocator, so the CPU
    // can never be more than one frame ahead; here each slot waits only on
    // its own last submission, which is what lets the CPU run ahead at all.
    TestWindow window;
    D3D12CommandQueue directQueue;
    REQUIRE(directQueue.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    L"Direct"));

    D3D12SwapChain swapChain;
    D3D12SwapChain::CreateOptions options;
    options.width = 640;
    options.height = 480;
    REQUIRE(swapChain.Initialize(SharedDevice(), directQueue, window.hwnd, options).success);

    HANDLE waitable = swapChain.FrameLatencyWaitableHandle();
    REQUIRE(waitable != nullptr);

    std::vector<FrameRecorder> ring;
    ring.reserve(D3D12SwapChain::kBufferCount);
    for (UINT i = 0; i < D3D12SwapChain::kBufferCount; ++i) {
        ring.emplace_back(*SharedDevice().Device());
    }
    std::vector<uint64_t> slotFence(D3D12SwapChain::kBufferCount, 0);

    uint64_t lastSignaled = 0;
    uint64_t deepestLag = 0;
    int submittedAheadOfGpu = 0;

    constexpr int kFrames = 12; // comfortably more than kBufferCount
    for (int frame = 0; frame < kFrames; ++frame) {
        // The frame-latency object is what bounds how far ahead the CPU may
        // run. Nothing in the product waited on it before this chunk.
        CHECK(WaitForSingleObject(waitable, 5000) == WAIT_OBJECT_0);

        const UINT index = swapChain.CurrentBackBufferIndex();
        if (slotFence[index] != 0) {
            REQUIRE(directQueue.WaitForValue(slotFence[index], 5000)
                    == D3D12CommandQueue::WaitResult::Signaled);
            const uint64_t lag = lastSignaled - slotFence[index];
            if (lag > deepestLag) deepestLag = lag;
        }

        ring[index].RecordAndExecute(swapChain, directQueue);
        CHECK(SUCCEEDED(swapChain.Present()));

        lastSignaled = directQueue.SignalNext();
        REQUIRE(lastSignaled != 0);
        slotFence[index] = lastSignaled;

        if (directQueue.CompletedValue() < lastSignaled) ++submittedAheadOfGpu;
    }

    // Deterministic regardless of how fast this GPU happens to be: fence
    // values increment by one per frame, and a slot is revisited every
    // kBufferCount frames, so the value it waits on is exactly
    // kBufferCount - 1 submissions behind the newest. The single-allocator
    // loop above has a lag of exactly 1 by construction.
    CHECK(deepestLag == D3D12SwapChain::kBufferCount - 1);

    // How much the CPU actually got ahead is hardware-dependent -- a 640x480
    // clear can retire faster than the CPU reaches the next check -- so this
    // is reported, never asserted.
    INFO("frames submitted before the GPU had caught up: " << submittedAheadOfGpu << " of " << kFrames);

    CHECK(directQueue.WaitForValue(lastSignaled, 5000) == D3D12CommandQueue::WaitResult::Signaled);
}

TEST_CASE("Resize succeeds and rebuilds back buffers at the new dimensions", "[graphics]")
{
    TestWindow window;
    D3D12CommandQueue directQueue;
    REQUIRE(directQueue.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    L"Direct"));

    D3D12SwapChain swapChain;
    D3D12SwapChain::CreateOptions options;
    options.width = 640;
    options.height = 480;
    REQUIRE(swapChain.Initialize(SharedDevice(), directQueue, window.hwnd, options).success);

    FrameRecorder recorder(*SharedDevice().Device());

    // Present one frame and wait for it to retire -- Resize's precondition.
    recorder.RecordAndExecute(swapChain, directQueue);
    REQUIRE(SUCCEEDED(swapChain.Present()));
    uint64_t fenceValue = directQueue.SignalNext();
    REQUIRE(directQueue.WaitForValue(fenceValue, 5000) == D3D12CommandQueue::WaitResult::Signaled);

    std::wstring error;
    REQUIRE(swapChain.Resize(1024, 768, error));
    CHECK(swapChain.Width() == 1024);
    CHECK(swapChain.Height() == 768);

    // Prove the rebuilt RTVs are actually usable, not just that the resize
    // call itself returned true.
    recorder.RecordAndExecute(swapChain, directQueue);
    HRESULT presentHr = swapChain.Present();
    CHECK(SUCCEEDED(presentHr));
    directQueue.SignalNext();
}

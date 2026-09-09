// Required validation spike 1 (.docs/design/11-decisions-and-risks.md:243):
// "Before Gate 1 completion, measure D3D11On12 overlay ordering and cost at
// 144 Hz, including resize and GPU validation." It has now been run and
// passed, so ADR-010 is Accepted rather than provisional.
//
// These cases cover the correctness half -- does the bridge attach at all,
// does a frame draw through it, does it survive resize. The cost half is
// measured in the app, where a real scene and a real display are involved.
//
// The single most load-bearing assertion here is the first one: the design
// specifies a DXGI_FORMAT_R8G8B8A8_UNORM swap chain (04-...:17) while the
// existing D3D11 renderer uses BGRA, and the plan for this batch assumed
// without evidence that Direct2D would require a BGRA swap chain. If that
// assumption were true, the swap-chain format would have to change and the
// design doc with it.

#include "D3D11On12Overlay.h"
#include "D3D12CommandQueue.h"
#include "D3D12Device.h"
#include "D3D12SwapChain.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace {

// The error strings here are ASCII by construction, but the conversion has
// to be explicit -- an implicit wchar_t-to-char narrowing is a warning, and
// this repo builds product and test code with warnings as errors.
std::string Narrow(const std::wstring& text)
{
    std::string narrowed;
    narrowed.reserve(text.size());
    for (wchar_t c : text) {
        narrowed.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return narrowed;
}

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

struct OverlayTestWindow {
    ATOM classAtom = 0;
    HWND hwnd = nullptr;

    OverlayTestWindow()
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"D3D11On12OverlayTestWindow";
        classAtom = RegisterClassExW(&wc);
        REQUIRE(classAtom != 0);

        hwnd = CreateWindowExW(0, wc.lpszClassName, L"D3D11On12OverlayTest", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480,
                                nullptr, nullptr, wc.hInstance, nullptr);
        REQUIRE(hwnd != nullptr);
        // Deliberately never ShowWindow()'d, matching SwapChainTests.cpp.
    }

    ~OverlayTestWindow()
    {
        if (hwnd != nullptr) DestroyWindow(hwnd);
        if (classAtom != 0) UnregisterClassW(MAKEINTATOM(classAtom), GetModuleHandleW(nullptr));
    }

    OverlayTestWindow(const OverlayTestWindow&) = delete;
    OverlayTestWindow& operator=(const OverlayTestWindow&) = delete;
};

// Everything one overlay test needs, wired together in the order the product
// does it.
struct OverlayHarness {
    OverlayTestWindow window;
    D3D12CommandQueue directQueue;
    D3D12SwapChain swapChain;
    D3D11On12Overlay overlay;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sharedBrush;
    std::wstring error;

    OverlayHarness()
    {
        REQUIRE(directQueue.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"Direct"));

        D3D12SwapChain::CreateOptions options;
        options.width = 640;
        options.height = 480;
        REQUIRE(swapChain.Initialize(SharedDevice(), directQueue, window.hwnd, options).success);

        ID3D12Device& device = *SharedDevice().Device();
        REQUIRE(SUCCEEDED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
        REQUIRE(SUCCEEDED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                     IID_PPV_ARGS(&commandList))));
        commandList->Close();
    }

    // Records a scene pass that leaves the back buffer in RENDER_TARGET --
    // deliberately NOT transitioning to PRESENT, because the overlay's
    // ReleaseWrappedResources is what performs that transition (04-...:46).
    void RecordSceneLeavingRenderTarget(UINT index)
    {
        REQUIRE(SUCCEEDED(allocator->Reset()));
        REQUIRE(SUCCEEDED(commandList->Reset(allocator.Get(), nullptr)));

        D3D12_RESOURCE_BARRIER toRenderTarget{};
        toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRenderTarget.Transition.pResource = swapChain.BackBuffer(index);
        toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &toRenderTarget);

        const float clearColor[4] = { 0.1f, 0.2f, 0.4f, 1.0f };
        commandList->ClearRenderTargetView(swapChain.BackBufferRtv(index), clearColor, 0, nullptr);

        REQUIRE(SUCCEEDED(commandList->Close()));
        ID3D12CommandList* lists[] = { commandList.Get() };
        directQueue.Queue()->ExecuteCommandLists(1, lists);
    }

    // One full scene + overlay + present cycle, returning D2D's EndDraw
    // result so a caller can assert it.
    HRESULT DrawOneOverlayFrame()
    {
        const UINT index = swapChain.CurrentBackBufferIndex();
        RecordSceneLeavingRenderTarget(index);

        ID2D1DeviceContext* target = overlay.BeginDraw(index);
        REQUIRE(target != nullptr);

        // Created once and reused across every back buffer -- the whole point
        // of the shared device context. Under the D2D 1.0 shape this would
        // have to be one brush per buffer.
        if (!sharedBrush) {
            REQUIRE(SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &sharedBrush)));
        }
        target->FillRectangle(D2D1::RectF(8.0f, 8.0f, 240.0f, 48.0f), sharedBrush.Get());

        const HRESULT endHr = overlay.EndDraw(index);

        CHECK(SUCCEEDED(swapChain.Present()));
        const uint64_t fence = directQueue.SignalNext();
        REQUIRE(fence != 0);
        REQUIRE(directQueue.WaitForValue(fence, 5000) == D3D12CommandQueue::WaitResult::Signaled);
        return endHr;
    }
};

// Counts D3D12 debug-layer messages at ERROR or CORRUPTION severity. Returns
// -1 when the info queue is unavailable, which is the ordinary case in a
// Release build or on a machine without the Graphics Tools feature -- that
// is reported, not failed.
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

TEST_CASE("Direct2D attaches to the design's R8G8B8A8 swap chain through D3D11On12", "[graphics]")
{
    // The whole BGRA question. If this fails, the swap chain's format has to
    // change and 04-rendering-and-streaming.md:17 with it, through change
    // control -- so the failure message must say which step refused.
    OverlayHarness harness;

    const bool initialized
        = harness.overlay.Initialize(SharedDevice(), harness.directQueue, harness.swapChain, harness.error);

    INFO("overlay init error: " << Narrow(harness.error));
    REQUIRE(initialized);
    CHECK(harness.overlay.IsReady());
    CHECK(harness.overlay.WriteFactory() != nullptr);
}

TEST_CASE("A scene pass and a D2D overlay pass share one back buffer and present", "[graphics]")
{
    OverlayHarness harness;
    REQUIRE(harness.overlay.Initialize(SharedDevice(), harness.directQueue, harness.swapChain, harness.error));

    // Proves the resource-state handshake: the scene leaves the buffer in
    // RENDER_TARGET and the overlay's Release moves it to PRESENT. Getting
    // this wrong is exactly what the debug layer reports as an invalid
    // state transition at Present.
    CHECK(SUCCEEDED(harness.DrawOneOverlayFrame()));
}

TEST_CASE("Overlay frames repeat across the whole back-buffer ring", "[graphics]")
{
    OverlayHarness harness;
    REQUIRE(harness.overlay.Initialize(SharedDevice(), harness.directQueue, harness.swapChain, harness.error));

    // More frames than buffers, so every wrapped resource is acquired and
    // released more than once.
    for (int frame = 0; frame < 3 * static_cast<int>(D3D12SwapChain::kBufferCount); ++frame) {
        CHECK(SUCCEEDED(harness.DrawOneOverlayFrame()));
    }
}

TEST_CASE("One D2D brush is reused across every back buffer", "[graphics]")
{
    // The reason for moving off CreateDxgiSurfaceRenderTarget. That shape
    // gives one independent ID2D1RenderTarget per back buffer, and a brush
    // belongs to whichever target created it -- so every cached resource had
    // to be duplicated per buffer. One device context retargeted at a bitmap
    // per buffer makes device resources shared, which is what the ported
    // chrome (one brush recoloured per primitive) depends on.
    OverlayHarness harness;
    REQUIRE(harness.overlay.Initialize(SharedDevice(), harness.directQueue, harness.swapChain, harness.error));

    for (int frame = 0; frame < 2 * static_cast<int>(D3D12SwapChain::kBufferCount); ++frame) {
        REQUIRE(SUCCEEDED(harness.DrawOneOverlayFrame()));
    }

    // Same brush object throughout: DrawOneOverlayFrame only creates it when
    // it is null, so surviving every buffer proves it was never invalidated.
    CHECK(harness.sharedBrush != nullptr);
    CHECK(harness.overlay.Context() != nullptr);
}

TEST_CASE("The overlay survives a swap-chain resize", "[graphics]")
{
    // The part of spike 1 most likely to break: the wrapped resources hold
    // references to the back buffers, so ResizeBuffers fails unless they are
    // dropped and flushed first, then rebuilt against the new buffers.
    OverlayHarness harness;
    REQUIRE(harness.overlay.Initialize(SharedDevice(), harness.directQueue, harness.swapChain, harness.error));
    REQUIRE(SUCCEEDED(harness.DrawOneOverlayFrame()));

    harness.overlay.ReleaseBackBufferReferences();

    std::wstring resizeError;
    REQUIRE(harness.swapChain.Resize(1024, 768, resizeError));
    CHECK(harness.swapChain.Width() == 1024);
    CHECK(harness.swapChain.Height() == 768);

    REQUIRE(harness.overlay.RecreateBackBufferReferences(harness.swapChain, harness.error));

    // Draw again, to prove the rebuilt targets are usable rather than merely
    // that the calls returned true.
    CHECK(SUCCEEDED(harness.DrawOneOverlayFrame()));
}

TEST_CASE("Overlay frames and a resize leave the debug layer silent", "[graphics]")
{
    // The "GPU validation" leg of spike 1. Passing tests are not evidence on
    // their own: debug-layer messages go to OutputDebugString and do not fail
    // anything unless the info queue is inspected, which is what this does.
    // A wrong resource state at Present -- the most likely way to get the
    // scene/overlay handshake wrong -- surfaces here as an ERROR.
    bool queueAvailable = false;
    const int before = DebugLayerErrorCount(queueAvailable);
    if (!queueAvailable) {
        // Release build, or no Graphics Tools feature installed. Reported
        // rather than silently passing as though it had been checked.
        WARN("D3D12 info queue unavailable -- debug-layer validation not exercised in this build");
        return;
    }

    OverlayHarness harness;
    REQUIRE(harness.overlay.Initialize(SharedDevice(), harness.directQueue, harness.swapChain, harness.error));
    for (int frame = 0; frame < 2 * static_cast<int>(D3D12SwapChain::kBufferCount); ++frame) {
        REQUIRE(SUCCEEDED(harness.DrawOneOverlayFrame()));
    }

    harness.overlay.ReleaseBackBufferReferences();
    std::wstring resizeError;
    REQUIRE(harness.swapChain.Resize(800, 600, resizeError));
    REQUIRE(harness.overlay.RecreateBackBufferReferences(harness.swapChain, harness.error));
    REQUIRE(SUCCEEDED(harness.DrawOneOverlayFrame()));

    const int after = DebugLayerErrorCount(queueAvailable);
    INFO("debug-layer errors before: " << before << ", after: " << after);
    CHECK(after == before);
}

TEST_CASE("Shutdown releases the bridge without needing the swap chain", "[graphics]")
{
    // 04-rendering-and-streaming.md:190 requires D2D and D3D11On12 to be
    // released before the D3D12 objects they were built over.
    OverlayHarness harness;
    REQUIRE(harness.overlay.Initialize(SharedDevice(), harness.directQueue, harness.swapChain, harness.error));
    REQUIRE(SUCCEEDED(harness.DrawOneOverlayFrame()));

    harness.overlay.Shutdown();

    CHECK_FALSE(harness.overlay.IsReady());
    // Idempotent: the destructor runs Shutdown again on the way out.
    harness.overlay.Shutdown();
}

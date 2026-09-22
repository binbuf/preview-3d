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
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
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

    using Pixel = std::array<std::uint8_t, 4>;

    // Read the real swap-chain pixels after Release/Flush. This catches a
    // missing chrome pass or wrong target, even if EndDraw returns success.
    std::vector<Pixel> DrawChromeAndReadback(const OverlayFrame& frame, UINT index = D3D12SwapChain::kBufferCount)
    {
        // Hidden windows can be occluded, so Present need not advance the
        // index. Allow a test to explicitly exercise each target bitmap.
        if (index == D3D12SwapChain::kBufferCount) index = swapChain.CurrentBackBufferIndex();
        RecordSceneLeavingRenderTarget(index);
        REQUIRE(SUCCEEDED(overlay.DrawChrome(index, { 0, 0, 0, 1 }, frame)));
        auto wait = [&] {
            const auto fence = directQueue.SignalNext();
            REQUIRE(fence != 0);
            REQUIRE(directQueue.WaitForValue(fence, 5000) == D3D12CommandQueue::WaitResult::Signaled);
        };
        wait(); // safe allocator reset after the scene and interop commands

        ID3D12Resource* source = swapChain.BackBuffer(index);
        const auto desc = source->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bytes = 0;
        SharedDevice().Device()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        REQUIRE(SUCCEEDED(SharedDevice().Device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))));
        REQUIRE(SUCCEEDED(allocator->Reset()));
        REQUIRE(SUCCEEDED(commandList->Reset(allocator.Get(), nullptr)));
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commandList->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = source;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        commandList->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commandList->ResourceBarrier(1, &barrier);
        REQUIRE(SUCCEEDED(commandList->Close()));
        ID3D12CommandList* lists[] = { commandList.Get() };
        directQueue.Queue()->ExecuteCommandLists(1, lists);
        wait();

        std::vector<Pixel> pixels(static_cast<std::size_t>(swapChain.Width()) * swapChain.Height());
        void* mapped = nullptr;
        D3D12_RANGE range{ 0, static_cast<SIZE_T>(bytes) };
        REQUIRE(SUCCEEDED(readback->Map(0, &range, &mapped)));
        for (UINT y = 0; y < swapChain.Height(); ++y) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(y) * swapChain.Width(),
                static_cast<const std::byte*>(mapped) + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
                static_cast<std::size_t>(swapChain.Width()) * sizeof(Pixel));
        }
        D3D12_RANGE noWrites{ 0, 0 };
        readback->Unmap(0, &noWrites);
        REQUIRE(SUCCEEDED(swapChain.Present()));
        wait();
        return pixels;
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

TEST_CASE("Real chrome paints the bars, information panel and navigation gizmo across buffers and resize", "[graphics][chrome]")
{
    bool queueAvailable = false;
    const int errorsBefore = DebugLayerErrorCount(queueAvailable);
    OverlayHarness harness;
    REQUIRE(harness.overlay.Initialize(SharedDevice(), harness.directQueue, harness.swapChain, harness.error));

    for (int pass = 0; pass < 4; ++pass) {
        if (pass == 3) {
            harness.overlay.ReleaseBackBufferReferences();
            REQUIRE(harness.swapChain.Resize(1024, 768, harness.error));
            REQUIRE(harness.overlay.RecreateBackBufferReferences(harness.swapChain, harness.error));
        }
        const int width = static_cast<int>(harness.swapChain.Width());
        const int height = static_cast<int>(harness.swapChain.Height());
        const float scale = pass == 3 ? 1.5f : 1.0f;
        OverlayFrame frame;
        frame.info.state = ViewerState::Ready;
        frame.info.hasModel = true;
        frame.info.dpiScale = scale;
        frame.info.barToolbarHeight = static_cast<int>(40 * scale);
        frame.info.barBottomBarHeight = static_cast<int>(40 * scale);
        frame.info.infoPanelWidth = 200;
        frame.info.infoPanelSections = { { L"Mesh Data", { { L"Triangles", L"12" } } } };
        const int closeSize = static_cast<int>(28 * scale);
        const int closeRight = width - static_cast<int>(12 * scale);
        const int closeTop = frame.info.barToolbarHeight + static_cast<int>(15 * scale);
        frame.info.infoPanelCloseButtonRect = { closeRight - closeSize, closeTop, closeRight, closeTop + closeSize };
        frame.info.infoButtonRect = { 8, height - 36, 40, height - 4 };
        frame.info.fullscreenButtonRect = { width - 40, height - 36, width - 8, height - 4 };
        frame.info.zoomTrackRect = { width - 250, height - 21, width - 120, height - 19 };
        frame.chrome.UpdateLayout(width, frame.info.barToolbarHeight, scale, true, false, false);
        frame.gizmo.UpdateLayout(width - 200, height, frame.info.barToolbarHeight, frame.info.barBottomBarHeight, scale);

        const auto pixels = harness.DrawChromeAndReadback(frame, static_cast<UINT>(pass) % D3D12SwapChain::kBufferCount);
        auto pixel = [&](int x, int y) { return pixels[static_cast<std::size_t>(y) * width + x]; };
        const OverlayHarness::Pixel bar{ 0x2C, 0x2C, 0x2E, 0xFF };
        const OverlayHarness::Pixel panel{ 0x24, 0x24, 0x26, 0xFF };
        CHECK(pixel(2, 2) == bar);
        CHECK(pixel(2, height - 2) == bar);
        CHECK(pixel(width - 2, height / 2) == panel);
        const auto closeGlyph = pixel((frame.info.infoPanelCloseButtonRect.left + frame.info.infoPanelCloseButtonRect.right) / 2,
            (frame.info.infoPanelCloseButtonRect.top + frame.info.infoPanelCloseButtonRect.bottom) / 2);
        CHECK(closeGlyph[0] > 180);
        CHECK(closeGlyph[1] > 180);
        CHECK(closeGlyph[2] > 180);

        const auto geometry = frame.gizmo.ComputeDraw(DirectX::XMQuaternionIdentity());
        const auto& node = geometry.positive[0];
        const int nodeX = static_cast<int>(geometry.centerX + node.x);
        const int nodeY = static_cast<int>(geometry.centerY + node.y);
        // The red node is several pixels wide; sample away from its white X.
        const auto red = pixel(nodeX, nodeY + static_cast<int>(geometry.nodeRadius * 0.6f));
        CHECK(red[0] > red[1] + 50);
        CHECK(red[0] > red[2] + 50);
    }
    if (queueAvailable) CHECK(DebugLayerErrorCount(queueAvailable) == errorsBefore);
    else WARN("D3D12 info queue unavailable -- debug-layer validation not exercised in this build");
}

TEST_CASE("The directional sun highlights on hover and gains an opaque drag backdrop", "[graphics][chrome]")
{
    OverlayHarness harness;
    REQUIRE(harness.overlay.Initialize(SharedDevice(), harness.directQueue, harness.swapChain, harness.error));

    const int width = static_cast<int>(harness.swapChain.Width());
    const int height = static_cast<int>(harness.swapChain.Height());

    auto makeFrame = [&] {
        OverlayFrame frame;
        frame.info.state = ViewerState::Ready;
        frame.info.hasModel = true;
        frame.info.barToolbarHeight = 40;
        frame.info.barBottomBarHeight = 0;
        frame.info.lightingMode = LightingMode::Directional;
        frame.info.directionalLightAngle = 0.0f;
        frame.info.directionalLightElevation = 0.0f;
        frame.chrome.UpdateLayout(width, 40, 1.0f, true, false, false);
        frame.gizmo.UpdateLayout(width, height, 40, 0, 1.0f);
        return frame;
    };

    OverlayFrame base = makeFrame();
    const auto identity = DirectX::XMQuaternionIdentity();
    const auto geometry = base.gizmo.ComputeDraw(identity);
    const auto sun = base.gizmo.ComputeSun(identity, 0.0f, 0.0f);
    const int sunX = static_cast<int>(geometry.centerX + sun.x);
    const int sunY = static_cast<int>(geometry.centerY + sun.y);

    const auto normal = harness.DrawChromeAndReadback(base);
    base.gizmo.hover = NavGizmo::Part::Light;
    const auto hovered = harness.DrawChromeAndReadback(base);
    base.info.lightDragging = true;
    const auto dragging = harness.DrawChromeAndReadback(base);

    auto changedAroundSun = [&](const auto& first, const auto& second) {
        std::size_t count = 0;
        for (int y = sunY - 22; y <= sunY + 22; ++y) {
            for (int x = sunX - 22; x <= sunX + 22; ++x) {
                if (x < 0 || y < 0 || x >= width || y >= height) continue;
                const auto index = static_cast<std::size_t>(y) * width + x;
                if (first[index] != second[index]) ++count;
            }
        }
        return count;
    };
    // Hover repaints the sun a lighter yellow...
    CHECK(changedAroundSun(normal, hovered) > 0);
    // ...and the drag backdrop adds a visibly larger, opaque control.
    CHECK(changedAroundSun(normal, dragging) > 0);
    CHECK(changedAroundSun(hovered, dragging) > 0);
}

TEST_CASE("The bottom bar animates while the render timer runs and then shows its duration", "[graphics][chrome]")
{
    OverlayHarness harness;
    REQUIRE(harness.overlay.Initialize(SharedDevice(), harness.directQueue, harness.swapChain, harness.error));

    const int width = static_cast<int>(harness.swapChain.Width());
    const int height = static_cast<int>(harness.swapChain.Height());
    OverlayFrame frame;
    frame.info.state = ViewerState::Ready;
    frame.info.hasModel = true;
    frame.info.barToolbarHeight = 40;
    frame.info.barBottomBarHeight = 40;
    frame.info.infoButtonRect = { 8, height - 36, 40, height - 4 };
    frame.info.fullscreenButtonRect = { width - 40, height - 36, width - 8, height - 4 };
    frame.info.zoomTrackRect = { width - 250, height - 21, width - 120, height - 19 };
    frame.info.renderTimerRunning = true;
    frame.chrome.UpdateLayout(width, frame.info.barToolbarHeight, 1.0f, true, false, false);

    frame.info.animationPhase = 0.0f;
    const auto firstSpinner = harness.DrawChromeAndReadback(frame);
    frame.info.animationPhase = 0.5f;
    const auto secondSpinner = harness.DrawChromeAndReadback(frame);

    auto differentPixels = [&](const auto& first, const auto& second) {
        std::size_t count = 0;
        for (int y = height - 34; y < height - 6; ++y) {
            for (int x = 48; x < 150; ++x) {
                const auto index = static_cast<std::size_t>(y) * width + x;
                if (first[index] != second[index]) ++count;
            }
        }
        return count;
    };
    CHECK(differentPixels(firstSpinner, secondSpinner) > 0);

    frame.info.renderTimerRunning = false;
    frame.info.renderDurationText = L"1.25 s";
    const auto duration = harness.DrawChromeAndReadback(frame);
    CHECK(differentPixels(secondSpinner, duration) > 0);
}

TEST_CASE("Loading and failure cards release the wrapped buffer and keep caption buttons visible", "[graphics][chrome]")
{
    OverlayHarness harness;
    REQUIRE(harness.overlay.Initialize(SharedDevice(), harness.directQueue, harness.swapChain, harness.error));
    for (const auto state : { ViewerState::Empty, ViewerState::Loading, ViewerState::Failed, ViewerState::Ready }) {
        OverlayFrame frame;
        frame.info.state = state;
        frame.info.errorSummary = L"Could not open this model.";
        frame.info.errorDetails = L"The file is not supported.";
        frame.chrome.UpdateLayout(640, 40, 1.0f, false, false, false);
        frame.chrome.hover = Chrome::Part::Close;
        const auto pixels = harness.DrawChromeAndReadback(frame);
        const RECT close = frame.chrome.Button(Chrome::Part::Close).rect;
        const auto caption = pixels[static_cast<std::size_t>(close.top + 2) * 640 + close.left + 2];
        CHECK(caption[0] > 150);
        CHECK(caption[0] > caption[1] + 50);
        if (state == ViewerState::Failed) {
            const RECT card = CalculateErrorCardRect(640, 480, 40, 1.0f);
            const OverlayHarness::Pixel fill{ 0x24, 0x24, 0x26, 0xFF };
            CHECK(pixels[static_cast<std::size_t>(card.bottom - 30) * 640 + card.left + 30] == fill);
        }
    }
}

TEST_CASE("The four lighting-mode buttons paint distinct glyphs", "[graphics][chrome]")
{
    OverlayHarness harness;
    REQUIRE(harness.overlay.Initialize(SharedDevice(), harness.directQueue, harness.swapChain, harness.error));

    const int width = static_cast<int>(harness.swapChain.Width());
    const int height = static_cast<int>(harness.swapChain.Height());
    OverlayFrame frame;
    frame.info.state = ViewerState::Ready;
    frame.info.hasModel = true;
    frame.info.barToolbarHeight = 40;
    frame.info.barBottomBarHeight = 40;
    frame.info.infoButtonRect = { 8, height - 36, 40, height - 4 };
    frame.info.fullscreenButtonRect = { width - 40, height - 36, width - 8, height - 4 };
    frame.info.zoomTrackRect = { width - 250, height - 21, width - 120, height - 19 };

    // Four equal square mode buttons, mirroring ComputeLightingToolbarLayout
    // in the app at 100% DPI.
    const int size = 34;
    const int gap = 3;
    const int top = height - 34;
    const int bottom = height - 4;
    int left = width / 2 - (4 * size + 3 * gap) / 2;
    frame.info.lightingToolbarRect = { left - 5, height - 40, left + 4 * size + 3 * gap + 5, height };
    frame.info.studioButtonRect = { left, top, left + size, bottom }; left += size + gap;
    frame.info.clayButtonRect = { left, top, left + size, bottom }; left += size + gap;
    frame.info.directionalButtonRect = { left, top, left + size, bottom }; left += size + gap;
    frame.info.wireframeButtonRect = { left, top, left + size, bottom };
    frame.info.lightingMode = LightingMode::Studio;
    frame.chrome.UpdateLayout(width, frame.info.barToolbarHeight, 1.0f, true, false, false);
    frame.gizmo.UpdateLayout(width - 200, height, frame.info.barToolbarHeight, frame.info.barBottomBarHeight, 1.0f);

    const auto pixels = harness.DrawChromeAndReadback(frame);

    struct Glyph { int bright; std::uint64_t hash; };
    auto glyph = [&](const RECT& rect) {
        Glyph result{ 0, 1469598103934665603ull };
        for (int y = rect.top; y < rect.bottom; ++y) {
            for (int x = rect.left; x < rect.right; ++x) {
                const auto& p = pixels[static_cast<std::size_t>(y) * width + x];
                if (p[0] > 190 && p[1] > 190 && p[2] > 190) {
                    ++result.bright;
                    result.hash = (result.hash ^ static_cast<std::uint64_t>(x * 131 + y)) * 1099511628211ull;
                }
            }
        }
        return result;
    };

    const Glyph studio = glyph(frame.info.studioButtonRect);
    const Glyph clay = glyph(frame.info.clayButtonRect);
    const Glyph directional = glyph(frame.info.directionalButtonRect);
    const Glyph wireframe = glyph(frame.info.wireframeButtonRect);

    // Every button actually paints something, and no two modes share a glyph.
    CHECK(studio.bright > 0);
    CHECK(clay.bright > 0);
    CHECK(directional.bright > 0);
    CHECK(wireframe.bright > 0);
    CHECK(studio.hash != clay.hash);
    CHECK(studio.hash != directional.hash);
    CHECK(studio.hash != wireframe.hash);
    CHECK(clay.hash != directional.hash);
    CHECK(clay.hash != wireframe.hash);
    CHECK(directional.hash != wireframe.hash);
}

TEST_CASE("The nav gizmo rides the directional-light sun marker on its outer ring", "[graphics][gizmo]")
{
    NavGizmo gizmo;
    gizmo.UpdateLayout(800, 600, 40, 40, 1.0f);
    const auto geometry = gizmo.ComputeDraw(DirectX::XMQuaternionIdentity());
    const float ring = geometry.outerRadius;
    REQUIRE(ring > 0.0f);

    const auto first = gizmo.ComputeSun(DirectX::XMQuaternionIdentity(), 0.25f);
    const auto second = gizmo.ComputeSun(DirectX::XMQuaternionIdentity(), 0.75f);
    CHECK(first.visible);
    CHECK(second.visible);
    // Projected by azimuth only, so the marker always sits on the ring.
    CHECK(std::abs(std::sqrt(first.x * first.x + first.y * first.y) - ring) < 0.01f);
    CHECK(std::abs(std::sqrt(second.x * second.x + second.y * second.y) - ring) < 0.01f);
    // The horizontal rotation moves the marker around the ring.
    CHECK((first.x != second.x || first.y != second.y));
    // Depth stays a normalized view-space coordinate either way.
    CHECK(std::abs(first.depth) <= 1.0f);
    CHECK(std::abs(second.depth) <= 1.0f);
}

TEST_CASE("The sun marker rises toward the gizmo center with elevation and round-trips", "[graphics][gizmo]")
{
    NavGizmo gizmo;
    gizmo.UpdateLayout(800, 600, 40, 40, 1.0f);
    const auto identity = DirectX::XMQuaternionIdentity();
    const auto geometry = gizmo.ComputeDraw(identity);
    const float ring = geometry.outerRadius;
    REQUIRE(ring > 0.0f);

    const auto horizon = gizmo.ComputeSun(identity, 0.25f, 0.0f);
    const auto mid = gizmo.ComputeSun(identity, 0.25f, 0.6f);
    const auto overhead = gizmo.ComputeSun(identity, 0.25f, 1.4835f);
    const float radiusHorizon = std::sqrt(horizon.x * horizon.x + horizon.y * horizon.y);
    const float radiusMid = std::sqrt(mid.x * mid.x + mid.y * mid.y);
    const float radiusOverhead = std::sqrt(overhead.x * overhead.x + overhead.y * overhead.y);

    // At the horizon the marker rides the ring; raising it pulls it inward.
    CHECK(std::abs(radiusHorizon - ring) < 0.01f);
    CHECK(radiusMid < radiusHorizon);
    CHECK(radiusOverhead < radiusMid);
    CHECK(radiusOverhead < 0.2f * ring);

    // The radial placement inverts back to the elevation it was drawn at.
    for (const float expected : { 0.0f, 0.3f, 0.7f, 1.2f })
    {
        const auto sun = gizmo.ComputeSun(identity, 0.4f, expected);
        REQUIRE(sun.visible);
        float recovered = -1.0f;
        REQUIRE(gizmo.LightElevationForPoint(geometry.centerX + sun.x, geometry.centerY + sun.y, recovered));
        CHECK(std::abs(recovered - expected) < 0.01f);
    }

    // A raised marker is still its own light drag handle.
    const auto marker = gizmo.ComputeSun(identity, 0.25f, 0.7f);
    CHECK(gizmo.HitTest(identity, geometry.centerX + marker.x, geometry.centerY + marker.y, true, 0.25f, 0.7f)
        == NavGizmo::Part::Light);
}

TEST_CASE("The nav gizmo carves the light ring out of the orbit ball only when asked", "[graphics][gizmo]")
{
    NavGizmo gizmo;
    gizmo.UpdateLayout(800, 600, 40, 40, 1.0f);
    const auto geometry = gizmo.ComputeDraw(DirectX::XMQuaternionIdentity());
    const float ring = geometry.outerRadius;
    const float cx = geometry.centerX;
    const float cy = geometry.centerY;
    REQUIRE(ring > 0.0f);

    // The white outline is the light-rotation target in Directional mode...
    CHECK(gizmo.HitTest(DirectX::XMQuaternionIdentity(), cx + ring, cy, true) == NavGizmo::Part::Light);
    // ...and not in the other modes, where the whole disc still orbits.
    CHECK(gizmo.HitTest(DirectX::XMQuaternionIdentity(), cx + ring, cy, false) != NavGizmo::Part::Light);
    // Well inside the disc stays the camera-orbit ball. (Off-axis so the
    // sample does not land on a stem or an axis node.)
    CHECK(gizmo.HitTest(DirectX::XMQuaternionIdentity(), cx + ring * 0.3f, cy + ring * 0.3f, true) == NavGizmo::Part::Ball);
}

TEST_CASE("Dragging the light ring recovers the angle the sun was drawn at", "[graphics][gizmo]")
{
    NavGizmo gizmo;
    gizmo.UpdateLayout(800, 600, 40, 40, 1.0f);
    const auto identity = DirectX::XMQuaternionIdentity();
    const auto geometry = gizmo.ComputeDraw(identity);

    for (const float expected : { 0.0f, 0.15f, 0.4f, 0.75f, 0.9f })
    {
        const auto sun = gizmo.ComputeSun(identity, expected);
        REQUIRE(sun.visible);
        float recovered = -1.0f;
        REQUIRE(gizmo.LightAngleForPoint(identity, geometry.centerX + sun.x, geometry.centerY + sun.y,
            expected, recovered));
        // Dragging the sun itself must not jump the light to the far side.
        const float gap = std::abs(std::remainder(recovered - expected, 1.0f));
        CHECK(gap < 0.01f);
    }
}

TEST_CASE("The light ring reaches every angle from a level camera", "[graphics][gizmo]")
{
    NavGizmo gizmo;
    gizmo.UpdateLayout(800, 600, 40, 40, 1.0f);
    const auto geometry = gizmo.ComputeDraw(DirectX::XMQuaternionIdentity());
    // Front and Right are the level views that used to collapse the light's
    // projected path to a line, leaving half the ring unreachable.
    const DirectX::XMVECTOR cameras[3] = {
        DirectX::XMQuaternionIdentity(),
        CanonicalViewOrientation(ViewDir::Front),
        CanonicalViewOrientation(ViewDir::Right) };

    for (const DirectX::XMVECTOR& camera : cameras)
    {
        bool positiveX = false, negativeX = false, positiveY = false, negativeY = false;
        for (int step = 0; step < 72; ++step)
        {
            const float expected = static_cast<float>(step) / 72.0f;
            const auto sun = gizmo.ComputeSun(camera, expected);
            REQUIRE(sun.visible);
            positiveX = positiveX || sun.x > 1.0f;
            negativeX = negativeX || sun.x < -1.0f;
            positiveY = positiveY || sun.y > 1.0f;
            negativeY = negativeY || sun.y < -1.0f;
            float recovered = -1.0f;
            REQUIRE(gizmo.LightAngleForPoint(camera, geometry.centerX + sun.x, geometry.centerY + sun.y,
                expected, recovered));
            CHECK(std::abs(std::remainder(recovered - expected, 1.0f)) < 0.01f);
        }
        // All four quadrants of the ring are reachable.
        CHECK(positiveX);
        CHECK(negativeX);
        CHECK(positiveY);
        CHECK(negativeY);
    }
}

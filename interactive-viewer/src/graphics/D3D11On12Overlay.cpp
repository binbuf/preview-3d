#include "D3D11On12Overlay.h"

#include <d3d11.h>

#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace {

std::wstring WithHr(const wchar_t* what, HRESULT hr)
{
    return std::wstring(what) + L" (HRESULT 0x" + std::to_wstring(static_cast<unsigned long>(hr)) + L").";
}

} // namespace

D3D11On12Overlay::~D3D11On12Overlay()
{
    Shutdown();
}

bool D3D11On12Overlay::Initialize(D3D12Device& device, D3D12CommandQueue& directQueue, D3D12SwapChain& swapChain,
                                   std::wstring& error)
{
    IUnknown* queues[] = { directQueue.Queue() };
    // BGRA support is required for D2D interop regardless of the swap chain's
    // own format -- it is a device capability flag, not a surface format.
    const HRESULT deviceHr = D3D11On12CreateDevice(device.Device(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                                     queues, 1, 0, &d3d11Device_, &d3d11Context_, nullptr);
    if (FAILED(deviceHr)) {
        error = WithHr(L"The D3D11On12 device could not be created", deviceHr);
        return false;
    }
    const HRESULT queryHr = d3d11Device_.As(&d3d11On12Device_);
    if (FAILED(queryHr)) {
        error = WithHr(L"The D3D11On12 device interface could not be queried", queryHr);
        return false;
    }

    D2D1_FACTORY_OPTIONS factoryOptions{};
#ifdef _DEBUG
    factoryOptions.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    const HRESULT factoryHr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                                 &factoryOptions, &d2dFactory_);
    if (FAILED(factoryHr)) {
        error = WithHr(L"The Direct2D factory could not be created", factoryHr);
        return false;
    }

    // The D2D device rides on the same underlying DXGI device as the 11on12
    // device, which is what lets one context target every back buffer.
    ComPtr<IDXGIDevice> dxgiDevice;
    const HRESULT dxgiHr = d3d11Device_.As(&dxgiDevice);
    if (FAILED(dxgiHr)) {
        error = WithHr(L"The D3D11On12 device is not a DXGI device", dxgiHr);
        return false;
    }
    const HRESULT d2dDeviceHr = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
    if (FAILED(d2dDeviceHr)) {
        error = WithHr(L"The Direct2D device could not be created", d2dDeviceHr);
        return false;
    }
    const HRESULT contextHr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_);
    if (FAILED(contextHr)) {
        error = WithHr(L"The Direct2D device context could not be created", contextHr);
        return false;
    }

    const HRESULT writeHr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf()));
    if (FAILED(writeHr)) {
        error = WithHr(L"The DirectWrite factory could not be created", writeHr);
        return false;
    }

    swapChain_ = &swapChain;
    return CreateTargetsForBackBuffers(swapChain, error);
}

bool D3D11On12Overlay::CreateTargetsForBackBuffers(D3D12SwapChain& swapChain, std::wstring& error)
{
    swapChain_ = &swapChain;

    for (UINT i = 0; i < D3D12SwapChain::kBufferCount; ++i) {
        ID3D12Resource* backBuffer = swapChain.BackBuffer(i);
        if (backBuffer == nullptr) {
            error = L"A swap-chain back buffer was missing when wrapping it for the overlay.";
            return false;
        }

        // In RENDER_TARGET, out PRESENT: the scene pass leaves the buffer as
        // a render target and the Release inside EndDraw is what moves it to
        // PRESENT. See this class's header comment and 04-...:46.
        D3D11_RESOURCE_FLAGS flags{};
        flags.BindFlags = D3D11_BIND_RENDER_TARGET;
        const HRESULT wrapHr = d3d11On12Device_->CreateWrappedResource(
            backBuffer, &flags, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT,
            IID_PPV_ARGS(&wrappedBackBuffers_[i]));
        if (FAILED(wrapHr)) {
            error = WithHr(L"A swap-chain back buffer could not be wrapped for D3D11On12", wrapHr);
            return false;
        }

        ComPtr<IDXGISurface> surface;
        const HRESULT surfaceHr = wrappedBackBuffers_[i].As(&surface);
        if (FAILED(surfaceHr)) {
            error = WithHr(L"A wrapped back buffer is not a DXGI surface", surfaceHr);
            return false;
        }

        // The format must match the swap chain's. The spike confirmed D2D
        // accepts R8G8B8A8_UNORM here -- the design's format (04-...:17) --
        // so no BGRA swap chain is needed.
        DXGI_SURFACE_DESC surfaceDesc{};
        surface->GetDesc(&surfaceDesc);
        const D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(surfaceDesc.Format, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        const HRESULT bitmapHr
            = d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &bitmapProperties, &targetBitmaps_[i]);
        if (FAILED(bitmapHr)) {
            error = WithHr(L"Direct2D could not create a target bitmap over the wrapped back buffer", bitmapHr);
            return false;
        }
    }
    return true;
}

void D3D11On12Overlay::ReleaseBackBufferReferences()
{
    if (d2dContext_) d2dContext_->SetTarget(nullptr);
    for (UINT i = 0; i < D3D12SwapChain::kBufferCount; ++i) {
        targetBitmaps_[i].Reset();
        wrappedBackBuffers_[i].Reset();
    }
    // The wrapped resources hold D3D11-side references; without a flush they
    // can outlive the Reset above and keep ResizeBuffers from succeeding.
    if (d3d11Context_) {
        d3d11Context_->ClearState();
        d3d11Context_->Flush();
    }
}

bool D3D11On12Overlay::RecreateBackBufferReferences(D3D12SwapChain& swapChain, std::wstring& error)
{
    if (!IsReady()) {
        error = L"The overlay bridge is not initialized.";
        return false;
    }
    return CreateTargetsForBackBuffers(swapChain, error);
}

bool D3D11On12Overlay::ConsumeTargetsWereRecreated() noexcept
{
    const bool was = targetsRecreated_;
    targetsRecreated_ = false;
    return was;
}

ID2D1DeviceContext* D3D11On12Overlay::BeginDraw(UINT backBufferIndex)
{
    if (!IsReady() || backBufferIndex >= D3D12SwapChain::kBufferCount) return nullptr;

    // Rebuild after a lost target, rather than going dark until the next
    // resize the way the D3D11 renderer does.
    if (!targetBitmaps_[backBufferIndex] && swapChain_ != nullptr) {
        std::wstring ignored;
        if (!CreateTargetsForBackBuffers(*swapChain_, ignored)) return nullptr;
        targetsRecreated_ = true;
    }
    if (!targetBitmaps_[backBufferIndex]) return nullptr;

    ID3D11Resource* resources[] = { wrappedBackBuffers_[backBufferIndex].Get() };
    d3d11On12Device_->AcquireWrappedResources(resources, 1);
    d2dContext_->SetTarget(targetBitmaps_[backBufferIndex].Get());
    d2dContext_->BeginDraw();
    return d2dContext_.Get();
}

HRESULT D3D11On12Overlay::EndDraw(UINT backBufferIndex)
{
    if (!IsReady() || backBufferIndex >= D3D12SwapChain::kBufferCount) return E_FAIL;

    const HRESULT hr = d2dContext_->EndDraw();
    d2dContext_->SetTarget(nullptr);

    ID3D11Resource* resources[] = { wrappedBackBuffers_[backBufferIndex].Get() };
    d3d11On12Device_->ReleaseWrappedResources(resources, 1);
    // Flush submits the 11on12 work onto the shared direct queue. Without it
    // the D2D commands are not on the queue when the caller signals its frame
    // fence and presents.
    d3d11Context_->Flush();

    if (hr == D2DERR_RECREATE_TARGET) {
        // Drop everything so the next BeginDraw rebuilds. The caller learns
        // through ConsumeTargetsWereRecreated that its own cached device
        // resources are gone too.
        ReleaseBackBufferReferences();
    }
    return hr;
}

void D3D11On12Overlay::Shutdown()
{
    ReleaseBackBufferReferences();
    writeFactory_.Reset();
    d2dContext_.Reset();
    d2dDevice_.Reset();
    d2dFactory_.Reset();
    d3d11On12Device_.Reset();
    d3d11Context_.Reset();
    d3d11Device_.Reset();
    swapChain_ = nullptr;
}

#include "D3D12Device.h"

using Microsoft::WRL::ComPtr;

namespace {

bool IsSoftwareAdapter(const DXGI_ADAPTER_DESC1& desc)
{
    return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
}

bool ProbeFeatureLevel11_0(IDXGIAdapter1* adapter)
{
    // Passing nullptr for ppDevice is a documented support-probe: it
    // reports whether device creation would succeed without actually
    // constructing a device, avoiding a throwaway ID3D12Device per
    // candidate adapter while walking the adapter list.
    return SUCCEEDED(
        D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
}

} // namespace

bool D3D12Device::TryEnableDebugLayerIfDeveloperBuild()
{
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        return true;
    }
    // Non-fatal: the D3D12/DXGI debug layers require the "Graphics Tools"
    // optional Windows feature, which may not be installed on every dev
    // machine. Falling through without the debug layer is the correct
    // behavior, not an error.
    return false;
#else
    return false;
#endif
}

ComPtr<IDXGIAdapter1> D3D12Device::SelectHardwareAdapter(IDXGIFactory6& factory)
{
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory.EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                          IID_PPV_ARGS(&adapter))
         != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !IsSoftwareAdapter(desc)
            && ProbeFeatureLevel11_0(adapter.Get())) {
            return adapter;
        }
        adapter.Reset();
    }
    return nullptr;
}

ComPtr<IDXGIAdapter1> D3D12Device::SelectWarpAdapter(IDXGIFactory6& factory)
{
    ComPtr<IDXGIAdapter1> adapter;
    // EnumWarpAdapter is inherited from IDXGIFactory4; IDXGIFactory6
    // derives from it.
    if (FAILED(factory.EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
        return nullptr;
    }
    return adapter;
}

D3D12Device::CreateResult D3D12Device::Initialize(const CreateOptions& options)
{
    CreateResult result;

    result.debugLayerEnabled = TryEnableDebugLayerIfDeveloperBuild();

    UINT factoryFlags = 0;
    if (result.debugLayerEnabled) {
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }

    result.hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_));
    if (FAILED(result.hr)) {
        return result;
    }

    ComPtr<IDXGIAdapter1> adapter = options.forceWarp ? SelectWarpAdapter(*factory_.Get())
                                                       : SelectHardwareAdapter(*factory_.Get());
    if (!adapter) {
        result.hr = DXGI_ERROR_NOT_FOUND;
        return result;
    }

    result.hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    if (FAILED(result.hr)) {
        return result;
    }

    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        result.adapterDescription.assign(desc.Description);
        result.isWarpAdapter = IsSoftwareAdapter(desc);
    }

    result.success = true;
    return result;
}

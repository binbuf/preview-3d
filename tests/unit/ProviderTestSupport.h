#pragma once

// Shared T13 test doubles for the provider suites.
//
// `MemoryStream` is a stack-allocated IStream double (the same shape used by
// ProviderStreamTests.cpp): `Release` never deletes, so the test owns it for its
// whole scope and the provider only releases the reference it added.
//
// `ProviderModule` loads the built provider DLL the way the Shell and the T17
// host do: the two exports are PRIVATE, so there is no import library and the
// entry points are resolved with GetProcAddress.

#include "FamilyRouting.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <unknwn.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace preview3d::test {

class MemoryStream final : public IStream {
public:
    explicit MemoryStream(std::vector<std::byte> data) : data_(std::move(data)) {}

    MemoryStream(const MemoryStream&) = delete;
    MemoryStream& operator=(const MemoryStream&) = delete;

    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppvObject) noexcept override
    {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IStream))) {
            *ppvObject = static_cast<IStream*>(this);
            ++ref_;
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG WINAPI AddRef() noexcept override { return static_cast<ULONG>(++ref_); }
    ULONG WINAPI Release() noexcept override { return static_cast<ULONG>(--ref_); }

    // ISequentialStream
    HRESULT WINAPI Read(void* pv, ULONG cb, ULONG* pcbRead) noexcept override
    {
        if (pv == nullptr || pcbRead == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        ULONG count = 0;
        if (position_ < data_.size()) {
            const std::uint64_t available =
                static_cast<std::uint64_t>(data_.size() - position_);
            count = static_cast<ULONG>((std::min)(
                available, static_cast<std::uint64_t>(cb)));
            if (count != 0) {
                std::memcpy(pv, data_.data() + position_, count);
                position_ += count;
            }
        }
        *pcbRead = count;
        return S_OK;
    }

    HRESULT WINAPI Write(const void*, ULONG, ULONG*) noexcept override
    {
        return STG_E_ACCESSDENIED;
    }

    // IStream
    HRESULT WINAPI Seek(LARGE_INTEGER move, DWORD origin,
                        ULARGE_INTEGER* newPosition) noexcept override
    {
        std::int64_t base = 0;
        switch (origin) {
            case STREAM_SEEK_SET: base = 0; break;
            case STREAM_SEEK_CUR: base = static_cast<std::int64_t>(position_); break;
            case STREAM_SEEK_END: base = static_cast<std::int64_t>(data_.size()); break;
            default: return STG_E_INVALIDFUNCTION;
        }
        const std::int64_t next = base + move.QuadPart;
        if (next < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        position_ = static_cast<std::uint64_t>(next);
        if (newPosition != nullptr) {
            newPosition->QuadPart = position_;
        }
        return S_OK;
    }

    HRESULT WINAPI SetSize(ULARGE_INTEGER) noexcept override { return E_NOTIMPL; }
    HRESULT WINAPI CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                          ULARGE_INTEGER*) noexcept override
    {
        return E_NOTIMPL;
    }
    HRESULT WINAPI Commit(DWORD) noexcept override { return S_OK; }
    HRESULT WINAPI Revert() noexcept override { return E_NOTIMPL; }
    HRESULT WINAPI LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override
    {
        return E_NOTIMPL;
    }
    HRESULT WINAPI UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                DWORD) noexcept override
    {
        return E_NOTIMPL;
    }
    HRESULT WINAPI Stat(STATSTG* pstatstg, DWORD) noexcept override
    {
        if (pstatstg == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        std::memset(pstatstg, 0, sizeof(*pstatstg));
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart = static_cast<ULONGLONG>(data_.size());
        return S_OK;
    }
    HRESULT WINAPI Clone(IStream**) noexcept override { return E_NOTIMPL; }

private:
    std::vector<std::byte> data_;
    std::uint64_t position_ = 0;
    LONG ref_ = 1;
};

using GetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
using CanUnloadNowFn = HRESULT(WINAPI*)();

struct ProviderModule {
    HMODULE handle = nullptr;
    GetClassObjectFn getClassObject = nullptr;
    CanUnloadNowFn canUnloadNow = nullptr;

    ProviderModule()
    {
        handle = ::LoadLibraryExW(PREVIEW3D_PROVIDER_DLL, nullptr, 0);
        if (handle != nullptr) {
            getClassObject = reinterpret_cast<GetClassObjectFn>(
                ::GetProcAddress(handle, "DllGetClassObject"));
            canUnloadNow = reinterpret_cast<CanUnloadNowFn>(
                ::GetProcAddress(handle, "DllCanUnloadNow"));
        }
    }

    ProviderModule(const ProviderModule&) = delete;
    ProviderModule& operator=(const ProviderModule&) = delete;

    ~ProviderModule()
    {
        if (handle != nullptr) {
            ::FreeLibrary(handle);
        }
    }

    bool Ready() const
    {
        return handle != nullptr && getClassObject != nullptr && canUnloadNow != nullptr;
    }
};

inline GUID GuidFromText(const char* text)
{
    const std::wstring wide(text, text + std::char_traits<char>::length(text));
    GUID guid{};
    REQUIRE(::CLSIDFromString(wide.c_str(), &guid) == S_OK);
    return guid;
}

inline GUID FamilyClsid(preview3d::provider::Family family)
{
    for (const auto& route : preview3d::provider::FamilyRoutes()) {
        if (route.family == family) {
            return GuidFromText(route.clsid);
        }
    }
    return GUID{};
}

} // namespace preview3d::test
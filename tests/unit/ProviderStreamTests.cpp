// T12 bounded stream backing coverage.
//
// Two layers:
//   - the `BoundedStreamSource` behaviour, exercised directly against an
//     in-memory `IStream` double: seekable range reads through the block cache,
//     non-seekable materialization, the oversized STATSTG fail-fast, short-read
//     inconsistency, deadline abort, ledger charging and the contiguous cap;
//   - the `IInitializeWithStream` COM boundary, loaded from the built DLL the
//     same way the Shell/T17 host does, covering null input, independent
//     adoption, repeated initialization and the oversized HRESULT.
//
// Every source is destroyed before its stack `MemoryStream` goes out of scope,
// and every COM object/factory is released, so DllCanUnloadNow returns to S_OK.

#include <catch2/catch_test_macros.hpp>

#include "StreamSource.h"

#include "FamilyRouting.h"

#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <propsys.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using preview3d::provider::AllocationLedger;
using preview3d::provider::BoundedStreamSource;
using preview3d::provider::Deadline;
using preview3d::provider::ProviderOutcome;

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

std::vector<std::byte> MakeData(std::size_t size)
{
    std::vector<std::byte> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = std::byte((i * 31u + 7u) & 0xFFu);
    }
    return data;
}

// A stack-allocated IStream double. `Release` never deletes: the test owns the
// object for its whole scope, and the source only releases the reference it
// added.
class MemoryStream final : public IStream {
public:
    explicit MemoryStream(std::vector<std::byte> data) : data_(std::move(data)) {}

    MemoryStream(const MemoryStream&) = delete;
    MemoryStream& operator=(const MemoryStream&) = delete;

    void SetSeekable(bool value) noexcept { seekable_ = value; }
    void SetStatSupported(bool value) noexcept { statSupported_ = value; }
    void SetStatSize(std::uint64_t size) noexcept
    {
        statSize_ = size;
        hasStatSize_ = true;
    }
    void SetSeekEndSize(std::uint64_t size) noexcept
    {
        seekEndSize_ = size;
        hasSeekEndSize_ = true;
    }
    void SetShortReadLimit(std::uint64_t limit) noexcept { shortReadLimit_ = limit; }
    // A hostile stream may report an arbitrary raw ULARGE_INTEGER from
    // STREAM_SEEK_END without going through signed position arithmetic.
    void SetSeekEndReportOnly(bool value) noexcept { seekEndReportOnly_ = value; }

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

    ULONG WINAPI AddRef() noexcept override
    {
        return static_cast<ULONG>(++ref_);
    }

    ULONG WINAPI Release() noexcept override
    {
        return static_cast<ULONG>(--ref_);
    }

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
            std::uint64_t allowed = available;
            if (shortReadLimit_ != UINT64_MAX) {
                allowed = (shortReadLimit_ > position_)
                              ? shortReadLimit_ - position_
                              : 0;
                allowed = (std::min)(allowed, available);
            }
            count = static_cast<ULONG>((std::min)(allowed,
                                                  static_cast<std::uint64_t>(cb)));
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
        if (!seekable_) {
            return E_NOTIMPL;
        }
        std::int64_t base = 0;
        switch (origin) {
            case STREAM_SEEK_SET: base = 0; break;
            case STREAM_SEEK_CUR: base = static_cast<std::int64_t>(position_); break;
            case STREAM_SEEK_END:
                if (seekEndReportOnly_) {
                    if (newPosition != nullptr) {
                        newPosition->QuadPart =
                            hasSeekEndSize_ ? seekEndSize_
                                            : static_cast<ULONGLONG>(data_.size());
                    }
                    return S_OK;
                }
                base = static_cast<std::int64_t>(hasSeekEndSize_ ? seekEndSize_
                                                                 : data_.size());
                break;
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
        if (!statSupported_) {
            return E_NOTIMPL;
        }
        std::memset(pstatstg, 0, sizeof(*pstatstg));
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart =
            hasStatSize_ ? statSize_ : static_cast<ULONGLONG>(data_.size());
        return S_OK;
    }

    HRESULT WINAPI Clone(IStream**) noexcept override { return E_NOTIMPL; }

private:
    std::vector<std::byte> data_;
    std::uint64_t position_ = 0;
    LONG ref_ = 1;
    bool seekable_ = true;
    bool statSupported_ = true;
    bool hasStatSize_ = false;
    std::uint64_t statSize_ = 0;
    bool hasSeekEndSize_ = false;
    std::uint64_t seekEndSize_ = 0;
    bool seekEndReportOnly_ = false;
    std::uint64_t shortReadLimit_ = UINT64_MAX;
};

// --- COM boundary loader (mirrors ProviderComTests) -------------------------

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

GUID GuidFromText(const char* text)
{
    const std::wstring wide(text, text + std::char_traits<char>::length(text));
    GUID guid{};
    CHECK(::CLSIDFromString(wide.c_str(), &guid) == S_OK);
    return guid;
}

GUID FirstFamilyClsid()
{
    return GuidFromText(preview3d::provider::FamilyRoutes().front().clsid);
}

IInitializeWithStream* CreateInitializable(ProviderModule& module)
{
    void* raw = nullptr;
    if (FAILED(module.getClassObject(FirstFamilyClsid(), __uuidof(IClassFactory),
                                     &raw))) {
        return nullptr;
    }
    auto* factory = static_cast<IClassFactory*>(raw);

    IInitializeWithStream* init = nullptr;
    const HRESULT hr = factory->CreateInstance(
        nullptr, __uuidof(IInitializeWithStream), reinterpret_cast<void**>(&init));
    factory->Release();
    return SUCCEEDED(hr) ? init : nullptr;
}

} // namespace

TEST_CASE("a seekable source serves checked arbitrary ranges", "[provider][stream]")
{
    const auto data = MakeData(200 * 1024);
    MemoryStream stream(data);
    AllocationLedger ledger;
    ProviderOutcome outcome = ProviderOutcome::BadFormat;

    auto source = BoundedStreamSource::Create(&stream, Deadline{}, ledger, outcome);
    REQUIRE(source != nullptr);
    CHECK(outcome == ProviderOutcome::Success);
    CHECK(source->Seekable());
    CHECK(source->Size() == data.size());

    const auto expect = [&](std::uint64_t offset, std::size_t size) {
        std::vector<std::byte> got(size);
        INFO("offset " << offset << " size " << size);
        REQUIRE(source->ReadAt(offset, got));
        CHECK(std::memcmp(got.data(), data.data() + offset, size) == 0);
    };

    expect(0, 10);
    expect(70 * 1024, 20 * 1024); // spans a block boundary
    expect(data.size() - 3, 3);

    std::vector<std::byte> one(1);
    CHECK_FALSE(source->ReadAt(data.size(), one));
    CHECK(source->FailureCause() == ProviderOutcome::BadFormat);
}

TEST_CASE("a non-seekable source materializes the bounded backing buffer",
          "[provider][stream]")
{
    const auto data = MakeData(100 * 1024);
    MemoryStream stream(data);
    stream.SetSeekable(false);
    stream.SetStatSupported(false);
    AllocationLedger ledger;
    ProviderOutcome outcome = ProviderOutcome::BadFormat;

    auto source = BoundedStreamSource::Create(&stream, Deadline{}, ledger, outcome);
    REQUIRE(source != nullptr);
    CHECK_FALSE(source->Seekable());
    CHECK(source->Size() == 0); // unknown before the first read

    std::vector<std::byte> got(10);
    REQUIRE(source->ReadAt(50 * 1024, got));
    CHECK(std::memcmp(got.data(), data.data() + 50 * 1024, got.size()) == 0);
    CHECK(source->Size() == data.size());

    const std::span<const std::byte> view = source->ContiguousView();
    REQUIRE(view.size() == data.size());
    CHECK(std::memcmp(view.data(), data.data(), data.size()) == 0);
}

TEST_CASE("an oversized STATSTG size hint fails fast", "[provider][stream]")
{
    MemoryStream stream(MakeData(64));
    stream.SetStatSize(300 * kMiB);
    AllocationLedger ledger;
    ProviderOutcome outcome = ProviderOutcome::Success;

    CHECK(BoundedStreamSource::Create(&stream, Deadline{}, ledger, outcome) ==
          nullptr);
    CHECK(outcome == ProviderOutcome::LimitExceeded);
    CHECK(ledger.Reserved() == 0);
}

TEST_CASE("a negative STATSTG size sentinel is rejected", "[provider][stream]")
{
    // A hostile Stat may report -1 as a signed LONGLONG. Reinterpreted unsigned
    // that is UINT64_MAX; it must fail closed, never become a validated size.
    MemoryStream stream(MakeData(64));
    stream.SetSeekable(false);
    stream.SetStatSize(static_cast<std::uint64_t>(-1));
    AllocationLedger ledger;
    ProviderOutcome outcome = ProviderOutcome::Success;

    CHECK(BoundedStreamSource::Create(&stream, Deadline{}, ledger, outcome) == nullptr);
    CHECK(outcome == ProviderOutcome::LimitExceeded);
    CHECK(ledger.Reserved() == 0);
}

TEST_CASE("a negative seek-end size sentinel is rejected", "[provider][stream]")
{
    // The STATSTG preflight is skipped, so the Seek(END) probe is the only size
    // source; it too must reject the -1/unknown sentinel explicitly.
    MemoryStream stream(MakeData(64));
    stream.SetStatSupported(false);
    stream.SetSeekEndSize(static_cast<std::uint64_t>(-1));
    stream.SetSeekEndReportOnly(true);
    AllocationLedger ledger;
    ProviderOutcome outcome = ProviderOutcome::Success;

    CHECK(BoundedStreamSource::Create(&stream, Deadline{}, ledger, outcome) == nullptr);
    CHECK(outcome == ProviderOutcome::LimitExceeded);
    CHECK(ledger.Reserved() == 0);
}

TEST_CASE("a short read inside the reported extent aborts", "[provider][stream]")
{
    const auto data = MakeData(200 * 1024);
    MemoryStream stream(data);
    stream.SetStatSize(data.size());
    stream.SetSeekEndSize(data.size());
    stream.SetShortReadLimit(100 * 1024);
    AllocationLedger ledger;
    ProviderOutcome outcome = ProviderOutcome::BadFormat;

    auto source = BoundedStreamSource::Create(&stream, Deadline{}, ledger, outcome);
    REQUIRE(source != nullptr);

    std::vector<std::byte> got(150 * 1024);
    CHECK_FALSE(source->ReadAt(0, got));
    CHECK(source->FailureCause() == ProviderOutcome::BadFormat);
}

TEST_CASE("an expired deadline aborts the next read", "[provider][stream]")
{
    MemoryStream stream(MakeData(128 * 1024));
    AllocationLedger ledger;
    ProviderOutcome outcome = ProviderOutcome::BadFormat;

    auto source = BoundedStreamSource::Create(&stream, Deadline{}, ledger, outcome);
    REQUIRE(source != nullptr);

    source->MutableDeadline().Restart(Deadline::Clock::now() -
                                      std::chrono::seconds(10));
    std::vector<std::byte> got(16);
    CHECK_FALSE(source->ReadAt(0, got));
    CHECK(source->FailureCause() == ProviderOutcome::Deadline);
}

TEST_CASE("the block cache and backing are charged to the ledger",
          "[provider][stream]")
{
    const std::uint64_t cacheBytes = BoundedStreamSource::kBlockBytes *
                                     BoundedStreamSource::kBlockSlots;

    // A ledger too small for the block cache rejects construction.
    {
        AllocationLedger tiny(1024);
        MemoryStream stream(MakeData(4 * 1024));
        ProviderOutcome outcome = ProviderOutcome::Success;
        CHECK(BoundedStreamSource::Create(&stream, Deadline{}, tiny, outcome) ==
              nullptr);
        CHECK(outcome == ProviderOutcome::LimitExceeded);
        CHECK(tiny.Reserved() == 0);
    }

    // A successful source holds exactly the cache, plus the backing once the
    // adapter requests contiguity, and releases everything on destruction.
    {
        AllocationLedger ledger;
        const auto data = MakeData(200 * 1024);
        MemoryStream stream(data);
        ProviderOutcome outcome = ProviderOutcome::BadFormat;
        auto source =
            BoundedStreamSource::Create(&stream, Deadline{}, ledger, outcome);
        REQUIRE(source != nullptr);
        CHECK(ledger.Reserved() == cacheBytes);

        const std::span<const std::byte> view = source->ContiguousView();
        REQUIRE(view.size() == data.size());
        CHECK(ledger.Reserved() == cacheBytes + data.size());

        source.reset();
        CHECK(ledger.Reserved() == 0);
    }
}

TEST_CASE("a seekable source over the contiguous cap returns an empty view",
          "[provider][stream]")
{
    MemoryStream stream(std::vector<std::byte>{});
    stream.SetStatSize(129 * kMiB);
    stream.SetSeekEndSize(129 * kMiB);
    AllocationLedger ledger;
    ProviderOutcome outcome = ProviderOutcome::BadFormat;

    auto source = BoundedStreamSource::Create(&stream, Deadline{}, ledger, outcome);
    REQUIRE(source != nullptr);
    CHECK(source->Size() == 129 * kMiB);

    const std::span<const std::byte> view = source->ContiguousView();
    CHECK(view.empty());
    CHECK(source->FailureCause() == ProviderOutcome::LimitExceeded);
    CHECK(ledger.Reserved() ==
          BoundedStreamSource::kBlockBytes * BoundedStreamSource::kBlockSlots);
}

TEST_CASE("IInitializeWithStream rejects null and repeated initialization",
          "[provider][stream][com]")
{
    ProviderModule module;
    REQUIRE(module.Ready());
    REQUIRE(module.canUnloadNow() == S_OK);

    IInitializeWithStream* init = CreateInitializable(module);
    REQUIRE(init != nullptr);

    CHECK(init->Initialize(nullptr, STGM_READ) == E_POINTER);

    MemoryStream stream(MakeData(4096));
    CHECK(init->Initialize(&stream, STGM_READ) == S_OK);

    IInitializeWithStream* again = nullptr;
    CHECK(init->QueryInterface(__uuidof(IInitializeWithStream),
                               reinterpret_cast<void**>(&again)) == S_OK);
    CHECK(again == init);
    if (again != nullptr) {
        again->Release();
    }

    MemoryStream second(MakeData(8));
    CHECK(init->Initialize(&second, STGM_READ) == E_UNEXPECTED);

    init->Release();
    CHECK(module.canUnloadNow() == S_OK);
}

TEST_CASE("IInitializeWithStream maps an oversized stream to the safe fallback",
          "[provider][stream][com]")
{
    ProviderModule module;
    REQUIRE(module.Ready());

    IInitializeWithStream* init = CreateInitializable(module);
    REQUIRE(init != nullptr);

    MemoryStream stream(MakeData(16));
    stream.SetStatSize(300 * kMiB);
    CHECK(init->Initialize(&stream, STGM_READ) ==
          HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));

    init->Release();
    CHECK(module.canUnloadNow() == S_OK);
}
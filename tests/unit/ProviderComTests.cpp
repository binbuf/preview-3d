// T11 provider COM core coverage.
//
// Behavioral companion to the [provider][scaffold] build-boundary cases: it
// loads the built DLL the same way the Shell and the T17 host do
// (LoadLibrary/GetProcAddress, because the two exports are PRIVATE and there is
// no import library) and exercises the class factory and lifetime contract from
// ADR-0013:
//
//   - a class factory per frozen family CLSID, routed from the CLSID alone;
//   - unknown CLSIDs -> CLASS_E_CLASSNOTAVAILABLE (no sniffing);
//   - IUnknown/IClassFactory interface identity and E_NOINTERFACE otherwise;
//   - aggregation rejection (CLASS_E_NOAGGREGATION);
//   - object/factory/lock refcounts gating DllCanUnloadNow.
//
// Every object is released, so each test leaves DllCanUnloadNow at S_OK for the
// next one (Catch2 test order is not fixed).

#include <catch2/catch_test_macros.hpp>

#include "FamilyRouting.h"

#include <windows.h>
#include <objbase.h>
#include <unknwn.h>

#include <string>

namespace {

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

const preview3d::provider::FamilyRoute& FirstRoute()
{
    return preview3d::provider::FamilyRoutes().front();
}

GUID FirstFamilyClsid()
{
    return GuidFromText(FirstRoute().clsid);
}

// Not one of the two interfaces DllGetClassObject supports.
const GUID kUnsupportedIid = {
    0x11111111, 0x2222, 0x3333, {0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}};

const GUID kUnknownClsid = {
    0x00000000, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

// Returns a +1 class factory for the CLSID, or nullptr (the caller checks).
IClassFactory* GetFactory(ProviderModule& module, REFCLSID clsid)
{
    void* raw = nullptr;
    if (FAILED(module.getClassObject(clsid, __uuidof(IClassFactory), &raw))) {
        return nullptr;
    }
    return static_cast<IClassFactory*>(raw);
}

} // namespace

TEST_CASE("every frozen family CLSID gets a class factory routed without sniffing",
          "[provider][com]")
{
    ProviderModule module;
    REQUIRE(module.Ready());
    REQUIRE(module.canUnloadNow() == S_OK);

    for (const preview3d::provider::FamilyRoute& route :
         preview3d::provider::FamilyRoutes()) {
        const GUID clsid = GuidFromText(route.clsid);
        void* raw = nullptr;
        const HRESULT hr = module.getClassObject(clsid, __uuidof(IClassFactory), &raw);
        INFO("family CLSID " << route.clsid);
        REQUIRE(hr == S_OK);
        REQUIRE(raw != nullptr);

        auto* factory = static_cast<IClassFactory*>(raw);

        // Interface identity: IUnknown and IClassFactory resolve to the object.
        void* unknown = nullptr;
        CHECK(factory->QueryInterface(__uuidof(IUnknown), &unknown) == S_OK);
        CHECK(unknown == raw);
        if (unknown != nullptr) {
            static_cast<IUnknown*>(unknown)->Release();
        }

        // A live factory keeps the module loaded.
        CHECK(module.canUnloadNow() == S_FALSE);
        factory->Release();
    }

    CHECK(module.canUnloadNow() == S_OK);
}

TEST_CASE("unknown CLSID, unknown IID and null out-param use the frozen HRESULTs",
          "[provider][com]")
{
    ProviderModule module;
    REQUIRE(module.Ready());

    void* raw = reinterpret_cast<void*>(1);
    CHECK(module.getClassObject(kUnknownClsid, __uuidof(IClassFactory), &raw) ==
          CLASS_E_CLASSNOTAVAILABLE);
    CHECK(raw == nullptr);

    CHECK(module.getClassObject(FirstFamilyClsid(), __uuidof(IUnknown), nullptr) ==
          E_POINTER);

    void* other = reinterpret_cast<void*>(1);
    CHECK(module.getClassObject(FirstFamilyClsid(), kUnsupportedIid, &other) ==
          E_NOINTERFACE);
    CHECK(other == nullptr);

    CHECK(module.canUnloadNow() == S_OK);
}

TEST_CASE("class factory rejects aggregation and bad arguments", "[provider][com]")
{
    ProviderModule module;
    REQUIRE(module.Ready());

    IClassFactory* factory = GetFactory(module, FirstFamilyClsid());
    REQUIRE(factory != nullptr);

    void* out = reinterpret_cast<void*>(1);
    CHECK(factory->CreateInstance(static_cast<IUnknown*>(factory), __uuidof(IUnknown),
                                  &out) == CLASS_E_NOAGGREGATION);
    CHECK(out == nullptr);

    CHECK(factory->CreateInstance(nullptr, __uuidof(IUnknown), nullptr) == E_POINTER);

    factory->Release();
    CHECK(module.canUnloadNow() == S_OK);
}

TEST_CASE("created provider object has stable identity and QueryInterface",
          "[provider][com]")
{
    ProviderModule module;
    REQUIRE(module.Ready());

    IClassFactory* factory = GetFactory(module, FirstFamilyClsid());
    REQUIRE(factory != nullptr);

    IUnknown* object = nullptr;
    REQUIRE(factory->CreateInstance(nullptr, __uuidof(IUnknown),
                                    reinterpret_cast<void**>(&object)) == S_OK);
    REQUIRE(object != nullptr);

    IUnknown* again = nullptr;
    CHECK(object->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&again)) ==
          S_OK);
    CHECK(again == object);
    if (again != nullptr) {
        again->Release();
    }

    void* none = reinterpret_cast<void*>(1);
    CHECK(object->QueryInterface(kUnsupportedIid, &none) == E_NOINTERFACE);
    CHECK(none == nullptr);

    CHECK(object->QueryInterface(__uuidof(IUnknown), nullptr) == E_POINTER);

    // An unsupported riid fails CreateInstance cleanly: no object is leaked.
    void* unsupported = reinterpret_cast<void*>(1);
    CHECK(factory->CreateInstance(nullptr, kUnsupportedIid, &unsupported) ==
          E_NOINTERFACE);
    CHECK(unsupported == nullptr);
    CHECK(module.canUnloadNow() == S_FALSE);

    object->Release();
    factory->Release();
    CHECK(module.canUnloadNow() == S_OK);
}

TEST_CASE("object refcounts keep the module alive until the last Release",
          "[provider][com]")
{
    ProviderModule module;
    REQUIRE(module.Ready());

    IClassFactory* factory = GetFactory(module, FirstFamilyClsid());
    REQUIRE(factory != nullptr);

    IUnknown* object = nullptr;
    REQUIRE(factory->CreateInstance(nullptr, __uuidof(IUnknown),
                                    reinterpret_cast<void**>(&object)) == S_OK);
    REQUIRE(object != nullptr);

    CHECK(module.canUnloadNow() == S_FALSE);

    object->AddRef();
    object->Release();
    CHECK(module.canUnloadNow() == S_FALSE);

    factory->Release();
    CHECK(module.canUnloadNow() == S_FALSE);

    object->Release();
    CHECK(module.canUnloadNow() == S_OK);
}

TEST_CASE("LockServer holds the module loaded until every lock is released",
          "[provider][com]")
{
    ProviderModule module;
    REQUIRE(module.Ready());

    IClassFactory* locked = GetFactory(module, FirstFamilyClsid());
    REQUIRE(locked != nullptr);
    CHECK(locked->LockServer(TRUE) == S_OK);
    locked->Release();

    // Object count is zero; the outstanding lock alone keeps the module.
    CHECK(module.canUnloadNow() == S_FALSE);

    IClassFactory* unlock = GetFactory(module, FirstFamilyClsid());
    REQUIRE(unlock != nullptr);
    CHECK(unlock->LockServer(FALSE) == S_OK);
    unlock->Release();

    CHECK(module.canUnloadNow() == S_OK);
}
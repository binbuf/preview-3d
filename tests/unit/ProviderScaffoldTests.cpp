// T05 provider build/test scaffold coverage.
//
// These tests are the automated companion to the manual `dumpbin /dependents`
// and `dumpbin /exports` checks in the task's Done-when list, and they run as
// part of the repository's baseline `x64\Release\Tests.Unit.exe` verify command.
//
// The provider is an in-proc COM server: its only importable surface is the two
// entry points `DllGetClassObject` and `DllCanUnloadNow`, and it must not import
// the viewer, the import worker, either import host, or the OpenUSD core. The
// test loads the built DLL at runtime (the same way the Shell and the T17 host
// harness do) and inspects the PE import/export directories directly, so a
// regression in the build boundary fails here without needing dumpbin on PATH.
//
// Behavioral COM tests (identity, refcount, lock server, unload, aggregation
// rejection) live in ProviderComTests.cpp (T11); this file pins the build
// boundary the later tasks rely on.

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

constexpr char kProviderDll[] = "Preview3DThumbnailProvider.dll";

// The product binaries the DLL must never import (design/05, testing-strategy).
// Every one of them is prefixed, so a prefix rule is both stronger and simpler
// than an exact-name denylist.
constexpr char kForbiddenImportPrefix[] = "preview3d";

std::wstring ProviderDllPath()
{
    return PREVIEW3D_PROVIDER_DLL;
}

struct LoadedModule
{
    HMODULE handle = nullptr;

    LoadedModule() : handle(::LoadLibraryExW(ProviderDllPath().c_str(), nullptr, 0)) {}
    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;
    ~LoadedModule()
    {
        if (handle != nullptr) ::FreeLibrary(handle);
    }
};

const IMAGE_NT_HEADERS* NtHeaders(const std::uint8_t* base)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    REQUIRE(dos->e_magic == IMAGE_DOS_SIGNATURE);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    REQUIRE(nt->Signature == IMAGE_NT_SIGNATURE);
    return nt;
}

std::vector<std::string> ImportedModuleNames(const std::uint8_t* base)
{
    std::vector<std::string> names;
    const auto& entry = NtHeaders(base)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (entry.VirtualAddress == 0 || entry.Size == 0) return names;

    const auto* descriptor =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + entry.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor)
    {
        names.emplace_back(reinterpret_cast<const char*>(base + descriptor->Name));
    }
    return names;
}

std::vector<std::string> ExportedNames(const std::uint8_t* base)
{
    std::vector<std::string> names;
    const auto& entry = NtHeaders(base)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (entry.VirtualAddress == 0 || entry.Size == 0) return names;

    const auto* directory =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + entry.VirtualAddress);
    const auto* nameRvas = reinterpret_cast<const DWORD*>(base + directory->AddressOfNames);
    for (DWORD i = 0; i < directory->NumberOfNames; ++i)
    {
        names.emplace_back(reinterpret_cast<const char*>(base + nameRvas[i]));
    }
    return names;
}

bool HasForbiddenPrefix(const std::string& name)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.rfind(kForbiddenImportPrefix, 0) == 0;
}

const GUID kUnknownClsid = {0x00000000, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};
const GUID kUnknownIid = {0x00000000, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02}};

} // namespace

TEST_CASE("provider DLL loads and exports exactly the two COM entry points", "[provider][scaffold]")
{
    const LoadedModule module;
    REQUIRE(module.handle != nullptr);

    const auto* base = reinterpret_cast<const std::uint8_t*>(module.handle);
    std::vector<std::string> exported = ExportedNames(base);
    std::sort(exported.begin(), exported.end());

    const std::vector<std::string> expected = {"DllCanUnloadNow", "DllGetClassObject"};
    CHECK(exported == expected);

    // A self-registration export must never appear: registration is
    // installer-owned (ADR-0006/0007).
    CHECK(::GetProcAddress(module.handle, "DllRegisterServer") == nullptr);
    CHECK(::GetProcAddress(module.handle, "DllUnregisterServer") == nullptr);
}

TEST_CASE("provider DLL imports no viewer, worker, host or core binary", "[provider][scaffold]")
{
    const LoadedModule module;
    REQUIRE(module.handle != nullptr);

    const auto* base = reinterpret_cast<const std::uint8_t*>(module.handle);
    const std::vector<std::string> imports = ImportedModuleNames(base);
    REQUIRE_FALSE(imports.empty());

    for (const std::string& name : imports)
    {
        INFO("imported module: " << name);
        CHECK_FALSE(HasForbiddenPrefix(name));
    }
}

TEST_CASE("provider COM entry points return the frozen results for unknown input",
          "[provider][scaffold]")
{
    const LoadedModule module;
    REQUIRE(module.handle != nullptr);

    const auto getClassObject = reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, void**)>(
        ::GetProcAddress(module.handle, "DllGetClassObject"));
    const auto canUnloadNow =
        reinterpret_cast<HRESULT(WINAPI*)()>(::GetProcAddress(module.handle, "DllCanUnloadNow"));
    REQUIRE(getClassObject != nullptr);
    REQUIRE(canUnloadNow != nullptr);

    void* object = reinterpret_cast<void*>(1);
    CHECK(getClassObject(kUnknownClsid, kUnknownIid, &object) == CLASS_E_CLASSNOTAVAILABLE);
    CHECK(object == nullptr);
    CHECK(getClassObject(kUnknownClsid, kUnknownIid, nullptr) == E_POINTER);
    CHECK(canUnloadNow() == S_OK);
}
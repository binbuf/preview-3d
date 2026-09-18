#include <catch2/catch_test_macros.hpp>

#include "OpenUsdSpikeProtocol.h"
#include "import_broker/SandboxLauncher.h"
#include "import_broker/SharedSection.h"
#include "model_core/OpenUsdIdentifier.h"
#include "platform/AppContainerSid.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef PREVIEW3D_IMPORT_HOST_EXE
#error "PREVIEW3D_IMPORT_HOST_EXE must be defined"
#endif

TEST_CASE("USD-009 compatibility identifiers anchor locally and reject authority escapes",
          "[usd-009][openusd][resolver][fuzz-regression]")
{
    using model_core::AnchorOpenUsdIdentifier;
    using model_core::IsSafeOpenUsdIdentifier;
    CHECK(IsSafeOpenUsdIdentifier("preview3d://root.usda"));
    CHECK(IsSafeOpenUsdIdentifier("preview3d://layers/mesh.usdc"));
    CHECK_FALSE(IsSafeOpenUsdIdentifier("preview3d://"));
    CHECK_FALSE(IsSafeOpenUsdIdentifier("preview3d://layers/../outside.usda"));
    CHECK_FALSE(IsSafeOpenUsdIdentifier("preview3d://layers\\outside.usda"));
    CHECK_FALSE(IsSafeOpenUsdIdentifier("preview3d://https:outside.usda"));
    const std::string embeddedNull = std::string("preview3d://layers/") + '\0' + "outside.usda";
    CHECK_FALSE(IsSafeOpenUsdIdentifier(embeddedNull));
    CHECK_FALSE(IsSafeOpenUsdIdentifier("preview3d://layers/outside\nusda"));

    CHECK(AnchorOpenUsdIdentifier("mesh.usda", "")
          == std::optional<std::string>("preview3d://mesh.usda"));
    CHECK(AnchorOpenUsdIdentifier("mesh.usda", "preview3d://layers/root.usda")
          == std::optional<std::string>("preview3d://layers/mesh.usda"));
    CHECK(AnchorOpenUsdIdentifier("./mesh.usda", "preview3d://layers/root.usda")
          == std::optional<std::string>("preview3d://layers/mesh.usda"));
    CHECK_FALSE(AnchorOpenUsdIdentifier("../outside.usda", "preview3d://layers/root.usda"));
    CHECK_FALSE(AnchorOpenUsdIdentifier("https://example.invalid/x.usda", ""));
    CHECK_FALSE(AnchorOpenUsdIdentifier("C:/outside.usda", ""));
    CHECK_FALSE(AnchorOpenUsdIdentifier("\\\\server\\share\\x.usda", ""));
}

namespace {

using namespace compatibility_host;

constexpr std::string_view kMesh = R"(
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    uniform token subdivisionScheme = "none"
)";

std::wstring OutputDirectory()
{
    const std::filesystem::path executable(PREVIEW3D_IMPORT_HOST_EXE);
    return executable.parent_path().wstring();
}

struct HostProfile {
    std::wstring name;
    platform::AppContainerSid sid;
    std::vector<std::byte> sidCopy;

    HostProfile()
        : name(L"Binbuf.Preview3D.ImportHost.Test-" + std::to_wstring(GetCurrentProcessId()) + L"-"
               + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()))
        , sid(platform::AppContainerSid::CreateOrOpen(name, L"Preview3D Import Host Test",
                                                       L"USD-002 zero-capability compatibility host"))
    {
        REQUIRE(sid);
        sidCopy.resize(GetLengthSid(sid.get()));
        REQUIRE(CopySid(static_cast<DWORD>(sidCopy.size()), sidCopy.data(), sid.get()));
        REQUIRE(platform::GrantDirectoryReadExecute(OutputDirectory(), sid.get()));
        REQUIRE(platform::GrantDirectoryReadExecute(
            (std::filesystem::path(OutputDirectory()) / L"usd").wstring(), sid.get()));
    }

    ~HostProfile()
    {
        if (!sidCopy.empty()) platform::RevokeDirectoryAccess(
            (std::filesystem::path(OutputDirectory()) / L"usd").wstring(), sidCopy.data());
        if (!sidCopy.empty()) platform::RevokeDirectoryAccess(OutputDirectory(), sidCopy.data());
        platform::AppContainerSid::Delete(name);
    }
};

class EnvironmentOverride {
public:
    EnvironmentOverride(const wchar_t* name, const std::wstring& value) : name_(name)
    {
        const DWORD length = GetEnvironmentVariableW(name_.c_str(), nullptr, 0);
        if (length) {
            std::wstring prior(length, L'\0');
            const DWORD copied = GetEnvironmentVariableW(name_.c_str(), prior.data(), length);
            if (copied && copied < length) { prior.resize(copied); prior_ = std::move(prior); }
        }
        REQUIRE(SetEnvironmentVariableW(name_.c_str(), value.c_str()));
    }
    ~EnvironmentOverride()
    {
        SetEnvironmentVariableW(name_.c_str(), prior_ ? prior_->c_str() : nullptr);
    }
private:
    std::wstring name_;
    std::optional<std::wstring> prior_;
};

struct PackedSection {
    platform::Win32Handle section;
    platform::MappedView view;
    OpenUsdSpikeSection* header{};
    std::size_t size{};
};

PackedSection Pack(std::span<const std::pair<std::string, std::string>> assets,
                   OpenUsdSpikeMode mode = OpenUsdSpikeMode::Compose,
                   std::string_view rootIdentifier = "preview3d://root.usda")
{
    std::size_t size = sizeof(OpenUsdSpikeSection);
    for (const auto& [name, bytes] : assets) size += bytes.size();
    PackedSection packed;
    packed.section = import_broker::CreateSharedSection(size);
    REQUIRE(packed.section);
    packed.view = platform::MappedView::Map(packed.section.get(), FILE_MAP_READ | FILE_MAP_WRITE, size);
    REQUIRE(packed.view);
    packed.size = size;
    std::memset(packed.view.bytes().data(), 0, size);
    packed.header = reinterpret_cast<OpenUsdSpikeSection*>(packed.view.bytes().data());
    *packed.header = OpenUsdSpikeSection{};
    packed.header->sectionByteLength = size;
    packed.header->entryCount = static_cast<std::uint32_t>(assets.size());
    packed.header->mode = mode;
    strcpy_s(packed.header->rootIdentifier, rootIdentifier.data());
    std::size_t offset = sizeof(OpenUsdSpikeSection);
    for (std::size_t index = 0; index < assets.size(); ++index) {
        const auto& [name, bytes] = assets[index];
        strcpy_s(packed.header->entries[index].identifier, name.c_str());
        packed.header->entries[index].byteOffset = offset;
        packed.header->entries[index].byteLength = bytes.size();
        std::memcpy(packed.view.bytes().data() + offset, bytes.data(), bytes.size());
        offset += bytes.size();
    }
    return packed;
}

std::string ReadFixture(const char* name)
{
    const auto path = std::filesystem::path(PREVIEW3D_USD_FIXTURES_DIR) / name;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    REQUIRE(input);
    const auto length = input.tellg();
    REQUIRE(length >= 0);
    std::string bytes(static_cast<std::size_t>(length), '\0');
    input.seekg(0);
    if (!bytes.empty()) input.read(bytes.data(), length);
    REQUIRE((input.good() || input.eof()));
    return bytes;
}

std::string DecodeBase64Fixture(const char* name)
{
    const auto encoded = ReadFixture(name);
    std::string decoded;
    std::uint32_t accumulator = 0;
    unsigned bits = 0;
    for (const unsigned char character : encoded) {
        if (std::isspace(character)) continue;
        if (character == '=') break;
        std::uint32_t value = 0;
        if (character >= 'A' && character <= 'Z') value = character - 'A';
        else if (character >= 'a' && character <= 'z') value = character - 'a' + 26;
        else if (character >= '0' && character <= '9') value = character - '0' + 52;
        else if (character == '+') value = 62;
        else if (character == '/') value = 63;
        else FAIL("invalid base64 fixture byte");
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((accumulator >> bits) & 0xffu));
        }
    }
    return decoded;
}

struct Launch {
    import_broker::SandboxProcess process;
    platform::Win32Handle request;
    platform::Win32Handle response;
};

Launch Start(const HostProfile& profile, const PackedSection& packed, std::size_t memoryLimit = 0)
{
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE inReadRaw{}, inWriteRaw{}, outReadRaw{}, outWriteRaw{};
    REQUIRE(CreatePipe(&inReadRaw, &inWriteRaw, &attributes, 0));
    REQUIRE(CreatePipe(&outReadRaw, &outWriteRaw, &attributes, 0));
    platform::Win32Handle inRead(inReadRaw), inWrite(inWriteRaw), outRead(outReadRaw), outWrite(outWriteRaw);
    REQUIRE(SetHandleInformation(inWrite.get(), HANDLE_FLAG_INHERIT, 0));
    REQUIRE(SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0));
    std::vector<HANDLE> inherited{inRead.get(), outWrite.get(), packed.section.get()};
    const std::wstring executable(PREVIEW3D_IMPORT_HOST_EXE);
    const std::wstring arguments = L"\"" + executable + L"\" --usd-002-spike "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(packed.section.get())) + L" "
        + std::to_wstring(packed.size);
    import_broker::SandboxLimits limits{};
    if (memoryLimit == 0) {
        MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
        REQUIRE(GlobalMemoryStatusEx(&memory));
        memoryLimit = static_cast<std::size_t>((std::min)(4ull * 1024 * 1024 * 1024,
                                                          memory.ullTotalPhys * 35 / 100));
    }
    limits.processMemoryLimitBytes = memoryLimit;
    auto process = import_broker::LaunchSuspendedSandboxed(executable, arguments, inherited,
        outWrite.get(), limits, profile.sid, inRead.get());
    REQUIRE(process);
    REQUIRE(import_broker::ResumeSandboxProcess(*process));
    inRead.reset(); outWrite.reset();
    OpenUsdSpikeControl control{};
    DWORD written = 0;
    REQUIRE(WriteFile(inWrite.get(), &control, sizeof(control), &written, nullptr));
    REQUIRE(written == sizeof(control));
    return {std::move(*process), std::move(inWrite), std::move(outRead)};
}

bool WaitForReply(Launch& launch, DWORD timeoutMilliseconds)
{
    const auto deadline = GetTickCount64() + timeoutMilliseconds;
    while (GetTickCount64() < deadline) {
        DWORD available = 0;
        if (PeekNamedPipe(launch.response.get(), nullptr, 0, nullptr, &available, nullptr)
            && available >= sizeof(OpenUsdSpikeControl)) {
            OpenUsdSpikeControl response{}; DWORD read = 0;
            return ReadFile(launch.response.get(), &response, sizeof(response), &read, nullptr)
                && read == sizeof(response) && response.magic == kOpenUsdSpikeMagic;
        }
        if (WaitForSingleObject(launch.process.process.get(), 0) == WAIT_OBJECT_0) return false;
        Sleep(10);
    }
    return false;
}

void CaptureDiagnostics(const OpenUsdSpikeSection& header)
{
    UNSCOPED_INFO("status=" << static_cast<std::uint32_t>(header.status)
        << " plugins=" << header.registeredPluginCount
        << " external=" << header.externalPluginCount
        << " opens=" << header.resolverOpenCount
        << " denied=" << header.resolverDeniedCount
        << " diagnostic=" << header.diagnostic);
}

std::vector<std::pair<std::string, std::string>> ComposedAssets()
{
    const std::string root = std::string(R"(#usda 1.0
(
    defaultPrim = "Root"
    metersPerUnit = 0.01
    upAxis = "Z"
    subLayers = [@sub.usda@]
)
def Xform "Root" {
    double3 xformOp:translate = (2, 3, 4)
    uniform token[] xformOpOrder = ["xformOp:translate"]
    def Mesh "Local" {
)") + std::string(kMesh) + R"(    }
    def Xform "Referenced" (prepend references = @ref.usda@) {}
    def Xform "PayloadSite" (prepend payload = @payload.usda@) {}
    def Material "Mat" {
        def Shader "Shader" {
            uniform token info:id = "UsdPreviewSurface"
            asset inputs:file = @texture.bin@
            token outputs:surface
        }
    }
}
)";
    const std::string sub = std::string("#usda 1.0\ndef Xform \"Root\" {\n    def Mesh \"Sub\" {\n")
        + std::string(kMesh) + "    }\n}\n";
    const std::string reference = std::string("#usda 1.0\n(defaultPrim = \"Ref\")\ndef Xform \"Ref\" {\n    def Mesh \"Mesh\" {\n")
        + std::string(kMesh) + "    }\n}\n";
    const std::string payload = std::string("#usda 1.0\n(defaultPrim = \"Payload\")\ndef Xform \"Payload\" {\n    def Mesh \"Mesh\" {\n")
        + std::string(kMesh) + "    }\n}\n";
    return {{"preview3d://root.usda", root}, {"preview3d://sub.usda", sub},
            {"preview3d://ref.usda", reference}, {"preview3d://payload.usda", payload},
            {"preview3d://texture.bin", "texture-bytes"}};
}

} // namespace

TEST_CASE("USD-002 OpenUSD composes only brokered assets in the compatibility sandbox",
          "[usd-002][openusd][sandbox][resolver]")
{
    HostProfile profile;
    EnvironmentOverride pluginPath(L"PXR_PLUGINPATH_NAME", OutputDirectory());
    EnvironmentOverride renamedPluginPath(L"PREVIEW3D_DISABLED_PLUGIN_PATH", OutputDirectory());
    auto assets = ComposedAssets();
    auto packed = Pack(assets);
    auto launch = Start(profile, packed);
    REQUIRE(WaitForReply(launch, 30'000));
    CaptureDiagnostics(*packed.header);
    INFO(packed.header->diagnostic);
    CHECK(packed.header->status == OpenUsdSpikeStatus::Success);
    CHECK(packed.header->state == 3);
    CHECK(packed.header->payloadCount == 1);
    CHECK(packed.header->meshCount == 4);
    CHECK(packed.header->pointCount == 12);
    CHECK(packed.header->faceCount == 4);
    CHECK(packed.header->textureAssetCount == 1);
    CHECK(packed.header->resolverOpenCount >= 5);
    CHECK(packed.header->resolverDeniedCount == 0);
    CHECK(packed.header->externalPluginCount == 0);
    CHECK(packed.header->upAxis == 'Z');
    CHECK(packed.header->metersPerUnit == 0.01);
    CHECK(packed.header->semanticDigest != 0);
    CHECK(packed.header->worldBounds[0] == 2.0);
    CHECK(packed.header->worldBounds[1] == 3.0);
    CHECK(packed.header->worldBounds[3] == 3.0);
    CHECK(packed.header->worldBounds[4] == 4.0);
    std::cout << "USD-002 compose startup-us=" << packed.header->startupMicroseconds
              << " load-none-us=" << packed.header->loadNoneMicroseconds
              << " first-geometry-us=" << packed.header->firstGeometryMicroseconds
              << " total-us=" << packed.header->totalMicroseconds
              << " private-bytes=" << packed.header->privateBytes
              << " peak-working-set=" << packed.header->peakWorkingSetBytes
              << " digest=0x" << std::hex << packed.header->semanticDigest << std::dec << '\n';
}

TEST_CASE("USD-002 OpenUSD reads USDZ and the common USDA semantic overlap from broker bytes",
          "[usd-002][openusd][usdz][equivalence]")
{
    HostProfile profile;
    SECTION("USDZ package root") {
        const std::vector<std::pair<std::string, std::string>> assets{
            {"preview3d://cube.usdz", DecodeBase64Fixture("cube.usdz.base64")}};
        auto packed = Pack(assets, OpenUsdSpikeMode::Compose, "preview3d://cube.usdz");
        auto launch = Start(profile, packed);
        REQUIRE(WaitForReply(launch, 30'000));
        CaptureDiagnostics(*packed.header);
        CHECK(packed.header->status == OpenUsdSpikeStatus::Success);
        CHECK(packed.header->meshCount >= 1);
        CHECK(packed.header->resolverOpenCount >= 1);
    }
    SECTION("TinyUSDZ and OpenUSD common USDA facts") {
        const std::vector<std::pair<std::string, std::string>> assets{
            {"preview3d://mesh.usda", ReadFixture("mesh.usda")}};
        auto packed = Pack(assets, OpenUsdSpikeMode::Compose, "preview3d://mesh.usda");
        auto launch = Start(profile, packed);
        REQUIRE(WaitForReply(launch, 30'000));
        CHECK(packed.header->status == OpenUsdSpikeStatus::Success);
        CHECK(packed.header->meshCount == 1);
        CHECK(packed.header->pointCount == 4);
        CHECK(packed.header->faceCount == 1);
        CHECK(packed.header->upAxis == 'Z');
        CHECK(packed.header->metersPerUnit == 0.01);
        CHECK(packed.header->worldBounds[0] == 1.0);
        CHECK(packed.header->worldBounds[1] == 2.0);
        CHECK(packed.header->worldBounds[2] == 4.0);
        CHECK(packed.header->worldBounds[3] == 3.0);
        CHECK(packed.header->worldBounds[4] == 4.0);
        CHECK(packed.header->worldBounds[5] == 4.0);
    }
}

TEST_CASE("USD-002 rejects invalid sections and bounded payload exhaustion",
          "[usd-002][openusd][negative][limits]")
{
    HostProfile profile;
    SECTION("invalid shared output contract") {
        const std::vector<std::pair<std::string, std::string>> assets{
            {"preview3d://root.usda", "#usda 1.0\ndef Mesh \"M\" {}\n"}};
        auto packed = Pack(assets);
        packed.header->entries[0].byteOffset = packed.size + 1;
        auto launch = Start(profile, packed);
        REQUIRE(WaitForReply(launch, 30'000));
        CHECK(packed.header->status == OpenUsdSpikeStatus::InvalidRequest);
        CHECK(packed.header->state == 1);
    }
    SECTION("payload budget") {
        auto assets = ComposedAssets();
        auto packed = Pack(assets);
        packed.header->maxPayloads = 0;
        auto launch = Start(profile, packed);
        REQUIRE(WaitForReply(launch, 30'000));
        CHECK(packed.header->status == OpenUsdSpikeStatus::ResourceLimit);
        CHECK(packed.header->state == 2);
    }
}

TEST_CASE("USD-002 resolver denies unapproved composition without filesystem fallback",
          "[usd-002][openusd][resolver][negative]")
{
    HostProfile profile;
    const std::vector<std::pair<std::string, std::string>> assets{{"preview3d://root.usda",
        "#usda 1.0\ndef Xform \"Root\" (references = @missing.usda@</Missing>) {}\n"}};
    auto packed = Pack(assets);
    auto launch = Start(profile, packed);
    REQUIRE(WaitForReply(launch, 30'000));
    CaptureDiagnostics(*packed.header);
    CHECK(packed.header->status == OpenUsdSpikeStatus::ResolverDenied);
    CHECK(packed.header->resolverDeniedCount > 0);
}

TEST_CASE("USD-002 host crash hang and Job limit are generation-local and restartable",
          "[usd-002][openusd][recovery][limits]")
{
    HostProfile profile;
    const std::vector<std::pair<std::string, std::string>> minimal{{"preview3d://root.usda",
        std::string("#usda 1.0\ndef Mesh \"M\" {\n") + std::string(kMesh) + "}\n"}};
    for (const auto mode : {OpenUsdSpikeMode::Crash, OpenUsdSpikeMode::Hang,
                            OpenUsdSpikeMode::ConsumeMemory}) {
        auto packed = Pack(minimal, mode);
        auto launch = Start(profile, packed, mode == OpenUsdSpikeMode::ConsumeMemory ? 64ull * 1024 * 1024 : 0);
        Sleep(150);
        CHECK(packed.header->state == 1);
        CHECK(TerminateJobObject(launch.process.job.get(), 99));
        CHECK(WaitForSingleObject(launch.process.process.get(), 5'000) == WAIT_OBJECT_0);
    }
    auto recovered = Pack(minimal);
    auto launch = Start(profile, recovered);
    REQUIRE(WaitForReply(launch, 30'000));
    CaptureDiagnostics(*recovered.header);
    CHECK(recovered.header->status == OpenUsdSpikeStatus::Success);
    CHECK(recovered.header->meshCount == 1);
}

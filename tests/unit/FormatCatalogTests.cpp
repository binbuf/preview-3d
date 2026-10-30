// T02 format catalog and content-router regression coverage.
//
// The catalog is compiled into this binary directly (header-only table plus the
// shipped FormatSniffer.cpp), so these cases exercise exactly the data and
// bounded probe the trusted process uses. They deliberately do not link a
// parser: the sniffer is a candidate selector, never a validation claim.

#include "model_core/FormatCatalog.h"
#include "model_core/FormatSniffer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace model_core;

namespace {

static_assert(static_cast<uint32_t>(FormatId::Gltf) == static_cast<uint32_t>(SourceFormatId::Gltf));
static_assert(static_cast<uint32_t>(FormatId::Stl) == static_cast<uint32_t>(SourceFormatId::Stl));
static_assert(static_cast<uint32_t>(FormatId::Ply) == static_cast<uint32_t>(SourceFormatId::Ply));
static_assert(static_cast<uint32_t>(FormatId::Glb) == static_cast<uint32_t>(SourceFormatId::Glb));
static_assert(static_cast<uint32_t>(FormatId::AsciiStl) == static_cast<uint32_t>(SourceFormatId::AsciiStl));
static_assert(static_cast<uint32_t>(FormatId::AsciiPly) == static_cast<uint32_t>(SourceFormatId::AsciiPly));
static_assert(static_cast<uint32_t>(FormatId::Obj) == static_cast<uint32_t>(SourceFormatId::Obj));
static_assert(static_cast<uint32_t>(FormatId::Fbx) == static_cast<uint32_t>(SourceFormatId::Fbx));
static_assert(static_cast<uint32_t>(FormatId::Usda) == static_cast<uint32_t>(SourceFormatId::Usda));
static_assert(static_cast<uint32_t>(FormatId::Usdc) == static_cast<uint32_t>(SourceFormatId::Usdc));
static_assert(static_cast<uint32_t>(FormatId::Usdz) == static_cast<uint32_t>(SourceFormatId::Usdz));
static_assert(static_cast<uint32_t>(FormatId::ThreeMf) == static_cast<uint32_t>(SourceFormatId::ThreeMf));
static_assert(static_cast<uint32_t>(FormatId::Step) == static_cast<uint32_t>(SourceFormatId::Step));

std::string Narrow(std::wstring_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        result.push_back(character >= 0 && character < 128
            ? static_cast<char>(character) : '?');
    }
    return result;
}

std::vector<std::byte> ToBytes(std::string_view text)
{
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

std::vector<std::byte> MakeGlb(uint32_t version, uint32_t declaredLength, std::size_t actualSize = 12)
{
    std::vector<std::byte> bytes(actualSize);
    if (actualSize >= 4) std::memcpy(bytes.data(), "glTF", 4);
    if (actualSize >= 8) {
        for (std::size_t index = 0; index < 4; ++index) {
            bytes[4 + index] = static_cast<std::byte>((version >> (8 * index)) & 0xFF);
        }
    }
    if (actualSize >= 12) {
        for (std::size_t index = 0; index < 4; ++index) {
            bytes[8 + index] = static_cast<std::byte>((declaredLength >> (8 * index)) & 0xFF);
        }
    }
    return bytes;
}

std::vector<std::byte> MakeBinaryStl(uint32_t facets)
{
    std::vector<std::byte> bytes(84 + static_cast<std::size_t>(facets) * 50);
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[80 + index] = static_cast<std::byte>((facets >> (8 * index)) & 0xFF);
    }
    return bytes;
}

void AppendLe16(std::vector<std::byte>& bytes, uint16_t value)
{
    for (std::size_t index = 0; index < 2; ++index) {
        bytes.push_back(static_cast<std::byte>((value >> (8 * index)) & 0xFF));
    }
}

void AppendLe32(std::vector<std::byte>& bytes, uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<std::byte>((value >> (8 * index)) & 0xFF));
    }
}

void AppendAscii(std::vector<std::byte>& bytes, std::string_view text)
{
    for (const char value : text) bytes.push_back(static_cast<std::byte>(value));
}

// Minimal stored ZIP with the given entry names and no payload. The sniffer
// reads only the End-Of-Central-Directory record and the central directory, so
// this is enough to prove the bounded package probe.
std::vector<std::byte> MakeZip(const std::vector<std::string_view>& names)
{
    struct Entry {
        std::string_view name;
        uint32_t localOffset = 0;
    };
    std::vector<Entry> entries;
    std::vector<std::byte> bytes;

    for (const auto name : names) {
        Entry entry;
        entry.name = name;
        entry.localOffset = static_cast<uint32_t>(bytes.size());
        entries.push_back(entry);
        AppendLe32(bytes, 0x04034b50);
        AppendLe16(bytes, 20); // version needed
        AppendLe16(bytes, 0);  // flags
        AppendLe16(bytes, 0);  // stored
        AppendLe16(bytes, 0);  // mod time
        AppendLe16(bytes, 0);  // mod date
        AppendLe32(bytes, 0);  // crc
        AppendLe32(bytes, 0);  // compressed size
        AppendLe32(bytes, 0);  // uncompressed size
        AppendLe16(bytes, static_cast<uint16_t>(name.size()));
        AppendLe16(bytes, 0);  // extra
        AppendAscii(bytes, name);
    }

    const uint32_t centralOffset = static_cast<uint32_t>(bytes.size());
    for (const auto& entry : entries) {
        AppendLe32(bytes, 0x02014b50);
        AppendLe16(bytes, 20); // version made by
        AppendLe16(bytes, 20); // version needed
        AppendLe16(bytes, 0);  // flags
        AppendLe16(bytes, 0);  // stored
        AppendLe16(bytes, 0);  // mod time
        AppendLe16(bytes, 0);  // mod date
        AppendLe32(bytes, 0);  // crc
        AppendLe32(bytes, 0);  // compressed size
        AppendLe32(bytes, 0);  // uncompressed size
        AppendLe16(bytes, static_cast<uint16_t>(entry.name.size()));
        AppendLe16(bytes, 0);  // extra
        AppendLe16(bytes, 0);  // comment
        AppendLe16(bytes, 0);  // disk start
        AppendLe16(bytes, 0);  // internal attributes
        AppendLe32(bytes, 0);  // external attributes
        AppendLe32(bytes, entry.localOffset);
        AppendAscii(bytes, entry.name);
    }
    const uint32_t centralBytes = static_cast<uint32_t>(bytes.size()) - centralOffset;

    AppendLe32(bytes, 0x06054b50);
    AppendLe16(bytes, 0); // disk
    AppendLe16(bytes, 0); // central disk
    AppendLe16(bytes, static_cast<uint16_t>(entries.size()));
    AppendLe16(bytes, static_cast<uint16_t>(entries.size()));
    AppendLe32(bytes, centralBytes);
    AppendLe32(bytes, centralOffset);
    AppendLe16(bytes, 0); // comment
    return bytes;
}

class TempFile {
public:
    explicit TempFile(std::string_view label, std::span<const std::byte> bytes)
    {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string name = "preview3d-format-catalog-" + std::to_string(stamp) + "-"
            + std::to_string(counter.fetch_add(1)) + "-" + std::string(label);
        path_ = std::filesystem::temp_directory_path() / name;
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }

    ~TempFile()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    std::wstring WidePath() const { return path_.wstring(); }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("format catalog descriptors are complete", "[format-catalog]")
{
    REQUIRE(Formats().size() >= 14);
    for (const auto& descriptor : Formats()) {
        INFO("descriptor " << descriptor.stableName);
        CHECK(descriptor.id != FormatId::Unknown);
        CHECK(descriptor.family != FormatId::Unknown);
        CHECK_FALSE(descriptor.stableName.empty());
        REQUIRE(descriptor.displayNameKey != nullptr);
        REQUIRE(descriptor.displayName != nullptr);
        CHECK(descriptor.displayNameKey[0] != '\0');
        CHECK(descriptor.displayName[0] != L'\0');
        CHECK(descriptor.importer != ImporterKind::Unknown);
        CHECK(FindById(descriptor.id) == &descriptor);
        CHECK(FindByStableName(descriptor.stableName) == &descriptor);
        for (const auto extension : descriptor.extensions) {
            CHECK_FALSE(extension.empty());
            CHECK(extension.front() != L'.');
            for (const wchar_t value : extension) {
                CHECK(((value >= L'a' && value <= L'z') || (value >= L'0' && value <= L'9')));
            }
        }
    }
}

TEST_CASE("format catalog extensions are unique and resolve", "[format-catalog]")
{
    std::set<std::wstring> seen;
    for (const auto& descriptor : Formats()) {
        for (const auto extension : descriptor.extensions) {
            INFO("extension " << Narrow(extension));
            CHECK(seen.insert(std::wstring(extension)).second);
            CHECK(FindByExtension(extension) == &descriptor);
            CHECK(FindByExtension(std::wstring(L".") + std::wstring(extension)) == &descriptor);
        }
    }

    const std::array<std::wstring_view, 16> required{
        L"gltf", L"glb", L"stl", L"ply", L"obj", L"fbx", L"3mf",
        L"usd", L"usda", L"usdc", L"usdz", L"step", L"stp",
        L"igs", L"iges", L"brep",
    };
    for (const auto extension : required) {
        INFO("required extension " << Narrow(extension));
        REQUIRE(FindByExtension(extension) != nullptr);
    }

    CHECK(FindByExtension(L"GLB") != nullptr);
    CHECK(FindByExtension(L"3MF") != nullptr);
    CHECK(FindByExtension(L"") == nullptr);
    CHECK(FindByExtension(L"bin") == nullptr);
}

TEST_CASE("every source format id has a catalog descriptor", "[format-catalog]")
{
    std::set<uint32_t> seenWireIds;
    for (uint32_t value = 1; value <= static_cast<uint32_t>(SourceFormatId::Brep); ++value) {
        const auto wire = static_cast<SourceFormatId>(value);
        INFO("wire id " << value);
        const auto* descriptor = FindBySourceFormatId(wire);
        REQUIRE(descriptor != nullptr);
        CHECK(seenWireIds.insert(value).second);
    }
    CHECK(FindBySourceFormatId(SourceFormatId::Unknown) == nullptr);
}

TEST_CASE("T08 IGES and BREP are catalog-backed CAD host formats", "[format-catalog][cad]")
{
    static_assert(static_cast<uint32_t>(SourceFormatId::Iges) == 14);
    static_assert(static_cast<uint32_t>(SourceFormatId::Brep) == 15);

    const auto* iges = FindById(FormatId::Iges);
    REQUIRE(iges != nullptr);
    CHECK(iges->importer == ImporterKind::CadHost);
    CHECK(iges->wireFormatId == SourceFormatId::Iges);
    CHECK(iges->defaultUpAxis == UpAxisId::Unknown);
    CHECK(iges->presentation == PresentationProfile::StudioCad);
    CHECK(HasCapability(iges->capabilities, CapabilityFlags::CadHost));
    CHECK(FindByExtension(L"igs") == iges);
    CHECK(FindByExtension(L".IGES") == iges);

    const auto* brep = FindById(FormatId::Brep);
    REQUIRE(brep != nullptr);
    CHECK(brep->importer == ImporterKind::CadHost);
    CHECK(brep->wireFormatId == SourceFormatId::Brep);
    CHECK(brep->defaultUpAxis == UpAxisId::Unknown);
    CHECK(FindByExtension(L"brep") == brep);
    // The provider's OCCT closure stays STEP-only.
    CHECK(brep->thumbnail == ThumbnailFamily::None);
}

TEST_CASE("content sniff identifies a GLB renamed to .bin", "[format-catalog]")
{
    const auto glb = MakeGlb(2, 12);
    TempFile file("model.bin", glb);

    const auto sniffed = SniffFile(file.WidePath());
    REQUIRE(sniffed.format == FormatId::Glb);
    CHECK(sniffed.confidence == SniffConfidence::Strong);

    const auto classification = ClassifyFormat(L"bin", sniffed);
    REQUIRE(classification.content != nullptr);
    CHECK(classification.content->id == FormatId::Glb);
    REQUIRE(classification.selected != nullptr);
    CHECK(classification.selected->id == FormatId::Glb);
    CHECK(classification.reason == ClassificationReason::Content);
    CHECK_FALSE(classification.HasExtension());
}

TEST_CASE("malformed GLB is not content-identified", "[format-catalog]")
{
    SECTION("wrong version") {
        TempFile file("bad-version.glb", MakeGlb(1, 12));
        CHECK(SniffFile(file.WidePath()).format == FormatId::Unknown);
    }
    SECTION("declared length exceeds the file") {
        TempFile file("truncated.glb", MakeGlb(2, 4096));
        CHECK(SniffFile(file.WidePath()).format == FormatId::Unknown);
    }
    SECTION("header truncated below 12 bytes") {
        TempFile file("short.glb", MakeGlb(2, 12, 8));
        CHECK(SniffFile(file.WidePath()).format == FormatId::Unknown);
    }
}

TEST_CASE("content sniff identifies the current format signatures", "[format-catalog]")
{
    const auto check = [](std::string_view label, std::vector<std::byte> bytes, FormatId expected) {
        INFO("signature " << std::string(label));
        const auto result = SniffHead(bytes, bytes.size());
        CHECK(result.format == expected);
    };

    check("ply", ToBytes("ply\nformat ascii 1.0\nelement vertex 0\nend_header\n"), FormatId::Ply);
    check("stl-ascii",
        ToBytes("solid sample\n facet normal 0 0 1\n  outer loop\n  endloop\n endfacet\nendsolid sample\n"),
        FormatId::Stl);
    check("stl-binary", MakeBinaryStl(1), FormatId::Stl);
    check("obj", ToBytes("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"), FormatId::Obj);
    check("fbx-binary", ToBytes("Kaydara FBX Binary  \x00\x1a\x00"), FormatId::Fbx);
    check("fbx-ascii", ToBytes("; FBX 7.4.0 project file\n"), FormatId::Fbx);
    check("usda", ToBytes("#usda 1.0\n"), FormatId::Usda);
    check("usdc", ToBytes("PXR-USDC"), FormatId::Usdc);
    check("step", ToBytes("ISO-10303-21;\nHEADER;\n"), FormatId::Step);
    check("gltf", ToBytes("{\"asset\":{\"version\":\"2.0\"}}"), FormatId::Gltf);
    check("unknown", ToBytes("hello world\n"), FormatId::Unknown);
}

TEST_CASE("ZIP central-directory probe distinguishes 3MF and USDZ", "[format-catalog]")
{
    SECTION("3MF content types") {
        TempFile file("model.3mf", MakeZip({ "[Content_Types].xml", "3D/3dmodel.model" }));
        const auto result = SniffFile(file.WidePath());
        CHECK(result.format == FormatId::ThreeMf);
        CHECK(result.confidence == SniffConfidence::Strong);
    }
    SECTION("USDZ layer entry") {
        TempFile file("model.usdz", MakeZip({ "model.usdc" }));
        CHECK(SniffFile(file.WidePath()).format == FormatId::Usdz);
    }
    SECTION("unrelated ZIP") {
        TempFile file("archive.zip", MakeZip({ "readme.txt" }));
        CHECK(SniffFile(file.WidePath()).format == FormatId::Unknown);
    }
    SECTION("head-only probe cannot guess a package") {
        const auto zip = MakeZip({ "[Content_Types].xml" });
        CHECK(SniffHead(zip, zip.size()).format == FormatId::Unknown);
    }
}

TEST_CASE("classification reports extension content both and mismatch", "[format-catalog]")
{
    const auto glbBytes = MakeGlb(2, 12);
    const auto glb = SniffHead(glbBytes, glbBytes.size());
    REQUIRE(glb.format == FormatId::Glb);

    SECTION("matching extension and content") {
        const auto result = ClassifyFormat(L"glb", glb);
        CHECK(result.reason == ClassificationReason::Both);
        REQUIRE(result.selected != nullptr);
        CHECK(result.selected->id == FormatId::Glb);
    }
    SECTION("same family gltf extension with GLB content") {
        const auto result = ClassifyFormat(L"gltf", glb);
        CHECK(result.reason == ClassificationReason::Both);
        REQUIRE(result.selected != nullptr);
        CHECK(result.selected->id == FormatId::Glb);
    }
    SECTION("content only") {
        const auto result = ClassifyFormat(L"bin", glb);
        CHECK(result.reason == ClassificationReason::Content);
        REQUIRE(result.selected != nullptr);
        CHECK(result.selected->id == FormatId::Glb);
    }
    SECTION("cross-family mismatch prefers content") {
        const auto result = ClassifyFormat(L"obj", glb);
        CHECK(result.reason == ClassificationReason::Mismatch);
        REQUIRE(result.extension != nullptr);
        CHECK(result.extension->id == FormatId::Obj);
        REQUIRE(result.selected != nullptr);
        CHECK(result.selected->id == FormatId::Glb);
    }
    SECTION("extension only") {
        const auto result = ClassifyFormat(L"stl", SniffResult{});
        CHECK(result.reason == ClassificationReason::Extension);
        REQUIRE(result.selected != nullptr);
        CHECK(result.selected->id == FormatId::Stl);
    }
    SECTION("neither") {
        const auto result = ClassifyFormat(L"bin", SniffResult{});
        CHECK(result.reason == ClassificationReason::None);
        CHECK(result.selected == nullptr);
    }
    SECTION("generic usd extension with concrete usdc content") {
        const auto result = ClassifyFormat(L"usd", SniffResult{ FormatId::Usdc, SniffConfidence::Strong, {} });
        CHECK(result.reason == ClassificationReason::Both);
        REQUIRE(result.selected != nullptr);
        CHECK(result.selected->id == FormatId::Usdc);
    }
}

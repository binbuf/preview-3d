// T02 format catalog and content-router regression coverage.
//
// The catalog is compiled into this binary directly (header-only table plus the
// shipped FormatSniffer.cpp), so these cases exercise exactly the data and
// bounded probe the trusted process uses. They deliberately do not link a
// parser: the sniffer is a candidate selector, never a validation claim.

#include "model_core/FormatCatalog.h"
#include "model_core/FormatSelection.h"
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
#include <tuple>
#include <utility>
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

// A truncated binary STL: an 84-byte header whose declared facet count does
// not fit the file.
std::vector<std::byte> MakeBinaryStlHeader(uint32_t facets)
{
    std::vector<std::byte> bytes(84);
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[80 + index] = static_cast<std::byte>((facets >> (8 * index)) & 0xFF);
    }
    return bytes;
}

std::vector<std::byte> MakeThreeDs(uint32_t declaredLength, std::size_t actualSize)
{
    std::vector<std::byte> bytes(actualSize);
    if (actualSize >= 2) {
        bytes[0] = static_cast<std::byte>(0x4D);
        bytes[1] = static_cast<std::byte>(0x4D);
    }
    if (actualSize >= 6) {
        for (std::size_t index = 0; index < 4; ++index) {
            bytes[2 + index] = static_cast<std::byte>(
                (declaredLength >> (8 * index)) & 0xFF);
        }
    }
    return bytes;
}

std::vector<std::byte> MakeXbf(uint64_t version)
{
    std::vector<std::byte> bytes(15);
    std::memcpy(bytes.data(), "BINFILE", 7);
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[7 + index] = static_cast<std::byte>((version >> (8 * index)) & 0xFF);
    }
    return bytes;
}

std::vector<std::byte> MakeIgesRecord(char section)
{
    std::vector<std::byte> bytes(81, static_cast<std::byte>(' '));
    bytes[72] = static_cast<std::byte>(section);
    constexpr std::string_view sequence = "0000001";
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        bytes[73 + index] = static_cast<std::byte>(sequence[index]);
    }
    bytes[80] = static_cast<std::byte>('\n');
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

    const std::array<std::wstring_view, 21> required{
        L"gltf", L"glb", L"stl", L"ply", L"obj", L"fbx", L"3mf",
        L"usd", L"usda", L"usdc", L"usdz", L"step", L"stp",
        L"stpnc", L"p21", L"210",
        L"igs", L"iges", L"brep", L"xbf", L"fcstd",
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
    for (uint32_t value = 1; value <= static_cast<uint32_t>(SourceFormatId::X3d); ++value) {
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

TEST_CASE("T09 XBF and FCStd are catalog-backed CAD host formats", "[format-catalog][cad]")
{
    static_assert(static_cast<uint32_t>(SourceFormatId::Xbf) == 16);
    static_assert(static_cast<uint32_t>(SourceFormatId::Fcstd) == 17);

    const auto* xbf = FindById(FormatId::Xbf);
    REQUIRE(xbf != nullptr);
    CHECK(xbf->importer == ImporterKind::CadHost);
    CHECK(xbf->wireFormatId == SourceFormatId::Xbf);
    CHECK(xbf->defaultUpAxis == UpAxisId::Unknown);
    CHECK(xbf->presentation == PresentationProfile::StudioCad);
    CHECK(HasCapability(xbf->capabilities, CapabilityFlags::CadHost));
    CHECK(xbf->thumbnail == ThumbnailFamily::None);
    CHECK(FindByExtension(L"xbf") == xbf);
    CHECK(FindByExtension(L".XBF") == xbf);

    const auto* fcstd = FindById(FormatId::Fcstd);
    REQUIRE(fcstd != nullptr);
    CHECK(fcstd->importer == ImporterKind::CadHost);
    CHECK(fcstd->wireFormatId == SourceFormatId::Fcstd);
    CHECK(fcstd->defaultUpAxis == UpAxisId::Unknown);
    CHECK(fcstd->presentation == PresentationProfile::StudioCad);
    CHECK(HasCapability(fcstd->capabilities, CapabilityFlags::Archive));
    CHECK(fcstd->thumbnail == ThumbnailFamily::None);
    CHECK(FindByExtension(L"fcstd") == fcstd);
    CHECK(FindByExtension(L".FCStd") == fcstd);
}

TEST_CASE("T10 Assimp compatibility formats are catalog-backed worker importers",
          "[format-catalog][assimp]")
{
    static_assert(static_cast<uint32_t>(SourceFormatId::Dae) == 18);
    static_assert(static_cast<uint32_t>(SourceFormatId::X3d) == 24);

    for (const auto& [id, extension, wire, up] : {
             std::tuple{FormatId::Dae, L"dae", SourceFormatId::Dae, UpAxisId::Y},
             std::tuple{FormatId::ThreeDs, L"3ds", SourceFormatId::ThreeDs, UpAxisId::Z},
             std::tuple{FormatId::Dxf, L"dxf", SourceFormatId::Dxf, UpAxisId::Z},
             std::tuple{FormatId::DirectX, L"x", SourceFormatId::DirectX, UpAxisId::Y},
             std::tuple{FormatId::Off, L"off", SourceFormatId::Off, UpAxisId::Y},
             std::tuple{FormatId::Amf, L"amf", SourceFormatId::Amf, UpAxisId::Z},
             std::tuple{FormatId::X3d, L"x3d", SourceFormatId::X3d, UpAxisId::Y},
         }) {
        INFO("Assimp format " << static_cast<uint32_t>(id));
        const auto* descriptor = FindById(id);
        REQUIRE(descriptor != nullptr);
        CHECK(descriptor->importer == ImporterKind::AssimpCompat);
        CHECK(descriptor->wireFormatId == wire);
        CHECK(descriptor->defaultUpAxis == up);
        CHECK(descriptor->unitPolicy == UnitPolicy::Unspecified);
        CHECK(FindByExtension(extension) == descriptor);
        CHECK(IsImportableFormat(descriptor));
        CHECK(FindBySourceFormatId(wire) == descriptor);
        CHECK(ProgIdForFormat(*descriptor) != nullptr);
    }
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

TEST_CASE("every catalog signature has a positive and negative probe", "[format-sniffer]")
{
    struct SignatureProbeCase {
        MagicKind kind = MagicKind::HeadPrefix;
        std::string_view bytes{};
        uint32_t offset = 0;
        std::string_view aux{};
        std::string label;
        std::vector<std::byte> positive{};
        FormatId expected = FormatId::Unknown;
        std::vector<std::byte> negative{};
        bool zip = false;
        std::vector<std::string_view> positiveZip{};
        std::vector<std::string_view> negativeZip{};
    };

    const auto head = [](MagicKind kind, std::string_view bytes, uint32_t offset,
                         std::string_view aux, std::string label,
                         std::vector<std::byte> positive, FormatId expected,
                         std::vector<std::byte> negative) {
        return SignatureProbeCase{ kind, bytes, offset, aux, std::move(label),
            std::move(positive), expected, std::move(negative), false, {}, {} };
    };
    const auto zip = [](MagicKind kind, std::string_view bytes, std::string_view aux,
                        std::string label, std::vector<std::string_view> positiveZip,
                        FormatId expected, std::vector<std::string_view> negativeZip) {
        return SignatureProbeCase{ kind, bytes, 0, aux, std::move(label), {}, expected, {},
            true, std::move(positiveZip), std::move(negativeZip) };
    };

    const std::vector<SignatureProbeCase> cases{
        head(MagicKind::GltfJsonAsset, {}, 0, {}, "gltf-json",
            ToBytes("{\"asset\":{\"version\":\"2.0\"}}"), FormatId::Gltf,
            ToBytes("{\"asset\":{}}")),
        head(MagicKind::GlbHeader, {}, 0, {}, "glb",
            MakeGlb(2, 12), FormatId::Glb, MakeGlb(1, 12)),
        head(MagicKind::BinaryStl, {}, 0, {}, "stl-binary",
            MakeBinaryStl(1), FormatId::Stl, MakeBinaryStlHeader(100)),
        head(MagicKind::AsciiStlText, {}, 0, {}, "stl-ascii",
            ToBytes("solid s\n facet normal 0 0 1\n endfacet\nendsolid s\n"), FormatId::Stl,
            ToBytes("solid s\n facet normal 0 0 1\n")),
        head(MagicKind::PlyHeader, {}, 0, {}, "ply",
            ToBytes("ply\nformat ascii 1.0\n"), FormatId::Ply,
            ToBytes("plywood\n")),
        head(MagicKind::ObjText, {}, 0, {}, "obj",
            ToBytes("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"), FormatId::Obj,
            ToBytes("v 0 0 0\nv 1 0 0\n")),
        head(MagicKind::HeadPrefix, "Kaydara FBX Binary", 0, {}, "fbx-binary",
            ToBytes("Kaydara FBX Binary  \x00\x1a\x00"), FormatId::Fbx,
            ToBytes("Kaydara FBX Ascii")),
        head(MagicKind::HeadPrefixCaseInsensitive, "; FBX", 0, {}, "fbx-ascii",
            ToBytes("  ; fbx 7.4.0 project file\n"), FormatId::Fbx,
            ToBytes("; FB")),
        head(MagicKind::HeadPrefix, "#usda", 0, {}, "usda",
            ToBytes("#usda 1.0\n"), FormatId::Usda,
            ToBytes("#usdb 1.0\n")),
        head(MagicKind::HeadPrefix, "PXR-USDC", 0, {}, "usdc",
            ToBytes("PXR-USDC"), FormatId::Usdc,
            ToBytes("PXR-USDT")),
        zip(MagicKind::ZipEntrySuffix, ".usdc", {}, "usdz-usdc",
            { "model.usdc" }, FormatId::Usdz, { "readme.txt" }),
        zip(MagicKind::ZipEntrySuffix, ".usda", {}, "usdz-usda",
            { "model.usda" }, FormatId::Usdz, { "readme.txt" }),
        zip(MagicKind::ZipEntrySuffix, ".usd", {}, "usdz-usd",
            { "model.usd" }, FormatId::Usdz, { "readme.txt" }),
        zip(MagicKind::ZipEntryEquals, "[Content_Types].xml", {}, "3mf-content-types",
            { "[Content_Types].xml" }, FormatId::ThreeMf, { "readme.txt" }),
        zip(MagicKind::ZipEntryEquals, "3D/3dmodel.model", {}, "3mf-model",
            { "3D/3dmodel.model" }, FormatId::ThreeMf, { "readme.txt" }),
        head(MagicKind::HeadPrefix, "ISO-10303-21", 0, {}, "step",
            ToBytes("ISO-10303-21;\nHEADER;\n"), FormatId::Step,
            ToBytes("ISO-10303-22;\n")),
        head(MagicKind::IgesRecord, {}, 0, {}, "iges",
            MakeIgesRecord('S'), FormatId::Iges, MakeIgesRecord('X')),
        head(MagicKind::BrepHeader, {}, 0, {}, "brep",
            ToBytes("CASCADE Topology V1, (c) Matra-Datavision\nLocations 0\n"),
            FormatId::Brep, ToBytes("CASCADE Topology V9\n")),
        head(MagicKind::XbfHeader, {}, 0, {}, "xbf",
            MakeXbf(0x0000000704030201ull), FormatId::Xbf,
            MakeXbf(0x0000000704030202ull)),
        zip(MagicKind::ZipEntryEqualsAndSuffix, "Document.xml", ".brp", "fcstd",
            { "Document.xml", "PartShape.brp" }, FormatId::Fcstd, { "Document.xml" }),
        head(MagicKind::XmlPrefixCaseInsensitive, "<COLLADA", 0, {}, "dae",
            ToBytes("\xEF\xBB\xBF  <?xml version=\"1.0\"?>\n<!-- c -->\n<COLLADA xmlns=\"x\">"),
            FormatId::Dae, ToBytes("<?xml version=\"1.0\"?><COLLADABAD>")),
        head(MagicKind::ThreeDsChunk, {}, 0, {}, "3ds",
            MakeThreeDs(303, 303), FormatId::ThreeDs, MakeThreeDs(400, 303)),
        head(MagicKind::DxfSection, {}, 0, {}, "dxf",
            ToBytes("\xEF\xBB\xBF\n0\nSECTION\n2\nENTITIES\n"), FormatId::Dxf,
            ToBytes("0\nNOTSECTION\n2\nENTITIES\n")),
        head(MagicKind::HeadPrefix, "xof 0303", 0, {}, "directx-0303",
            ToBytes("xof 0303txt 0032\n"), FormatId::DirectX,
            ToBytes("xof 9999txt 0032\n")),
        head(MagicKind::HeadPrefix, "xof 0302", 0, {}, "directx-0302",
            ToBytes("xof 0302txt 0064\n"), FormatId::DirectX,
            ToBytes("xof 9999txt 0064\n")),
        head(MagicKind::OffHeader, {}, 0, {}, "off",
            ToBytes("  \n OFF\n8 6 0\n"), FormatId::Off,
            ToBytes("OFFICE\n")),
        head(MagicKind::XmlPrefixCaseInsensitive, "<amf", 0, {}, "amf",
            ToBytes("\xEF\xBB\xBF<!-- c --><?xml version=\"1.0\"?><amf unit=\"millimeter\">"),
            FormatId::Amf, ToBytes("<?xml version=\"1.0\"?><amfX>")),
        head(MagicKind::XmlPrefixCaseInsensitive, "<X3D", 0, {}, "x3d",
            ToBytes("\xEF\xBB\xBF\n<?xml version=\"1.0\"?>\n<!-- c -->\n<X3D profile=\"Interchange\">"),
            FormatId::X3d, ToBytes("<?xml version=\"1.0\"?><X3DX>")),
    };

    // A catalog entry cannot silently add a dead signature: every declared
    // signature must have a probe case below.
    for (const auto& descriptor : Formats()) {
        for (const auto& signature : descriptor.signatures) {
            INFO("descriptor " << descriptor.stableName << " kind "
                 << static_cast<uint32_t>(signature.kind) << " bytes "
                 << std::string(signature.bytes));
            const auto found = std::find_if(cases.begin(), cases.end(),
                [&](const SignatureProbeCase& item) {
                    return item.kind == signature.kind && item.bytes == signature.bytes
                        && item.offset == signature.offset && item.aux == signature.aux;
                });
            REQUIRE(found != cases.end());
        }
    }

    std::set<FormatId> selected;
    for (const auto& item : cases) {
        INFO("signature probe " << item.label);
        if (item.zip) {
            TempFile positive("positive.zip", MakeZip(item.positiveZip));
            const auto positiveResult = SniffFile(positive.WidePath());
            CHECK(positiveResult.format == item.expected);
            CHECK(positiveResult.confidence == SniffConfidence::Strong);
            selected.insert(positiveResult.format);

            TempFile negative("negative.zip", MakeZip(item.negativeZip));
            CHECK(SniffFile(negative.WidePath()).format == FormatId::Unknown);
        } else {
            const auto positiveResult = SniffHead(item.positive, item.positive.size());
            CHECK(positiveResult.format == item.expected);
            CHECK(positiveResult.confidence != SniffConfidence::None);
            selected.insert(positiveResult.format);

            CHECK(SniffHead(item.negative, item.negative.size()).format == FormatId::Unknown);
        }
    }

    // Every descriptor that declares a signature is reachable through the
    // table-driven probe. Descriptors with an empty signature span are the
    // wire-only content variants and the generic `.usd` extension identity.
    for (const auto& descriptor : Formats()) {
        if (descriptor.signatures.empty()) continue;
        INFO("reachable descriptor " << descriptor.stableName);
        CHECK(selected.contains(descriptor.id));
    }
}

TEST_CASE("renamed catalog content is detected and asks the user", "[format-sniffer]")
{
    struct RenamedCase {
        std::string label;
        std::vector<std::byte> bytes;
        FormatId expected;
    };

    const std::vector<RenamedCase> cases{
        { "dae", ToBytes("<?xml version=\"1.0\"?>\n<COLLADA xmlns=\"x\">"), FormatId::Dae },
        { "dxf", ToBytes("0\nSECTION\n2\nENTITIES\n"), FormatId::Dxf },
        { "off", ToBytes("OFF\n8 6 0\n"), FormatId::Off },
        { "x3d", ToBytes("<?xml version=\"1.0\"?>\n<X3D profile=\"Interchange\">"), FormatId::X3d },
        { "3ds", MakeThreeDs(303, 303), FormatId::ThreeDs },
        { "iges", MakeIgesRecord('S'), FormatId::Iges },
        { "brep", ToBytes("CASCADE Topology V1, (c) Matra-Datavision\nLocations 0\n"),
            FormatId::Brep },
        { "xbf", MakeXbf(0x0000000704030201ull), FormatId::Xbf },
        { "fcstd", MakeZip({ "Document.xml", "PartShape.brp" }), FormatId::Fcstd },
    };

    for (const auto& item : cases) {
        INFO("renamed " << item.label);
        TempFile file("renamed-" + item.label + ".bin", item.bytes);
        const auto sniffed = SniffFile(file.WidePath());
        CHECK(sniffed.format == item.expected);
        CHECK(sniffed.confidence != SniffConfidence::None);

        const auto classification = ClassifyFormat(L"bin", sniffed);
        REQUIRE(classification.content != nullptr);
        CHECK(classification.content->id == item.expected);
        CHECK(classification.reason == ClassificationReason::Content);
        CHECK_FALSE(classification.HasExtension());

        const auto decision = DecideOpenFormat(classification);
        CHECK(decision.action == OpenFormatAction::AskUser);
        REQUIRE(decision.format != nullptr);
        CHECK(decision.format->id == item.expected);
    }
}

TEST_CASE("cross-family content mismatch asks the user", "[format-sniffer]")
{
    const auto bytes = ToBytes("<?xml version=\"1.0\"?>\n<COLLADA xmlns=\"x\">");
    const auto sniffed = SniffHead(bytes, bytes.size());
    REQUIRE(sniffed.format == FormatId::Dae);

    const auto classification = ClassifyFormat(L"obj", sniffed);
    CHECK(classification.reason == ClassificationReason::Mismatch);
    REQUIRE(classification.extension != nullptr);
    CHECK(classification.extension->id == FormatId::Obj);
    REQUIRE(classification.selected != nullptr);
    CHECK(classification.selected->id == FormatId::Dae);

    const auto decision = DecideOpenFormat(classification);
    CHECK(decision.action == OpenFormatAction::AskUser);
    CHECK(decision.extensionCandidate == classification.extension);
    REQUIRE(decision.format != nullptr);
    CHECK(decision.format->id == FormatId::Dae);
}

TEST_CASE("structural and priority matching selects the strongest candidate", "[format-sniffer]")
{
    SECTION("ZIP package priority") {
        TempFile threeMf("both.3mf", MakeZip({ "[Content_Types].xml", "model.usdc" }));
        CHECK(SniffFile(threeMf.WidePath()).format == FormatId::ThreeMf);
    }
    SECTION("FCStd package outranks a USD layer") {
        TempFile fcstd("both.fcstd",
            MakeZip({ "Document.xml", "PartShape.brp", "model.usdc" }));
        CHECK(SniffFile(fcstd.WidePath()).format == FormatId::Fcstd);
    }
    SECTION("strong structural STL outranks weak OBJ statements") {
        const auto bytes = ToBytes(
            "solid s\nv 0 0 0\nf 1 2 3\nfacet normal 0 0 1\nendsolid s\n");
        CHECK(SniffHead(bytes, bytes.size()).format == FormatId::Stl);
    }
}

TEST_CASE("generic ZIP packages stay Unknown", "[format-sniffer]")
{
    const std::vector<std::vector<std::string_view>> packages{
        { "readme.txt" },
        { "Document.xml" },
        { "PartShape.brp" },
        { "foo/bar.txt", "baz.bin" },
    };
    for (const auto& entries : packages) {
        TempFile file("generic.zip", MakeZip(entries));
        CHECK(SniffFile(file.WidePath()).format == FormatId::Unknown);
    }
}

TEST_CASE("real catalog fixtures are content-detected", "[format-sniffer]")
{
    struct FixtureCase {
        const wchar_t* path;
        FormatId expected;
    };
    const std::array<FixtureCase, 13> fixtures{
        FixtureCase{ L"tests/fixtures/assimp-spike/box.dae", FormatId::Dae },
        FixtureCase{ L"tests/fixtures/assimp-spike/triangle.dxf", FormatId::Dxf },
        FixtureCase{ L"tests/fixtures/assimp-spike/box.off", FormatId::Off },
        FixtureCase{ L"tests/fixtures/assimp-spike/box.x3d", FormatId::X3d },
        FixtureCase{ L"tests/fixtures/assimp-spike/box.3ds", FormatId::ThreeDs },
        FixtureCase{ L"tests/fixtures/assimp-spike/box.x", FormatId::DirectX },
        FixtureCase{ L"tests/fixtures/assimp-spike/box.amf", FormatId::Amf },
        FixtureCase{ L"tests/fixtures/valid-file/wire.igs", FormatId::Iges },
        FixtureCase{ L"tests/fixtures/valid-file/wire.brep", FormatId::Brep },
        FixtureCase{ L"tests/fixtures/cad-spike/assembly.xbf", FormatId::Xbf },
        FixtureCase{ L"tests/fixtures/valid-file/wire.FCStd", FormatId::Fcstd },
        FixtureCase{ L"tests/fixtures/valid-file/renamed-dae.xml", FormatId::Dae },
        FixtureCase{ L"tests/fixtures/valid-file/renamed-dxf.txt", FormatId::Dxf },
    };

    const std::filesystem::path root(PREVIEW3D_REPO_ROOT);
    for (const auto& fixture : fixtures) {
        const std::filesystem::path path = root / std::wstring(fixture.path);
        INFO("fixture " << path.string());
        REQUIRE(std::filesystem::exists(path));
        const auto sniffed = SniffFile(path.wstring());
        CHECK(sniffed.format == fixture.expected);
        CHECK(sniffed.confidence != SniffConfidence::None);
    }
}

TEST_CASE("text signatures tolerate BOM and leading whitespace", "[format-sniffer]")
{
    const auto check = [](std::string_view label, std::string_view text, FormatId expected) {
        INFO("text variant " << std::string(label));
        const auto bytes = ToBytes(text);
        CHECK(SniffHead(bytes, bytes.size()).format == expected);
    };

    check("gltf-bom", "\xEF\xBB\xBF \n {\"asset\":{\"version\":\"2.0\"}}", FormatId::Gltf);
    check("ply-bom", "\xEF\xBB\xBF \n ply\nformat ascii 1.0\n", FormatId::Ply);
    check("stl-bom",
        "\xEF\xBB\xBF \n solid s\n facet normal 0 0 1\nendsolid s\n", FormatId::Stl);
    check("obj-bom", "\xEF\xBB\xBF \n # comment\n v 0 0 0\n f 1 1 1\n", FormatId::Obj);
    check("usda-bom", "\xEF\xBB\xBF#usda 1.0\n", FormatId::Usda);
    check("step-bom", "\xEF\xBB\xBFISO-10303-21;\n", FormatId::Step);
    check("fbx-ascii-whitespace", "  \n ; FBX 7.4.0 project file\n", FormatId::Fbx);
    check("brep-blank-line", "\n \nCASCADE Topology V2, (c) Matra-Datavision\n",
        FormatId::Brep);
    check("brep-drawable-prefix",
        "DBRep_DrawableShape\n\nCASCADE Topology V1, (c) Matra-Datavision\n",
        FormatId::Brep);
}

TEST_CASE("truncated bounded signatures stay Unknown", "[format-sniffer]")
{
    SECTION("IGES shorter than one 80-column record") {
        const auto bytes = ToBytes("                                        ");
        CHECK(SniffHead(bytes, bytes.size()).format == FormatId::Unknown);
    }
    SECTION("XBF shorter than the BINFILE version header") {
        const auto bytes = ToBytes("BINFILE");
        CHECK(SniffHead(bytes, bytes.size()).format == FormatId::Unknown);
    }
    SECTION("3DS shorter than the main chunk header") {
        const auto bytes = ToBytes("MM");
        CHECK(SniffHead(bytes, bytes.size()).format == FormatId::Unknown);
    }
}

TEST_CASE("ZIP64 and spanned archives stay Unknown", "[format-sniffer]")
{
    const auto patch16 = [](std::vector<std::byte>& bytes, std::size_t offset, uint16_t value) {
        for (std::size_t index = 0; index < 2; ++index) {
            bytes[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 0xFF);
        }
    };
    const auto patch32 = [](std::vector<std::byte>& bytes, std::size_t offset, uint32_t value) {
        for (std::size_t index = 0; index < 4; ++index) {
            bytes[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 0xFF);
        }
    };

    const auto base = MakeZip({ "[Content_Types].xml" });
    const std::size_t eocd = base.size() - 22;
    REQUIRE(base[eocd] == static_cast<std::byte>(0x50));

    SECTION("multi-disk archive") {
        auto bytes = base;
        patch16(bytes, eocd + 4, 1);
        TempFile file("multidisk.zip", bytes);
        CHECK(SniffFile(file.WidePath()).format == FormatId::Unknown);
    }
    SECTION("spanned archive") {
        auto bytes = base;
        patch16(bytes, eocd + 6, 1);
        TempFile file("spanned.zip", bytes);
        CHECK(SniffFile(file.WidePath()).format == FormatId::Unknown);
    }
    SECTION("ZIP64 sentinel central directory") {
        auto bytes = base;
        patch32(bytes, eocd + 12, 0xFFFFFFFFu);
        patch32(bytes, eocd + 16, 0xFFFFFFFFu);
        TempFile file("zip64.zip", bytes);
        CHECK(SniffFile(file.WidePath()).format == FormatId::Unknown);
    }
}

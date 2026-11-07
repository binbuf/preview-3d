// T07 reader selection regression coverage.
//
// Pure tests: the compile-time reader table, the CLI grammar, the forced-reader
// resolver and the Open As list are all header-only product decisions. No GPU,
// worker or parser is involved, and no path authority is exercised. The worker
// still validates the actual bytes for every forced selection.

#include "InfoPanel.h"
#include "OpenOptions.h"
#include "model_core/FormatCatalog.h"
#include "model_core/FormatSelection.h"
#include "model_core/FormatSniffer.h"
#include "model_core/ReaderCatalog.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace model_core;

namespace {

FormatClassification Classified(std::wstring_view extension, FormatId content)
{
    SniffResult sniff;
    sniff.format = content;
    sniff.confidence = content == FormatId::Unknown ? SniffConfidence::None : SniffConfidence::Strong;
    return ClassifyFormat(extension, sniff);
}

std::wstring Wide(std::string_view text)
{
    return std::wstring(text.begin(), text.end());
}

std::vector<std::wstring> Lines(const std::wstring& text)
{
    std::vector<std::wstring> lines;
    std::wstring current;
    for (const wchar_t character : text) {
        if (character == L'\n') {
            if (!current.empty()) lines.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

std::vector<std::wstring> SplitTabs(const std::wstring& line)
{
    std::vector<std::wstring> fields;
    std::wstring current;
    for (const wchar_t character : line) {
        if (character == L'\t') {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    fields.push_back(current);
    return fields;
}

const InfoPanelSection* FindSection(const std::vector<InfoPanelSection>& sections,
                                    std::wstring_view title)
{
    for (const auto& section : sections) {
        if (section.title == title) return &section;
    }
    return nullptr;
}

} // namespace

TEST_CASE("--list-readers includes every shipped reader exactly once", "[reader-selection]")
{
    const auto readers = Readers();
    REQUIRE(readers.size() == 11);

    // Stable name order is the listing contract.
    std::set<std::string> names;
    std::string previous;
    for (const auto& reader : readers) {
        INFO("reader " << std::string(reader.name));
        CHECK_FALSE(reader.name.empty());
        CHECK(reader.displayName != nullptr);
        CHECK(reader.family != FormatId::Unknown);
        CHECK(reader.importer != ImporterKind::Unknown);
        CHECK(reader.score > 0);
        CHECK_FALSE(reader.extensions.empty());
        CHECK(names.insert(std::string(reader.name)).second);
        if (!previous.empty()) CHECK(previous < reader.name);
        previous = std::string(reader.name);
        CHECK(FindReaderByName(reader.name) == &reader);
        CHECK(std::string(ImporterKindName(reader.importer)) != "unknown");
    }

    // Every importable catalog extension is owned by at least one reader, and
    // every reader extension resolves through the catalog. That is the
    // completeness/drift check for the shipped build.
    std::set<std::wstring> readerExtensions;
    for (const auto& reader : readers) {
        for (const auto extension : reader.extensions) {
            INFO("reader extension " << std::string(reader.name));
            REQUIRE(FindByExtension(extension) != nullptr);
            readerExtensions.insert(std::wstring(extension));
        }
    }
    std::set<std::wstring> catalogExtensions;
    for (const auto& extension : DirectOpenExtensions()) {
        catalogExtensions.insert(extension.size() > 1 ? extension.substr(1) : extension);
    }
    CHECK(readerExtensions == catalogExtensions);

    // A reader's direct importer must agree with every catalog descriptor it
    // owns. The USD compatibility reader is the one deliberate alternate: its
    // descriptors declare the fast-path importer, and the reader itself is the
    // separately built OpenUSD host.
    for (const auto& reader : readers) {
        for (const auto extension : reader.extensions) {
            const auto* descriptor = FindByExtension(extension);
            REQUIRE(descriptor != nullptr);
            if (reader.importer == ImporterKind::UsdCompatibilityHost) {
                CHECK(descriptor->importer == ImporterKind::UsdFastPath);
            } else {
                INFO("reader " << std::string(reader.name) << " extension "
                     << std::string(descriptor->stableName));
                CHECK(descriptor->importer == reader.importer);
            }
        }
    }

    // Compatibility readers score below the native reader for their family.
    const auto* usd = FindReaderByName("usd");
    const auto* usdCompat = FindReaderByName("usd-compat");
    REQUIRE(usd != nullptr);
    REQUIRE(usdCompat != nullptr);
    CHECK(usdCompat->isCompatibility);
    CHECK(usdCompat->score < usd->score);
    CHECK_FALSE(usd->isCompatibility);
    CHECK(FindReaderByName("assimp")->isCompatibility);

    // No reserved or unshipped identity is claimable.
    for (const char* name : { "vrml", "unknown", "plugin", "usd-fast", "assimp-native",
                              "step-compat", "gltf2", "dae-compat" }) {
        INFO("reserved " << name);
        CHECK(FindReaderByName(name) == nullptr);
    }

    const std::wstring listing = open_options::BuildReaderListText();
    const auto lines = Lines(listing);
    REQUIRE(lines.size() == readers.size());
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto fields = SplitTabs(lines[index]);
        REQUIRE(fields.size() == 4);
        const auto& reader = readers[index];
        CHECK(fields[0] == Wide(reader.name));
        CHECK(fields[2] == Wide(ImporterKindName(reader.importer)));
        CHECK(fields[3] == std::to_wstring(reader.score));
        // The extensions field lists the reader's suffixes in table order.
        std::wstring expected;
        for (const auto extension : reader.extensions) {
            if (!expected.empty()) expected += L',';
            expected += std::wstring(extension);
        }
        CHECK(fields[1] == expected);
    }
    // One line per reader: no duplicate name appears.
    std::set<std::wstring> listed;
    for (const auto& line : lines) CHECK(listed.insert(SplitTabs(line)[0]).second);
}

TEST_CASE("--reader is case-insensitive, bounded and rejects unknown names",
          "[reader-selection]")
{
    for (const auto& reader : Readers()) {
        std::wstring upper(reader.name.begin(), reader.name.end());
        for (wchar_t& character : upper) {
            if (character >= L'a' && character <= L'z')
                character = static_cast<wchar_t>(character - L'a' + L'A');
        }
        CAPTURE(upper);
        const auto option = open_options::ParseReaderName(upper);
        CHECK(option.valid);
        CHECK(option.error == open_options::ReaderParseError::None);
        CHECK(option.reader == &reader);
    }

    CHECK(open_options::ParseReaderName(L"gltf").reader == FindReaderByName("gltf"));
    CHECK(open_options::ParseReaderName(L"GLTF").reader == FindReaderByName("gltf"));
    CHECK(open_options::ParseReaderName(L"Usd-Compat").reader == FindReaderByName("usd-compat"));
    CHECK(open_options::ParseReaderName(L"step").reader == FindReaderByName("step"));
    CHECK(open_options::ParseReaderName(L"CAD").reader == FindReaderByName("cad"));

    CHECK(open_options::ParseReaderName(L"").error == open_options::ReaderParseError::Empty);
    CHECK_FALSE(open_options::ParseReaderName(L"").valid);
    CHECK(open_options::ParseReaderName(L"nope").error == open_options::ReaderParseError::Unknown);
    CHECK_FALSE(open_options::ParseReaderName(L"nope").valid);
    CHECK(open_options::ParseReaderName(L"vrml").error == open_options::ReaderParseError::Unknown);
    CHECK(open_options::ParseReaderName(L"gltf2").error == open_options::ReaderParseError::Unknown);
    CHECK(open_options::ParseReaderName(L"gltf ").error == open_options::ReaderParseError::Unknown);
    CHECK(open_options::ParseReaderName(L"gl\u00e4tf").error
          == open_options::ReaderParseError::NonAscii);

    const std::wstring tooLong(65, L'g');
    CHECK(open_options::ParseReaderName(tooLong).error
          == open_options::ReaderParseError::TooLong);
    CHECK_FALSE(open_options::ParseReaderName(tooLong).valid);

    // --reader is a reader name, not a family alias: a stable format name that
    // is not a shipped reader identity stays rejected where they differ.
    CHECK_FALSE(open_options::ParseReaderName(L"usda").valid);
    CHECK_FALSE(open_options::ParseReaderName(L"glb").valid);
}

TEST_CASE("reader table declares its families and closed option names", "[reader-selection]")
{
    const auto* cad = FindReaderByName("cad");
    const auto* step = FindReaderByName("step");
    const auto* usd = FindReaderByName("usd");
    const auto* usdCompat = FindReaderByName("usd-compat");
    const auto* gltf = FindReaderByName("gltf");
    REQUIRE(cad != nullptr);
    REQUIRE(step != nullptr);
    REQUIRE(usd != nullptr);
    REQUIRE(usdCompat != nullptr);
    REQUIRE(gltf != nullptr);

    const std::vector<FormatId> cadFamilies{
        FormatId::Iges, FormatId::Brep, FormatId::Xbf, FormatId::Fcstd };
    CHECK(ReaderFamilies(*cad) == cadFamilies);
    CHECK(ReaderCoversFamily(*cad, FormatId::Iges));
    CHECK(ReaderCoversFamily(*cad, FormatId::Fcstd));
    CHECK_FALSE(ReaderCoversFamily(*cad, FormatId::Step));
    CHECK(ReaderFamilies(*step) == std::vector<FormatId>{ FormatId::Step });
    CHECK(ReaderFamilies(*gltf) == std::vector<FormatId>{ FormatId::Gltf });

    const auto hasOption = [](const ReaderDescriptor& reader, std::string_view name) {
        return std::find(reader.optionNames.begin(), reader.optionNames.end(), name)
            != reader.optionNames.end();
    };
    for (const auto* reader : { cad, step }) {
        for (const char* option : { "quality", "read_wire", "linear_deflection",
                                    "angular_deflection", "relative_deflection", "unit_scale" })
            CHECK(hasOption(*reader, option));
        CHECK_FALSE(hasOption(*reader, "subdivision_level"));
    }
    for (const auto* reader : { usd, usdCompat }) {
        CHECK(hasOption(*reader, "subdivision_level"));
        CHECK_FALSE(hasOption(*reader, "quality"));
    }
    CHECK(gltf->optionNames.empty());
    CHECK(FindReaderByName("stl")->optionNames.empty());
}

TEST_CASE("default reader selection keeps extension and content classification",
          "[reader-selection]")
{
    SECTION("extension and content agree") {
        const auto selection = SelectDefaultReader(Classified(L"glb", FormatId::Glb));
        REQUIRE(selection);
        CHECK_FALSE(selection.forced);
        CHECK_FALSE(selection.forceCompatibilityHost);
        REQUIRE(selection.format != nullptr);
        CHECK(selection.format->id == FormatId::Glb);
        REQUIRE(selection.reader != nullptr);
        CHECK(selection.reader == FindReaderByName("gltf"));
    }
    SECTION("suffix only") {
        const auto selection = SelectDefaultReader(Classified(L"obj", FormatId::Unknown));
        REQUIRE(selection);
        REQUIRE(selection.reader != nullptr);
        CHECK(selection.reader == FindReaderByName("obj"));
        CHECK(selection.format->id == FormatId::Obj);
    }
    SECTION("content only") {
        const auto selection = SelectDefaultReader(Classified(L"bin", FormatId::Glb));
        REQUIRE(selection);
        REQUIRE(selection.reader != nullptr);
        CHECK(selection.reader == FindReaderByName("gltf"));
        CHECK(selection.format->id == FormatId::Glb);
    }
    SECTION("USD keeps the fast path by default") {
        const auto selection = SelectDefaultReader(Classified(L"usdz", FormatId::Usdz));
        REQUIRE(selection);
        CHECK(selection.reader == FindReaderByName("usd"));
        CHECK_FALSE(selection.forceCompatibilityHost);
    }
    SECTION("CAD and compatibility families") {
        const auto cad = SelectDefaultReader(Classified(L"iges", FormatId::Iges));
        REQUIRE(cad);
        CHECK(cad.reader == FindReaderByName("cad"));
        const auto assimp = SelectDefaultReader(Classified(L"dae", FormatId::Dae));
        REQUIRE(assimp);
        CHECK(assimp.reader == FindReaderByName("assimp"));
        CHECK(assimp.reader->isCompatibility);
    }
    SECTION("unclassified file has no default reader") {
        const auto selection = SelectDefaultReader(Classified(L"bin", FormatId::Unknown));
        CHECK_FALSE(selection);
        CHECK(selection.error == ReaderSelectionError::NoReaderForFormat);
        CHECK(selection.format == nullptr);
        CHECK(selection.reader == nullptr);
    }
}

TEST_CASE("a forced reader owns its extensions and falls back only when unambiguous",
          "[reader-selection]")
{
    const auto* gltf = FindReaderByName("gltf");
    const auto* cad = FindReaderByName("cad");
    const auto* step = FindReaderByName("step");
    const auto* assimp = FindReaderByName("assimp");
    const auto* usd = FindReaderByName("usd");
    const auto* usdCompat = FindReaderByName("usd-compat");
    REQUIRE(gltf != nullptr);
    REQUIRE(cad != nullptr);
    REQUIRE(step != nullptr);
    REQUIRE(assimp != nullptr);
    REQUIRE(usd != nullptr);
    REQUIRE(usdCompat != nullptr);

    SECTION("owned extension selects the exact descriptor") {
        const auto selection = ResolveForcedReader(*gltf, Classified(L"glb", FormatId::Glb));
        REQUIRE(selection);
        CHECK(selection.forced);
        CHECK(selection.format->id == FormatId::Glb);
        CHECK_FALSE(selection.forceCompatibilityHost);

        const auto usdSelection = ResolveForcedReader(*usdCompat,
            Classified(L"usdz", FormatId::Usdz));
        REQUIRE(usdSelection);
        CHECK(usdSelection.format->id == FormatId::Usdz);
        CHECK(usdSelection.forceCompatibilityHost);

        const auto fast = ResolveForcedReader(*usd, Classified(L"usdz", FormatId::Usdz));
        REQUIRE(fast);
        CHECK_FALSE(fast.forceCompatibilityHost);
    }

    SECTION("a single-family reader falls back to its own family") {
        // The suffix says STL, but the user explicitly forced glTF. The worker
        // still validates the bytes and fails typed; selection is not a parse.
        const auto gltfOnStl = ResolveForcedReader(*gltf, Classified(L"stl", FormatId::Stl));
        REQUIRE(gltfOnStl);
        CHECK(gltfOnStl.format->family == FormatId::Gltf);
        CHECK_FALSE(gltfOnStl.forceCompatibilityHost);

        const auto stepOnIges = ResolveForcedReader(*step, Classified(L"iges", FormatId::Iges));
        REQUIRE(stepOnIges);
        CHECK(stepOnIges.format->id == FormatId::Step);
    }

    SECTION("a multi-family reader rejects a suffix it does not own") {
        const auto selection = ResolveForcedReader(*cad, Classified(L"step", FormatId::Step));
        CHECK_FALSE(selection);
        CHECK(selection.error == ReaderSelectionError::ReaderCannotOpenFormat);
        CHECK(selection.format == nullptr);
        CHECK(selection.diagnostic.find("cad") != std::string::npos);
        CHECK(selection.diagnostic.find("step") != std::string::npos);

        const auto assimpOnObj = ResolveForcedReader(*assimp, Classified(L"obj", FormatId::Obj));
        CHECK_FALSE(assimpOnObj);
        CHECK(assimpOnObj.error == ReaderSelectionError::ReaderCannotOpenFormat);
        CHECK(assimpOnObj.diagnostic.find("assimp") != std::string::npos);
    }

    SECTION("content-only classification still resolves owned content") {
        const auto cadOnContent = ResolveForcedReader(*cad, Classified(L"bin", FormatId::Iges));
        REQUIRE(cadOnContent);
        CHECK(cadOnContent.format->id == FormatId::Iges);

        const auto cadOnGlb = ResolveForcedReader(*cad, Classified(L"bin", FormatId::Glb));
        CHECK_FALSE(cadOnGlb);
        CHECK(cadOnGlb.error == ReaderSelectionError::ReaderCannotOpenFormat);
    }

    SECTION("an unclassified file is rejected by multi-family readers") {
        const auto selection = ResolveForcedReader(*assimp, Classified(L"bin", FormatId::Unknown));
        CHECK_FALSE(selection);
        CHECK(selection.error == ReaderSelectionError::ReaderCannotOpenFormat);
        CHECK(selection.diagnostic.find("assimp") != std::string::npos);
    }
}

TEST_CASE("Open As lists the default and shipped alternates for every family",
          "[reader-selection]")
{
    const auto families = open_options::OpenAsFamilyEntries();
    REQUIRE_FALSE(families.empty());

    // Every importable family appears exactly once.
    std::set<FormatId> expectedFamilies;
    for (const auto* descriptor : open_options::OpenAsFormats())
        expectedFamilies.insert(descriptor->family);
    std::set<FormatId> seenFamilies;
    for (const auto& entry : families) {
        INFO("family " << static_cast<uint32_t>(entry.family));
        CHECK(seenFamilies.insert(entry.family).second);
        REQUIRE_FALSE(entry.readers.empty());
        CHECK(entry.format != nullptr);
        CHECK(entry.format->family == entry.family);
        // Default first, exactly one default, and it is the score winner.
        CHECK(entry.readers.front().isDefault);
        CHECK(std::count_if(entry.readers.begin(), entry.readers.end(),
                  [](const auto& reader) { return reader.isDefault; }) == 1);
        CHECK(entry.readers.front().reader == DefaultReaderForFamily(entry.family));
        for (std::size_t index = 1; index < entry.readers.size(); ++index)
            CHECK_FALSE(entry.readers[index].isDefault);
    }
    CHECK(seenFamilies == expectedFamilies);

    // Only the USD family currently has a shipped alternate. It is the OpenUSD
    // compatibility host, never a native reader.
    const auto usdFamily = std::find_if(families.begin(), families.end(),
        [](const auto& entry) { return entry.family == FormatId::Usd; });
    REQUIRE(usdFamily != families.end());
    REQUIRE(usdFamily->readers.size() == 2);
    CHECK(usdFamily->readers[0].reader == FindReaderByName("usd"));
    CHECK(usdFamily->readers[0].isDefault);
    CHECK(usdFamily->readers[1].reader == FindReaderByName("usd-compat"));
    CHECK_FALSE(usdFamily->readers[1].isDefault);
    CHECK(usdFamily->readers[1].reader->isCompatibility);

    // Every other family lists exactly its default reader (no internal host
    // duplication).
    for (const auto& entry : families) {
        if (entry.family == FormatId::Usd) continue;
        CHECK(entry.readers.size() == 1);
        CHECK(entry.readers.front().reader == DefaultReaderForFamily(entry.family));
    }

    const std::wstring listing = open_options::BuildOpenAsReaderText();
    CHECK(listing.find(L"usd\tusd\tdefault\n") != std::wstring::npos);
    CHECK(listing.find(L"usd\tusd-compat\talternate\n") != std::wstring::npos);
    CHECK(listing.find(L"gltf\tgltf\tdefault\n") != std::wstring::npos);
    CHECK(listing.find(L"step\tstep\tdefault\n") != std::wstring::npos);
    CHECK(listing.find(L"iges\tcad\tdefault\n") != std::wstring::npos);
    CHECK(listing.find(L"dae\tassimp\tdefault\n") != std::wstring::npos);
    std::size_t expectedLines = 0;
    for (const auto& entry : families) expectedLines += entry.readers.size();
    CHECK(Lines(listing).size() == expectedLines);
}

TEST_CASE("a forced-reader failure is typed and names the reader", "[reader-selection]")
{
    const auto* cad = FindReaderByName("cad");
    REQUIRE(cad != nullptr);

    const auto selection = ResolveForcedReader(*cad, Classified(L"step", FormatId::Step));
    REQUIRE_FALSE(selection);
    CHECK(selection.error == ReaderSelectionError::ReaderCannotOpenFormat);

    // The failure card text always names the reader and never a path.
    const std::wstring diagnostic = open_options::ReaderSelectionDiagnostic(selection);
    CHECK(diagnostic.find(L"Reader:") != std::wstring::npos);
    CHECK(diagnostic.find(L"cad") != std::wstring::npos);
    CHECK(diagnostic.find(L"OCCT CAD") != std::wstring::npos);

    // A single-family forced reader that cannot parse the bytes is still a
    // normal worker/host failure; the label the viewer appends names it.
    const auto* gltf = FindReaderByName("gltf");
    REQUIRE(gltf != nullptr);
    const auto accepted = ResolveForcedReader(*gltf, Classified(L"stl", FormatId::Stl));
    REQUIRE(accepted);
    const std::wstring label = open_options::ReaderLabel(*gltf);
    CHECK(label.find(L"gltf") != std::wstring::npos);
    CHECK(label.find(L"glTF") != std::wstring::npos);

    // No default reader for an unclassified file is typed too.
    const auto none = SelectDefaultReader(Classified(L"bin", FormatId::Unknown));
    CHECK(none.error == ReaderSelectionError::NoReaderForFormat);
    CHECK(open_options::ReaderSelectionDiagnostic(none).find(L"no reader") != std::wstring::npos);
}

TEST_CASE("the info panel names the selected reader", "[reader-selection][info-panel]")
{
    ModelData metadata;
    metadata.source.format = model_core::SourceFormatId::Glb;

    // Existing layout is unchanged when no reader label is supplied.
    auto sections = BuildInfoPanelSections(metadata, false);
    const auto* scene = FindSection(sections, L"Scene Data");
    REQUIRE(scene != nullptr);
    CHECK(std::none_of(scene->rows.begin(), scene->rows.end(),
        [](const InfoPanelRow& row) { return row.label == L"Reader"; }));

    sections = BuildInfoPanelSections(metadata, false, GroundAxis::Automatic,
        model_core::PresentationProfile::Default, L"glTF (gltf)");
    scene = FindSection(sections, L"Scene Data");
    REQUIRE(scene != nullptr);
    const auto readerRow = std::find_if(scene->rows.begin(), scene->rows.end(),
        [](const InfoPanelRow& row) { return row.label == L"Reader"; });
    REQUIRE(readerRow != scene->rows.end());
    CHECK(readerRow->value == L"glTF (gltf)");
    // The existing Format row keeps its index.
    CHECK(scene->rows[2].label == L"Format");
    CHECK(scene->rows[2].value == L"GLB");
}

// T03 -- catalog-driven Open As, --format parsing and installer drift.
//
// Pure tests: no GPU, worker or parser. The installer case reads Preview3D.nsi
// and must never invoke makensis.

#include "OpenOptions.h"
#include "model_core/FormatCatalog.h"
#include "model_core/FormatSelection.h"
#include "model_core/FormatSniffer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace model_core;

namespace {

std::string Narrow(std::wstring_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value)
        result.push_back(static_cast<char>(character));
    return result;
}

FormatClassification Classified(std::wstring_view extension, FormatId content)
{
    SniffResult sniff;
    sniff.format = content;
    sniff.confidence = content == FormatId::Unknown ? SniffConfidence::None : SniffConfidence::Strong;
    return ClassifyFormat(extension, sniff);
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream source(path, std::ios::binary);
    REQUIRE(source.is_open());
    std::ostringstream text;
    text << source.rdbuf();
    return text.str();
}

struct MacroCall
{
    std::string name;
    std::string argument;
    std::string define;
};

std::vector<MacroCall> ParseMacros(const std::string& text, std::string_view macroName)
{
    std::vector<MacroCall> calls;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string prefix = "!insertmacro " + std::string(macroName) + " \"";
        const auto start = line.find(prefix);
        if (start == std::string::npos) continue;
        const auto firstQuote = start + prefix.size() - 1;
        const auto secondQuote = line.find('"', firstQuote + 1);
        if (secondQuote == std::string::npos) continue;
        const auto defineStart = line.find("${", secondQuote);
        const auto defineEnd = defineStart == std::string::npos ? std::string::npos
            : line.find('}', defineStart);
        if (defineEnd == std::string::npos) continue;
        MacroCall call;
        call.name = std::string(macroName);
        call.argument = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
        call.define = line.substr(defineStart + 2, defineEnd - defineStart - 2);
        calls.push_back(std::move(call));
    }
    return calls;
}

std::map<std::string, std::string> ParseProgIdDefines(const std::string& text)
{
    std::map<std::string, std::string> values;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string prefix = "!define PROGID_";
        const auto start = line.find(prefix);
        if (start == std::string::npos) continue;
        const auto nameStart = start + std::string("!define ").size();
        const auto nameEnd = line.find_first_of(" \t", nameStart);
        const auto firstQuote = line.find('"', nameEnd);
        const auto secondQuote = line.find('"', firstQuote + 1);
        if (nameEnd == std::string::npos || firstQuote == std::string::npos ||
            secondQuote == std::string::npos) continue;
        values[line.substr(nameStart, nameEnd - nameStart)] =
            line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
    }
    return values;
}

std::string DefineForProgId(const std::map<std::string, std::string>& defines, const char* progId)
{
    for (const auto& [name, value] : defines)
        if (value == progId) return name;
    return {};
}

} // namespace

TEST_CASE("--format accepts case-insensitive shipped stable names", "[open-as]")
{
    for (const wchar_t* name : { L"gltf", L"GLB", L"Glb", L"step", L"STEP", L"3mf" }) {
        const auto option = open_options::ParseFormatName(name);
        CAPTURE(std::wstring(name));
        CHECK(option.valid);
        CHECK(option.descriptor != nullptr);
    }
    CHECK(open_options::ParseFormatName(L"gltf").descriptor == FindById(FormatId::Gltf));
    CHECK(open_options::ParseFormatName(L"GLB").descriptor == FindById(FormatId::Glb));
    CHECK(open_options::ParseFormatName(L"gltf").descriptor->importer == ImporterKind::GltfWorker);
}

TEST_CASE("--format rejects empty, unknown, non-ASCII and unshipped names", "[open-as]")
{
    CHECK_FALSE(open_options::ParseFormatName(L"").valid);
    CHECK_FALSE(open_options::ParseFormatName(L"nope").valid);
    CHECK_FALSE(open_options::ParseFormatName(L"gltf2").valid);
    CHECK_FALSE(open_options::ParseFormatName(L"gl\u00e4tf").valid);
    // T08 shipped the OCCT CAD host readers, so IGES/BREP stable names are now
    // claimable; still-reserved identities stay rejected.
    CHECK(open_options::ParseFormatName(L"iges").valid);
    CHECK(open_options::ParseFormatName(L"IGES").valid);
    CHECK(open_options::ParseFormatName(L"brep").valid);
    CHECK_FALSE(open_options::ParseFormatName(L"xbf").valid);
    CHECK_FALSE(open_options::ParseFormatName(L"fcstd").valid);
}

TEST_CASE("Open As selection keeps existing suffix behaviour and asks on conflict", "[open-as]")
{
    const auto agrees = DecideOpenFormat(Classified(L"glb", FormatId::Glb));
    CHECK(agrees.action == OpenFormatAction::OpenExtension);
    CHECK(agrees.format == FindById(FormatId::Glb));

    const auto suffixOnly = DecideOpenFormat(Classified(L"obj", FormatId::Unknown));
    CHECK(suffixOnly.action == OpenFormatAction::OpenExtension);
    CHECK(suffixOnly.format == FindById(FormatId::Obj));

    const auto contentOnly = DecideOpenFormat(Classified(L"bin", FormatId::Glb));
    CHECK(contentOnly.action == OpenFormatAction::AskUser);
    CHECK(contentOnly.format == FindById(FormatId::Glb));
    CHECK(contentOnly.extensionCandidate == nullptr);

    const auto mismatch = DecideOpenFormat(Classified(L"obj", FormatId::Glb));
    CHECK(mismatch.action == OpenFormatAction::AskUser);
    CHECK(mismatch.format == FindById(FormatId::Glb));
    CHECK(mismatch.extensionCandidate == FindById(FormatId::Obj));

    const auto nothing = DecideOpenFormat(Classified(L"txt", FormatId::Unknown));
    CHECK(nothing.action == OpenFormatAction::Reject);
    CHECK(nothing.format == nullptr);
}

TEST_CASE("Extension/content mismatch reporting names both candidates", "[open-as]")
{
    const std::string summary = FormatChoiceSummary(Classified(L"obj", FormatId::Glb));
    CHECK(summary == "extension=obj content=glb selected=glb reason=mismatch");
    const std::string contentOnly = FormatChoiceSummary(Classified(L"bin", FormatId::Glb));
    CHECK(contentOnly == "extension=unknown content=glb selected=glb reason=content");
}

TEST_CASE("Catalog extension list and --list-formats text stay catalog-driven", "[open-as]")
{
    const auto extensions = DirectOpenExtensions();
    for (const wchar_t* expected : { L".glb", L".gltf", L".stl", L".ply", L".obj", L".fbx",
                                     L".3mf", L".usd", L".usda", L".usdc", L".usdz",
                                     L".step", L".stp", L".igs", L".iges", L".brep" }) {
        CHECK(std::find(extensions.begin(), extensions.end(), expected) != extensions.end());
    }
    CHECK(std::find(extensions.begin(), extensions.end(), L".mtl") == extensions.end());

    const std::wstring listing = open_options::BuildFormatListText(
        [](const FormatDescriptor& descriptor) { return std::wstring(descriptor.displayName); });
    for (const auto* descriptor : open_options::OpenAsFormats()) {
        if (descriptor->extensions.empty()) continue;
        const std::wstring name(descriptor->stableName.begin(), descriptor->stableName.end());
        CHECK(listing.find(name + L"\t") != std::wstring::npos);
    }
    // One line per directly registered catalog descriptor.
    CHECK(std::count(listing.begin(), listing.end(), L'\n')
          == static_cast<std::ptrdiff_t>(open_options::OpenAsFormats().size()));
}

TEST_CASE("Installer registration matches the catalog for install and uninstall", "[format-installer]")
{
    const auto path = std::filesystem::path(PREVIEW3D_REPO_ROOT) / L"packaging"
        / L"installer" / L"Preview3D.nsi";
    const std::string text = ReadFile(path);
    const auto uninstallStart = text.find("Section \"Uninstall\"");
    REQUIRE(uninstallStart != std::string::npos);
    const std::string install = text.substr(0, uninstallStart);
    const std::string uninstall = text.substr(uninstallStart);

    const auto defines = ParseProgIdDefines(text);
    const auto registerExtensions = ParseMacros(install, "RegisterExtension");
    const auto unregisterExtensions = ParseMacros(uninstall, "UnregisterExtension");

    std::size_t expectedExtensions = 0;
    for (const auto& descriptor : Formats()) {
        const char* progId = ProgIdForFormat(descriptor);
        if (!progId || descriptor.extensions.empty()) continue;
        const std::string define = DefineForProgId(defines, progId);
        CAPTURE(progId);
        REQUIRE_FALSE(define.empty());
        CHECK(install.find("!insertmacro RegisterProgId \"${" + define + "}\"") != std::string::npos);
        CHECK(uninstall.find("DeleteRegKey HKLM \"Software\\Classes\\${" + define + "}\"")
              != std::string::npos);
        for (const auto extension : descriptor.extensions) {
            std::string argument = ".";
            for (const wchar_t character : extension) argument.push_back(static_cast<char>(character));
            CAPTURE(argument);
            const auto registered = std::find_if(registerExtensions.begin(), registerExtensions.end(),
                [&](const MacroCall& call) {
                    return call.argument == argument && call.define == define;
                });
            CHECK(registered != registerExtensions.end());
            const auto unregistered = std::find_if(unregisterExtensions.begin(), unregisterExtensions.end(),
                [&](const MacroCall& call) {
                    return call.argument == argument && call.define == define;
                });
            CHECK(unregistered != unregisterExtensions.end());
            ++expectedExtensions;
        }
    }

    // Extra installer entries are drift too: every extension macro must be a
    // catalog extension with the family's stable ProgID.
    CHECK(registerExtensions.size() == expectedExtensions);
    CHECK(unregisterExtensions.size() == expectedExtensions);
    for (const auto& call : registerExtensions) {
        CAPTURE(call.argument);
        REQUIRE(call.argument.size() > 1);
        const auto* descriptor = FindByExtension(std::wstring(call.argument.begin() + 1, call.argument.end()));
        REQUIRE(descriptor != nullptr);
        CHECK(ProgIdForFormat(*descriptor) != nullptr);
        CHECK(defines.at(call.define) == ProgIdForFormat(*descriptor));
    }
}

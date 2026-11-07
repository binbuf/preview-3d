// T03 -- catalog-driven Open As, --format parsing and installer drift.
//
// Pure tests: no GPU, worker or parser. The installer case reads Preview3D.nsi
// and must never invoke makensis.

#include "FamilyRouting.h"
#include "OpenOptions.h"
#include "model_core/FormatCatalog.h"
#include "model_core/FormatSelection.h"
#include "model_core/FormatSniffer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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

// The thumbnail registration script keeps its own PowerShell table because it
// is staged beside the installer. Parse it so a test can prove it still agrees
// with FamilyRouting.h and the catalog instead of trusting a comment.
struct RegistrationFamily
{
    std::string clsid;
    std::vector<std::string> extensions;
};

std::vector<RegistrationFamily> ParseThumbnailRegistration(const std::string& text)
{
    std::vector<RegistrationFamily> families;
    RegistrationFamily current;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto clsidStart = line.find("Clsid");
        if (clsidStart != std::string::npos) {
            const auto firstQuote = line.find('\'', clsidStart);
            const auto secondQuote = firstQuote == std::string::npos
                ? std::string::npos : line.find('\'', firstQuote + 1);
            if (secondQuote != std::string::npos) {
                if (!current.clsid.empty()) families.push_back(current);
                current = {};
                current.clsid = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
            }
            continue;
        }
        const auto extensionsStart = line.find("Extensions");
        if (extensionsStart == std::string::npos || current.clsid.empty()) continue;
        const auto listStart = line.find("@(", extensionsStart);
        if (listStart == std::string::npos) continue;
        std::size_t cursor = listStart + 2;
        while (true) {
            const auto open = line.find('\'', cursor);
            if (open == std::string::npos) break;
            const auto close = line.find('\'', open + 1);
            if (close == std::string::npos) break;
            current.extensions.push_back(line.substr(open + 1, close - open - 1));
            cursor = close + 1;
        }
        families.push_back(current);
        current = {};
    }
    if (!current.clsid.empty()) families.push_back(current);
    return families;
}

model_core::FormatId CatalogFamilyForRoute(preview3d::provider::Family family)
{
    using preview3d::provider::Family;
    switch (family) {
    case Family::Gltf: return FormatId::Gltf;
    case Family::Stl: return FormatId::Stl;
    case Family::Ply: return FormatId::Ply;
    case Family::Obj: return FormatId::Obj;
    case Family::Fbx: return FormatId::Fbx;
    case Family::ThreeMf: return FormatId::ThreeMf;
    case Family::Usd: return FormatId::Usd;
    case Family::Step: return FormatId::Step;
    default: return FormatId::Unknown;
    }
}

// Union of every catalog descriptor in a family (Gltf covers .gltf/.glb, Usd
// covers .usd/.usda/.usdc/.usdz), with the leading dot the provider stores.
std::set<std::string> CatalogExtensionsForFamily(FormatId family)
{
    std::set<std::string> result;
    for (const auto& descriptor : Formats()) {
        if (descriptor.family != family) continue;
        for (const auto extension : descriptor.extensions) {
            result.insert("." + Narrow(extension));
        }
    }
    return result;
}

std::set<std::string> ExtensionSet(std::span<const char* const> extensions)
{
    std::set<std::string> result;
    for (const char* extension : extensions) result.insert(extension);
    return result;
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
    // T09 shipped the OCCT-native XBF/FCStd readers through the same host.
    CHECK(open_options::ParseFormatName(L"xbf").valid);
    CHECK(open_options::ParseFormatName(L"fcstd").valid);
    // T10 shipped the bounded Assimp compatibility family, so these stable
    // names are claimable; VRML remains a reserved identity with no descriptor.
    for (const wchar_t* name : { L"dae", L"3ds", L"dxf", L"directx", L"off", L"amf", L"x3d" })
        CHECK(open_options::ParseFormatName(name).valid);
    CHECK_FALSE(open_options::ParseFormatName(L"vrml").valid);
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
                                     L".step", L".stp", L".igs", L".iges", L".brep",
                                    L".xbf", L".fcstd" }) {
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

TEST_CASE("STEP aliases resolve through the catalog and every open surface", "[format-aliases]")
{
    const auto* step = FindById(FormatId::Step);
    REQUIRE(step != nullptr);
    // Aliases belong to the existing family: no new stable name or ProgID.
    CHECK(step->stableName == "step");
    REQUIRE(ProgIdForFormat(*step) != nullptr);
    CHECK(std::string(ProgIdForFormat(*step)) == "Binbuf.Preview3D.STEP.1");
    CHECK(step->thumbnail == ThumbnailFamily::Step);

    const std::wstring_view aliases[] = { L"stpnc", L"p21", L"210" };
    for (const auto alias : aliases) {
        CAPTURE(std::wstring(alias));
        CHECK(FindByExtension(alias) == step);
        CHECK(FindByExtension(std::wstring(L".") + std::wstring(alias)) == step);
        std::wstring upper(alias);
        for (wchar_t& character : upper) {
            character = static_cast<wchar_t>(towupper(character));
        }
        CHECK(FindByExtension(upper) == step);
        CHECK(FindByExtension(std::wstring(L".") + upper) == step);
    }

    // The direct-open list has each alias exactly once.
    const auto extensions = DirectOpenExtensions();
    for (const wchar_t* expected : { L".stpnc", L".p21", L".210" }) {
        CAPTURE(std::wstring(expected));
        CHECK(std::count(extensions.begin(), extensions.end(), expected) == 1);
    }

    // The Open As list (and --list-formats) prints the STEP stable name and all
    // aliases on the single STEP line.
    const std::wstring listing = open_options::BuildFormatListText(
        [](const FormatDescriptor& descriptor) { return std::wstring(descriptor.displayName); });
    CHECK(listing.find(L"step\tstep,stp,stpnc,p21,210\t") != std::wstring::npos);
    for (const wchar_t* alias : { L"stpnc", L"p21", L"210" }) {
        CAPTURE(std::wstring(alias));
        CHECK(listing.find(alias) != std::wstring::npos);
    }
}

TEST_CASE("Installer registers and unregisters every STEP alias", "[format-aliases][format-installer]")
{
    const auto path = std::filesystem::path(PREVIEW3D_REPO_ROOT) / L"packaging"
        / L"installer" / L"Preview3D.nsi";
    const std::string text = ReadFile(path);
    const auto uninstallStart = text.find("Section \"Uninstall\"");
    REQUIRE(uninstallStart != std::string::npos);
    const std::string install = text.substr(0, uninstallStart);
    const std::string uninstall = text.substr(uninstallStart);

    const auto defines = ParseProgIdDefines(text);
    const std::string define = DefineForProgId(defines, "Binbuf.Preview3D.STEP.1");
    REQUIRE_FALSE(define.empty());
    const auto registerExtensions = ParseMacros(install, "RegisterExtension");
    const auto unregisterExtensions = ParseMacros(uninstall, "UnregisterExtension");

    const auto* step = FindById(FormatId::Step);
    REQUIRE(step != nullptr);
    REQUIRE(step->extensions.size() == 5);
    for (const auto extension : step->extensions) {
        std::string argument = ".";
        for (const wchar_t character : extension) argument.push_back(static_cast<char>(character));
        CAPTURE(argument);
        CHECK(std::find_if(registerExtensions.begin(), registerExtensions.end(),
                  [&](const MacroCall& call) {
                      return call.argument == argument && call.define == define;
                  }) != registerExtensions.end());
        CHECK(std::find_if(unregisterExtensions.begin(), unregisterExtensions.end(),
                  [&](const MacroCall& call) {
                      return call.argument == argument && call.define == define;
                  }) != unregisterExtensions.end());
    }

    // No extra STEP entries in either direction.
    const auto stepRegistrations = std::count_if(registerExtensions.begin(), registerExtensions.end(),
        [&](const MacroCall& call) { return call.define == define; });
    const auto stepUnregistrations = std::count_if(unregisterExtensions.begin(), unregisterExtensions.end(),
        [&](const MacroCall& call) { return call.define == define; });
    CHECK(stepRegistrations == static_cast<std::ptrdiff_t>(step->extensions.size()));
    CHECK(stepUnregistrations == static_cast<std::ptrdiff_t>(step->extensions.size()));
}

TEST_CASE("Thumbnail routes and registration script carry the STEP aliases", "[format-aliases]")
{
    // FamilyRouting.h is the provider's extension authority; the catalog is the
    // product's. They must agree for every family, not just STEP.
    for (const auto& route : preview3d::provider::FamilyRoutes()) {
        const FormatId family = CatalogFamilyForRoute(route.family);
        CAPTURE(route.clsid);
        REQUIRE(family != FormatId::Unknown);
        const auto catalogExtensions = CatalogExtensionsForFamily(family);
        CHECK_FALSE(catalogExtensions.empty());
        CHECK(ExtensionSet(route.extensions) == catalogExtensions);
    }

    const preview3d::provider::FamilyRoute* stepRoute = nullptr;
    for (const auto& route : preview3d::provider::FamilyRoutes()) {
        if (route.family == preview3d::provider::Family::Step) stepRoute = &route;
    }
    REQUIRE(stepRoute != nullptr);
    CHECK(std::string(stepRoute->clsid) == "{6EE961AC-AC3B-4958-A898-E30523FEE79D}");
    const auto stepExtensions = ExtensionSet(stepRoute->extensions);
    for (const char* alias : { ".stpnc", ".p21", ".210" }) {
        CAPTURE(alias);
        CHECK(stepExtensions.count(alias) == 1);
    }

    // The installer's staged PowerShell table is a second copy of the same
    // identities; check it against FamilyRouting.h so it cannot drift.
    const auto path = std::filesystem::path(PREVIEW3D_REPO_ROOT) / L"packaging"
        / L"installer" / L"Preview3DThumbnailRegistration.ps1";
    const auto families = ParseThumbnailRegistration(ReadFile(path));
    REQUIRE(families.size() == preview3d::provider::FamilyRoutes().size());
    for (const auto& route : preview3d::provider::FamilyRoutes()) {
        CAPTURE(route.clsid);
        const auto registered = std::find_if(families.begin(), families.end(),
            [&](const RegistrationFamily& family) { return family.clsid == route.clsid; });
        REQUIRE(registered != families.end());
        const std::set<std::string> scriptExtensions(
            registered->extensions.begin(), registered->extensions.end());
        CHECK(scriptExtensions == ExtensionSet(route.extensions));
    }
}

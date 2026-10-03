// Runtime localization core. The pack files themselves are guarded by
// interactive-viewer/tools/generate-language-packs.ps1 -Verify (the
// Localization CI workflow); these cases exercise the C++ loader/parser and the
// fallback/formatting contract against the real interactive-viewer/lang tree.

#include "Localization.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#ifndef PREVIEW3D_LANG_DIR
#error "PREVIEW3D_LANG_DIR must be defined by Tests.Unit.vcxproj"
#endif

namespace
{
std::wstring LanguageDirectory()
{
    return std::wstring(PREVIEW3D_LANG_DIR);
}

bool LanguageDirectoryExists()
{
    return std::filesystem::exists(std::filesystem::path(LanguageDirectory()));
}

// The active locale is process-global state. Cases that load a translated pack
// must not leak it into later cases (WireFormatTests expects English literals),
// and the reset must happen even if a REQUIRE aborts the case.
struct EnglishLocaleGuard
{
    ~EnglishLocaleGuard()
    {
        LocalizationLoadDirectoryForTesting(LanguageDirectory(), L"en");
    }
};
}

TEST_CASE("Loc falls back to the in-source English literal for unknown keys", "[localization]")
{
    LocalizationLoadDirectoryForTesting(LanguageDirectory(), L"en");
    CHECK(Loc("unit.nonexistent.key", L"Fallback text") == L"Fallback text");
}

TEST_CASE("LocFormat substitutes positional placeholders", "[localization]")
{
    LocalizationLoadDirectoryForTesting(LanguageDirectory(), L"en");
    CHECK(LocFormat("unit.placeholder", L"{0} and {1}", { L"left", L"right" }) == L"left and right");
}

TEST_CASE("An unknown language code falls back to English", "[localization]")
{
    if (!LanguageDirectoryExists()) SKIP("language pack directory not found");
    LocalizationLoadDirectoryForTesting(LanguageDirectory(), L"zz-ZZ");
    CHECK(LocalizationLanguage() == L"en");
}

TEST_CASE("The shipped English catalogue resolves real keys", "[localization]")
{
    if (!LanguageDirectoryExists()) SKIP("language pack directory not found");
    REQUIRE(LocalizationLoadDirectoryForTesting(LanguageDirectory(), L"en"));
    CHECK(LocalizationLanguage() == L"en");
    CHECK(Loc("empty.dropTitle", L"x") == L"Drop a 3D model here");
    CHECK(Loc("tooltip.grid", L"x") == L"Toggle ground grid");
}

TEST_CASE("A translated pack loads and preserves its placeholders", "[localization]")
{
    if (!LanguageDirectoryExists()) SKIP("language pack directory not found");
    EnglishLocaleGuard restoreEnglish;
    REQUIRE(LocalizationLoadDirectoryForTesting(LanguageDirectory(), L"fr"));
    CHECK(LocalizationLanguage() == L"fr");
    // A real translation replaces the English fallback...
    CHECK(Loc("empty.dropTitle", L"Drop a 3D model here") != L"Drop a 3D model here");
    // ...and any placeholders the sentence carries survive the round trip.
    const std::wstring directional =
        Loc("hud.directionalLight", L"Directional light  {0}\u00B0 / {1}\u00B0");
    CHECK(directional.find(L"{0}") != std::wstring::npos);
    CHECK(directional.find(L"{1}") != std::wstring::npos);
}

TEST_CASE("LocalizedJoin owns the separating space and trims the fragment", "[localization]")
{
    // The regression: a " units" fallback translated to "unidades" (no space)
    // rendered "29.092unidades".
    CHECK(LocalizedJoin(L"29.092", L"unidades") == L"29.092 unidades");
    CHECK(LocalizedJoin(L"29.092", L" unidades") == L"29.092 unidades");
    CHECK(LocalizedJoin(L"Loading STEP", L"• checking text") == L"Loading STEP • checking text");
    CHECK(LocalizedJoin(L"Format:", L"glTF") == L"Format: glTF");
    CHECK(LocalizedJoin(L"", L"x") == L"x");
    CHECK(LocalizedJoin(L"prefix", L"   ") == L"prefix");
}

TEST_CASE("The language list is offered with English first", "[localization]")
{
    if (!LanguageDirectoryExists()) SKIP("language pack directory not found");
    EnglishLocaleGuard restoreEnglish;
    LocalizationLoadDirectoryForTesting(LanguageDirectory(), L"fr");
    const std::vector<LanguageInfo>& languages = LocalizationAvailableLanguages();
    REQUIRE_FALSE(languages.empty());
    CHECK(languages.front().code == L"en");
    bool hasFrench = false;
    for (const LanguageInfo& language : languages)
    {
        if (language.code == L"fr") hasFrench = true;
    }
    CHECK(hasFrench);
}
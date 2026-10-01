#pragma once

// Runtime localization for the viewer GUI.
//
// Design: every user-facing string is authored in place as an English literal
// fallback — `Loc("key", L"English text")` — so the application always renders
// correct English even when no language pack is present, and translators have a
// stable, greppable key to target. Non-English strings live in UTF-8 JSON
// packs under `lang\<code>.json` beside the executable; a pack is a flat
// `{ "key": "translated text" }` object. External data files (not satellite
// resource DLLs) are deliberate: the release deliberately signs only its own
// executable images, and Smart App Control would treat per-language DLLs as
// unsigned binaries.
//
// Threading: packs are loaded and swapped on the UI thread only. The render
// thread never calls Loc()/LocFormat() directly; localized overlay strings are
// resolved on the UI thread and carried to it in OverlayInfo.

#include <initializer_list>
#include <string>
#include <vector>

struct LanguageInfo
{
    std::wstring code;        // pack file stem, e.g. "en", "pt-BR", "zh_Hans"
    std::wstring displayName; // endonym, e.g. "Português (Brasil)"
};

// Scans the executable directory for language packs and selects the OS UI
// language. Must run before any Loc()/LocFormat() call (i.e. before the first
// window is created). Safe to call once; a language pack added after this point
// is picked up by a later LocalizationSetLanguage match.
void LocalizationInitialize();

// Activates a pack. An empty string, an unknown code, or an unreadable pack
// selects the OS UI language (falling back to English). Missing keys always
// fall back to the English literal at the call site.
void LocalizationSetLanguage(const std::wstring& code);
const std::wstring& LocalizationLanguage();

// Test seam: rescans packs from an explicit directory (rather than the
// executable's own `lang\`) and activates `code`. Returns true when a pack was
// loaded. The application never calls this; the unit suite uses it to exercise
// the loader and parser against the shipped interactive-viewer/lang tree.
bool LocalizationLoadDirectoryForTesting(const std::wstring& directory, const std::wstring& code);

// The BCP-47-ish code chosen for the current OS UI language, matched against
// the installed packs. Returns "en" when nothing matches.
std::wstring LocalizationSystemLanguage();

// Installed packs, English first, then the rest by code. Rebuilt by
// LocalizationInitialize() and after a failed SetLanguage.
const std::vector<LanguageInfo>& LocalizationAvailableLanguages();
std::wstring LocalizationDisplayName(const std::wstring& code);

// Translated string for `key`, or the English `fallback` literal when the
// active pack does not define it. Returns by value so a caller on the render
// thread (e.g. a graphics failure path) can never hold a reference across a
// concurrent language change.
std::wstring Loc(const char* key, const wchar_t* fallback);

// As Loc(), with positional `{0}`, `{1}`, ... placeholders replaced from
// `args`. Use for any sentence a translation may need to reorder.
std::wstring LocFormat(const char* key, const wchar_t* fallback,
    std::initializer_list<std::wstring> args);

// Joins a localized fragment to surrounding text with exactly one space,
// trimming any leading/trailing whitespace the fragment carries. Localized
// strings must never rely on significant surrounding whitespace: translators
// and machine translation routinely add or drop it (a `" units"` fallback came
// back as `"unidades"`, rendering `29.092unidades`). Route every join between a
// localized fragment and adjacent text through here, and write the fallback
// without the separator space.
std::wstring LocalizedJoin(const std::wstring& prefix, const std::wstring& fragment);
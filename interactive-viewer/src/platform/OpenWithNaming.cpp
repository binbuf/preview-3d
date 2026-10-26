#include "OpenWithNaming.h"

#include <cwctype>

namespace open_with
{
namespace
{
std::wstring Fold(std::wstring text)
{
    for (auto& character : text) character = static_cast<wchar_t>(towlower(character));
    return text;
}

std::wstring Sanitize(std::wstring text)
{
    std::wstring result;
    result.reserve(text.size());
    bool pendingSpace = false;
    for (wchar_t character : text)
    {
        if (character < L' ' || character == 0x7f) character = L' ';
        if (character == L' ')
        {
            pendingSpace = !result.empty();
            continue;
        }
        if (pendingSpace)
        {
            result.push_back(L' ');
            pendingSpace = false;
        }
        result.push_back(character);
    }
    return result;
}

std::wstring FileNamePart(const std::wstring& path)
{
    const std::size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

bool EndsWithExe(const std::wstring& text)
{
    if (text.size() < 4) return false;
    return Fold(text.substr(text.size() - 4)) == L".exe";
}

bool EqualsFold(const std::wstring& left, const std::wstring& right)
{
    return Fold(left) == Fold(right);
}

std::wstring StemPart(const std::wstring& path)
{
    std::wstring name = FileNamePart(path);
    const std::size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

std::wstring StemKey(const std::wstring& text)
{
    std::wstring key = Fold(text);
    if (EndsWithExe(key)) key.resize(key.size() - 4);
    std::erase_if(key, [](wchar_t character) {
        return character == L' ' || character == L'-' || character == L'_' || character == L'.';
    });
    return key;
}

bool AllLowercase(const std::wstring& text)
{
    bool hasAlpha = false;
    for (wchar_t character : text)
    {
        if (std::iswalpha(character))
        {
            hasAlpha = true;
            if (std::iswupper(character)) return false;
        }
    }
    return hasAlpha;
}

std::wstring TitleCaseStem(std::wstring text)
{
    bool startOfWord = true;
    for (wchar_t& character : text)
    {
        if (character == L' ' || character == L'-' || character == L'_')
        {
            character = L' ';
            startOfWord = true;
            continue;
        }
        if (startOfWord && std::iswalpha(character))
        {
            character = static_cast<wchar_t>(towupper(character));
            startOfWord = false;
        }
    }
    return text;
}
}

bool LooksLikeFileName(const std::wstring& uiName, const std::wstring& handlerName)
{
    const std::wstring name = Sanitize(uiName);
    if (name.empty()) return true;
    if (EndsWithExe(name)) return true;
    if (name.find(L'\\') != std::wstring::npos || name.find(L'/') != std::wstring::npos) return true;

    const std::wstring fileName = FileNamePart(handlerName);
    if (!fileName.empty() && EqualsFold(name, fileName)) return true;

    // Some handlers return the bare stem ("f3d") instead of the file name
    // ("f3d.exe"). Treat an all-lowercase stem match as a file name too, but
    // leave intentionally cased product names such as "BambuStudio" alone.
    return AllLowercase(name) && StemKey(name) == StemKey(fileName);
}

std::wstring FriendlyFileStem(const std::wstring& handlerName)
{
    std::wstring stem = Sanitize(StemPart(handlerName));
    if (stem.empty()) return L"Unknown app";

    bool allLowercase = true;
    bool hasAlpha = false;
    bool hasDigit = false;
    for (wchar_t character : stem)
    {
        if (std::iswdigit(character)) hasDigit = true;
        if (std::iswalpha(character))
        {
            hasAlpha = true;
            if (std::iswupper(character)) { allLowercase = false; break; }
        }
    }
    if (hasAlpha && allLowercase && hasDigit && stem.size() <= 5)
    {
        for (wchar_t& character : stem)
            if (std::iswalpha(character)) character = static_cast<wchar_t>(towupper(character));
        return stem;
    }

    if (hasAlpha && allLowercase)
    {
        // Turn separators into spaces before title-casing so "bambu-studio"
        // becomes "Bambu Studio" rather than "Bambu-studio".
        std::wstring spaced;
        spaced.reserve(stem.size());
        for (wchar_t character : stem)
        {
            if (character == L'-' || character == L'_') character = L' ';
            spaced.push_back(character);
        }
        return TitleCaseStem(std::move(spaced));
    }
    return stem;
}

std::wstring NormalizeDisplayName(const std::wstring& uiName, const std::wstring& handlerName,
    const std::wstring& fileDescription, const std::wstring& productName)
{
    std::wstring name = Sanitize(uiName);
    if (!LooksLikeFileName(name, handlerName)) return name;

    for (const std::wstring* candidate : { &fileDescription, &productName })
    {
        std::wstring value = Sanitize(*candidate);
        if (!value.empty() && !EndsWithExe(value) &&
            value.find(L'\\') == std::wstring::npos && value.find(L'/') == std::wstring::npos &&
            !LooksLikeFileName(value, handlerName))
        {
            return value;
        }
    }
    return FriendlyFileStem(handlerName);
}
}

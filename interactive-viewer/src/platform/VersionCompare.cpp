#include "VersionCompare.h"

#include <algorithm>
#include <cwctype>
#include <limits>

namespace preview3d_update
{
namespace
{
struct ParsedVersion
{
    int core[3]{ 0, 0, 0 };
    std::wstring prerelease;
};

bool IsAsciiDigit(wchar_t character)
{
    return character >= L'0' && character <= L'9';
}

bool IsAsciiAlphaNumeric(wchar_t character)
{
    return IsAsciiDigit(character) || (character >= L'a' && character <= L'z') ||
        (character >= L'A' && character <= L'Z');
}

bool ParseCoreNumber(const std::wstring& text, int& result)
{
    if (text.empty()) return false;
    long long value = 0;
    for (const wchar_t character : text)
    {
        if (!IsAsciiDigit(character)) return false;
        value = value * 10 + (character - L'0');
        if (value > std::numeric_limits<int>::max()) return false;
    }
    result = static_cast<int>(value);
    return true;
}

bool ParseVersion(const std::wstring& text, ParsedVersion& parsed)
{
    std::wstring value = text;
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](wchar_t character) { return std::iswspace(character) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](wchar_t character) { return std::iswspace(character) != 0; }).base();
    if (first >= last) return false;
    value = std::wstring(first, last);

    // Tolerate a UTF-8 BOM that a Windows editor may have written into the
    // published version.txt.
    if (!value.empty() && value.front() == 0xFEFF) value.erase(value.begin());
    if (!value.empty() && (value.front() == L'v' || value.front() == L'V'))
    {
        value.erase(value.begin());
    }
    if (value.empty()) return false;

    // Build metadata never affects precedence.
    if (const std::size_t plus = value.find(L'+'); plus != std::wstring::npos)
    {
        value.erase(plus);
    }

    if (const std::size_t dash = value.find(L'-'); dash != std::wstring::npos)
    {
        parsed.prerelease = value.substr(dash + 1);
        value.erase(dash);
        if (parsed.prerelease.empty()) return false;
        for (const wchar_t character : parsed.prerelease)
        {
            if (!IsAsciiAlphaNumeric(character) && character != L'.' && character != L'-') return false;
        }
    }
    if (value.empty()) return false;

    int component = 0;
    std::size_t start = 0;
    while (start <= value.size())
    {
        if (component >= 3) return false;
        const std::size_t dot = value.find(L'.', start);
        const std::wstring part = value.substr(start, dot == std::wstring::npos ? std::wstring::npos : dot - start);
        if (!ParseCoreNumber(part, parsed.core[component])) return false;
        ++component;
        if (dot == std::wstring::npos) break;
        start = dot + 1;
    }
    return component >= 1;
}

int ComparePrerelease(const std::wstring& left, const std::wstring& right)
{
    if (left.empty() && right.empty()) return 0;
    if (left.empty()) return 1;    // A release outranks any prerelease.
    if (right.empty()) return -1;

    std::size_t leftStart = 0;
    std::size_t rightStart = 0;
    for (;;)
    {
        const std::size_t leftDot = left.find(L'.', leftStart);
        const std::size_t rightDot = right.find(L'.', rightStart);
        const std::wstring leftPart = left.substr(leftStart,
            leftDot == std::wstring::npos ? std::wstring::npos : leftDot - leftStart);
        const std::wstring rightPart = right.substr(rightStart,
            rightDot == std::wstring::npos ? std::wstring::npos : rightDot - rightStart);

        const bool leftNumeric = !leftPart.empty() &&
            std::all_of(leftPart.begin(), leftPart.end(), IsAsciiDigit);
        const bool rightNumeric = !rightPart.empty() &&
            std::all_of(rightPart.begin(), rightPart.end(), IsAsciiDigit);
        if (leftNumeric && rightNumeric)
        {
            // Compare digit strings without converting: a malicious or
            // careless prerelease may contain more digits than long long.
            const std::wstring leftTrimmed = leftPart.find_first_not_of(L'0') == std::wstring::npos
                ? L"0" : leftPart.substr(leftPart.find_first_not_of(L'0'));
            const std::wstring rightTrimmed = rightPart.find_first_not_of(L'0') == std::wstring::npos
                ? L"0" : rightPart.substr(rightPart.find_first_not_of(L'0'));
            if (leftTrimmed.size() != rightTrimmed.size()) return leftTrimmed.size() < rightTrimmed.size() ? -1 : 1;
            if (leftTrimmed != rightTrimmed) return leftTrimmed < rightTrimmed ? -1 : 1;
        }
        else if (leftNumeric != rightNumeric)
        {
            // Numeric identifiers always have lower precedence than alphanumeric ones.
            return leftNumeric ? -1 : 1;
        }
        else if (leftPart != rightPart)
        {
            return leftPart < rightPart ? -1 : 1;
        }

        const bool leftDone = leftDot == std::wstring::npos;
        const bool rightDone = rightDot == std::wstring::npos;
        if (leftDone && rightDone) return 0;
        if (leftDone) return -1;
        if (rightDone) return 1;
        leftStart = leftDot + 1;
        rightStart = rightDot + 1;
    }
}
}

std::wstring NormalizeVersion(const std::wstring& text)
{
    ParsedVersion parsed;
    if (!ParseVersion(text, parsed)) return {};
    std::wstring normalized = std::to_wstring(parsed.core[0]) + L"." +
        std::to_wstring(parsed.core[1]) + L"." + std::to_wstring(parsed.core[2]);
    if (!parsed.prerelease.empty()) normalized += L"-" + parsed.prerelease;
    return normalized;
}

bool IsVersionNewer(const std::wstring& candidate, const std::wstring& current)
{
    ParsedVersion candidateVersion;
    ParsedVersion currentVersion;
    if (!ParseVersion(candidate, candidateVersion) || !ParseVersion(current, currentVersion))
    {
        return false;
    }

    for (int index = 0; index < 3; ++index)
    {
        if (candidateVersion.core[index] != currentVersion.core[index])
        {
            return candidateVersion.core[index] > currentVersion.core[index];
        }
    }
    return ComparePrerelease(candidateVersion.prerelease, currentVersion.prerelease) > 0;
}

std::wstring ExtractGitHubReleaseTag(const std::wstring& json)
{
    // Search for the single field the update check needs. GitHub's pretty
    // printed or compact JSON both contain the key exactly once.
    const std::wstring key = L"\"tag_name\"";
    std::size_t position = json.find(key);
    if (position == std::wstring::npos) return {};
    position += key.size();
    while (position < json.size() && std::iswspace(json[position]) != 0) ++position;
    if (position >= json.size() || json[position] != L':') return {};
    ++position;
    while (position < json.size() && std::iswspace(json[position]) != 0) ++position;
    if (position >= json.size() || json[position] != L'"') return {};
    ++position;
    std::wstring tag;
    for (; position < json.size(); ++position)
    {
        const wchar_t character = json[position];
        if (character == L'"') return tag;
        if (character == L'\\')
        {
            // Version tags never need escapes; reject rather than decode
            // arbitrary JSON into the displayed value.
            return {};
        }
        if (character < 0x20) return {};
        tag.push_back(character);
        if (tag.size() > 128) return {};
    }
    return {};
}
}

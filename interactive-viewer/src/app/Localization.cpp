#include "framework.h"
#include "Localization.h"

#include <cwctype>
#include <mutex>
#include <unordered_map>

namespace
{
// A language pack is a flat key -> translated-text map; nested objects are not
// part of the format. Missing keys fall back to the call site's English.
struct Pack
{
    std::unordered_map<std::string, std::wstring> strings;
};

Pack g_pack;
std::unordered_map<const char*, std::wstring> g_fallbacks;
std::wstring g_langDirectory;
std::wstring g_current = L"en";
std::vector<LanguageInfo> g_languages;
// Guards g_pack/g_fallbacks against the render thread reading a string whose
// creation path (a graphics failure during load) races a UI-thread language
// change. Initialization and language selection also take it.
std::mutex g_lookupMutex;

struct KnownLanguage
{
    const wchar_t* code;
    const wchar_t* displayName;
};

// Endonyms for every shipped pack. Kept independent of the code list so a
// pack dropped in by a translator still appears (its code is its name) while a
// known one shows properly.
constexpr KnownLanguage kKnownLanguages[] = {
    { L"en",      L"English" },
    { L"en-GB",   L"English (United Kingdom)" },
    { L"ar",      L"العربية" },
    { L"az",      L"Azərbaycan dili" },
    { L"bn",      L"বাংলা" },
    { L"ckb",     L"کوردیی ناوەندی" },
    { L"cs",      L"Čeština" },
    { L"de",      L"Deutsch" },
    { L"el",      L"Ελληνικά" },
    { L"es",      L"Español" },
    { L"fa",      L"فارسی" },
    { L"fi",      L"Suomi" },
    { L"fr",      L"Français" },
    { L"he",      L"עברית" },
    { L"hi",      L"हिन्दी" },
    { L"hr",      L"Hrvatski" },
    { L"hu",      L"Magyar" },
    { L"id",      L"Bahasa Indonesia" },
    { L"it",      L"Italiano" },
    { L"ja",      L"日本語" },
    { L"ko",      L"한국어" },
    { L"lt",      L"Lietuvių" },
    { L"nb",      L"Norsk bokmål" },
    { L"nl",      L"Nederlands" },
    { L"pl",      L"Polski" },
    { L"pt",      L"Português" },
    { L"pt-BR",   L"Português (Brasil)" },
    { L"ro",      L"Română" },
    { L"ru",      L"Русский" },
    { L"sk",      L"Slovenčina" },
    { L"sv",      L"Svenska" },
    { L"ta",      L"தமிழ்" },
    { L"th",      L"ไทย" },
    { L"tr",      L"Türkçe" },
    { L"uk",      L"Українська" },
    { L"vi",      L"Tiếng Việt" },
    { L"zh-Hant", L"繁體中文" },
    { L"zh_Hans", L"简体中文" },
};

std::wstring ToLowerAscii(const std::wstring& value)
{
    std::wstring lowered = value;
    for (wchar_t& character : lowered)
    {
        if (character >= L'A' && character <= L'Z') character = static_cast<wchar_t>(character + (L'a' - L'A'));
    }
    return lowered;
}

std::wstring BaseLanguage(const std::wstring& code)
{
    const std::size_t separator = code.find_first_of(L"-_");
    return separator == std::wstring::npos ? code : code.substr(0, separator);
}

const wchar_t* KnownDisplayName(const std::wstring& code)
{
    for (const KnownLanguage& known : kKnownLanguages)
    {
        if (code == known.code) return known.displayName;
    }
    return nullptr;
}

bool ReadFileUtf8(const std::wstring& path, std::string& outContent)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    // A pack is bounded to keep a malformed or malicious file from being read
    // wholesale; the largest real packs are well under this.
    constexpr LONGLONG kMaximumPackBytes = 2 * 1024 * 1024;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > kMaximumPackBytes)
    {
        CloseHandle(file);
        return false;
    }
    outContent.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(file, outContent.data(), static_cast<DWORD>(outContent.size()), &bytesRead, nullptr);
    CloseHandle(file);
    return ok && bytesRead == outContent.size();
}

void AppendUtf8(std::string& buffer, std::uint32_t codePoint)
{
    if (codePoint <= 0x7F)
    {
        buffer.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint <= 0x7FF)
    {
        buffer.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        buffer.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else if (codePoint <= 0xFFFF)
    {
        buffer.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        buffer.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        buffer.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else
    {
        buffer.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        buffer.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        buffer.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        buffer.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

int HexDigit(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

// Minimal, purpose-built JSON scanner for the flat pack shape. It validates
// structure as it goes and rejects anything unexpected rather than guessing.
class JsonReader
{
public:
    explicit JsonReader(const std::string& text) : text_(text) {}

    bool ParseFlatObject(std::unordered_map<std::string, std::wstring>& output)
    {
        // A UTF-8 BOM is tolerated (some editors add one).
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB && static_cast<unsigned char>(text_[2]) == 0xBF)
        {
            position_ = 3;
        }
        SkipWhitespace();
        if (!Take('{')) return false;
        SkipWhitespace();
        if (Peek() == '}') { ++position_; return true; }
        while (true)
        {
            std::string key;
            if (!ReadString(key)) return false;
            SkipWhitespace();
            if (!Take(':')) return false;
            SkipWhitespace();
            std::string value;
            if (!ReadString(value)) return false;
            output[key] = Utf8ToWide(value);
            SkipWhitespace();
            if (Take(',')) { SkipWhitespace(); continue; }
            if (Take('}')) return true;
            return false;
        }
    }

private:
    char Peek() const { return position_ < text_.size() ? text_[position_] : '\0'; }

    void SkipWhitespace()
    {
        while (position_ < text_.size())
        {
            const char character = text_[position_];
            if (character == ' ' || character == '\t' || character == '\r' || character == '\n') ++position_;
            else break;
        }
    }

    bool Take(char expected)
    {
        if (Peek() != expected) return false;
        ++position_;
        return true;
    }

    bool ReadString(std::string& output)
    {
        if (!Take('"')) return false;
        output.clear();
        while (position_ < text_.size())
        {
            const char character = text_[position_++];
            if (character == '"') return true;
            if (character != '\\')
            {
                output.push_back(character);
                continue;
            }
            if (position_ >= text_.size()) return false;
            const char escaped = text_[position_++];
            switch (escaped)
            {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u':
            {
                std::uint32_t codePoint = 0;
                if (!ReadHex4(codePoint)) return false;
                // Combine a UTF-16 surrogate pair when present.
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF && position_ + 1 < text_.size() &&
                    text_[position_] == '\\' && text_[position_ + 1] == 'u')
                {
                    position_ += 2;
                    std::uint32_t low = 0;
                    if (!ReadHex4(low)) return false;
                    if (low >= 0xDC00 && low <= 0xDFFF)
                        codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                    else
                        AppendUtf8(output, low), codePoint = 0xFFFD;
                }
                AppendUtf8(output, codePoint);
                break;
            }
            default: return false;
            }
        }
        return false; // unterminated
    }

    bool ReadHex4(std::uint32_t& output)
    {
        if (position_ + 4 > text_.size()) return false;
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index)
        {
            const int digit = HexDigit(text_[position_ + index]);
            if (digit < 0) return false;
            value = (value << 4) | static_cast<std::uint32_t>(digit);
        }
        position_ += 4;
        output = value;
        return true;
    }

    static std::wstring Utf8ToWide(const std::string& input)
    {
        if (input.empty()) return {};
        const int length = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
        if (length <= 0) return {};
        std::wstring output(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), length);
        return output;
    }

    const std::string& text_;
    std::size_t position_ = 0;
};

bool LoadPack(const std::wstring& code, Pack& output)
{
    if (g_langDirectory.empty()) return false;
    std::string content;
    if (!ReadFileUtf8(g_langDirectory + L"\\" + code + L".json", content)) return false;
    Pack parsed;
    JsonReader reader(content);
    if (!reader.ParseFlatObject(parsed.strings) || parsed.strings.empty()) return false;
    output = std::move(parsed);
    return true;
}

// Finds the installed pack whose code best matches a locale name such as
// "zh-CN" or "pt-BR".
std::wstring MatchInstalled(const std::wstring& locale)
{
    const std::wstring lowered = ToLowerAscii(locale);
    auto isInstalled = [](const std::wstring& code)
    {
        const std::wstring target = ToLowerAscii(code);
        for (const LanguageInfo& language : g_languages)
        {
            if (ToLowerAscii(language.code) == target) return true;
        }
        return false;
    };
    auto installedCode = [](const std::wstring& code) -> std::wstring
    {
        const std::wstring target = ToLowerAscii(code);
        for (const LanguageInfo& language : g_languages)
        {
            if (ToLowerAscii(language.code) == target) return language.code;
        }
        return {};
    };

    if (isInstalled(lowered)) return installedCode(lowered);

    if (lowered.rfind(L"zh", 0) == 0)
    {
        const bool traditional = lowered.find(L"hant") != std::wstring::npos ||
            lowered.find(L"-tw") != std::wstring::npos || lowered.find(L"-hk") != std::wstring::npos ||
            lowered.find(L"-mo") != std::wstring::npos;
        const std::wstring preferred = traditional ? L"zh-Hant" : L"zh_Hans";
        if (isInstalled(preferred)) return installedCode(preferred);
    }

    std::wstring base = BaseLanguage(lowered);
    // Norwegian locales share one bokmål pack.
    if (base == L"no" || base == L"nn") base = L"nb";
    if (isInstalled(base)) return installedCode(base);

    if (isInstalled(L"en")) return installedCode(L"en");
    return g_languages.empty() ? L"en" : g_languages.front().code;
}

void RebuildLanguageList()
{
    g_languages.clear();
    WIN32_FIND_DATAW entry{};
    const std::wstring pattern = g_langDirectory.empty() ? std::wstring() : g_langDirectory + L"\\*.json";
    HANDLE find = pattern.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW(pattern.c_str(), &entry);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                std::wstring name = entry.cFileName;
                const std::size_t dot = name.rfind(L".json");
                if (dot != std::wstring::npos) name.erase(dot);
                if (name.empty()) continue;
                LanguageInfo info;
                info.code = name;
                if (const wchar_t* known = KnownDisplayName(name)) info.displayName = known;
                else info.displayName = name;
                g_languages.push_back(std::move(info));
            }
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }

    std::sort(g_languages.begin(), g_languages.end(), [](const LanguageInfo& left, const LanguageInfo& right)
    {
        if (left.code == L"en") return right.code != L"en";
        if (right.code == L"en") return false;
        return left.code < right.code;
    });

    // English is always offered, even if its pack file was removed, so the
    // selector can always return to the built-in fallback strings.
    if (g_languages.empty())
    {
        g_languages.push_back({ L"en", KnownDisplayName(L"en") });
    }
}
}

void LocalizationInitialize()
{
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));
    if (length > 0 && length < ARRAYSIZE(modulePath))
    {
        std::wstring directory(modulePath, length);
        const std::size_t slash = directory.find_last_of(L'\\');
        if (slash != std::wstring::npos) directory.erase(slash);
        g_langDirectory = directory + L"\\lang";
    }

    RebuildLanguageList();

    // Select the OS UI language; LocalizationSetLanguage() may override it with
    // the persisted user preference once settings are available.
    LocalizationSetLanguage(LocalizationSystemLanguage());
}

bool LocalizationLoadDirectoryForTesting(const std::wstring& directory, const std::wstring& code)
{
    g_langDirectory = directory;
    while (!g_langDirectory.empty() &&
        (g_langDirectory.back() == L'\\' || g_langDirectory.back() == L'/'))
    {
        g_langDirectory.pop_back();
    }
    RebuildLanguageList();
    LocalizationSetLanguage(code);
    std::lock_guard<std::mutex> guard(g_lookupMutex);
    return !g_pack.strings.empty();
}

void LocalizationSetLanguage(const std::wstring& code)
{
    std::wstring requested = code;
    if (requested.empty()) requested = LocalizationSystemLanguage();

    const std::wstring matched = MatchInstalled(requested);
    Pack pack;
    const bool loaded = LoadPack(matched, pack);
    std::lock_guard<std::mutex> guard(g_lookupMutex);
    if (loaded)
    {
        g_pack = std::move(pack);
        g_current = matched;
    }
    else
    {
        // No usable pack (English is compiled into the call sites): clear any
        // previous pack so every key falls back to its English literal.
        g_pack.strings.clear();
        g_current = matched.empty() ? L"en" : matched;
    }
    g_fallbacks.clear();
}

const std::wstring& LocalizationLanguage()
{
    return g_current;
}

std::wstring LocalizationSystemLanguage()
{
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) return L"en";
    return MatchInstalled(locale);
}

const std::vector<LanguageInfo>& LocalizationAvailableLanguages()
{
    return g_languages;
}

std::wstring LocalizationDisplayName(const std::wstring& code)
{
    if (const wchar_t* known = KnownDisplayName(code)) return known;
    return code;
}

std::wstring Loc(const char* key, const wchar_t* fallback)
{
    std::lock_guard<std::mutex> guard(g_lookupMutex);
    const auto found = g_pack.strings.find(key);
    if (found != g_pack.strings.end()) return found->second;
    std::wstring& slot = g_fallbacks[key];
    if (slot.empty() && fallback && *fallback) slot = fallback;
    return slot;
}

std::wstring LocalizedJoin(const std::wstring& prefix, const std::wstring& fragment)
{
    std::size_t begin = 0;
    std::size_t end = fragment.size();
    while (begin < end && (fragment[begin] == L' ' || fragment[begin] == L'\t')) ++begin;
    while (end > begin && (fragment[end - 1] == L' ' || fragment[end - 1] == L'\t')) --end;
    if (begin == end) return prefix;

    std::wstring joined = prefix;
    if (!joined.empty() && joined.back() != L' ') joined.push_back(L' ');
    joined.append(fragment, begin, end - begin);
    return joined;
}

std::wstring LocFormat(const char* key, const wchar_t* fallback, std::initializer_list<std::wstring> args)
{
    const std::wstring text = Loc(key, fallback);
    std::wstring output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        const wchar_t character = text[index];
        if (character == L'{' && index + 2 < text.size() && text[index + 2] == L'}' &&
            text[index + 1] >= L'0' && text[index + 1] <= L'9')
        {
            const std::size_t argument = static_cast<std::size_t>(text[index + 1] - L'0');
            if (argument < args.size())
            {
                output += *(args.begin() + static_cast<std::ptrdiff_t>(argument));
                index += 2;
                continue;
            }
        }
        output.push_back(character);
    }
    return output;
}
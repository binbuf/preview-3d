#include "OpenWithCache.h"

#include <windows.h>

#include <array>
#include <cwctype>
#include <limits>
#include <unordered_set>

namespace open_with
{
namespace
{
constexpr std::uint32_t kMagic = 0x3143574f; // "OWC1" in little endian.
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 24;
constexpr std::size_t kMaximumCacheBytes = 256 * 1024;
constexpr std::size_t kMaximumHandlers = 256;
constexpr std::size_t kMaximumExtensionBytes = 16;
constexpr std::size_t kMaximumHandlerNameBytes = 2048;
constexpr std::size_t kMaximumDisplayNameBytes = 1024;
constexpr std::size_t kMaximumCatalogIdBytes = 128;

void AppendU16(std::vector<std::byte>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::byte>(value & 0xff));
    bytes.push_back(static_cast<std::byte>((value >> 8) & 0xff));
}

void AppendU32(std::vector<std::byte>& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
}

void AppendU64(std::vector<std::byte>& bytes, std::uint64_t value)
{
    for (unsigned shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
}

bool ReadU16(std::span<const std::byte> bytes, std::size_t& cursor, std::uint16_t& value)
{
    if (cursor > bytes.size() || bytes.size() - cursor < 2) return false;
    value = static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[cursor])) |
        static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[cursor + 1]) << 8);
    cursor += 2;
    return true;
}

bool ReadU32(std::span<const std::byte> bytes, std::size_t& cursor, std::uint32_t& value)
{
    if (cursor > bytes.size() || bytes.size() - cursor < 4) return false;
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[cursor++])) << shift;
    return true;
}

bool ReadU64(std::span<const std::byte> bytes, std::size_t& cursor, std::uint64_t& value)
{
    if (cursor > bytes.size() || bytes.size() - cursor < 8) return false;
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8)
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[cursor++])) << shift;
    return true;
}

bool WideToUtf8(const std::wstring& text, std::string& output)
{
    output.clear();
    if (text.empty()) return true;
    if (text.find(L'\0') != std::wstring::npos) return false;
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return false;
    output.resize(static_cast<std::size_t>(count));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), output.data(), count, nullptr, nullptr) == count;
}

bool Utf8ToWide(std::span<const std::byte> bytes, std::wstring& output)
{
    output.clear();
    if (bytes.empty()) return true;
    if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
    const char* data = reinterpret_cast<const char*>(bytes.data());
    const int byteCount = static_cast<int>(bytes.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, byteCount, nullptr, 0);
    if (count <= 0) return false;
    output.resize(static_cast<std::size_t>(count));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, byteCount, output.data(), count) != count)
        return false;
    return output.find(L'\0') == std::wstring::npos;
}

bool AppendString(std::vector<std::byte>& bytes, const std::wstring& text, std::size_t maximum)
{
    std::string utf8;
    if (!WideToUtf8(text, utf8) || utf8.size() > maximum ||
        utf8.size() > (std::numeric_limits<std::uint16_t>::max)()) return false;
    AppendU16(bytes, static_cast<std::uint16_t>(utf8.size()));
    bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(utf8.data()),
        reinterpret_cast<const std::byte*>(utf8.data() + utf8.size()));
    return bytes.size() <= kMaximumCacheBytes;
}

bool ReadString(std::span<const std::byte> bytes, std::size_t& cursor, std::size_t maximum,
    std::wstring& output)
{
    std::uint16_t length = 0;
    if (!ReadU16(bytes, cursor, length) || length > maximum || cursor > bytes.size() ||
        bytes.size() - cursor < length) return false;
    const auto encoded = bytes.subspan(cursor, length);
    cursor += length;
    return Utf8ToWide(encoded, output);
}

std::wstring Fold(std::wstring text)
{
    for (auto& character : text) character = static_cast<wchar_t>(towlower(character));
    return text;
}
}

bool EncodeCache(const CacheState& state, std::vector<std::byte>& output)
{
    output.clear();
    if (state.handlers.size() > kMaximumHandlers) return false;
    output.reserve(kHeaderBytes + state.handlers.size() * 128);
    AppendU32(output, kMagic);
    AppendU16(output, kVersion);
    AppendU16(output, static_cast<std::uint16_t>(state.handlers.size()));
    AppendU32(output, state.catalogRevision);
    AppendU64(output, state.lastDiscoveryFileTime);
    AppendU32(output, 0); // Patched with the final byte count below.

    for (const auto& handler : state.handlers)
    {
        if (handler.extension.empty() || handler.handlerName.empty() || handler.displayName.empty() ||
            !AppendString(output, handler.extension, kMaximumExtensionBytes) ||
            !AppendString(output, handler.handlerName, kMaximumHandlerNameBytes) ||
            !AppendString(output, handler.displayName, kMaximumDisplayNameBytes) ||
            !AppendString(output, handler.catalogId, kMaximumCatalogIdBytes))
        {
            output.clear();
            return false;
        }
    }
    if (output.size() > kMaximumCacheBytes || output.size() > (std::numeric_limits<std::uint32_t>::max)())
    {
        output.clear();
        return false;
    }
    const auto size = static_cast<std::uint32_t>(output.size());
    for (unsigned byte = 0; byte < 4; ++byte)
        output[20 + byte] = static_cast<std::byte>((size >> (byte * 8)) & 0xff);
    return true;
}

bool DecodeCache(std::span<const std::byte> bytes, CacheState& output)
{
    output = {};
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaximumCacheBytes) return false;
    std::size_t cursor = 0;
    std::uint32_t magic = 0, catalogRevision = 0, encodedSize = 0;
    std::uint16_t version = 0, count = 0;
    std::uint64_t lastDiscovery = 0;
    if (!ReadU32(bytes, cursor, magic) || !ReadU16(bytes, cursor, version) ||
        !ReadU16(bytes, cursor, count) || !ReadU32(bytes, cursor, catalogRevision) ||
        !ReadU64(bytes, cursor, lastDiscovery) || !ReadU32(bytes, cursor, encodedSize) ||
        magic != kMagic || version != kVersion || count > kMaximumHandlers || encodedSize != bytes.size())
        return false;

    CacheState decoded;
    decoded.catalogRevision = catalogRevision;
    decoded.lastDiscoveryFileTime = lastDiscovery;
    decoded.handlers.reserve(count);
    std::unordered_set<std::wstring> identities;
    for (std::uint16_t index = 0; index < count; ++index)
    {
        CachedHandler handler;
        if (!ReadString(bytes, cursor, kMaximumExtensionBytes, handler.extension) ||
            !ReadString(bytes, cursor, kMaximumHandlerNameBytes, handler.handlerName) ||
            !ReadString(bytes, cursor, kMaximumDisplayNameBytes, handler.displayName) ||
            !ReadString(bytes, cursor, kMaximumCatalogIdBytes, handler.catalogId) ||
            handler.extension.empty() || handler.extension.front() != L'.' ||
            handler.handlerName.empty() || handler.displayName.empty()) return false;
        const std::wstring identity = Fold(handler.extension) + L"\n" + Fold(handler.handlerName);
        if (!identities.insert(identity).second) return false;
        decoded.handlers.push_back(std::move(handler));
    }
    if (cursor != bytes.size()) return false;
    output = std::move(decoded);
    return true;
}
}

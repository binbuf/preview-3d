#include "model_core/FormatSniffer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

namespace model_core {
namespace {

constexpr std::array<unsigned char, 4> kZipLocalHeader{ 0x50, 0x4b, 0x03, 0x04 };
constexpr std::array<unsigned char, 4> kZipEndOfCentralDirectory{ 0x50, 0x4b, 0x05, 0x06 };
constexpr std::array<unsigned char, 4> kZipCentralHeader{ 0x50, 0x4b, 0x01, 0x02 };

constexpr unsigned char FoldAscii(unsigned char value) noexcept
{
    return (value >= 'A' && value <= 'Z') ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

constexpr bool StartsWith(std::string_view value, std::string_view prefix) noexcept
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

constexpr bool StartsWithInsensitive(std::string_view value, std::string_view prefix) noexcept
{
    if (value.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (FoldAscii(static_cast<unsigned char>(value[index]))
            != FoldAscii(static_cast<unsigned char>(prefix[index]))) return false;
    }
    return true;
}

bool ContainsInsensitive(std::string_view haystack, std::string_view needle) noexcept
{
    if (needle.empty() || haystack.size() < needle.size()) return false;
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        bool equal = true;
        for (std::size_t index = 0; index < needle.size(); ++index) {
            if (FoldAscii(static_cast<unsigned char>(haystack[start + index]))
                != FoldAscii(static_cast<unsigned char>(needle[index]))) {
                equal = false;
                break;
            }
        }
        if (equal) return true;
    }
    return false;
}

std::string_view AsciiView(std::span<const std::byte> bytes) noexcept
{
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

std::uint16_t ReadLe16(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset]))
        | static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1]) << 8);
}

std::uint32_t ReadLe32(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset]))
        | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8)
        | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2])) << 16)
        | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3])) << 24);
}

bool HasPrefix(std::span<const std::byte> bytes, std::span<const unsigned char> prefix) noexcept
{
    if (bytes.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (std::to_integer<unsigned char>(bytes[index]) != prefix[index]) return false;
    }
    return true;
}

std::size_t SkipUtf8Bom(std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() >= 3
        && std::to_integer<unsigned char>(bytes[0]) == 0xEF
        && std::to_integer<unsigned char>(bytes[1]) == 0xBB
        && std::to_integer<unsigned char>(bytes[2]) == 0xBF) {
        return 3;
    }
    return 0;
}

std::size_t SkipWhitespace(std::string_view text, std::size_t start) noexcept
{
    while (start < text.size()) {
        const char value = text[start];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') break;
        ++start;
    }
    return start;
}

bool LooksLikeBinaryStl(std::span<const std::byte> head, uint64_t totalBytes) noexcept
{
    if (totalBytes < 84 || head.size() < 84) return false;
    const std::uint32_t facetCount = ReadLe32(head, 80);
    if (facetCount == 0) return false;
    const std::uint64_t needed = 84ull + 50ull * facetCount;
    if (needed > totalBytes) return false;
    // Trailing bytes are tolerated by the product parser; bound the tolerance
    // so an arbitrary binary file whose bytes 80..83 happen to be a small
    // number is not treated as an STL candidate.
    return totalBytes - needed <= 1024;
}

bool LooksLikeAsciiStl(std::string_view text, std::size_t start) noexcept
{
    if (start >= text.size()) return false;
    const std::string_view tail = text.substr(start);
    if (!StartsWithInsensitive(tail, "solid") || tail.size() < 5) return false;
    if (tail.size() > 5) {
        const char next = tail[5];
        if (next != ' ' && next != '\t' && next != '\r' && next != '\n') return false;
    }
    return ContainsInsensitive(tail, "facet") && ContainsInsensitive(tail, "endsolid");
}

bool LooksLikeGltfJson(std::string_view text, std::size_t start) noexcept
{
    if (start >= text.size() || text[start] != '{') return false;
    return ContainsInsensitive(text.substr(start), "\"asset\"")
        && ContainsInsensitive(text.substr(start), "\"version\"");
}

bool LineStartsWithKeyword(std::string_view line, std::string_view keyword) noexcept
{
    if (!StartsWith(line, keyword)) return false;
    return line.size() == keyword.size()
        || line[keyword.size()] == ' ' || line[keyword.size()] == '\t'
        || line[keyword.size()] == '\r';
}

bool LooksLikeObj(std::string_view text, std::size_t start) noexcept
{
    bool hasVertex = false;
    bool hasFace = false;
    bool hasPoint = false;
    std::size_t position = start;
    std::size_t lines = 0;
    while (position < text.size() && lines < 4096) {
        const std::size_t end = std::min(text.find('\n', position), text.size());
        std::string_view line = text.substr(position, end - position);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos) {
            position = end + (end < text.size() ? 1 : 0);
            continue;
        }
        line.remove_prefix(first);
        if (line.front() == '#') {
            position = end + (end < text.size() ? 1 : 0);
            continue;
        }
        if (LineStartsWithKeyword(line, "v")) hasVertex = true;
        else if (LineStartsWithKeyword(line, "f")) hasFace = true;
        else if (LineStartsWithKeyword(line, "p") || LineStartsWithKeyword(line, "l")) hasPoint = true;
        else if (!LineStartsWithKeyword(line, "vn") && !LineStartsWithKeyword(line, "vt")
            && !LineStartsWithKeyword(line, "vp") && !LineStartsWithKeyword(line, "o")
            && !LineStartsWithKeyword(line, "g") && !LineStartsWithKeyword(line, "s")
            && !LineStartsWithKeyword(line, "mtllib") && !LineStartsWithKeyword(line, "usemtl")) {
            // Not an OBJ statement: keep scanning a bounded number of lines so
            // an authored comment/export note before the geometry is tolerated.
        }
        ++lines;
        position = end + (end < text.size() ? 1 : 0);
    }
    return hasVertex && (hasFace || hasPoint);
}

bool ReadAt(std::ifstream& file, uint64_t offset, std::span<std::byte> output) noexcept
{
    file.clear();
    if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) return false;
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) return false;
    file.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    return file.gcount() == static_cast<std::streamsize>(output.size());
}

bool EntryEquals(std::string_view name, std::string_view expected) noexcept
{
    if (name.size() != expected.size()) return false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        if (FoldAscii(static_cast<unsigned char>(name[index]))
            != FoldAscii(static_cast<unsigned char>(expected[index]))) return false;
    }
    return true;
}

bool EntryHasSuffix(std::string_view name, std::string_view suffix) noexcept
{
    if (name.size() < suffix.size()) return false;
    return EntryEquals(name.substr(name.size() - suffix.size()), suffix);
}

SniffResult ProbeZipCentralDirectory(std::ifstream& file, uint64_t totalBytes) noexcept
{
    const uint64_t tailBytes = std::min<uint64_t>(totalBytes, kSniffEocdSearchBytes + 22ull);
    if (tailBytes < 22) return {};
    std::vector<std::byte> tail(static_cast<std::size_t>(tailBytes));
    if (!ReadAt(file, totalBytes - tailBytes, tail)) return {};

    std::size_t eocd = std::string_view::npos;
    for (std::size_t index = tail.size() - 22 + 1; index-- > 0;) {
        if (std::to_integer<unsigned char>(tail[index]) == kZipEndOfCentralDirectory[0]
            && std::to_integer<unsigned char>(tail[index + 1]) == kZipEndOfCentralDirectory[1]
            && std::to_integer<unsigned char>(tail[index + 2]) == kZipEndOfCentralDirectory[2]
            && std::to_integer<unsigned char>(tail[index + 3]) == kZipEndOfCentralDirectory[3]) {
            eocd = index;
            break;
        }
    }
    if (eocd == std::string_view::npos || eocd + 22 > tail.size()) return {};

    const auto span = std::span<const std::byte>(tail);
    const std::uint16_t diskNumber = ReadLe16(span, eocd + 4);
    const std::uint16_t centralDisk = ReadLe16(span, eocd + 6);
    const std::uint16_t entriesThisDisk = ReadLe16(span, eocd + 8);
    const std::uint16_t entries = ReadLe16(span, eocd + 10);
    const std::uint32_t centralBytes = ReadLe32(span, eocd + 12);
    const std::uint32_t centralOffset = ReadLe32(span, eocd + 16);
    const std::uint16_t commentBytes = ReadLe16(span, eocd + 20);

    // Reject ZIP64 and spanned/multi-disk archives rather than guessing a
    // truncated field. Sniffing is advisory; Unknown is always safe.
    if (diskNumber != 0 || centralDisk != 0 || entriesThisDisk != entries) return {};
    if (entries == 0 || entries > kSniffMaxZipEntries) return {};
    if (centralBytes == 0 || centralBytes > kSniffMaxCentralDirectoryBytes) return {};
    if (centralBytes == std::numeric_limits<std::uint32_t>::max()
        || centralOffset == std::numeric_limits<std::uint32_t>::max()) return {};
    if (centralOffset > totalBytes || centralBytes > totalBytes - centralOffset) return {};
    if (eocd + 22ull + commentBytes > tail.size()) return {};

    std::vector<std::byte> directory(centralBytes);
    if (!ReadAt(file, centralOffset, directory)) return {};

    bool threeMf = false;
    bool usd = false;
    std::size_t offset = 0;
    const auto directorySpan = std::span<const std::byte>(directory);
    for (std::uint16_t entry = 0; entry < entries; ++entry) {
        if (offset > directory.size() || directory.size() - offset < 46) return {};
        for (std::size_t index = 0; index < kZipCentralHeader.size(); ++index) {
            if (std::to_integer<unsigned char>(directory[offset + index]) != kZipCentralHeader[index]) return {};
        }
        const std::uint16_t nameBytes = ReadLe16(directorySpan, offset + 28);
        const std::uint16_t extraBytes = ReadLe16(directorySpan, offset + 30);
        const std::uint16_t commentBytesEntry = ReadLe16(directorySpan, offset + 32);
        const std::size_t next = offset + 46 + nameBytes + extraBytes + commentBytesEntry;
        if (next > directory.size() || next < offset) return {};
        const std::string_view name(
            reinterpret_cast<const char*>(directory.data()) + offset + 46, nameBytes);
        if (EntryEquals(name, "[Content_Types].xml")) threeMf = true;
        if (EntryHasSuffix(name, ".usdc") || EntryHasSuffix(name, ".usda")
            || EntryHasSuffix(name, ".usd")) {
            usd = true;
        }
        offset = next;
    }

    if (threeMf) return { FormatId::ThreeMf, SniffConfidence::Strong, "zip-content-types" };
    if (usd) return { FormatId::Usdz, SniffConfidence::Strong, "zip-usd-layer" };
    return {};
}

bool IsZipHead(std::span<const std::byte> head) noexcept
{
    return HasPrefix(head, kZipLocalHeader);
}

} // namespace

SniffResult SniffHead(std::span<const std::byte> head, uint64_t totalBytes) noexcept
{
    if (head.empty()) return {};

    // GLB is the only format whose magic is followed by a structural header the
    // sniffer can check itself. A truncated or version-mismatched GLB must not
    // fall through to a weaker textual heuristic.
    if (HasPrefix(head, std::span<const unsigned char>(
            reinterpret_cast<const unsigned char*>("glTF"), 4))) {
        if (totalBytes < 12 || head.size() < 12) {
            return { FormatId::Unknown, SniffConfidence::Weak, "glb-header-truncated" };
        }
        const std::uint32_t version = ReadLe32(head, 4);
        const std::uint32_t declared = ReadLe32(head, 8);
        if (version == 2 && declared >= 12 && static_cast<uint64_t>(declared) <= totalBytes) {
            return { FormatId::Glb, SniffConfidence::Strong, "glb-header" };
        }
        return { FormatId::Unknown, SniffConfidence::Weak, "glb-header-invalid" };
    }

    if (IsZipHead(head)) {
        // The central directory is outside the head window. SniffFile performs
        // the bounded probe; SniffHead alone cannot identify the package.
        return { FormatId::Unknown, SniffConfidence::Weak, "zip-requires-central-directory" };
    }

    const std::size_t bom = SkipUtf8Bom(head);
    const std::string_view text = AsciiView(head);
    const std::string_view fromBom = text.substr(std::min(bom, text.size()));

    if (StartsWith(fromBom, "PXR-USDC")) {
        return { FormatId::Usdc, SniffConfidence::Strong, "usdc-magic" };
    }
    if (StartsWith(fromBom, "#usda")) {
        return { FormatId::Usda, SniffConfidence::Strong, "usda-header" };
    }
    if (StartsWith(fromBom, "ISO-10303-21")) {
        return { FormatId::Step, SniffConfidence::Strong, "step-iso10303-21" };
    }
    if (StartsWith(fromBom, "Kaydara FBX Binary")) {
        return { FormatId::Fbx, SniffConfidence::Strong, "fbx-binary-magic" };
    }

    const std::size_t trimmed = SkipWhitespace(text, bom);
    if (StartsWithInsensitive(text.substr(trimmed), "ply")
        && (text.size() == trimmed + 3
            || text[trimmed + 3] == ' ' || text[trimmed + 3] == '\t'
            || text[trimmed + 3] == '\r' || text[trimmed + 3] == '\n')) {
        return { FormatId::Ply, SniffConfidence::Strong, "ply-header" };
    }

    if (LooksLikeAsciiStl(text, trimmed)) {
        return { FormatId::Stl, SniffConfidence::Strong, "stl-ascii-solid" };
    }
    if (LooksLikeBinaryStl(head, totalBytes)) {
        return { FormatId::Stl, SniffConfidence::Strong, "stl-facet-count" };
    }
    if (StartsWithInsensitive(text.substr(trimmed), "; FBX")) {
        return { FormatId::Fbx, SniffConfidence::Weak, "fbx-ascii-header" };
    }
    if (LooksLikeGltfJson(text, trimmed)) {
        return { FormatId::Gltf, SniffConfidence::Strong, "gltf-json-asset" };
    }
    if (LooksLikeObj(text, trimmed)) {
        return { FormatId::Obj, SniffConfidence::Weak, "obj-statements" };
    }
    return {};
}

SniffResult SniffFile(const std::wstring& path)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) return {};
    file.seekg(0, std::ios::end);
    const std::streamoff end = file.tellg();
    if (end <= 0) return {};
    const uint64_t totalBytes = static_cast<uint64_t>(end);

    const std::size_t headBytes = static_cast<std::size_t>(
        std::min<uint64_t>(totalBytes, kSniffHeadBytes));
    std::vector<std::byte> head(headBytes);
    if (!ReadAt(file, 0, head)) return {};

    const SniffResult headResult = SniffHead(head, totalBytes);
    if (headResult.format != FormatId::Unknown) return headResult;
    if (IsZipHead(head)) return ProbeZipCentralDirectory(file, totalBytes);
    return headResult;
}

} // namespace model_core

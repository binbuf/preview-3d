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

constexpr std::array<unsigned char, 4> kZipEndOfCentralDirectory{ 0x50, 0x4b, 0x05, 0x06 };
constexpr char kXbfMagic[] = { 'B', 'I', 'N', 'F', 'I', 'L', 'E' };
constexpr std::uint64_t kXbfFormatVersion = 0x0000000704030201ull;

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

bool EqualInsensitive(std::string_view left, std::string_view right) noexcept
{
    return left.size() == right.size() && StartsWithInsensitive(left, right);
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

std::uint64_t ReadLe64(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
            << (index * 8);
    }
    return value;
}

bool MatchesAt(std::span<const std::byte> bytes, std::size_t offset,
               std::string_view token) noexcept
{
    if (offset > bytes.size() || bytes.size() - offset < token.size()) return false;
    for (std::size_t index = 0; index < token.size(); ++index) {
        if (std::to_integer<unsigned char>(bytes[offset + index])
            != static_cast<unsigned char>(token[index])) return false;
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

// Skip an optional XML declaration and comments before the root element. A
// truncated declaration/comment is not a match; the caller sees end-of-text.
std::size_t SkipXmlPreamble(std::string_view text, std::size_t start) noexcept
{
    for (;;) {
        start = SkipWhitespace(text, start);
        if (start >= text.size()) return start;
        if (StartsWith(text.substr(start), "<?")) {
            const std::size_t end = text.find("?>", start + 2);
            if (end == std::string_view::npos) return text.size();
            start = end + 2;
            continue;
        }
        if (StartsWith(text.substr(start), "<!--")) {
            const std::size_t end = text.find("-->", start + 4);
            if (end == std::string_view::npos) return text.size();
            start = end + 3;
            continue;
        }
        return start;
    }
}

constexpr bool IsTokenBoundary(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n'
        || value == '>' || value == '/' || value == '?';
}

std::string_view TrimAscii(std::string_view line) noexcept
{
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    line.remove_prefix(first);
    const std::size_t last = line.find_last_not_of(" \t");
    return line.substr(0, last + 1);
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

bool LooksLikePly(std::string_view text, std::size_t start) noexcept
{
    if (!StartsWithInsensitive(text.substr(start), "ply")) return false;
    const std::size_t next = start + 3;
    return next >= text.size() || text[next] == ' ' || text[next] == '\t'
        || text[next] == '\r' || text[next] == '\n';
}

// DXF is a group-code stream. The first non-empty lines of an ASCII DXF are
// normally `0`, `SECTION`; tolerate a BOM, blank lines, and a small export
// preamble by looking only at the first four non-empty lines.
bool LooksLikeDxfSection(std::string_view text) noexcept
{
    std::string_view lines[4]{};
    std::size_t count = 0;
    std::size_t position = 0;
    while (count < 4 && position <= text.size()) {
        std::size_t end = text.find('\n', position);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(position, end - position);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        line = TrimAscii(line);
        if (!line.empty()) lines[count++] = line;
        if (end == text.size()) break;
        position = end + 1;
    }
    for (std::size_t index = 0; index + 1 < count; ++index) {
        if (lines[index] == "0" && EqualInsensitive(lines[index + 1], "SECTION")) return true;
    }
    return false;
}

bool LooksLikeOff(std::string_view text, std::size_t start) noexcept
{
    if (!StartsWith(text.substr(start), "OFF")) return false;
    const std::size_t next = start + 3;
    return next >= text.size() || text[next] == ' ' || text[next] == '\t'
        || text[next] == '\r' || text[next] == '\n';
}

// IGES records are exactly 80 columns; column 73 (index 72) carries the
// S/G/D/P/T section letter. A record followed by more bytes must end at a
// line break so an arbitrary 80-byte prefix is not accepted.
bool LooksLikeIgesRecord(std::span<const std::byte> head) noexcept
{
    if (head.size() < 80) return false;
    const char section = static_cast<char>(std::to_integer<unsigned char>(head[72]));
    if (section != 'S' && section != 'G' && section != 'D'
        && section != 'P' && section != 'T') return false;
    if (head.size() > 80) {
        const char next = static_cast<char>(std::to_integer<unsigned char>(head[80]));
        if (next != '\r' && next != '\n') return false;
    }
    return true;
}

bool LooksLikeBrepHeader(std::string_view text, std::size_t start) noexcept
{
    const std::string_view tail = text.substr(start);
    constexpr std::string_view kDrawable = "DBRep_DrawableShape";
    if (StartsWith(tail, kDrawable)) {
        return tail.size() == kDrawable.size() || IsTokenBoundary(tail[kDrawable.size()]);
    }
    constexpr std::string_view kHeader = "CASCADE Topology V";
    if (!StartsWith(tail, kHeader)) return false;
    if (tail.size() <= kHeader.size()) return false;
    const char version = tail[kHeader.size()];
    if (version != '1' && version != '2' && version != '3') return false;
    if (tail.size() == kHeader.size() + 1) return true;
    const char next = tail[kHeader.size() + 1];
    // The canonical header is `CASCADE Topology V1, (c) ...`; the comma is
    // part of the version line, not a generic token boundary.
    return IsTokenBoundary(next) || next == ',';
}

bool LooksLikeThreeDs(std::span<const std::byte> head, uint64_t totalBytes) noexcept
{
    if (head.size() < 6 || totalBytes < 6) return false;
    if (std::to_integer<unsigned char>(head[0]) != 0x4D
        || std::to_integer<unsigned char>(head[1]) != 0x4D) return false;
    const std::uint32_t declared = ReadLe32(head, 2);
    return declared >= 6 && static_cast<std::uint64_t>(declared) <= totalBytes;
}

bool LooksLikeXbf(std::span<const std::byte> head) noexcept
{
    if (head.size() < 15) return false;
    if (!MatchesAt(head, 0, std::string_view(kXbfMagic, sizeof(kXbfMagic)))) return false;
    return ReadLe64(head, 7) == kXbfFormatVersion;
}

enum class SignatureVerdict : uint8_t {
    NoMatch = 0,
    Match,
    Veto,
};

struct SignatureEvaluation {
    SignatureVerdict verdict = SignatureVerdict::NoMatch;
    SniffConfidence confidence = SniffConfidence::None;
    std::string_view detail{};
};

SignatureEvaluation EvaluateGlb(std::span<const std::byte> head, uint64_t totalBytes) noexcept
{
    if (!MatchesAt(head, 0, "glTF")) return {};
    if (head.size() < 12 || totalBytes < 12) {
        return { SignatureVerdict::Veto, SniffConfidence::Weak, "glb-header-truncated" };
    }
    const std::uint32_t version = ReadLe32(head, 4);
    const std::uint32_t declared = ReadLe32(head, 8);
    if (version == 2 && declared >= 12 && static_cast<std::uint64_t>(declared) <= totalBytes) {
        return { SignatureVerdict::Match, SniffConfidence::Strong, "glb-header" };
    }
    return { SignatureVerdict::Veto, SniffConfidence::Weak, "glb-header-invalid" };
}

SignatureEvaluation EvaluateHeadSignature(const MagicSignature& signature,
                                          std::span<const std::byte> head,
                                          uint64_t totalBytes) noexcept
{
    const std::string_view text = AsciiView(head);
    const std::size_t bom = SkipUtf8Bom(head);

    switch (signature.kind) {
    case MagicKind::HeadPrefix: {
        if (signature.offset > head.size() - bom) return {};
        if (!MatchesAt(head, bom + signature.offset, signature.bytes)) return {};
        return { SignatureVerdict::Match, SniffConfidence::Strong, signature.bytes };
    }
    case MagicKind::HeadPrefixCaseInsensitive: {
        const std::size_t start = SkipWhitespace(text, bom);
        if (signature.offset > text.size() - start) return {};
        const std::size_t at = start + signature.offset;
        if (!StartsWithInsensitive(text.substr(at), signature.bytes)) return {};
        return { SignatureVerdict::Match, SniffConfidence::Weak, signature.bytes };
    }
    case MagicKind::GltfJsonAsset: {
        const std::size_t start = SkipWhitespace(text, bom);
        if (!LooksLikeGltfJson(text, start)) return {};
        return { SignatureVerdict::Match, SniffConfidence::Strong, "gltf-json-asset" };
    }
    case MagicKind::BinaryStl:
        if (!LooksLikeBinaryStl(head, totalBytes)) return {};
        return { SignatureVerdict::Match, SniffConfidence::Strong, "stl-facet-count" };
    case MagicKind::AsciiStlText: {
        const std::size_t start = SkipWhitespace(text, bom);
        if (!LooksLikeAsciiStl(text, start)) return {};
        return { SignatureVerdict::Match, SniffConfidence::Strong, "stl-ascii-solid" };
    }
    case MagicKind::PlyHeader: {
        const std::size_t start = SkipWhitespace(text, bom);
        if (!LooksLikePly(text, start)) return {};
        return { SignatureVerdict::Match, SniffConfidence::Strong, "ply-header" };
    }
    case MagicKind::ObjText: {
        const std::size_t start = SkipWhitespace(text, bom);
        if (!LooksLikeObj(text, start)) return {};
        return { SignatureVerdict::Match, SniffConfidence::Weak, "obj-statements" };
    }
    case MagicKind::ZipEntryEquals:
    case MagicKind::ZipEntrySuffix:
    case MagicKind::ZipEntryEqualsAndSuffix:
        // ZIP identity lives in the central directory; SniffFile owns it.
        return {};
    case MagicKind::GlbHeader:
        return EvaluateGlb(head, totalBytes);
    case MagicKind::ThreeDsChunk:
        if (!LooksLikeThreeDs(head, totalBytes)) return {};
        return { SignatureVerdict::Match, SniffConfidence::Strong, "3ds-main-chunk" };
    case MagicKind::XmlPrefixCaseInsensitive: {
        const std::size_t start = SkipXmlPreamble(text, bom);
        if (start >= text.size()) return {};
        if (!StartsWithInsensitive(text.substr(start), signature.bytes)) return {};
        const std::size_t next = start + signature.bytes.size();
        if (next < text.size() && !IsTokenBoundary(text[next])) return {};
        return { SignatureVerdict::Match, SniffConfidence::Strong, signature.bytes };
    }
    case MagicKind::DxfSection:
        if (!LooksLikeDxfSection(text.substr(bom))) return {};
        return { SignatureVerdict::Match, SniffConfidence::Strong, "dxf-section" };
    case MagicKind::OffHeader: {
        const std::size_t start = SkipWhitespace(text, bom);
        if (!LooksLikeOff(text, start)) return {};
        return { SignatureVerdict::Match, SniffConfidence::Weak, "off-header" };
    }
    case MagicKind::IgesRecord:
        if (!LooksLikeIgesRecord(head)) return {};
        return { SignatureVerdict::Match, SniffConfidence::Strong, "iges-record" };
    case MagicKind::BrepHeader: {
        const std::size_t start = SkipWhitespace(text, bom);
        if (!LooksLikeBrepHeader(text, start)) return {};
        return { SignatureVerdict::Match, SniffConfidence::Strong, "brep-header" };
    }
    case MagicKind::XbfHeader:
        if (!LooksLikeXbf(head)) return {};
        return { SignatureVerdict::Match, SniffConfidence::Strong, "xbf-header" };
    }
    return {};
}

constexpr bool IsZipKind(MagicKind kind) noexcept
{
    return kind == MagicKind::ZipEntryEquals || kind == MagicKind::ZipEntrySuffix
        || kind == MagicKind::ZipEntryEqualsAndSuffix;
}

constexpr int ConfidenceRank(SniffConfidence confidence) noexcept
{
    switch (confidence) {
    case SniffConfidence::Strong: return 2;
    case SniffConfidence::Weak: return 1;
    case SniffConfidence::None: return 0;
    }
    return 0;
}

bool BetterMatch(SniffConfidence candidateConfidence, uint8_t candidatePriority,
                 SniffConfidence bestConfidence, uint8_t bestPriority) noexcept
{
    const int candidateRank = ConfidenceRank(candidateConfidence);
    const int bestRank = ConfidenceRank(bestConfidence);
    if (candidateRank != bestRank) return candidateRank > bestRank;
    return candidatePriority > bestPriority;
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
    return EqualInsensitive(name, expected);
}

bool EntryHasSuffix(std::string_view name, std::string_view suffix) noexcept
{
    if (name.size() < suffix.size()) return false;
    return EntryEquals(name.substr(name.size() - suffix.size()), suffix);
}

bool ZipHasEntryEqualsAndSuffix(std::span<const std::string_view> names,
                                std::string_view expected,
                                std::string_view suffix) noexcept
{
    bool hasEntry = false;
    bool hasSuffix = false;
    for (const auto name : names) {
        if (!hasEntry && EntryEquals(name, expected)) hasEntry = true;
        if (!hasSuffix && EntryHasSuffix(name, suffix)) hasSuffix = true;
        if (hasEntry && hasSuffix) return true;
    }
    return false;
}

bool MatchesZipSignatures(std::span<const MagicSignature> signatures,
                          std::span<const std::string_view> names,
                          std::string_view& detail) noexcept
{
    for (const auto& signature : signatures) {
        switch (signature.kind) {
        case MagicKind::ZipEntryEquals:
            for (const auto name : names) {
                if (EntryEquals(name, signature.bytes)) {
                    detail = signature.bytes;
                    return true;
                }
            }
            break;
        case MagicKind::ZipEntrySuffix:
            for (const auto name : names) {
                if (EntryHasSuffix(name, signature.bytes)) {
                    detail = signature.bytes;
                    return true;
                }
            }
            break;
        case MagicKind::ZipEntryEqualsAndSuffix:
            if (ZipHasEntryEqualsAndSuffix(names, signature.bytes, signature.aux)) {
                detail = signature.bytes;
                return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
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

    std::vector<std::string_view> names;
    names.reserve(entries);
    std::size_t offset = 0;
    const auto directorySpan = std::span<const std::byte>(directory);
    for (std::uint16_t entry = 0; entry < entries; ++entry) {
        if (offset > directory.size() || directory.size() - offset < 46) return {};
        if (!MatchesAt(directorySpan, offset, "\x50\x4b\x01\x02")) return {};
        const std::uint16_t nameBytes = ReadLe16(directorySpan, offset + 28);
        const std::uint16_t extraBytes = ReadLe16(directorySpan, offset + 30);
        const std::uint16_t commentBytesEntry = ReadLe16(directorySpan, offset + 32);
        const std::size_t next = offset + 46 + nameBytes + extraBytes + commentBytesEntry;
        if (next > directory.size() || next < offset) return {};
        names.emplace_back(
            reinterpret_cast<const char*>(directory.data()) + offset + 46, nameBytes);
        offset = next;
    }

    const FormatDescriptor* best = nullptr;
    std::string_view bestDetail{};
    for (const auto& descriptor : Formats()) {
        std::string_view detail{};
        if (!MatchesZipSignatures(descriptor.signatures, names, detail)) continue;
        if (best == nullptr || descriptor.priority > best->priority) {
            best = &descriptor;
            bestDetail = detail;
        }
    }
    if (best != nullptr) return { best->id, SniffConfidence::Strong, bestDetail };
    return {};
}

bool IsZipHead(std::span<const std::byte> head) noexcept
{
    return MatchesAt(head, 0, "\x50\x4b\x03\x04");
}

} // namespace

SniffResult SniffHead(std::span<const std::byte> head, uint64_t totalBytes) noexcept
{
    if (head.empty()) return {};

    // ZIP-based identities live in the central directory. SniffHead alone
    // cannot claim a package from the local header.
    if (IsZipHead(head)) {
        return { FormatId::Unknown, SniffConfidence::Weak, "zip-requires-central-directory" };
    }

    const FormatDescriptor* best = nullptr;
    SniffConfidence bestConfidence = SniffConfidence::None;
    std::string_view bestDetail{};

    // The catalog table is in FormatId order; priority is applied when more
    // than one descriptor matches, so a later high-priority entry can win.
    for (const auto& descriptor : Formats()) {
        for (const auto& signature : descriptor.signatures) {
            if (IsZipKind(signature.kind)) continue;
            const SignatureEvaluation evaluation
                = EvaluateHeadSignature(signature, head, totalBytes);
            if (evaluation.verdict == SignatureVerdict::Veto) {
                // A recognized strong envelope that fails its own structural
                // check must not fall through to a weaker textual heuristic.
                return { FormatId::Unknown, evaluation.confidence, evaluation.detail };
            }
            if (evaluation.verdict != SignatureVerdict::Match) continue;
            if (best == nullptr
                || BetterMatch(evaluation.confidence, descriptor.priority,
                               bestConfidence, best->priority)) {
                best = &descriptor;
                bestConfidence = evaluation.confidence;
                bestDetail = evaluation.detail;
            }
        }
    }

    if (best != nullptr) return { best->id, bestConfidence, bestDetail };
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
    if (IsZipHead(head)) {
        const SniffResult zipResult = ProbeZipCentralDirectory(file, totalBytes);
        if (zipResult.format != FormatId::Unknown) return zipResult;
    }
    return headResult;
}

} // namespace model_core

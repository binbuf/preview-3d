#include "UsdZipPreflight.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace import_worker {
namespace {

constexpr uint32_t kLocalHeader = 0x04034b50u;
constexpr uint32_t kCentralHeader = 0x02014b50u;
constexpr uint32_t kEndOfCentralDirectory = 0x06054b50u;
constexpr size_t kEocdBytes = 22;
constexpr size_t kMaxCommentBytes = 65'535;

bool Add(size_t left, size_t right, size_t& result)
{
    if (right > (std::numeric_limits<size_t>::max)() - left) return false;
    result = left + right;
    return true;
}

bool Read16(std::span<const std::byte> bytes, size_t offset, uint16_t& value)
{
    if (offset > bytes.size() || bytes.size() - offset < 2) return false;
    value = uint16_t(std::to_integer<uint8_t>(bytes[offset]))
        | uint16_t(uint16_t(std::to_integer<uint8_t>(bytes[offset + 1])) << 8);
    return true;
}

bool Read32(std::span<const std::byte> bytes, size_t offset, uint32_t& value)
{
    if (offset > bytes.size() || bytes.size() - offset < 4) return false;
    value = uint32_t(std::to_integer<uint8_t>(bytes[offset]))
        | (uint32_t(std::to_integer<uint8_t>(bytes[offset + 1])) << 8)
        | (uint32_t(std::to_integer<uint8_t>(bytes[offset + 2])) << 16)
        | (uint32_t(std::to_integer<uint8_t>(bytes[offset + 3])) << 24);
    return true;
}

bool CheckCrc32(std::span<const std::byte> bytes, uint32_t expected,
                const std::function<bool()>& isCancelled)
{
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < bytes.size(); ++index) {
        if ((index & 0xffffu) == 0 && isCancelled && isCancelled()) return false;
        crc ^= std::to_integer<uint8_t>(bytes[index]);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc == expected;
}

bool SafeName(std::span<const std::byte> name, uint32_t maxDepth, std::string& folded)
{
    if (name.empty() || name.size() > 1024) return false;
    folded.clear();
    folded.reserve(name.size());
    uint32_t depth = 1;
    size_t componentStart = 0;
    for (size_t index = 0; index < name.size(); ++index) {
        const unsigned char ch = std::to_integer<unsigned char>(name[index]);
        if (ch < 0x20 || ch == 0x7f || ch == '\\' || ch == ':'
            || (index == 0 && ch == '/')) return false;
        if (ch == '/') {
            if (index == componentStart) return false;
            const size_t length = index - componentStart;
            if ((length == 1 && std::to_integer<unsigned char>(name[componentStart]) == '.')
                || (length == 2 && std::to_integer<unsigned char>(name[componentStart]) == '.'
                    && std::to_integer<unsigned char>(name[componentStart + 1]) == '.')) return false;
            componentStart = index + 1;
            if (++depth > maxDepth) return false;
        }
        folded.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (componentStart == name.size()) return false;
    const size_t finalLength = name.size() - componentStart;
    if ((finalLength == 1 && std::to_integer<unsigned char>(name[componentStart]) == '.')
        || (finalLength == 2 && std::to_integer<unsigned char>(name[componentStart]) == '.'
            && std::to_integer<unsigned char>(name[componentStart + 1]) == '.')) return false;
    return true;
}

bool IsLayerName(std::string_view name)
{
    const size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos) return false;
    std::string extension(name.substr(dot + 1));
    std::ranges::transform(extension, extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension == "usd" || extension == "usda" || extension == "usdc";
}

} // namespace

UsdzPreflightError InspectUsdz(std::span<const std::byte> bytes,
                               UsdzArchiveView* archive,
                               const UsdzPreflightLimits& limits,
                               const std::function<bool()>& isCancelled)
{
    if (archive) *archive = {};
    if (isCancelled && isCancelled()) return UsdzPreflightError::Cancelled;
    if (bytes.size() < kEocdBytes) return UsdzPreflightError::NotZip;
    const size_t earliest = bytes.size() > kEocdBytes + kMaxCommentBytes
        ? bytes.size() - kEocdBytes - kMaxCommentBytes : 0;
    size_t eocd = bytes.size() - kEocdBytes;
    uint32_t signature = 0;
    for (;;) {
        if (isCancelled && ((bytes.size() - eocd) & 0xfffu) == 0 && isCancelled())
            return UsdzPreflightError::Cancelled;
        if (Read32(bytes, eocd, signature) && signature == kEndOfCentralDirectory) break;
        if (eocd == earliest) return UsdzPreflightError::NotZip;
        --eocd;
    }

    uint16_t disk = 0, centralDisk = 0, entriesOnDisk = 0, entries = 0, commentLength = 0;
    uint32_t centralSize = 0, centralOffset = 0;
    if (!Read16(bytes, eocd + 4, disk) || !Read16(bytes, eocd + 6, centralDisk)
        || !Read16(bytes, eocd + 8, entriesOnDisk) || !Read16(bytes, eocd + 10, entries)
        || !Read32(bytes, eocd + 12, centralSize) || !Read32(bytes, eocd + 16, centralOffset)
        || !Read16(bytes, eocd + 20, commentLength)) return UsdzPreflightError::Truncated;
    size_t eocdEnd = 0;
    if (!Add(eocd, kEocdBytes + commentLength, eocdEnd) || eocdEnd != bytes.size())
        return UsdzPreflightError::InvalidDirectory;
    if (disk || centralDisk || entriesOnDisk != entries) return UsdzPreflightError::MultiDisk;
    if (entries == 0xffffu || centralSize == 0xffffffffu || centralOffset == 0xffffffffu)
        return UsdzPreflightError::Zip64;
    if (!entries || entries > limits.maxEntries) return UsdzPreflightError::TooManyEntries;
    size_t centralEnd = 0;
    if (!Add(centralOffset, centralSize, centralEnd) || centralEnd != eocd)
        return UsdzPreflightError::InvalidDirectory;

    std::unordered_set<std::string> names;
    std::vector<std::pair<size_t, size_t>> localRanges;
    localRanges.reserve(entries);
    uint64_t expanded = 0;
    size_t cursor = centralOffset;
    for (uint32_t entry = 0; entry < entries; ++entry) {
        if (isCancelled && isCancelled()) return UsdzPreflightError::Cancelled;
        uint16_t flags = 0, method = 0, nameLength = 0, extraLength = 0, entryComment = 0,
                 startDisk = 0;
        uint32_t crc = 0, compressed = 0, uncompressed = 0, localOffset = 0;
        if (!Read32(bytes, cursor, signature) || signature != kCentralHeader
            || !Read16(bytes, cursor + 8, flags) || !Read16(bytes, cursor + 10, method)
            || !Read32(bytes, cursor + 16, crc)
            || !Read32(bytes, cursor + 20, compressed) || !Read32(bytes, cursor + 24, uncompressed)
            || !Read16(bytes, cursor + 28, nameLength) || !Read16(bytes, cursor + 30, extraLength)
            || !Read16(bytes, cursor + 32, entryComment) || !Read16(bytes, cursor + 34, startDisk)
            || !Read32(bytes, cursor + 42, localOffset)) return UsdzPreflightError::Truncated;
        if (startDisk) return UsdzPreflightError::MultiDisk;
        if ((flags & 0x0001u) != 0) return UsdzPreflightError::Encrypted;
        if ((flags & 0x0008u) != 0) return UsdzPreflightError::InvalidDirectory;
        if (method != 0) return UsdzPreflightError::UnsupportedCompression;
        if (compressed != uncompressed) return UsdzPreflightError::InvalidDirectory;
        if (compressed == 0xffffffffu || uncompressed == 0xffffffffu
            || localOffset == 0xffffffffu) return UsdzPreflightError::Zip64;
        if (uncompressed > limits.maxEntryBytes) return UsdzPreflightError::EntryTooLarge;
        if (compressed == 0 ? uncompressed != 0
            : uint64_t(uncompressed) > uint64_t(compressed) * limits.maxExpansionRatio)
            return UsdzPreflightError::ExpansionRatio;
        if (uncompressed > limits.maxExpandedBytes
            || expanded > limits.maxExpandedBytes - uncompressed)
            return UsdzPreflightError::AggregateTooLarge;
        expanded += uncompressed;

        size_t nameOffset = 0, entryEnd = 0;
        if (!Add(cursor, 46, nameOffset) || !Add(nameOffset, nameLength, entryEnd)
            || !Add(entryEnd, extraLength, entryEnd) || !Add(entryEnd, entryComment, entryEnd)
            || entryEnd > centralEnd) return UsdzPreflightError::Truncated;
        std::string folded;
        if (!SafeName(bytes.subspan(nameOffset, nameLength), limits.maxPathDepth, folded))
            return UsdzPreflightError::UnsafePath;
        if (!names.insert(folded).second) return UsdzPreflightError::DuplicatePath;

        uint16_t localFlags = 0, localMethod = 0, localNameLength = 0, localExtraLength = 0;
        uint32_t localCrc = 0, localCompressed = 0, localUncompressed = 0;
        if (!Read32(bytes, localOffset, signature) || signature != kLocalHeader
            || !Read16(bytes, localOffset + 6, localFlags) || !Read16(bytes, localOffset + 8, localMethod)
            || !Read32(bytes, localOffset + 14, localCrc)
            || !Read32(bytes, localOffset + 18, localCompressed)
            || !Read32(bytes, localOffset + 22, localUncompressed)
            || !Read16(bytes, localOffset + 26, localNameLength)
            || !Read16(bytes, localOffset + 28, localExtraLength)) return UsdzPreflightError::Truncated;
        if (localFlags != flags || localMethod != method || localCrc != crc
            || localCompressed != compressed
            || localUncompressed != uncompressed || localNameLength != nameLength)
            return UsdzPreflightError::InvalidDirectory;
        size_t localNameOffset = 0, dataOffset = 0, dataEnd = 0;
        if (!Add(localOffset, 30, localNameOffset) || !Add(localNameOffset, localNameLength, dataOffset)
            || !Add(dataOffset, localExtraLength, dataOffset) || !Add(dataOffset, compressed, dataEnd)
            || dataEnd > centralOffset) return UsdzPreflightError::Truncated;
        if (!std::equal(bytes.begin() + static_cast<ptrdiff_t>(nameOffset),
                        bytes.begin() + static_cast<ptrdiff_t>(nameOffset + nameLength),
                        bytes.begin() + static_cast<ptrdiff_t>(localNameOffset)))
            return UsdzPreflightError::InvalidDirectory;
        if ((dataOffset & 63u) != 0) return UsdzPreflightError::Misaligned;
        if (!CheckCrc32(bytes.subspan(dataOffset, compressed), crc, isCancelled))
            return isCancelled && isCancelled() ? UsdzPreflightError::Cancelled
                                                : UsdzPreflightError::InvalidDirectory;
        localRanges.emplace_back(size_t(localOffset), dataEnd);
        if (archive) {
            std::string name;
            name.resize(nameLength);
            std::memcpy(name.data(), bytes.data() + nameOffset, nameLength);
            archive->entries.push_back(UsdzEntryView{
                std::move(name), std::move(folded), uint64_t(dataOffset), uint64_t(uncompressed)});
        }
        cursor = entryEnd;
    }
    if (cursor != centralEnd) return UsdzPreflightError::InvalidDirectory;
    std::ranges::sort(localRanges);
    for (size_t index = 1; index < localRanges.size(); ++index)
        if (localRanges[index].first < localRanges[index - 1].second)
            return UsdzPreflightError::InvalidDirectory;
    if (archive) {
        if (archive->entries.empty() || !IsLayerName(archive->entries.front().name)) {
            *archive = {};
            return UsdzPreflightError::InvalidDirectory;
        }
        archive->rootLayerName = archive->entries.front().name;
    }
    return UsdzPreflightError::None;
}

UsdzPreflightError PreflightUsdz(std::span<const std::byte> bytes,
                                 const UsdzPreflightLimits& limits)
{
    return InspectUsdz(bytes, nullptr, limits);
}

} // namespace import_worker

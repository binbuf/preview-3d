#include "FcstdPreflight.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_set>
#include <utility>

#include <zlib.h>

namespace step_host {
namespace {

constexpr std::uint32_t kLocalHeader = 0x04034b50u;
constexpr std::uint32_t kCentralHeader = 0x02014b50u;
constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50u;
constexpr std::uint32_t kZip64Locator = 0x07064b50u;
constexpr std::uint32_t kZip64EndOfCentralDirectory = 0x06064b50u;

bool Add(std::uint64_t left, std::uint64_t right, std::uint64_t& result)
{
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) return false;
    result = left + right;
    return true;
}

bool Read16(std::span<const std::byte> bytes, std::uint64_t offset, std::uint16_t& value)
{
    if (offset > bytes.size() || bytes.size() - offset < 2) return false;
    value = std::uint16_t(std::to_integer<std::uint8_t>(bytes[std::size_t(offset)]))
        | std::uint16_t(std::uint16_t(std::to_integer<std::uint8_t>(bytes[std::size_t(offset + 1)])) << 8);
    return true;
}

bool Read32(std::span<const std::byte> bytes, std::uint64_t offset, std::uint32_t& value)
{
    if (offset > bytes.size() || bytes.size() - offset < 4) return false;
    value = std::uint32_t(std::to_integer<std::uint8_t>(bytes[std::size_t(offset)]))
        | (std::uint32_t(std::to_integer<std::uint8_t>(bytes[std::size_t(offset + 1)])) << 8)
        | (std::uint32_t(std::to_integer<std::uint8_t>(bytes[std::size_t(offset + 2)])) << 16)
        | (std::uint32_t(std::to_integer<std::uint8_t>(bytes[std::size_t(offset + 3)])) << 24);
    return true;
}

bool Read64(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t& value)
{
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    if (!Read32(bytes, offset, low) || !Read32(bytes, offset + 4, high)) return false;
    value = std::uint64_t(low) | (std::uint64_t(high) << 32);
    return true;
}

bool SafeEntryName(std::span<const std::byte> name, std::uint32_t maxDepth, std::string& folded)
{
    if (name.empty() || name.size() > 1024) return false;
    folded.clear();
    folded.reserve(name.size());
    std::uint32_t depth = 1;
    std::size_t componentStart = 0;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const unsigned char ch = std::to_integer<unsigned char>(name[index]);
        if (ch < 0x20 || ch == 0x7f || ch == '\\' || ch == ':' || (index == 0 && ch == '/'))
            return false;
        if (ch == '/') {
            if (index == componentStart) return false;
            const std::size_t length = index - componentStart;
            if ((length == 1 && std::to_integer<unsigned char>(name[componentStart]) == '.')
                || (length == 2 && std::to_integer<unsigned char>(name[componentStart]) == '.'
                    && std::to_integer<unsigned char>(name[componentStart + 1]) == '.'))
                return false;
            componentStart = index + 1;
            if (++depth > maxDepth) return false;
        }
        folded.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (componentStart == name.size()) return true; // directory entry
    const std::size_t finalLength = name.size() - componentStart;
    if ((finalLength == 1 && std::to_integer<unsigned char>(name[componentStart]) == '.')
        || (finalLength == 2 && std::to_integer<unsigned char>(name[componentStart]) == '.'
            && std::to_integer<unsigned char>(name[componentStart + 1]) == '.'))
        return false;
    return true;
}

// Resolve the ZIP64 extra field (id 1) at [position, extraEnd). 32-bit
// sentinels select which 64-bit fields are present in the fixed order
// uncompressed, compressed, local-header offset.
FcstdPreflightStatus ResolveZip64(std::span<const std::byte> bytes, std::uint64_t position,
                                  std::uint64_t extraEnd, std::uint32_t uncompressed32,
                                  std::uint32_t compressed32, std::uint32_t offset32,
                                  std::uint64_t& uncompressed, std::uint64_t& compressed,
                                  std::uint64_t& offset)
{
    while (position + 4 <= extraEnd) {
        std::uint16_t id = 0;
        std::uint16_t size = 0;
        if (!Read16(bytes, position, id) || !Read16(bytes, position + 2, size)
            || position + 4 + size > extraEnd)
            return FcstdPreflightStatus::Truncated;
        position += 4;
        if (id != 1) {
            position += size;
            continue;
        }
        const std::uint64_t end = position + size;
        if (uncompressed32 == 0xffffffffu) {
            if (position + 8 > end) return FcstdPreflightStatus::InvalidDirectory;
            if (!Read64(bytes, position, uncompressed)) return FcstdPreflightStatus::Truncated;
            position += 8;
        }
        if (compressed32 == 0xffffffffu) {
            if (position + 8 > end) return FcstdPreflightStatus::InvalidDirectory;
            if (!Read64(bytes, position, compressed)) return FcstdPreflightStatus::Truncated;
            position += 8;
        }
        if (offset32 == 0xffffffffu) {
            if (position + 8 > end) return FcstdPreflightStatus::InvalidDirectory;
            if (!Read64(bytes, position, offset)) return FcstdPreflightStatus::Truncated;
            position += 8;
        }
        return FcstdPreflightStatus::Ok;
    }
    return FcstdPreflightStatus::Ok;
}

bool CheckCrc32(std::span<const std::byte> data, std::uint32_t expected,
                const std::function<bool()>& isCancelled)
{
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t index = 0; index < data.size(); ++index) {
        if ((index & 0xffffu) == 0 && isCancelled && isCancelled()) return false;
        crc ^= std::to_integer<std::uint8_t>(data[index]);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc == expected;
}

} // namespace

FcstdPreflightStatus InspectFcstd(std::span<const std::byte> bytes, FcstdPackage* package,
                                  const FcstdLimits& limits,
                                  const std::function<bool()>& isCancelled)
{
    if (package) *package = {};
    if (isCancelled && isCancelled()) return FcstdPreflightStatus::Cancelled;
    if (bytes.size() < 22) return FcstdPreflightStatus::NotZip;
    if (bytes.size() > limits.maxSourceBytes) return FcstdPreflightStatus::SourceLimit;

    constexpr std::size_t kEocdBytes = 22;
    constexpr std::size_t kMaxCommentBytes = 65'535;
    const std::uint64_t earliest = bytes.size() > kEocdBytes + kMaxCommentBytes
        ? bytes.size() - kEocdBytes - kMaxCommentBytes : 0;
    std::uint64_t eocd = bytes.size() - kEocdBytes;
    std::uint32_t signature = 0;
    for (;;) {
        if (isCancelled && ((bytes.size() - eocd) & 0xfffu) == 0 && isCancelled())
            return FcstdPreflightStatus::Cancelled;
        if (Read32(bytes, eocd, signature) && signature == kEndOfCentralDirectory) break;
        if (eocd == earliest) return FcstdPreflightStatus::NotZip;
        --eocd;
    }

    std::uint16_t disk = 0, centralDisk = 0, entriesOnDisk = 0, entries16 = 0, commentLength = 0;
    std::uint32_t centralSize32 = 0, centralOffset32 = 0;
    if (!Read16(bytes, eocd + 4, disk) || !Read16(bytes, eocd + 6, centralDisk)
        || !Read16(bytes, eocd + 8, entriesOnDisk) || !Read16(bytes, eocd + 10, entries16)
        || !Read32(bytes, eocd + 12, centralSize32) || !Read32(bytes, eocd + 16, centralOffset32)
        || !Read16(bytes, eocd + 20, commentLength))
        return FcstdPreflightStatus::Truncated;
    std::uint64_t eocdEnd = 0;
    if (!Add(eocd, kEocdBytes + commentLength, eocdEnd) || eocdEnd != bytes.size())
        return FcstdPreflightStatus::InvalidDirectory;
    if (disk || centralDisk || entriesOnDisk != entries16)
        return FcstdPreflightStatus::MultiDisk;

    std::uint64_t entries = entries16;
    std::uint64_t centralSize = centralSize32;
    std::uint64_t centralOffset = centralOffset32;
    if (entries16 == 0xffffu || centralSize32 == 0xffffffffu || centralOffset32 == 0xffffffffu) {
        if (eocd < 20) return FcstdPreflightStatus::Truncated;
        std::uint32_t locatorSignature = 0;
        std::uint64_t zip64Offset = 0;
        std::uint32_t totalDisks = 0;
        if (!Read32(bytes, eocd - 20, locatorSignature) || locatorSignature != kZip64Locator
            || !Read64(bytes, eocd - 12, zip64Offset) || !Read32(bytes, eocd - 4, totalDisks))
            return FcstdPreflightStatus::InvalidDirectory;
        if (totalDisks != 1) return FcstdPreflightStatus::MultiDisk;
        std::uint64_t zip64End = 0;
        if (!Add(zip64Offset, 56, zip64End) || zip64End > eocd - 20)
            return FcstdPreflightStatus::InvalidDirectory;
        std::uint32_t zip64Signature = 0;
        std::uint64_t recordSize = 0;
        if (!Read32(bytes, zip64Offset, zip64Signature)
            || zip64Signature != kZip64EndOfCentralDirectory
            || !Read64(bytes, zip64Offset + 4, recordSize) || recordSize < 44
            || !Read64(bytes, zip64Offset + 24, entries)
            || !Read64(bytes, zip64Offset + 32, entries)
            || !Read64(bytes, zip64Offset + 40, centralSize)
            || !Read64(bytes, zip64Offset + 48, centralOffset))
            return FcstdPreflightStatus::Truncated;
    }
    if (!entries || entries > limits.maxEntries) return FcstdPreflightStatus::EntryLimit;
    std::uint64_t centralEnd = 0;
    if (!Add(centralOffset, centralSize, centralEnd) || centralEnd > eocd)
        return FcstdPreflightStatus::InvalidDirectory;

    std::unordered_set<std::string> names;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(std::size_t(entries));
    std::uint64_t expanded = 0;
    std::uint64_t cursor = centralOffset;
    FcstdPackage parsed;
    bool hasDocument = false;
    bool hasShape = false;

    for (std::uint64_t entryIndex = 0; entryIndex < entries; ++entryIndex) {
        if (isCancelled && isCancelled()) return FcstdPreflightStatus::Cancelled;
        std::uint16_t flags = 0, method = 0, nameLength = 0, extraLength = 0, entryComment = 0,
                     startDisk = 0;
        std::uint32_t crc = 0, compressed32 = 0, uncompressed32 = 0, localOffset32 = 0;
        if (!Read32(bytes, cursor, signature) || signature != kCentralHeader
            || !Read16(bytes, cursor + 8, flags) || !Read16(bytes, cursor + 10, method)
            || !Read32(bytes, cursor + 16, crc) || !Read32(bytes, cursor + 20, compressed32)
            || !Read32(bytes, cursor + 24, uncompressed32) || !Read16(bytes, cursor + 28, nameLength)
            || !Read16(bytes, cursor + 30, extraLength) || !Read16(bytes, cursor + 32, entryComment)
            || !Read16(bytes, cursor + 34, startDisk) || !Read32(bytes, cursor + 42, localOffset32))
            return FcstdPreflightStatus::Truncated;
        if (startDisk) return FcstdPreflightStatus::MultiDisk;
        if ((flags & 0x0001u) != 0 || (flags & 0x0040u) != 0)
            return FcstdPreflightStatus::Encrypted;
        if ((flags & 0x0008u) != 0) return FcstdPreflightStatus::DataDescriptor;
        if (method != 0 && method != 8) return FcstdPreflightStatus::UnsupportedCompression;

        std::uint64_t next = 0;
        if (!Add(cursor, 46 + std::uint64_t(nameLength) + extraLength + entryComment, next)
            || next > centralEnd)
            return FcstdPreflightStatus::Truncated;
        std::string folded;
        if (!SafeEntryName(bytes.subspan(std::size_t(cursor + 46), nameLength),
                           limits.maxPathDepth, folded))
            return FcstdPreflightStatus::UnsafePath;
        if (!names.insert(folded).second) return FcstdPreflightStatus::DuplicateEntry;

        std::uint64_t compressed = compressed32;
        std::uint64_t uncompressed = uncompressed32;
        std::uint64_t localOffset = localOffset32;
        if (compressed32 == 0xffffffffu || uncompressed32 == 0xffffffffu
            || localOffset32 == 0xffffffffu) {
            const FcstdPreflightStatus zip64 = ResolveZip64(
                bytes, cursor + 46 + nameLength,
                cursor + 46 + std::uint64_t(nameLength) + extraLength, uncompressed32,
                compressed32, localOffset32, uncompressed, compressed, localOffset);
            if (zip64 != FcstdPreflightStatus::Ok) return zip64;
        }

        const bool directory = !folded.empty() && folded.back() == '/';
        if (directory && (compressed || uncompressed))
            return FcstdPreflightStatus::InvalidDirectory;
        if (uncompressed > limits.maxEntryBytes) return FcstdPreflightStatus::EntryTooLarge;
        if ((compressed == 0 && uncompressed != 0)
            || (compressed != 0
                && (uncompressed / compressed > limits.maxExpansionRatio
                    || (uncompressed / compressed == limits.maxExpansionRatio
                        && uncompressed % compressed != 0))))
            return FcstdPreflightStatus::ExpansionRatio;
        if (uncompressed > limits.maxExpandedBytes
            || expanded > limits.maxExpandedBytes - uncompressed)
            return FcstdPreflightStatus::AggregateTooLarge;
        expanded += uncompressed;

        // Local header must agree with the central directory. The local name is
        // compared byte-for-byte so an archive cannot hide one part behind
        // another's directory entry.
        std::uint64_t localEnd = 0;
        if (!Add(localOffset, 30, localEnd) || localEnd > centralOffset)
            return FcstdPreflightStatus::InvalidDirectory;
        std::uint16_t localFlags = 0, localMethod = 0, localNameLength = 0, localExtraLength = 0;
        std::uint32_t localCrc = 0, localCompressed32 = 0, localUncompressed32 = 0;
        if (!Read32(bytes, localOffset, signature) || signature != kLocalHeader
            || !Read16(bytes, localOffset + 6, localFlags) || !Read16(bytes, localOffset + 8, localMethod)
            || !Read32(bytes, localOffset + 14, localCrc)
            || !Read32(bytes, localOffset + 18, localCompressed32)
            || !Read32(bytes, localOffset + 22, localUncompressed32)
            || !Read16(bytes, localOffset + 26, localNameLength)
            || !Read16(bytes, localOffset + 28, localExtraLength))
            return FcstdPreflightStatus::Truncated;
        if (localFlags != flags || localMethod != method || localNameLength != nameLength)
            return FcstdPreflightStatus::InvalidDirectory;
        std::uint64_t localCompressed = localCompressed32;
        std::uint64_t localUncompressed = localUncompressed32;
        std::uint64_t ignoredOffset = 0;
        if (localCompressed32 == 0xffffffffu || localUncompressed32 == 0xffffffffu) {
            const FcstdPreflightStatus zip64 = ResolveZip64(
                bytes, localOffset + 30 + nameLength,
                localOffset + 30 + std::uint64_t(nameLength) + localExtraLength,
                localUncompressed32, localCompressed32, 0, localUncompressed, localCompressed,
                ignoredOffset);
            if (zip64 != FcstdPreflightStatus::Ok) return zip64;
        }
        if (localCrc != crc || localCompressed != compressed || localUncompressed != uncompressed)
            return FcstdPreflightStatus::InvalidDirectory;
        std::uint64_t localNameOffset = 0;
        std::uint64_t dataOffset = 0;
        std::uint64_t dataEnd = 0;
        if (!Add(localOffset, 30, localNameOffset)
            || !Add(localNameOffset, localNameLength, localNameOffset)
            || !Add(localNameOffset, localExtraLength, dataOffset)
            || !Add(dataOffset, compressed, dataEnd) || dataEnd > centralOffset)
            return FcstdPreflightStatus::Truncated;
        if (!std::equal(bytes.begin() + std::ptrdiff_t(cursor + 46),
                        bytes.begin() + std::ptrdiff_t(cursor + 46 + nameLength),
                        bytes.begin() + std::ptrdiff_t(localOffset + 30)))
            return FcstdPreflightStatus::InvalidDirectory;

        ranges.emplace_back(localOffset, dataEnd);
        FcstdEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(bytes.data() + cursor + 46), nameLength);
        entry.foldedName = folded;
        entry.dataOffset = dataOffset;
        entry.compressedBytes = compressed;
        entry.expandedBytes = uncompressed;
        entry.crc32 = crc;
        entry.method = method;
        entry.directory = directory;
        if (!directory) {
            if (folded == "document.xml") {
                hasDocument = true;
                parsed.documentEntry = entry.name;
            } else if (folded == "guidocument.xml") {
                parsed.hasGuiDocument = true;
            }
            if (folded.size() >= 4 && folded.ends_with(".brp")) hasShape = true;
            parsed.entries.push_back(std::move(entry));
        }
        cursor = next;
    }
    if (cursor != centralEnd) return FcstdPreflightStatus::InvalidDirectory;
    std::ranges::sort(ranges);
    for (std::size_t index = 1; index < ranges.size(); ++index)
        if (ranges[index].first < ranges[index - 1].second)
            return FcstdPreflightStatus::Overlap;
    if (!hasDocument) return FcstdPreflightStatus::MissingDocument;
    if (!hasShape) return FcstdPreflightStatus::MissingShape;
    if (package) *package = std::move(parsed);
    return FcstdPreflightStatus::Ok;
}

FcstdPreflightStatus ExtractFcstdEntry(std::span<const std::byte> bytes,
                                       const FcstdEntry& entry,
                                       std::vector<std::byte>& output,
                                       std::uint64_t maxExpandedBytes,
                                       const std::function<bool()>& isCancelled)
{
    output.clear();
    if (isCancelled && isCancelled()) return FcstdPreflightStatus::Cancelled;
    if (entry.expandedBytes > maxExpandedBytes || entry.expandedBytes > SIZE_MAX)
        return FcstdPreflightStatus::EntryTooLarge;
    std::uint64_t end = 0;
    if (!Add(entry.dataOffset, entry.compressedBytes, end) || end > bytes.size())
        return FcstdPreflightStatus::Truncated;
    try {
        output.resize(std::size_t(entry.expandedBytes));
    } catch (const std::bad_alloc&) {
        return FcstdPreflightStatus::EntryTooLarge;
    }
    const auto input = bytes.subspan(std::size_t(entry.dataOffset),
                                     std::size_t(entry.compressedBytes));
    if (entry.method == 0) {
        if (entry.compressedBytes != entry.expandedBytes) {
            output.clear();
            return FcstdPreflightStatus::InvalidDirectory;
        }
        if (!input.empty()) std::memcpy(output.data(), input.data(), input.size());
    } else if (entry.method == 8) {
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            output.clear();
            return FcstdPreflightStatus::InvalidDirectory;
        }
        std::uint64_t inOffset = 0;
        std::uint64_t outOffset = 0;
        int result = Z_OK;
        while (result == Z_OK) {
            if (isCancelled && isCancelled()) {
                inflateEnd(&stream);
                output.clear();
                return FcstdPreflightStatus::Cancelled;
            }
            if (stream.avail_in == 0 && inOffset < input.size()) {
                const auto count = (std::min)(std::uint64_t(UINT_MAX),
                                              std::uint64_t(input.size()) - inOffset);
                stream.next_in = reinterpret_cast<Bytef*>(
                    const_cast<std::byte*>(input.data() + inOffset));
                stream.avail_in = static_cast<uInt>(count);
                inOffset += count;
            }
            if (stream.avail_out == 0 && outOffset < output.size()) {
                const auto count = (std::min)(std::uint64_t(UINT_MAX),
                                              std::uint64_t(output.size()) - outOffset);
                stream.next_out = reinterpret_cast<Bytef*>(output.data() + outOffset);
                stream.avail_out = static_cast<uInt>(count);
                outOffset += count;
            }
            result = inflate(&stream, Z_NO_FLUSH);
            if (result == Z_BUF_ERROR) break;
        }
        const bool valid = result == Z_STREAM_END
            && stream.total_in == entry.compressedBytes
            && stream.total_out == entry.expandedBytes;
        inflateEnd(&stream);
        if (!valid) {
            output.clear();
            return FcstdPreflightStatus::InvalidDirectory;
        }
    } else {
        output.clear();
        return FcstdPreflightStatus::UnsupportedCompression;
    }
    if (isCancelled && isCancelled()) {
        output.clear();
        return FcstdPreflightStatus::Cancelled;
    }
    if (!CheckCrc32(output, entry.crc32, isCancelled)) {
        output.clear();
        return isCancelled && isCancelled() ? FcstdPreflightStatus::Cancelled
                                            : FcstdPreflightStatus::InvalidDirectory;
    }
    return FcstdPreflightStatus::Ok;
}

const FcstdEntry* FindFcstdEntry(const FcstdPackage& package, std::string_view foldedName)
{
    std::string folded(foldedName);
    std::ranges::transform(folded, folded.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    for (const auto& entry : package.entries)
        if (entry.foldedName == folded) return &entry;
    return nullptr;
}

model_core::ImportErrorCode MapFcstdPreflightStatus(std::uint32_t status)
{
    switch (static_cast<FcstdPreflightStatus>(status)) {
    case FcstdPreflightStatus::Ok:
        return model_core::ImportErrorCode::None;
    case FcstdPreflightStatus::NotZip:
    case FcstdPreflightStatus::Truncated:
    case FcstdPreflightStatus::DataDescriptor:
    case FcstdPreflightStatus::UnsafePath:
    case FcstdPreflightStatus::DuplicateEntry:
    case FcstdPreflightStatus::InvalidDirectory:
    case FcstdPreflightStatus::Overlap:
    case FcstdPreflightStatus::MissingDocument:
    case FcstdPreflightStatus::MissingShape:
        return model_core::ImportErrorCode::MalformedData;
    case FcstdPreflightStatus::Encrypted:
    case FcstdPreflightStatus::UnsupportedCompression:
        return model_core::ImportErrorCode::UnsupportedRequiredFeature;
    case FcstdPreflightStatus::MultiDisk:
    case FcstdPreflightStatus::EntryLimit:
    case FcstdPreflightStatus::EntryTooLarge:
    case FcstdPreflightStatus::AggregateTooLarge:
    case FcstdPreflightStatus::ExpansionRatio:
    case FcstdPreflightStatus::SourceLimit:
        return model_core::ImportErrorCode::ResourceLimit;
    case FcstdPreflightStatus::ReadFailure:
        return model_core::ImportErrorCode::FileUnavailable;
    case FcstdPreflightStatus::Cancelled:
        return model_core::ImportErrorCode::Cancelled;
    }
    return model_core::ImportErrorCode::InternalImporterFailure;
}

} // namespace step_host

#include "parser_core/PlyParserCore.h"

#include "platform/CheckedMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace parser_core {

namespace {

// Header/list/decode-unit limits supplement the Tier A count and scratch caps.
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxLineLength = 4096;
constexpr std::size_t kMaxHeaderLines = 4096;
constexpr std::size_t kMaxElementCount = 64;
constexpr std::size_t kMaxPropertiesPerElement = 64;

std::uint16_t ByteSwap16(std::uint16_t v)
{
    return static_cast<std::uint16_t>((v << 8) | (v >> 8));
}

std::uint32_t ByteSwap32(std::uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8)
        | ((v & 0xFF000000u) >> 24);
}

std::uint64_t ByteSwap64(std::uint64_t v)
{
    return (static_cast<std::uint64_t>(ByteSwap32(static_cast<std::uint32_t>(v))) << 32)
        | ByteSwap32(static_cast<std::uint32_t>(v >> 32));
}

std::optional<std::vector<HeaderLine>> SplitHeaderLines(std::string_view scanRegion)
{
    std::vector<HeaderLine> lines;
    std::size_t pos = 0;
    while (pos < scanRegion.size()) {
        if (lines.size() >= kMaxHeaderLines) {
            return std::nullopt;
        }
        std::size_t newlinePos = scanRegion.find('\n', pos);
        std::size_t lineEndExclNewline = (newlinePos == std::string_view::npos) ? scanRegion.size() : newlinePos;
        std::string_view line = scanRegion.substr(pos, lineEndExclNewline - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.size() > kMaxLineLength) {
            return std::nullopt;
        }
        std::size_t endOffset = (newlinePos == std::string_view::npos) ? scanRegion.size() : newlinePos + 1;
        lines.push_back({ line, endOffset });
        // Header limits apply to text only. Binary vertex data can contain
        // arbitrarily long runs without LF (or thousands of incidental LFs).
        // Do not split/check the payload as if it were more header lines.
        const auto first = line.find_first_not_of(" \t");
        const auto last = line.find_last_not_of(" \t");
        if (first != std::string_view::npos && line.substr(first, last - first + 1) == "end_header") {
            break;
        }
        if (newlinePos == std::string_view::npos) {
            break;
        }
        pos = newlinePos + 1;
    }
    return lines;
}

std::vector<std::string_view> Tokenize(std::string_view line)
{
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (i > start) {
            tokens.push_back(line.substr(start, i - start));
        }
    }
    return tokens;
}

std::optional<std::uint64_t> ParseDecimalUInt64(std::string_view text)
{
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return std::nullopt; // overflow
        }
        value = value * 10 + digit;
    }
    return value;
}

} // namespace

std::size_t ScalarByteSize(PlyScalarType type)
{
    switch (type) {
    case PlyScalarType::Int8:
    case PlyScalarType::UInt8:
        return 1;
    case PlyScalarType::Int16:
    case PlyScalarType::UInt16:
        return 2;
    case PlyScalarType::Int32:
    case PlyScalarType::UInt32:
    case PlyScalarType::Float32:
        return 4;
    case PlyScalarType::Float64:
        return 8;
    }
    return 0;
}

float NormalizeColor(double value, PlyScalarType type)
{
    double scale = 1.0;
    switch (type) {
    case PlyScalarType::UInt8: scale = 255.0; break;
    case PlyScalarType::UInt16: scale = 65535.0; break;
    case PlyScalarType::UInt32: scale = 4294967295.0; break;
    case PlyScalarType::Int8: scale = 127.0; break;
    case PlyScalarType::Int16: scale = 32767.0; break;
    case PlyScalarType::Int32: scale = 2147483647.0; break;
    default: break;
    }
    return static_cast<float>(std::clamp(value / scale, 0.0, 1.0));
}

std::optional<PlyScalarType> ParseScalarTypeName(std::string_view name)
{
    if (name == "char" || name == "int8")
        return PlyScalarType::Int8;
    if (name == "uchar" || name == "uint8")
        return PlyScalarType::UInt8;
    if (name == "short" || name == "int16")
        return PlyScalarType::Int16;
    if (name == "ushort" || name == "uint16")
        return PlyScalarType::UInt16;
    if (name == "int" || name == "int32")
        return PlyScalarType::Int32;
    if (name == "uint" || name == "uint32")
        return PlyScalarType::UInt32;
    if (name == "float" || name == "float32")
        return PlyScalarType::Float32;
    if (name == "double" || name == "float64")
        return PlyScalarType::Float64;
    return std::nullopt;
}

double ReadScalarAsDouble(PlyScalarType type, bool bigEndian, std::span<const std::byte> bytes)
{
    switch (type) {
    case PlyScalarType::Int8: {
        int8_t v;
        std::memcpy(&v, bytes.data(), 1);
        return static_cast<double>(v);
    }
    case PlyScalarType::UInt8: {
        uint8_t v;
        std::memcpy(&v, bytes.data(), 1);
        return static_cast<double>(v);
    }
    case PlyScalarType::Int16: {
        uint16_t raw;
        std::memcpy(&raw, bytes.data(), 2);
        if (bigEndian)
            raw = ByteSwap16(raw);
        int16_t v;
        std::memcpy(&v, &raw, 2);
        return static_cast<double>(v);
    }
    case PlyScalarType::UInt16: {
        uint16_t raw;
        std::memcpy(&raw, bytes.data(), 2);
        if (bigEndian)
            raw = ByteSwap16(raw);
        return static_cast<double>(raw);
    }
    case PlyScalarType::Int32: {
        uint32_t raw;
        std::memcpy(&raw, bytes.data(), 4);
        if (bigEndian)
            raw = ByteSwap32(raw);
        int32_t v;
        std::memcpy(&v, &raw, 4);
        return static_cast<double>(v);
    }
    case PlyScalarType::UInt32: {
        uint32_t raw;
        std::memcpy(&raw, bytes.data(), 4);
        if (bigEndian)
            raw = ByteSwap32(raw);
        return static_cast<double>(raw);
    }
    case PlyScalarType::Float32: {
        uint32_t raw;
        std::memcpy(&raw, bytes.data(), 4);
        if (bigEndian)
            raw = ByteSwap32(raw);
        float v;
        std::memcpy(&v, &raw, 4);
        return static_cast<double>(v);
    }
    case PlyScalarType::Float64: {
        uint64_t raw;
        std::memcpy(&raw, bytes.data(), 8);
        if (bigEndian)
            raw = ByteSwap64(raw);
        double v;
        std::memcpy(&v, &raw, 8);
        return v;
    }
    }
    return 0.0;
}

std::variant<PlyHeader, model_core::ImportErrorCode> ParseHeader(std::span<const std::byte> source)
{
    std::size_t scanLimit = source.size() < kMaxHeaderBytes ? source.size() : kMaxHeaderBytes;
    std::string_view scanRegion(reinterpret_cast<const char*>(source.data()), scanLimit);

    auto linesOpt = SplitHeaderLines(scanRegion);
    if (!linesOpt) {
        return model_core::ImportErrorCode::ResourceLimit;
    }
    const auto& lines = *linesOpt;

    if (lines.empty() || lines[0].text != "ply") {
        return model_core::ImportErrorCode::MalformedData;
    }

    PlyHeader header;
    header.elements.reserve(kMaxElementCount); // avoids currentElement pointer invalidation below
    bool formatSeen = false;
    bool endHeaderSeen = false;
    PlyElement* currentElement = nullptr;

    for (std::size_t i = 1; i < lines.size(); ++i) {
        auto tokens = Tokenize(lines[i].text);
        if (tokens.empty()) {
            continue; // blank line tolerated
        }

        if (tokens[0] == "end_header") {
            header.bodyOffset = lines[i].endOffsetInSource;
            endHeaderSeen = true;
            break;
        }
        if (tokens[0] == "comment" || tokens[0] == "obj_info") {
            continue;
        }
        if (tokens[0] == "format") {
            if (formatSeen || tokens.size() != 3 || tokens[2] != "1.0") {
                return model_core::ImportErrorCode::MalformedData;
            }
            if (tokens[1] == "binary_little_endian") {
                header.format = PlyFormat::BinaryLittleEndian;
            } else if (tokens[1] == "binary_big_endian") {
                header.format = PlyFormat::BinaryBigEndian;
            } else if (tokens[1] == "ascii") {
                header.format = PlyFormat::Ascii;
            } else {
                return model_core::ImportErrorCode::MalformedData; // unrecognized format keyword
            }
            formatSeen = true;
            continue;
        }
        if (tokens[0] == "element") {
            if (!formatSeen || tokens.size() != 3) {
                return model_core::ImportErrorCode::MalformedData;
            }
            if (header.elements.size() >= kMaxElementCount) {
                return model_core::ImportErrorCode::ResourceLimit;
            }
            auto countOpt = ParseDecimalUInt64(tokens[2]);
            if (!countOpt) {
                return model_core::ImportErrorCode::MalformedData;
            }
            PlyElement element;
            element.name = std::string(tokens[1]);
            element.count = *countOpt;
            header.elements.push_back(std::move(element));
            currentElement = &header.elements.back();
            continue;
        }
        if (tokens[0] == "property") {
            if (currentElement == nullptr) {
                return model_core::ImportErrorCode::MalformedData;
            }
            if (currentElement->properties.size() >= kMaxPropertiesPerElement) {
                return model_core::ImportErrorCode::ResourceLimit;
            }
            if (tokens.size() >= 2 && tokens[1] == "list") {
                if (tokens.size() != 5) {
                    return model_core::ImportErrorCode::MalformedData;
                }
                auto countType = ParseScalarTypeName(tokens[2]);
                auto valueType = ParseScalarTypeName(tokens[3]);
                if (!countType || !valueType) {
                    return model_core::ImportErrorCode::MalformedData;
                }
                PlyProperty prop;
                prop.isList = true;
                prop.countType = *countType;
                prop.valueType = *valueType;
                prop.name = std::string(tokens[4]);
                currentElement->properties.push_back(std::move(prop));
            } else {
                if (tokens.size() != 3) {
                    return model_core::ImportErrorCode::MalformedData;
                }
                auto valueType = ParseScalarTypeName(tokens[1]);
                if (!valueType) {
                    return model_core::ImportErrorCode::MalformedData;
                }
                PlyProperty prop;
                prop.isList = false;
                prop.valueType = *valueType;
                prop.name = std::string(tokens[2]);
                currentElement->properties.push_back(std::move(prop));
            }
            continue;
        }
        return model_core::ImportErrorCode::MalformedData; // unrecognized header keyword
    }

    if (!endHeaderSeen) {
        bool truncatedByCap = (scanLimit == kMaxHeaderBytes) && (source.size() > kMaxHeaderBytes);
        return truncatedByCap ? model_core::ImportErrorCode::ResourceLimit
                              : model_core::ImportErrorCode::MalformedData;
    }
    if (!formatSeen) {
        return model_core::ImportErrorCode::MalformedData;
    }

    return header;
}

std::optional<std::span<const std::byte>> ReadBytes(std::span<const std::byte> source, std::uint64_t& cursor,
                                                    std::uint64_t n)
{
    auto end = platform::CheckedAdd(cursor, n);
    if (!end || *end > source.size()) {
        return std::nullopt;
    }
    auto bytes = source.subspan(static_cast<std::size_t>(cursor), static_cast<std::size_t>(n));
    cursor = *end;
    return bytes;
}

bool IsAsciiPly(std::span<const std::byte> sourceHeader)
{
    auto parsed = ParseHeader(sourceHeader);
    if (const auto* header = std::get_if<PlyHeader>(&parsed))
        return header->format == PlyFormat::Ascii;
    return false;
}

Vec3f Cross(const Vec3f& a, const Vec3f& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

float Dot(const Vec3f& a, const Vec3f& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace parser_core
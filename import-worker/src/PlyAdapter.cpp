#include "PlyAdapter.h"

#include "model_core/Checksum.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/CheckedMath.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace import_worker {

namespace {

using namespace model_core;
using platform::CheckedAdd;
using platform::CheckedMultiply;

// No full Tier-A hard-limits table enforcement yet (.docs/design/03-file-formats-and-ingestion.md)
// -- self-contained sanity constants, same simplification StlAdapter.cpp's
// kMaxFacets already made.
constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxLineLength = 4096;
constexpr size_t kMaxHeaderLines = 4096;
constexpr size_t kMaxElementCount = 64;
constexpr size_t kMaxPropertiesPerElement = 64;
constexpr uint64_t kMaxVertices = 20'000'000;
constexpr uint64_t kMaxFaces = 4'000'000;
constexpr uint64_t kMaxPolygonVerticesPerFace = 255; // largest value a conventional uchar count_type encodes
constexpr uint64_t kMaxTrianglesAfterTriangulation = 8'000'000; // running total across all faces --
    // kMaxFaces * (kMaxPolygonVerticesPerFace-2) alone amplifies past any single-file limit
constexpr uint64_t kMaxSkippedElementRecordCount = 4'000'000;
constexpr uint64_t kMaxSkippedListLength = 65'536; // generic bound for any list-typed property
    // this adapter doesn't specifically recognize (vertex-element lists, non-index face lists,
    // and any property on an unrecognized element)

enum class PlyScalarType {
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Float32,
    Float64,
};

size_t ScalarByteSize(PlyScalarType type)
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

uint16_t ByteSwap16(uint16_t v)
{
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}

uint32_t ByteSwap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8)
        | ((v & 0xFF000000u) >> 24);
}

uint64_t ByteSwap64(uint64_t v)
{
    return (static_cast<uint64_t>(ByteSwap32(static_cast<uint32_t>(v))) << 32)
        | ByteSwap32(static_cast<uint32_t>(v >> 32));
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

struct PlyProperty {
    bool isList = false;
    PlyScalarType countType = PlyScalarType::UInt8; // list only
    PlyScalarType valueType = PlyScalarType::Float32;
    std::string name;
};

struct PlyElement {
    std::string name;
    uint64_t count = 0;
    std::vector<PlyProperty> properties;
};

struct PlyHeader {
    bool bigEndian = false;
    std::vector<PlyElement> elements;
    uint64_t bodyOffset = 0; // byte offset in the source where binary data begins
};

struct HeaderLine {
    std::string_view text; // trimmed (no trailing '\r'), not including '\n'
    size_t endOffsetInSource;
};

std::optional<std::vector<HeaderLine>> SplitHeaderLines(std::string_view scanRegion)
{
    std::vector<HeaderLine> lines;
    size_t pos = 0;
    while (pos < scanRegion.size()) {
        if (lines.size() >= kMaxHeaderLines) {
            return std::nullopt;
        }
        size_t newlinePos = scanRegion.find('\n', pos);
        size_t lineEndExclNewline = (newlinePos == std::string_view::npos) ? scanRegion.size() : newlinePos;
        std::string_view line = scanRegion.substr(pos, lineEndExclNewline - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.size() > kMaxLineLength) {
            return std::nullopt;
        }
        size_t endOffset = (newlinePos == std::string_view::npos) ? scanRegion.size() : newlinePos + 1;
        lines.push_back({ line, endOffset });
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
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (i > start) {
            tokens.push_back(line.substr(start, i - start));
        }
    }
    return tokens;
}

std::optional<uint64_t> ParseDecimalUInt64(std::string_view text)
{
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return std::nullopt; // overflow
        }
        value = value * 10 + digit;
    }
    return value;
}

// Text-header phase only -- everything from offset 0 up to (and including)
// "end_header"'s terminating newline. Binary element data is read
// separately by ImportPly, using PlyHeader::bodyOffset as the start cursor.
std::variant<PlyHeader, ImportErrorCode> ParseHeader(std::span<const std::byte> source)
{
    size_t scanLimit = source.size() < kMaxHeaderBytes ? source.size() : kMaxHeaderBytes;
    std::string_view scanRegion(reinterpret_cast<const char*>(source.data()), scanLimit);

    auto linesOpt = SplitHeaderLines(scanRegion);
    if (!linesOpt) {
        return ImportErrorCode::ResourceLimit;
    }
    const auto& lines = *linesOpt;

    if (lines.empty() || lines[0].text != "ply") {
        return ImportErrorCode::MalformedData;
    }

    PlyHeader header;
    header.elements.reserve(kMaxElementCount); // avoids currentElement pointer invalidation below
    bool formatSeen = false;
    bool endHeaderSeen = false;
    PlyElement* currentElement = nullptr;

    for (size_t i = 1; i < lines.size(); ++i) {
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
                return ImportErrorCode::MalformedData;
            }
            if (tokens[1] == "binary_little_endian") {
                header.bigEndian = false;
            } else if (tokens[1] == "binary_big_endian") {
                header.bigEndian = true;
            } else {
                // "ascii" (Tier B, out of scope) or an unrecognized keyword.
                return ImportErrorCode::MalformedData;
            }
            formatSeen = true;
            continue;
        }
        if (tokens[0] == "element") {
            if (!formatSeen || tokens.size() != 3) {
                return ImportErrorCode::MalformedData;
            }
            if (header.elements.size() >= kMaxElementCount) {
                return ImportErrorCode::ResourceLimit;
            }
            auto countOpt = ParseDecimalUInt64(tokens[2]);
            if (!countOpt) {
                return ImportErrorCode::MalformedData;
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
                return ImportErrorCode::MalformedData;
            }
            if (currentElement->properties.size() >= kMaxPropertiesPerElement) {
                return ImportErrorCode::ResourceLimit;
            }
            if (tokens.size() >= 2 && tokens[1] == "list") {
                if (tokens.size() != 5) {
                    return ImportErrorCode::MalformedData;
                }
                auto countType = ParseScalarTypeName(tokens[2]);
                auto valueType = ParseScalarTypeName(tokens[3]);
                if (!countType || !valueType) {
                    return ImportErrorCode::MalformedData;
                }
                PlyProperty prop;
                prop.isList = true;
                prop.countType = *countType;
                prop.valueType = *valueType;
                prop.name = std::string(tokens[4]);
                currentElement->properties.push_back(std::move(prop));
            } else {
                if (tokens.size() != 3) {
                    return ImportErrorCode::MalformedData;
                }
                auto valueType = ParseScalarTypeName(tokens[1]);
                if (!valueType) {
                    return ImportErrorCode::MalformedData;
                }
                PlyProperty prop;
                prop.isList = false;
                prop.valueType = *valueType;
                prop.name = std::string(tokens[2]);
                currentElement->properties.push_back(std::move(prop));
            }
            continue;
        }
        return ImportErrorCode::MalformedData; // unrecognized header keyword
    }

    if (!endHeaderSeen) {
        bool truncatedByCap = (scanLimit == kMaxHeaderBytes) && (source.size() > kMaxHeaderBytes);
        return truncatedByCap ? ImportErrorCode::ResourceLimit : ImportErrorCode::MalformedData;
    }
    if (!formatSeen) {
        return ImportErrorCode::MalformedData;
    }

    return header;
}

std::optional<std::span<const std::byte>> ReadBytes(std::span<const std::byte> source, uint64_t& cursor,
                                                      uint64_t n)
{
    auto end = CheckedAdd(cursor, n);
    if (!end || *end > source.size()) {
        return std::nullopt;
    }
    auto bytes = source.subspan(static_cast<size_t>(cursor), static_cast<size_t>(n));
    cursor = *end;
    return bytes;
}

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3 operator-(const Vec3& other) const { return { x - other.x, y - other.y, z - other.z }; }
    Vec3 operator+(const Vec3& other) const { return { x + other.x, y + other.y, z + other.z }; }
};

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

float Dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace

std::variant<PlyImportResult, ImportErrorCode> ImportPly(std::span<const std::byte> sourcePlyBytes,
                                                            std::span<std::byte> destination,
                                                            uint64_t generationId,
                                                            uint32_t maxChunkCount)
{
    if (maxChunkCount < 1) {
        return ImportErrorCode::ResourceLimit;
    }

    auto headerResult = ParseHeader(sourcePlyBytes);
    if (auto* err = std::get_if<ImportErrorCode>(&headerResult)) {
        return *err;
    }
    PlyHeader header = std::move(std::get<PlyHeader>(headerResult));

    PlyElement* vertexElement = nullptr;
    PlyElement* faceElement = nullptr;
    for (auto& element : header.elements) {
        if (element.name == "vertex" && vertexElement == nullptr) {
            vertexElement = &element;
        } else if (element.name == "face" && faceElement == nullptr) {
            faceElement = &element;
        }
    }
    if (vertexElement == nullptr) {
        return ImportErrorCode::MalformedData;
    }
    if (vertexElement->count > kMaxVertices) {
        return ImportErrorCode::ResourceLimit;
    }

    int xIdx = -1, yIdx = -1, zIdx = -1;
    int nxIdx = -1, nyIdx = -1, nzIdx = -1;
    int uIdx = -1, vIdx = -1, sIdx = -1, tIdx = -1;
    for (size_t i = 0; i < vertexElement->properties.size(); ++i) {
        const PlyProperty& p = vertexElement->properties[i];
        if (p.isList) {
            continue; // a list-typed property is never recognized as position/normal/uv
        }
        int idx = static_cast<int>(i);
        if (p.name == "x")
            xIdx = idx;
        else if (p.name == "y")
            yIdx = idx;
        else if (p.name == "z")
            zIdx = idx;
        else if (p.name == "nx")
            nxIdx = idx;
        else if (p.name == "ny")
            nyIdx = idx;
        else if (p.name == "nz")
            nzIdx = idx;
        else if (p.name == "u")
            uIdx = idx;
        else if (p.name == "v")
            vIdx = idx;
        else if (p.name == "s")
            sIdx = idx;
        else if (p.name == "t")
            tIdx = idx;
    }
    if (xIdx < 0 || yIdx < 0 || zIdx < 0) {
        return ImportErrorCode::MalformedData;
    }
    bool hasNormal = (nxIdx >= 0 && nyIdx >= 0 && nzIdx >= 0);
    bool hasUvUv = (uIdx >= 0 && vIdx >= 0);
    bool hasUvSt = (!hasUvUv) && (sIdx >= 0 && tIdx >= 0);
    bool hasUv = hasUvUv || hasUvSt;
    int actualUIdx = hasUvUv ? uIdx : (hasUvSt ? sIdx : -1);
    int actualVIdx = hasUvUv ? vIdx : (hasUvSt ? tIdx : -1);

    bool hasFace = (faceElement != nullptr && faceElement->count > 0);
    int listPropIdx = -1;
    if (faceElement != nullptr) {
        if (faceElement->count > kMaxFaces) {
            return ImportErrorCode::ResourceLimit;
        }
        if (hasFace) {
            for (size_t i = 0; i < faceElement->properties.size(); ++i) {
                const auto& p = faceElement->properties[i];
                if (p.isList && (p.name == "vertex_indices" || p.name == "vertex_index")) {
                    if (listPropIdx >= 0) {
                        return ImportErrorCode::MalformedData; // more than one -- ambiguous
                    }
                    listPropIdx = static_cast<int>(i);
                }
            }
            if (listPropIdx < 0) {
                return ImportErrorCode::MalformedData;
            }
        }
    }

    for (const auto& element : header.elements) {
        bool isVertex = (&element == vertexElement);
        bool isFace = (faceElement != nullptr && &element == faceElement);
        if (isVertex || isFace) {
            continue;
        }
        if (element.count > kMaxSkippedElementRecordCount) {
            return ImportErrorCode::ResourceLimit;
        }
    }

    uint64_t cursor = header.bodyOffset;
    if (cursor > sourcePlyBytes.size()) {
        return ImportErrorCode::MalformedData;
    }

    auto readScalar = [&](PlyScalarType type) -> std::optional<double> {
        auto bytes = ReadBytes(sourcePlyBytes, cursor, ScalarByteSize(type));
        if (!bytes) {
            return std::nullopt;
        }
        return ReadScalarAsDouble(type, header.bigEndian, *bytes);
    };

    auto skipList = [&](const PlyProperty& prop) -> std::optional<ImportErrorCode> {
        auto countOpt = readScalar(prop.countType);
        if (!countOpt || *countOpt < 0.0) {
            return ImportErrorCode::MalformedData;
        }
        if (*countOpt > static_cast<double>(kMaxSkippedListLength)) {
            return ImportErrorCode::ResourceLimit;
        }
        uint64_t count = static_cast<uint64_t>(*countOpt);
        size_t valueSize = ScalarByteSize(prop.valueType);
        for (uint64_t i = 0; i < count; ++i) {
            if (!ReadBytes(sourcePlyBytes, cursor, valueSize)) {
                return ImportErrorCode::MalformedData;
            }
        }
        return std::nullopt;
    };

    std::vector<VertexPositionNormalUv0F32> meshVertices;
    std::vector<VertexPositionOnlyF32> pointVertices;
    std::vector<uint32_t> meshIndices;
    uint64_t triangleTotal = 0;

    if (hasFace) {
        meshVertices.reserve(static_cast<size_t>(vertexElement->count));
        meshIndices.reserve(static_cast<size_t>(vertexElement->count) * 3);
    } else {
        pointVertices.reserve(static_cast<size_t>(vertexElement->count));
    }

    // Walk declared elements in header order -- never assume vertex precedes
    // face on disk, even though every real file does.
    for (const auto& element : header.elements) {
        bool isVertex = (&element == vertexElement);
        bool isFace = (faceElement != nullptr && &element == faceElement);

        for (uint64_t recordIndex = 0; recordIndex < element.count; ++recordIndex) {
            if (isVertex) {
                double x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0, u = 0, v = 0;
                for (size_t pi = 0; pi < element.properties.size(); ++pi) {
                    const PlyProperty& prop = element.properties[pi];
                    if (prop.isList) {
                        auto err = skipList(prop);
                        if (err) {
                            return *err;
                        }
                        continue;
                    }
                    auto valOpt = readScalar(prop.valueType);
                    if (!valOpt) {
                        return ImportErrorCode::MalformedData;
                    }
                    int idx = static_cast<int>(pi);
                    if (idx == xIdx)
                        x = *valOpt;
                    else if (idx == yIdx)
                        y = *valOpt;
                    else if (idx == zIdx)
                        z = *valOpt;
                    else if (hasNormal && idx == nxIdx)
                        nx = *valOpt;
                    else if (hasNormal && idx == nyIdx)
                        ny = *valOpt;
                    else if (hasNormal && idx == nzIdx)
                        nz = *valOpt;
                    else if (hasUv && idx == actualUIdx)
                        u = *valOpt;
                    else if (hasUv && idx == actualVIdx)
                        v = *valOpt;
                    // else: a recognized-but-unused (color, etc.) or wholly
                    // unrecognized scalar -- read for cursor correctness,
                    // discarded from wire output.
                }

                float px = static_cast<float>(x), py = static_cast<float>(y), pz = static_cast<float>(z);
                if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) {
                    // Fails the whole file, unlike StlAdapter.cpp's per-facet
                    // drop: PLY vertices are shared/indexed across faces, so
                    // dropping just this one would require renumbering every
                    // face that referenced it.
                    return ImportErrorCode::MalformedData;
                }

                float pnx = 0.0f, pny = 0.0f, pnz = 1.0f;
                if (hasNormal) {
                    float rnx = static_cast<float>(nx), rny = static_cast<float>(ny),
                          rnz = static_cast<float>(nz);
                    bool finite = std::isfinite(rnx) && std::isfinite(rny) && std::isfinite(rnz);
                    float lenSq = rnx * rnx + rny * rny + rnz * rnz;
                    if (finite && lenSq > 1e-12f) {
                        float invLen = 1.0f / std::sqrt(lenSq);
                        pnx = rnx * invLen;
                        pny = rny * invLen;
                        pnz = rnz * invLen;
                    }
                    // else: non-finite or zero supplied normal -- per-vertex
                    // fallback to (0,0,1) already set above, file is not
                    // failed (topology-safe, same tolerance spirit as STL).
                }

                if (hasFace) {
                    VertexPositionNormalUv0F32 vertex{};
                    vertex.px = px;
                    vertex.py = py;
                    vertex.pz = pz;
                    vertex.nx = pnx;
                    vertex.ny = pny;
                    vertex.nz = pnz;
                    vertex.u = hasUv ? static_cast<float>(u) : 0.0f;
                    vertex.v = hasUv ? static_cast<float>(v) : 0.0f;
                    meshVertices.push_back(vertex);
                } else {
                    pointVertices.push_back(VertexPositionOnlyF32{ px, py, pz });
                }
            } else if (isFace) {
                std::vector<uint64_t> faceIndices;
                std::optional<ImportErrorCode> hardError;

                for (size_t pi = 0; pi < element.properties.size() && !hardError; ++pi) {
                    const PlyProperty& prop = element.properties[pi];
                    if (!prop.isList) {
                        auto valOpt = readScalar(prop.valueType);
                        if (!valOpt) {
                            hardError = ImportErrorCode::MalformedData;
                        }
                        continue;
                    }

                    auto countOpt = readScalar(prop.countType);
                    if (!countOpt || *countOpt < 0.0) {
                        hardError = ImportErrorCode::MalformedData;
                        continue;
                    }

                    if (static_cast<int>(pi) == listPropIdx) {
                        if (*countOpt > static_cast<double>(kMaxPolygonVerticesPerFace)) {
                            hardError = ImportErrorCode::ResourceLimit;
                            continue;
                        }
                        uint64_t count = static_cast<uint64_t>(*countOpt);
                        faceIndices.clear();
                        faceIndices.reserve(static_cast<size_t>(count));
                        for (uint64_t k = 0; k < count; ++k) {
                            auto idxOpt = readScalar(prop.valueType);
                            if (!idxOpt || *idxOpt < 0.0) {
                                hardError = ImportErrorCode::MalformedData;
                                break;
                            }
                            faceIndices.push_back(static_cast<uint64_t>(*idxOpt));
                        }
                    } else {
                        if (*countOpt > static_cast<double>(kMaxSkippedListLength)) {
                            hardError = ImportErrorCode::ResourceLimit;
                            continue;
                        }
                        uint64_t count = static_cast<uint64_t>(*countOpt);
                        size_t valueSize = ScalarByteSize(prop.valueType);
                        for (uint64_t k = 0; k < count; ++k) {
                            if (!ReadBytes(sourcePlyBytes, cursor, valueSize)) {
                                hardError = ImportErrorCode::MalformedData;
                                break;
                            }
                        }
                    }
                }
                if (hardError) {
                    return *hardError;
                }

                if (faceIndices.size() < 3) {
                    continue; // degenerate face dropped
                }
                bool outOfRange = false;
                for (uint64_t idx : faceIndices) {
                    if (idx >= vertexElement->count) {
                        outOfRange = true;
                        break;
                    }
                }
                if (outOfRange) {
                    continue; // dropped, rest of the file keeps going
                }

                uint64_t triCount = static_cast<uint64_t>(faceIndices.size()) - 2;
                auto newTotalOpt = CheckedAdd(triangleTotal, triCount);
                if (!newTotalOpt || *newTotalOpt > kMaxTrianglesAfterTriangulation) {
                    return ImportErrorCode::ResourceLimit;
                }
                triangleTotal = *newTotalOpt;

                for (size_t i = 1; i + 1 < faceIndices.size(); ++i) {
                    meshIndices.push_back(static_cast<uint32_t>(faceIndices[0]));
                    meshIndices.push_back(static_cast<uint32_t>(faceIndices[i]));
                    meshIndices.push_back(static_cast<uint32_t>(faceIndices[i + 1]));
                }
            } else {
                // An element this adapter doesn't recognize: walk its
                // declared properties purely to advance the cursor, storing
                // nothing -- "unknown elements/properties skipped within
                // bounds," per the design doc.
                for (const auto& prop : element.properties) {
                    if (prop.isList) {
                        auto err = skipList(prop);
                        if (err) {
                            return *err;
                        }
                    } else {
                        auto valOpt = readScalar(prop.valueType);
                        if (!valOpt) {
                            return ImportErrorCode::MalformedData;
                        }
                    }
                }
            }
        }
    }

    ChunkTopology topology = ChunkTopology::Unknown;
    uint32_t vertexLayoutId = 0;
    uint64_t vertexCountOut = 0;
    uint64_t indexCountOut = 0;
    size_t vertexStride = 0;
    const void* vertexData = nullptr;
    const void* indexData = nullptr;

    if (hasFace) {
        if (meshIndices.empty()) {
            return ImportErrorCode::MalformedData; // every face dropped
        }
        if (!hasNormal) {
            std::vector<Vec3> accum(meshVertices.size(), Vec3{});
            for (size_t i = 0; i + 2 < meshIndices.size(); i += 3) {
                uint32_t ia = meshIndices[i], ib = meshIndices[i + 1], ic = meshIndices[i + 2];
                Vec3 pa{ meshVertices[ia].px, meshVertices[ia].py, meshVertices[ia].pz };
                Vec3 pb{ meshVertices[ib].px, meshVertices[ib].py, meshVertices[ib].pz };
                Vec3 pc{ meshVertices[ic].px, meshVertices[ic].py, meshVertices[ic].pz };
                Vec3 faceNormal = Cross(pb - pa, pc - pa);
                if (Dot(faceNormal, faceNormal) <= 1e-12f) {
                    continue; // degenerate triangle contributes nothing
                }
                accum[ia] = accum[ia] + faceNormal;
                accum[ib] = accum[ib] + faceNormal;
                accum[ic] = accum[ic] + faceNormal;
            }
            for (size_t i = 0; i < meshVertices.size(); ++i) {
                float lenSq = Dot(accum[i], accum[i]);
                if (lenSq <= 1e-12f) {
                    meshVertices[i].nx = 0.0f;
                    meshVertices[i].ny = 0.0f;
                    meshVertices[i].nz = 1.0f;
                } else {
                    float invLen = 1.0f / std::sqrt(lenSq);
                    meshVertices[i].nx = accum[i].x * invLen;
                    meshVertices[i].ny = accum[i].y * invLen;
                    meshVertices[i].nz = accum[i].z * invLen;
                }
            }
        }
        topology = ChunkTopology::TriangleList;
        vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionNormalUv0_F32);
        vertexCountOut = meshVertices.size();
        indexCountOut = meshIndices.size();
        vertexStride = sizeof(VertexPositionNormalUv0F32);
        vertexData = meshVertices.data();
        indexData = meshIndices.data();
    } else {
        if (pointVertices.empty()) {
            return ImportErrorCode::MalformedData;
        }
        topology = ChunkTopology::PointList;
        vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionOnly_F32);
        vertexCountOut = pointVertices.size();
        indexCountOut = 0;
        vertexStride = sizeof(VertexPositionOnlyF32);
        vertexData = pointVertices.data();
        indexData = nullptr;
    }

    // Compute layout and total size before writing anything -- mirrors
    // StlAdapter.cpp/GltfAdapter.cpp's "compute everything, check once, then
    // write sequentially, header last" structure. Always exactly one chunk.
    auto vertexBytesOpt = CheckedMultiply(vertexCountOut, static_cast<uint64_t>(vertexStride));
    if (!vertexBytesOpt) {
        return ImportErrorCode::ResourceLimit;
    }
    uint64_t vertexBytes = *vertexBytesOpt;

    uint64_t indexBytes = 0;
    if (hasFace) {
        auto indexBytesOpt = CheckedMultiply(indexCountOut, static_cast<uint64_t>(sizeof(uint32_t)));
        if (!indexBytesOpt) {
            return ImportErrorCode::ResourceLimit;
        }
        indexBytes = *indexBytesOpt;
    }

    auto payloadSizeOpt = CheckedAdd(vertexBytes, indexBytes);
    if (!payloadSizeOpt) {
        return ImportErrorCode::ResourceLimit;
    }
    uint64_t headerAndTable = static_cast<uint64_t>(kSectionHeaderSize) + kChunkDescriptorSize;
    auto sectionLengthOpt = CheckedAdd(headerAndTable, *payloadSizeOpt);
    if (!sectionLengthOpt) {
        return ImportErrorCode::ResourceLimit;
    }
    uint64_t payloadOffset = headerAndTable;
    uint64_t payloadSize = *payloadSizeOpt;
    uint64_t sectionLength = *sectionLengthOpt;

    if (sectionLength > destination.size()) {
        return ImportErrorCode::ResourceLimit;
    }

    std::memcpy(destination.data() + payloadOffset, vertexData, vertexBytes);
    if (hasFace) {
        std::memcpy(destination.data() + payloadOffset + vertexBytes, indexData, indexBytes);
    }

    ChunkDescriptor descriptor{};
    descriptor.sourceRangeOffset = 0;
    descriptor.sourceRangeLength = 0;
    descriptor.normalizedRangeOffset = payloadOffset;
    descriptor.normalizedRangeLength = payloadSize;
    descriptor.topology = topology;
    descriptor.indexCount = static_cast<uint32_t>(indexCountOut);
    descriptor.vertexCount = static_cast<uint32_t>(vertexCountOut);
    descriptor.vertexLayoutId = vertexLayoutId;
    descriptor.lodLevel = 0;
    descriptor.chunkId = 1;
    descriptor.byteSize = payloadSize;
    descriptor.dependencyCount = 0;
    descriptor.chunkChecksum = Fnv1a64(destination.subspan(payloadOffset, payloadSize));

    std::memcpy(destination.data() + kSectionHeaderSize, &descriptor, sizeof(descriptor));

    SectionHeader sectionHeader{};
    sectionHeader.magic = kSectionMagic;
    sectionHeader.protocolVersion = kCurrentProtocolVersion;
    sectionHeader.generationId = generationId;
    sectionHeader.sectionLength = sectionLength;
    sectionHeader.chunkCount = 1;
    sectionHeader.reserved = 0;
    sectionHeader.sectionChecksum
        = Fnv1a64(destination.subspan(kSectionHeaderSize, sectionLength - kSectionHeaderSize));

    std::memcpy(destination.data(), &sectionHeader, sizeof(sectionHeader));

    PlyImportResult result;
    result.chunkCount = 1;
    result.sectionBytesWritten = sectionLength;
    return result;
}

} // namespace import_worker

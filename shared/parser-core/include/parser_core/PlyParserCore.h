#pragma once

// Format-agnostic PLY parser primitives shared by the import worker and the
// thumbnail provider (T07, ADR-0004): the scalar-type model, endian-aware
// scalar reads, color normalization, and the bounded header parser. Only
// std/model-core/platform dependencies appear here; the wire-emitting body and
// the MappedFile-backed streaming stay in the worker adapter, so this source
// compiles into both Preview3DImportWorker.exe and
// Preview3DThumbnailProvider.dll with no broker/worker/viewer header.

#include "model_core/ImportError.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace parser_core {

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

std::size_t ScalarByteSize(PlyScalarType type);
std::optional<PlyScalarType> ParseScalarTypeName(std::string_view name);

// Maps a raw PLY color scalar to [0,1], honoring the declared integer range
// (uchar/ushort/uint/char/short/int). float/double inputs are clamped as-is.
float NormalizeColor(double value, PlyScalarType type);

// Reads one scalar of the given type, swapping for big-endian sources.
double ReadScalarAsDouble(PlyScalarType type, bool bigEndian, std::span<const std::byte> bytes);

struct PlyProperty {
    bool isList = false;
    PlyScalarType countType = PlyScalarType::UInt8; // list only
    PlyScalarType valueType = PlyScalarType::Float32;
    std::string name;
};

struct PlyElement {
    std::string name;
    std::uint64_t count = 0;
    std::vector<PlyProperty> properties;
};

enum class PlyFormat {
    BinaryLittleEndian,
    BinaryBigEndian,
    Ascii,
};

struct PlyHeader {
    PlyFormat format = PlyFormat::BinaryLittleEndian;
    std::vector<PlyElement> elements;
    std::uint64_t bodyOffset = 0; // byte offset in the source where body data begins
};

struct HeaderLine {
    std::string_view text; // trimmed (no trailing '\r'), not including '\n'
    std::size_t endOffsetInSource;
};

// Bounded text-header phase: everything from offset 0 up to (and including)
// "end_header"'s terminating newline. Never scans past the header limit.
std::variant<PlyHeader, model_core::ImportErrorCode> ParseHeader(std::span<const std::byte> source);

// Checked advance of a cursor over a span; nullopt on overflow/short read.
std::optional<std::span<const std::byte>> ReadBytes(std::span<const std::byte> source, std::uint64_t& cursor,
                                                    std::uint64_t n);

bool IsAsciiPly(std::span<const std::byte> sourceHeader);

// Small float vector used for generated face normals.
struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3f operator-(const Vec3f& other) const { return { x - other.x, y - other.y, z - other.z }; }
    Vec3f operator+(const Vec3f& other) const { return { x + other.x, y + other.y, z + other.z }; }
};

Vec3f Cross(const Vec3f& a, const Vec3f& b);
float Dot(const Vec3f& a, const Vec3f& b);

} // namespace parser_core
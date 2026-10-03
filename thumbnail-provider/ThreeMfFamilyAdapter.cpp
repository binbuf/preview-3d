// T32 3MF family adapter implementation (see ThreeMfFamilyAdapter.h).

#include "ThreeMfFamilyAdapter.h"

#include "ContainmentStage.h"
#include "Deadline.h"
#include "ProviderLimits.h"
#include "ThreeMfOpcPreflight.h"

#include <Bindings/Cpp/lib3mf_implicit.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace preview3d::provider {
namespace {

using Lib3MFException = Lib3MF::ELib3MFException;

// Provider-side package ceilings (T32 Scope): the worker uses a 4 GiB/200:1
// policy, but the thumbnail host is capped far lower.
constexpr std::uint64_t kAggregateExpansionMaxBytes = 128ull * 1024 * 1024;
constexpr std::uint32_t kExpansionRatioMax = 100;
constexpr std::uint32_t kMaxArchiveEntries = 4096;
constexpr std::uint32_t kMaxPathDepth = 32;

// One contained texture attachment may hold at most this many encoded bytes
// before it is discarded unread. Independent of the decoded-pixel budget.
constexpr std::uint64_t kEmbeddedImageEncodedMaxBytes = 32ull * 1024 * 1024;

// Per-lattice preview triangle ceiling (design/03, "bounded Beam Lattice").
constexpr std::uint64_t kLatticeTrianglesMax = 262'144;

// Hierarchy depth cap (the worker uses the same 256-level combined cap).
constexpr std::uint32_t kMaxHierarchyDepth = 256;

constexpr float kDefaultVertexColor = 0.8f;

bool Finite(double value) noexcept
{
    return std::isfinite(value) && std::abs(value) <= 1.0e30;
}

float SrgbToLinearComponent(float value) noexcept
{
    const float c = std::clamp(value, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

void Identity(double result[16]) noexcept
{
    std::fill(result, result + 16, 0.0);
    result[0] = result[5] = result[10] = result[15] = 1.0;
}

bool ToWire(const Lib3MF::sTransform& source, double result[16]) noexcept
{
    Identity(result);
    for (std::uint32_t row = 0; row < 4; ++row) {
        for (std::uint32_t column = 0; column < 3; ++column) {
            result[row * 4 + column] = static_cast<double>(source.m_Fields[row][column]);
            if (!Finite(result[row * 4 + column])) {
                return false;
            }
        }
    }
    return true;
}

bool ValidMatrix(const double matrix[16]) noexcept
{
    for (std::uint32_t index = 0; index < 16; ++index) {
        if (!Finite(matrix[index])) {
            return false;
        }
    }
    if (matrix[3] != 0.0 || matrix[7] != 0.0 || matrix[11] != 0.0 || matrix[15] != 1.0) {
        return false;
    }
    const double determinant =
        matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9])
        - matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8])
        + matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
    return Finite(determinant) && std::abs(determinant) >= 1.0e-18;
}

// Row-vector composition: `result = left * right` (a component is transformed
// into its containing object before the build placement is applied).
bool Multiply(const double left[16], const double right[16], double result[16]) noexcept
{
    double scratch[16]{};
    for (std::uint32_t row = 0; row < 4; ++row) {
        for (std::uint32_t column = 0; column < 4; ++column) {
            for (std::uint32_t item = 0; item < 4; ++item) {
                scratch[row * 4 + column] += left[row * 4 + item] * right[item * 4 + column];
            }
        }
    }
    if (!ValidMatrix(scratch)) {
        return false;
    }
    std::copy(scratch, scratch + 16, result);
    return true;
}

void TransformPoint(const double matrix[16], const float point[3], double result[3]) noexcept
{
    for (std::uint32_t axis = 0; axis < 3; ++axis) {
        result[axis] = static_cast<double>(point[0]) * matrix[axis]
            + static_cast<double>(point[1]) * matrix[4 + axis]
            + static_cast<double>(point[2]) * matrix[8 + axis] + matrix[12 + axis];
    }
}

bool AllowedType(Lib3MF::eObjectType type) noexcept
{
    switch (type) {
        case Lib3MF::eObjectType::Model:
        case Lib3MF::eObjectType::Support:
        case Lib3MF::eObjectType::SolidSupport:
        case Lib3MF::eObjectType::Surface:
        case Lib3MF::eObjectType::Other:
            return true;
    }
    return false;
}

// --- Allowlisted contained-image structural validation ----------------------

struct ImageInfo {
    bool png = false;
    bool jpeg = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

std::uint32_t ReadBe32(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 24)
        | (static_cast<std::uint32_t>(p[1]) << 16)
        | (static_cast<std::uint32_t>(p[2]) << 8)
        | static_cast<std::uint32_t>(p[3]);
}

std::uint32_t ReadBe16(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 8) | static_cast<std::uint32_t>(p[1]);
}

ImageInfo SniffImage(std::span<const std::byte> bytes) noexcept
{
    ImageInfo info;
    const auto* b = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::size_t n = bytes.size();
    static constexpr std::uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n >= 24 && std::memcmp(b, kPngSignature, sizeof(kPngSignature)) == 0) {
        info.png = true;
        info.width = ReadBe32(b + 16);
        info.height = ReadBe32(b + 20);
        return info;
    }
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
        info.jpeg = true;
        std::size_t pos = 2;
        while (pos + 3 < n) {
            if (b[pos] != 0xFF) {
                break;
            }
            const std::uint8_t marker = b[pos + 1];
            pos += 2;
            if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
                continue;
            }
            if (pos + 1 >= n) {
                break;
            }
            const std::uint32_t length = ReadBe16(b + pos);
            if (length < 2 || pos + length > n) {
                break;
            }
            const bool isSof = (marker >= 0xC0 && marker <= 0xC3)
                || (marker >= 0xC5 && marker <= 0xC7)
                || (marker >= 0xC9 && marker <= 0xCB)
                || (marker >= 0xCD && marker <= 0xCF);
            if (isSof && length >= 7) {
                info.height = ReadBe16(b + pos + 3);
                info.width = ReadBe16(b + pos + 5);
                break;
            }
            pos += length;
        }
        return info;
    }
    return info;
}

// --- Lattice generation (bounded, deterministic) ---------------------------

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, double value) noexcept { return {a.x * value, a.y * value, a.z * value}; }
double Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double Length(Vec3 value) noexcept { return std::sqrt(Dot(value, value)); }

Vec3 Normalized(Vec3 value) noexcept
{
    const double length = Length(value);
    return length > 1.0e-20 && Finite(length) ? value * (1.0 / length) : Vec3{0, 0, 1};
}

struct GenVertex {
    Vec3 position;
    Vec3 normal;
    float color[4] = {kDefaultVertexColor, kDefaultVertexColor, kDefaultVertexColor, 1.0f};
};

struct GenTriangle {
    GenVertex vertex[3];
};

void Basis(Vec3 axis, Vec3& first, Vec3& second) noexcept
{
    const Vec3 reference = std::abs(axis.z) < 0.8 ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    first = Normalized(Cross(reference, axis));
    second = Normalized(Cross(axis, first));
}

void AddSphere(std::vector<GenTriangle>& output, Vec3 center, double radius, Vec3 axis,
               std::uint32_t radial, std::uint32_t latitude, const float color[4])
{
    Vec3 u{}, v{};
    axis = Normalized(axis);
    Basis(axis, u, v);
    std::vector<std::vector<GenVertex>> rings;
    for (std::uint32_t row = 1; row < latitude; ++row) {
        const double theta = std::numbers::pi_v<double> * static_cast<double>(row)
            / static_cast<double>(latitude);
        std::vector<GenVertex> ring;
        ring.reserve(radial);
        for (std::uint32_t column = 0; column < radial; ++column) {
            const double angle = 2.0 * std::numbers::pi_v<double> * column / radial;
            const Vec3 normal = axis * std::cos(theta)
                + (u * std::cos(angle) + v * std::sin(angle)) * std::sin(theta);
            GenVertex vertex{center + normal * radius, normal, {}};
            std::copy(color, color + 4, vertex.color);
            ring.push_back(vertex);
        }
        rings.push_back(std::move(ring));
    }
    if (rings.empty()) {
        return;
    }
    GenVertex top{center + axis * radius, axis, {}};
    std::copy(color, color + 4, top.color);
    for (std::uint32_t column = 0; column < radial; ++column) {
        output.push_back({top, rings.front()[column], rings.front()[(column + 1) % radial]});
    }
    for (std::size_t row = 1; row < rings.size(); ++row) {
        for (std::uint32_t column = 0; column < radial; ++column) {
            const std::uint32_t next = (column + 1) % radial;
            output.push_back({rings[row - 1][column], rings[row][column], rings[row][next]});
            output.push_back({rings[row - 1][column], rings[row][next], rings[row - 1][next]});
        }
    }
    GenVertex bottom{center - axis * radius, axis * -1.0, {}};
    std::copy(color, color + 4, bottom.color);
    for (std::uint32_t column = 0; column < radial; ++column) {
        output.push_back({rings.back()[column], bottom, rings.back()[(column + 1) % radial]});
    }
}

void AddBeam(std::vector<GenTriangle>& output, Vec3 begin, Vec3 end, double radius0,
             double radius1, Lib3MF::eBeamLatticeCapMode cap0, Lib3MF::eBeamLatticeCapMode cap1,
             const float color[4], std::uint32_t radial)
{
    const Vec3 delta = end - begin;
    const double length = Length(delta);
    if (!(length > 1.0e-20) || !Finite(length)) {
        return;
    }
    const Vec3 axis = delta * (1.0 / length);

    // Circular frustum side, tapered radially.
    Vec3 u{}, v{};
    Basis(axis, u, v);
    std::vector<GenVertex> ring0, ring1;
    ring0.reserve(radial);
    ring1.reserve(radial);
    const double slope = (radius1 - radius0) / length;
    for (std::uint32_t column = 0; column < radial; ++column) {
        const double angle = 2.0 * std::numbers::pi_v<double> * column / radial;
        const Vec3 radialNormal = u * std::cos(angle) + v * std::sin(angle);
        const Vec3 normal = Normalized(radialNormal - axis * slope);
        GenVertex first{begin + radialNormal * radius0, normal, {}};
        GenVertex second{end + radialNormal * radius1, normal, {}};
        std::copy(color, color + 4, first.color);
        std::copy(color, color + 4, second.color);
        ring0.push_back(first);
        ring1.push_back(second);
    }
    for (std::uint32_t column = 0; column < radial; ++column) {
        const std::uint32_t next = (column + 1) % radial;
        output.push_back({ring0[column], ring0[next], ring1[next]});
        output.push_back({ring0[column], ring1[next], ring1[column]});
    }

    const auto addCap = [&](bool endCap, Lib3MF::eBeamLatticeCapMode cap, double radius) {
        const Vec3 center = endCap ? end : begin;
        const Vec3 direction = endCap ? axis : axis * -1.0;
        const auto& ring = endCap ? ring1 : ring0;
        if (cap == Lib3MF::eBeamLatticeCapMode::Butt) {
            GenVertex middle{center, direction, {}};
            std::copy(color, color + 4, middle.color);
            for (std::uint32_t column = 0; column < radial; ++column) {
                const std::uint32_t next = (column + 1) % radial;
                if (endCap) {
                    output.push_back({middle, ring[column], ring[next]});
                } else {
                    output.push_back({middle, ring[next], ring[column]});
                }
            }
        } else {
            std::uint32_t latitude = (std::max)(2u, radial / 2);
            if (cap == Lib3MF::eBeamLatticeCapMode::HemiSphere) {
                // A hemisphere cap needs only the half sphere from the pole.
                std::vector<GenTriangle> sphere;
                AddSphere(sphere, center, radius, direction, radial, latitude, color);
                // Keep only the cap-side half (z >= center along the axis): the
                // AddSphere output above is a full sphere, so filter triangles
                // whose centroid projects behind the cap plane.
                for (const GenTriangle& triangle : sphere) {
                    const Vec3 centroid = (triangle.vertex[0].position
                        + triangle.vertex[1].position + triangle.vertex[2].position) * (1.0 / 3.0);
                    if (Dot(centroid - center, direction) >= -1.0e-12) {
                        output.push_back(triangle);
                    }
                }
            } else {
                AddSphere(output, center, radius, direction, radial, latitude, color);
            }
        }
    };
    addCap(false, cap0, radius0);
    addCap(true, cap1, radius1);
}

std::uint64_t SphereTriangles(std::uint32_t radial) noexcept
{
    const std::uint32_t latitude = (std::max)(2u, radial / 2);
    return 2ull * radial * (latitude - 1);
}

std::uint64_t BeamTriangles(const Lib3MF::sBeam& beam, std::uint32_t radial) noexcept
{
    std::uint64_t count = 2ull * radial;
    for (const auto cap : beam.m_CapModes) {
        count += cap == Lib3MF::eBeamLatticeCapMode::Butt ? radial : SphereTriangles(radial);
    }
    return count;
}

// --- Axis-aligned inside clipping box (bounded subset) ---------------------

struct Aabb {
    Vec3 minimum;
    Vec3 maximum;
};

double Coordinate(const Vec3& point, std::uint32_t axis) noexcept
{
    return axis == 0 ? point.x : axis == 1 ? point.y : point.z;
}

bool Inside(const Vec3& point, std::uint32_t plane, const Aabb& box) noexcept
{
    const std::uint32_t axis = plane / 2;
    const double value = Coordinate(point, axis);
    const double limit = (plane & 1) == 0 ? Coordinate(box.minimum, axis)
                                          : Coordinate(box.maximum, axis);
    return (plane & 1) == 0 ? value >= limit : value <= limit;
}

GenVertex Interpolate(const GenVertex& a, const GenVertex& b, double t) noexcept
{
    GenVertex result;
    result.position = a.position + (b.position - a.position) * t;
    result.normal = Normalized(a.normal + (b.normal - a.normal) * t);
    for (std::uint32_t channel = 0; channel < 4; ++channel) {
        result.color[channel] = static_cast<float>(
            static_cast<double>(a.color[channel])
            + (static_cast<double>(b.color[channel]) - a.color[channel]) * t);
    }
    return result;
}

void ClipInside(std::vector<GenTriangle>& triangles, const Aabb& box)
{
    for (std::uint32_t plane = 0; plane < 6; ++plane) {
        std::vector<GenTriangle> clipped;
        for (const auto& triangle : triangles) {
            std::vector<GenVertex> polygon(std::begin(triangle.vertex),
                                           std::end(triangle.vertex));
            std::vector<GenVertex> next;
            for (std::size_t index = 0; index < polygon.size(); ++index) {
                const auto& a = polygon[index];
                const auto& b = polygon[(index + 1) % polygon.size()];
                const bool inA = Inside(a.position, plane, box);
                const bool inB = Inside(b.position, plane, box);
                if (inA) {
                    next.push_back(a);
                }
                if (inA != inB) {
                    const std::uint32_t axis = plane / 2;
                    const double limit = (plane & 1) == 0 ? Coordinate(box.minimum, axis)
                                                          : Coordinate(box.maximum, axis);
                    const double denominator = Coordinate(b.position, axis)
                        - Coordinate(a.position, axis);
                    if (std::abs(denominator) > 1.0e-30) {
                        const double t = (limit - Coordinate(a.position, axis)) / denominator;
                        next.push_back(Interpolate(a, b, std::clamp(t, 0.0, 1.0)));
                    }
                }
            }
            for (std::size_t index = 1; index + 1 < next.size(); ++index) {
                clipped.push_back({next[0], next[index], next[index + 1]});
            }
        }
        triangles.swap(clipped);
        if (triangles.empty()) {
            return;
        }
    }
}

// --- Required-extension scan -----------------------------------------------

bool EqualsIgnoreCase(char a, char b) noexcept
{
    return std::tolower(static_cast<unsigned char>(a))
        == std::tolower(static_cast<unsigned char>(b));
}

bool LooksLikeNameBoundary(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/';
}

bool AllowedNamespace(std::string_view uri) noexcept
{
    return uri == "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
        || uri == "http://schemas.microsoft.com/3dmanufacturing/material/2015/02"
        || uri == "http://schemas.microsoft.com/3dmanufacturing/production/2015/06"
        || uri == "http://schemas.microsoft.com/3dmanufacturing/beamlattice/2017/02"
        || uri == "http://schemas.microsoft.com/3dmanufacturing/beamlattice/balls/2020/07";
}

// Bounded byte-level scan of a root model part. Rejects a `requiredextensions`
// entry whose prefix resolves to a namespace outside the allowlist, and any DTD
// or entity declaration. This is deliberately not a general XML parser.
ErrorCode ScanModelPart(std::span<const std::byte> part)
{
    const char* data = reinterpret_cast<const char*>(part.data());
    const std::size_t size = part.size();
    for (std::size_t index = 0; index + 10 <= size; ++index) {
        if (data[index] == '<' && data[index + 1] == '!' && data[index + 2] == 'D'
            && EqualsIgnoreCase(data[index + 3], 'O') && EqualsIgnoreCase(data[index + 4], 'C')
            && EqualsIgnoreCase(data[index + 5], 'T') && EqualsIgnoreCase(data[index + 6], 'Y')
            && EqualsIgnoreCase(data[index + 7], 'P') && EqualsIgnoreCase(data[index + 8], 'E')) {
            return ErrorCode::MalformedData;
        }
        if (data[index] == '<' && data[index + 1] == '!' && EqualsIgnoreCase(data[index + 2], 'E')
            && EqualsIgnoreCase(data[index + 3], 'N') && EqualsIgnoreCase(data[index + 4], 'T')
            && EqualsIgnoreCase(data[index + 5], 'I') && EqualsIgnoreCase(data[index + 6], 'T')
            && EqualsIgnoreCase(data[index + 7], 'Y')) {
            return ErrorCode::MalformedData;
        }
    }

    // Locate the root <model ...> start tag.
    std::size_t position = 0;
    bool found = false;
    while (position + 6 <= size) {
        if (data[position] == '<' && (position + 6 == size || size - position >= 6)
            && std::strncmp(data + position + 1, "model", 5) == 0
            && (position + 6 >= size || LooksLikeNameBoundary(data[position + 6]))) {
            found = true;
            break;
        }
        ++position;
    }
    if (!found) {
        return ErrorCode::None; // no model element: lib3mf will reject it
    }

    // Parse the start-tag attributes into (name, value) pairs.
    std::size_t cursor = position + 6;
    std::vector<std::pair<std::string, std::string>> attributes;
    while (cursor < size) {
        while (cursor < size && (data[cursor] == ' ' || data[cursor] == '\t'
               || data[cursor] == '\r' || data[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor >= size) {
            break;
        }
        if (data[cursor] == '>' || data[cursor] == '/') {
            break;
        }
        const std::size_t nameStart = cursor;
        while (cursor < size && data[cursor] != '=' && data[cursor] != ' '
               && data[cursor] != '\t' && data[cursor] != '\r' && data[cursor] != '\n'
               && data[cursor] != '>' && data[cursor] != '/') {
            ++cursor;
        }
        if (cursor == nameStart) {
            break;
        }
        std::string name(data + nameStart, cursor - nameStart);
        while (cursor < size && (data[cursor] == ' ' || data[cursor] == '\t'
               || data[cursor] == '\r' || data[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor >= size || data[cursor] != '=') {
            break;
        }
        ++cursor;
        while (cursor < size && (data[cursor] == ' ' || data[cursor] == '\t'
               || data[cursor] == '\r' || data[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor >= size || (data[cursor] != '"' && data[cursor] != '\'')) {
            return ErrorCode::MalformedData;
        }
        const char quote = data[cursor++];
        const std::size_t valueStart = cursor;
        while (cursor < size && data[cursor] != quote) {
            ++cursor;
        }
        if (cursor >= size) {
            return ErrorCode::MalformedData;
        }
        std::string value(data + valueStart, cursor - valueStart);
        ++cursor;
        if (attributes.size() >= 256) {
            return ErrorCode::ResourceLimit;
        }
        attributes.emplace_back(std::move(name), std::move(value));
    }

    std::string_view required;
    for (const auto& [name, value] : attributes) {
        if (name == "requiredextensions") {
            required = value;
            break;
        }
    }
    if (required.empty()) {
        return ErrorCode::None;
    }
    if (required.size() > 4096) {
        return ErrorCode::ResourceLimit;
    }

    std::size_t offset = 0;
    std::uint32_t count = 0;
    while (offset < required.size()) {
        while (offset < required.size() && (required[offset] == ' ' || required[offset] == '\t'
               || required[offset] == '\r' || required[offset] == '\n')) {
            ++offset;
        }
        if (offset == required.size()) {
            break;
        }
        const std::size_t start = offset;
        while (offset < required.size() && required[offset] != ' ' && required[offset] != '\t'
               && required[offset] != '\r' && required[offset] != '\n') {
            ++offset;
        }
        if (++count > 32 || offset - start > 128) {
            return ErrorCode::ResourceLimit;
        }
        const std::string prefix(required.substr(start, offset - start));
        std::string_view uri;
        const std::string declaration = "xmlns:" + prefix;
        for (const auto& [name, value] : attributes) {
            if (name == declaration) {
                uri = value;
                break;
            }
        }
        if (!AllowedNamespace(uri)) {
            return ErrorCode::UnsupportedRequiredFeature;
        }
    }
    return ErrorCode::None;
}

} // namespace

// The opaque owner declared in the header. It keeps the lib3mf wrapper/model
// and the flattened root-build occurrences alive for the whole call.
struct ThreeMfModelHolder {
    struct Occurrence {
        Lib3MF::PMeshObject mesh;
        double transform[16]{};
    };

    Lib3MF::PWrapper wrapper;
    Lib3MF::PModel model;
    std::vector<Occurrence> occurrences;
    bool hasColor = false;
    bool haveOrigin = false;
    double origin[3] = {0.0, 0.0, 0.0};
};

namespace {

// Memory-backed read/seek callbacks over the bounded provider stream.
struct CallbackContext {
    const std::byte* data = nullptr;
    std::uint64_t size = 0;
    std::uint64_t position = 0;
    Deadline* deadline = nullptr;
    bool failed = false;
};

void ReadCallback(Lib3MF_uint64 destinationValue, Lib3MF_uint64 requested,
                  Lib3MF_pvoid userData) noexcept
{
    auto& context = *static_cast<CallbackContext*>(userData);
    auto* destination = reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(destinationValue));
    const std::uint64_t available = context.position < context.size
        ? context.size - context.position : 0;
    if (requested > available) {
        context.failed = true;
        if (requested <= (std::numeric_limits<std::size_t>::max)()) {
            std::memset(destination, 0, static_cast<std::size_t>(requested));
        }
        return;
    }
    if (requested != 0) {
        std::memcpy(destination, context.data + context.position,
                    static_cast<std::size_t>(requested));
    }
    context.position += requested;
}

void SeekCallback(Lib3MF_uint64 position, Lib3MF_pvoid userData) noexcept
{
    auto& context = *static_cast<CallbackContext*>(userData);
    if (position > context.size) {
        context.failed = true;
        return;
    }
    context.position = position;
}

void ProgressCallback(bool* abort, Lib3MF_double, Lib3MF::eProgressIdentifier,
                      Lib3MF_pvoid userData) noexcept
{
    const auto& context = *static_cast<const CallbackContext*>(userData);
    *abort = context.failed
        || (context.deadline != nullptr && !context.deadline->Checkpoint());
}

ErrorCode MapOpcError(import_worker::ThreeMfOpcError error) noexcept
{
    using E = import_worker::ThreeMfOpcError;
    switch (error) {
        case E::None:
            return ErrorCode::None;
        case E::Cancelled:
            return ErrorCode::Cancelled;
        case E::UnsupportedRequiredFeature:
            return ErrorCode::UnsupportedRequiredFeature;
        case E::ExternalRelationship:
            return ErrorCode::UnsafeReference;
        case E::EntryLimit:
        case E::EntryTooLarge:
        case E::AggregateTooLarge:
        case E::ExpansionRatio:
        case E::RelationshipLimit:
        case E::ModelPartLimit:
            return ErrorCode::ArchiveLimit;
        case E::NotZip:
        case E::Truncated:
        case E::MultiDisk:
        case E::InvalidDirectory:
        case E::UnsupportedCompression:
        case E::Encrypted:
        case E::UnsafePath:
        case E::DuplicatePart:
        case E::Overlap:
        case E::MissingContentTypes:
        case E::MissingRootRelationships:
        case E::MissingStartPart:
        case E::InvalidXml:
            return ErrorCode::MalformedData;
    }
    return ErrorCode::MalformedData;
}

ErrorCode MapLib3mfError(const Lib3MF::ELib3MFException& error) noexcept
{
    return error.getErrorCode() == LIB3MF_ERROR_CALCULATIONABORTED
        ? ErrorCode::Cancelled : ErrorCode::MalformedData;
}

// Resolves one 3MF property reference to a linear RGBA vertex color. Returns
// false only for a bounded/unsupported failure that must fail the call; an
// absent or unknown optional property falls back to the neutral color.
bool ResolveProperty(const Lib3MF::PModel& model, std::uint32_t resourceId,
                     std::uint32_t propertyId, float color[4], bool& hasTexture,
                     std::uint64_t& imageCount, std::uint64_t& imagePixels,
                     std::unordered_set<std::uint32_t>& validatedTextures)
{
    const auto setNeutral = [&] {
        color[0] = color[1] = color[2] = kDefaultVertexColor;
        color[3] = 1.0f;
    };
    setNeutral();
    hasTexture = false;
    if (resourceId == 0) {
        return true;
    }
    switch (model->GetPropertyTypeByID(resourceId)) {
        case Lib3MF::ePropertyType::BaseMaterial: {
            const auto group = model->GetBaseMaterialGroupByID(resourceId);
            if (!group) {
                return false;
            }
            const Lib3MF::sColor value = group->GetDisplayColor(propertyId);
            color[0] = SrgbToLinearComponent(static_cast<float>(value.m_Red) / 255.0f);
            color[1] = SrgbToLinearComponent(static_cast<float>(value.m_Green) / 255.0f);
            color[2] = SrgbToLinearComponent(static_cast<float>(value.m_Blue) / 255.0f);
            color[3] = static_cast<float>(value.m_Alpha) / 255.0f;
            return true;
        }
        case Lib3MF::ePropertyType::Colors: {
            const auto group = model->GetColorGroupByID(resourceId);
            if (!group) {
                return false;
            }
            const Lib3MF::sColor value = group->GetColor(propertyId);
            color[0] = SrgbToLinearComponent(static_cast<float>(value.m_Red) / 255.0f);
            color[1] = SrgbToLinearComponent(static_cast<float>(value.m_Green) / 255.0f);
            color[2] = SrgbToLinearComponent(static_cast<float>(value.m_Blue) / 255.0f);
            color[3] = static_cast<float>(value.m_Alpha) / 255.0f;
            return true;
        }
        case Lib3MF::ePropertyType::TexCoord: {
            const auto group = model->GetTexture2DGroupByID(resourceId);
            if (!group || !group->GetTexture2D()) {
                return false;
            }
            const std::uint32_t textureId = group->GetTexture2D()->GetUniqueResourceID();
            hasTexture = true;
            if (validatedTextures.insert(textureId).second) {
                const auto texture = group->GetTexture2D();
                const auto attachment = texture->GetAttachment();
                if (attachment) {
                    const std::uint64_t encodedSize = attachment->GetStreamSize();
                    if (encodedSize != 0 && encodedSize <= kEmbeddedImageEncodedMaxBytes) {
                        std::vector<Lib3MF_uint8> encodedBuffer;
                        attachment->WriteToBuffer(encodedBuffer);
                        const std::span<const std::byte> encoded(
                            reinterpret_cast<const std::byte*>(encodedBuffer.data()),
                            encodedBuffer.size());
                        const ImageInfo info = SniffImage(encoded);
                        const bool mimeMatches =
                            (texture->GetContentType() == Lib3MF::eTextureType::PNG && info.png)
                            || (texture->GetContentType() == Lib3MF::eTextureType::JPEG && info.jpeg);
                        const std::uint64_t pixels =
                            static_cast<std::uint64_t>(info.width) * info.height;
                        if (mimeMatches && pixels != 0
                            && imagePixels + pixels <= ProviderLimits::kDecodedTexturePixelsMax) {
                            imagePixels += pixels;
                            ++imageCount;
                        }
                    }
                }
            }
            color[0] = color[1] = color[2] = 1.0f;
            color[3] = 1.0f;
            return true;
        }
        case Lib3MF::ePropertyType::Composite: {
            const auto composite = model->GetCompositeMaterialsByID(resourceId);
            if (!composite || !composite->GetBaseMaterialGroup()) {
                return false;
            }
            std::vector<Lib3MF::sCompositeConstituent> values;
            composite->GetComposite(propertyId, values);
            if (values.empty() || values.size() > 4096) {
                return false;
            }
            double sum = 0.0;
            for (const auto& value : values) {
                if (!Finite(value.m_MixingRatio) || value.m_MixingRatio < 0.0) {
                    return false;
                }
                sum += value.m_MixingRatio;
            }
            if (!Finite(sum)) {
                return false;
            }
            const bool equalWeights = sum == 0.0;
            if (equalWeights) {
                sum = static_cast<double>(values.size());
            }
            double mixed[4]{};
            for (const auto& value : values) {
                const Lib3MF::sColor constituent =
                    composite->GetBaseMaterialGroup()->GetDisplayColor(value.m_PropertyID);
                const float weight = equalWeights ? 1.0f
                                                  : static_cast<float>(value.m_MixingRatio);
                mixed[0] += SrgbToLinearComponent(static_cast<float>(constituent.m_Red) / 255.0f)
                    * weight;
                mixed[1] += SrgbToLinearComponent(static_cast<float>(constituent.m_Green) / 255.0f)
                    * weight;
                mixed[2] += SrgbToLinearComponent(static_cast<float>(constituent.m_Blue) / 255.0f)
                    * weight;
                mixed[3] += static_cast<float>(constituent.m_Alpha) / 255.0f * weight;
            }
            for (std::uint32_t channel = 0; channel < 4; ++channel) {
                color[channel] = static_cast<float>(mixed[channel] / sum);
            }
            return true;
        }
        case Lib3MF::ePropertyType::Multi: {
            const auto group = model->GetMultiPropertyGroupByID(resourceId);
            if (!group) {
                return false;
            }
            const std::uint32_t layers = group->GetLayerCount();
            std::vector<std::uint32_t> indices;
            group->GetMultiProperty(propertyId, indices);
            if (!layers || layers != indices.size() || layers > 16) {
                return false;
            }
            float accumulated[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            bool initialized = false;
            for (std::uint32_t layerIndex = 0; layerIndex < layers; ++layerIndex) {
                const auto layer = group->GetLayer(layerIndex);
                float current[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                bool layerTexture = false;
                if (!ResolveProperty(model, layer.m_ResourceID, indices[layerIndex], current,
                                     layerTexture, imageCount, imagePixels, validatedTextures)) {
                    return false;
                }
                if (!initialized) {
                    std::copy(current, current + 4, accumulated);
                    initialized = true;
                    continue;
                }
                if (layer.m_TheBlendMethod == Lib3MF::eBlendMethod::Multiply) {
                    for (std::uint32_t channel = 0; channel < 4; ++channel) {
                        accumulated[channel] *= current[channel];
                    }
                } else if (layer.m_TheBlendMethod == Lib3MF::eBlendMethod::Mix) {
                    const float alpha = current[3];
                    for (std::uint32_t channel = 0; channel < 3; ++channel) {
                        accumulated[channel] = accumulated[channel] * (1.0f - alpha)
                            + current[channel] * alpha;
                    }
                    accumulated[3] = alpha + accumulated[3] * (1.0f - alpha);
                } else {
                    return false;
                }
                hasTexture = hasTexture || layerTexture;
            }
            std::copy(accumulated, accumulated + 4, color);
            return true;
        }
        case Lib3MF::ePropertyType::NoPropertyType:
        default:
            setNeutral();
            return true;
    }
}

bool ValidClippingBox(const Lib3MF::PMeshObject& mesh, Aabb& box)
{
    if (!mesh || mesh->GetType() != Lib3MF::eObjectType::Model
        || mesh->GetVertexCount() != 8 || mesh->GetTriangleCount() != 12) {
        return false;
    }
    std::vector<Lib3MF::sPosition> positions;
    std::vector<Lib3MF::sTriangle> triangles;
    mesh->GetVertices(positions);
    mesh->GetTriangleIndices(triangles);
    if (positions.size() != 8 || triangles.size() != 12) {
        return false;
    }
    box.minimum = {(std::numeric_limits<double>::max)(), (std::numeric_limits<double>::max)(),
                   (std::numeric_limits<double>::max)()};
    box.maximum = {-box.minimum.x, -box.minimum.y, -box.minimum.z};
    for (const auto& position : positions) {
        const Vec3 point{position.m_Coordinates[0], position.m_Coordinates[1],
                         position.m_Coordinates[2]};
        if (!Finite(point.x) || !Finite(point.y) || !Finite(point.z)) {
            return false;
        }
        box.minimum.x = (std::min)(box.minimum.x, point.x);
        box.minimum.y = (std::min)(box.minimum.y, point.y);
        box.minimum.z = (std::min)(box.minimum.z, point.z);
        box.maximum.x = (std::max)(box.maximum.x, point.x);
        box.maximum.y = (std::max)(box.maximum.y, point.y);
        box.maximum.z = (std::max)(box.maximum.z, point.z);
    }
    if (!(box.maximum.x > box.minimum.x && box.maximum.y > box.minimum.y
          && box.maximum.z > box.minimum.z)) {
        return false;
    }
    for (const auto& position : positions) {
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            const double value = position.m_Coordinates[axis];
            if (value != Coordinate(box.minimum, axis) && value != Coordinate(box.maximum, axis)) {
                return false;
            }
        }
    }
    for (const auto& triangle : triangles) {
        for (std::uint32_t index : triangle.m_Indices) {
            if (index >= positions.size()) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

ThreeMfAdapter::ThreeMfAdapter() noexcept = default;
ThreeMfAdapter::~ThreeMfAdapter() noexcept = default;

void ThreeMfAdapter::ResetState() noexcept
{
    holder_.reset();
    parsed_ = false;
    usedLattice_ = false;
    omittedUnsupportedTexture_ = false;
    buildItemCount_ = 0;
    occurrenceCount_ = 0;
    inspectedTriangles_ = 0;
    latticeTriangles_ = 0;
    embeddedImageCount_ = 0;
    embeddedImagePixels_ = 0;
    bytes_ = {};
    ownedBytes_.clear();
    ownedBytes_.shrink_to_fit();
    bytesReservation_.reset();
    input_ = AdapterInput{};
}

void ThreeMfAdapter::Reset() noexcept { ResetState(); }

ErrorCode ThreeMfAdapter::Initialize(const AdapterInput& input) noexcept
{
    return RunContainedStageMember([this, &input]() { return InitializeImpl(input); },
                                   DiagnosticStage::AdapterInitialize);
}

ErrorCode ThreeMfAdapter::InitializeImpl(const AdapterInput& input)
{
    ResetState();
    if (input.source == nullptr || input.limits == nullptr || input.deadline == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    input_ = input;
    return ErrorCode::None;
}

ErrorCode ThreeMfAdapter::SourceReadFailure() const noexcept
{
    if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    return ErrorCode::MalformedData;
}

ErrorCode ThreeMfAdapter::LoadSourceBytes()
{
    bytes_ = input_.source->ContiguousView();
    if (!bytes_.empty()) {
        return ErrorCode::None;
    }
    const std::uint64_t size = input_.source->Size();
    if (size == 0) {
        return ErrorCode::MalformedData;
    }
    if (size > ProviderLimits::kContiguousBackingMaxBytes) {
        return ErrorCode::ResourceLimit;
    }
    if (input_.ledger != nullptr) {
        auto reservation = input_.ledger->ReserveScoped(size);
        if (!reservation.has_value()) {
            return ErrorCode::ResourceLimit;
        }
        bytesReservation_ = std::move(reservation);
    }
    ownedBytes_.resize(static_cast<std::size_t>(size));
    if (!input_.source->ReadAt(0, ownedBytes_)) {
        ownedBytes_.clear();
        return SourceReadFailure();
    }
    bytes_ = std::span<const std::byte>(ownedBytes_.data(), ownedBytes_.size());
    return ErrorCode::None;
}

ErrorCode ThreeMfAdapter::PreflightPackage()
{
    import_worker::ThreeMfOpcPackage package;
    import_worker::ThreeMfOpcLimits limits;
    limits.maxEntries = kMaxArchiveEntries;
    limits.maxPathDepth = kMaxPathDepth;
    limits.maxEntryBytes = kAggregateExpansionMaxBytes;
    limits.maxExpandedBytes = kAggregateExpansionMaxBytes;
    limits.maxExpansionRatio = kExpansionRatioMax;

    const Deadline* deadline = input_.deadline;
    const auto cancelled = [deadline] { return deadline != nullptr && deadline->Checkpoint() == false; };
    const auto error = import_worker::InspectThreeMfOpc(bytes_, &package, limits, cancelled);
    const ErrorCode mapped = MapOpcError(error);
    if (mapped != ErrorCode::None) {
        return mapped;
    }

    std::uint64_t expanded = 0;
    std::vector<std::byte> part;
    for (const auto& entry : package.parts) {
        if (entry.name.size() < 6
            || entry.name.compare(entry.name.size() - 6, 6, ".model") != 0) {
            continue;
        }
        if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
            return ErrorCode::Cancelled;
        }
        const std::uint64_t remaining = expanded < kAggregateExpansionMaxBytes
            ? kAggregateExpansionMaxBytes - expanded : 0;
        const auto extracted = import_worker::ExtractThreeMfOpcPart(
            bytes_, entry, part, remaining, cancelled);
        if (extracted == import_worker::ThreeMfOpcError::Cancelled) {
            return ErrorCode::Cancelled;
        }
        if (extracted != import_worker::ThreeMfOpcError::None) {
            return MapOpcError(extracted);
        }
        expanded += part.size();
        const ErrorCode scan = ScanModelPart(std::span<const std::byte>(part.data(), part.size()));
        if (scan != ErrorCode::None) {
            return scan;
        }
    }
    return ErrorCode::None;
}

ErrorCode ThreeMfAdapter::LoadModel()
{
    try {
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        if (!wrapper) {
            return ErrorCode::InternalImporterFailure;
        }
        auto model = wrapper->CreateModel();
        if (!model) {
            return ErrorCode::InternalImporterFailure;
        }
        auto reader = model->QueryReader("3mf");
        if (!reader) {
            return ErrorCode::MalformedData;
        }
        CallbackContext context;
        context.data = bytes_.data();
        context.size = bytes_.size();
        context.deadline = input_.deadline;
        // lib3mf 2.5 strict mode rejects ordinary packages emitted by current
        // slicers. The product-owned OPC/required-extension boundary above and
        // this adapter validate every value that can affect the preview, so the
        // library runs in compatible mode and still fails closed at those owned
        // boundaries.
        reader->SetStrictModeActive(false);
        reader->SetProgressCallback(&ProgressCallback, &context);
        reader->ReadFromCallback(&ReadCallback, bytes_.size(), &SeekCallback, &context);
        if (context.failed) {
            return input_.deadline != nullptr && !input_.deadline->Checkpoint()
                ? ErrorCode::Cancelled : ErrorCode::MalformedData;
        }
        holder_ = std::make_unique<ThreeMfModelHolder>();
        holder_->wrapper = wrapper;
        holder_->model = model;
        return ErrorCode::None;
    } catch (const Lib3MFException& error) {
        return MapLib3mfError(error);
    } catch (const std::bad_alloc&) {
        return ErrorCode::OutOfMemory;
    } catch (...) {
        return ErrorCode::InternalImporterFailure;
    }
}

ErrorCode ThreeMfAdapter::BuildScene()
{
    try {
        const Lib3MF::PModel& model = holder_->model;
        const double meters = static_cast<double>(model->GetUnit());
        // Every eModelUnit value is in range; an out-of-range value would have
        // been rejected by lib3mf. Keep an explicit finite guard anyway.
        if (!Finite(meters)) {
            return ErrorCode::MalformedData;
        }
        auto build = model->GetBuildItems();
        if (!build || build->Count() == 0) {
            return ErrorCode::EmptyGeometry;
        }
        if (build->Count() > ProviderLimits::kNodesMax) {
            return ErrorCode::ResourceLimit;
        }
        buildItemCount_ = build->Count();

        std::unordered_set<std::uint32_t> recursion;
        std::uint64_t triangleTotal = 0;

        const std::function<ErrorCode(const Lib3MF::PObject&, const double[16],
                                      std::uint32_t)> visit =
            [&](const Lib3MF::PObject& object, const double parent[16], std::uint32_t depth)
            -> ErrorCode {
            if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
                return ErrorCode::Cancelled;
            }
            if (!object || depth > kMaxHierarchyDepth) {
                return ErrorCode::ResourceLimit;
            }
            if (!AllowedType(object->GetType())) {
                return ErrorCode::UnsupportedRequiredFeature;
            }
            const std::uint32_t key = object->GetUniqueResourceID();
            if (key == 0) {
                return ErrorCode::MalformedData;
            }
            if (object->IsMeshObject()) {
                auto mesh = std::dynamic_pointer_cast<Lib3MF::CMeshObject>(object);
                if (!mesh) {
                    return ErrorCode::MalformedData;
                }
                if (holder_->occurrences.size() >= ProviderLimits::kNodesMax) {
                    return ErrorCode::ResourceLimit;
                }
                const std::uint64_t meshTriangles = mesh->GetTriangleCount();
                const std::uint64_t meshVertices = mesh->GetVertexCount();
                if (meshTriangles > ProviderLimits::kTrianglesInspectedMax
                    || meshVertices > ProviderLimits::kPointsInspectedMax) {
                    return ErrorCode::ResourceLimit;
                }
                const auto nextTriangles = CheckedAdd(triangleTotal, meshTriangles);
                if (!nextTriangles.has_value()
                    || *nextTriangles > ProviderLimits::kTrianglesInspectedMax) {
                    return ErrorCode::ResourceLimit;
                }
                triangleTotal = *nextTriangles;
                ThreeMfModelHolder::Occurrence occurrence;
                occurrence.mesh = std::move(mesh);
                std::copy(parent, parent + 16, occurrence.transform);
                holder_->occurrences.push_back(std::move(occurrence));
                return ErrorCode::None;
            }
            if (!object->IsComponentsObject() || !recursion.insert(key).second) {
                return object->IsComponentsObject() ? ErrorCode::MalformedData
                                                    : ErrorCode::UnsupportedRequiredFeature;
            }
            auto components = std::dynamic_pointer_cast<Lib3MF::CComponentsObject>(object);
            if (!components) {
                recursion.erase(key);
                return ErrorCode::MalformedData;
            }
            const std::uint32_t count = components->GetComponentCount();
            if (count == 0 || count > ProviderLimits::kNodesMax) {
                recursion.erase(key);
                return ErrorCode::ResourceLimit;
            }
            for (std::uint32_t index = 0; index < count; ++index) {
                if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
                    recursion.erase(key);
                    return ErrorCode::Cancelled;
                }
                const auto component = components->GetComponent(index);
                if (!component || !component->GetObjectResource()) {
                    recursion.erase(key);
                    return ErrorCode::MalformedData;
                }
                double local[16]{};
                if (component->HasTransform()) {
                    if (!ToWire(component->GetTransform(), local) || !ValidMatrix(local)) {
                        recursion.erase(key);
                        return ErrorCode::MalformedData;
                    }
                } else {
                    Identity(local);
                }
                double world[16]{};
                if (!Multiply(local, parent, world)) {
                    recursion.erase(key);
                    return ErrorCode::MalformedData;
                }
                const ErrorCode child = visit(component->GetObjectResource(), world, depth + 1);
                if (child != ErrorCode::None) {
                    recursion.erase(key);
                    return child;
                }
            }
            recursion.erase(key);
            return ErrorCode::None;
        };

        for (std::uint64_t index = 0; index < build->Count(); ++index) {
            if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
                return ErrorCode::Cancelled;
            }
            if (!build->MoveNext()) {
                return ErrorCode::MalformedData;
            }
            const auto item = build->GetCurrent();
            if (!item || !item->GetObjectResource()) {
                return ErrorCode::MalformedData;
            }
            double placement[16]{};
            if (item->HasObjectTransform()) {
                if (!ToWire(item->GetObjectTransform(), placement) || !ValidMatrix(placement)) {
                    return ErrorCode::MalformedData;
                }
            } else {
                Identity(placement);
            }
            const ErrorCode result = visit(item->GetObjectResource(), placement, 1);
            if (result != ErrorCode::None) {
                return result;
            }
        }
        if (holder_->occurrences.empty()) {
            return ErrorCode::EmptyGeometry;
        }
        occurrenceCount_ = holder_->occurrences.size();
        inspectedTriangles_ = triangleTotal;
        return ErrorCode::None;
    } catch (const Lib3MFException& error) {
        return MapLib3mfError(error);
    } catch (const std::bad_alloc&) {
        return ErrorCode::OutOfMemory;
    } catch (...) {
        return ErrorCode::InternalImporterFailure;
    }
}

ErrorCode ThreeMfAdapter::Parse() noexcept
{
    return RunContainedStageMember([this]() { return ParseImpl(); }, DiagnosticStage::Parse);
}

ErrorCode ThreeMfAdapter::ParseImpl()
{
    if (input_.deadline == nullptr || !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    const ErrorCode load = LoadSourceBytes();
    if (load != ErrorCode::None) {
        return load;
    }
    if (bytes_.size() > ProviderLimits::kStreamMaxBytes) {
        return ErrorCode::ResourceLimit;
    }
    const ErrorCode package = PreflightPackage();
    if (package != ErrorCode::None) {
        return package;
    }
    const ErrorCode model = LoadModel();
    if (model != ErrorCode::None) {
        return model;
    }
    const ErrorCode scene = BuildScene();
    if (scene != ErrorCode::None) {
        holder_.reset();
        return scene;
    }
    parsed_ = true;
    return ErrorCode::None;
}

ErrorCode ThreeMfAdapter::EnumerateMaterials(IMaterialSink& sink) noexcept
{
    return RunContainedStageMember([this, &sink]() { return EnumerateMaterialsImpl(sink); },
                                   DiagnosticStage::Materials);
}

ErrorCode ThreeMfAdapter::EnumerateMaterialsImpl(IMaterialSink& sink)
{
    if (!parsed_ || holder_ == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    // Every triangle carries its resolved 3MF color as a vertex color that
    // modulates this white base. 0 still selects the neutral fallback.
    model_core::MaterialPayload white = NeutralMaterial();
    white.baseColorFactor[0] = 1.0f;
    white.baseColorFactor[1] = 1.0f;
    white.baseColorFactor[2] = 1.0f;
    white.baseColorFactor[3] = 1.0f;
    sink.OnMaterial(1, white);
    return ErrorCode::None;
}

ErrorCode ThreeMfAdapter::EnumerateGeometry(IGeometrySink& sink) noexcept
{
    return RunContainedStageMember([this, &sink]() { return EnumerateGeometryImpl(sink); },
                                   DiagnosticStage::Geometry);
}

ErrorCode ThreeMfAdapter::EnumerateGeometryImpl(IGeometrySink& sink)
{
    if (!parsed_ || holder_ == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    const Lib3MF::PModel& model = holder_->model;
    std::unordered_set<std::uint32_t> validatedTextures;
    std::uint64_t emittedTriangles = 0;

    const auto emitVertex = [&](const double world[16], const float point[3],
                                const float normal[3], const float color[4],
                                VertexSample& vertex) {
        double transformed[3]{};
        TransformPoint(world, point, transformed);
        if (!holder_->haveOrigin) {
            holder_->origin[0] = transformed[0];
            holder_->origin[1] = transformed[1];
            holder_->origin[2] = transformed[2];
            holder_->haveOrigin = true;
        }
        vertex.position[0] = static_cast<float>(transformed[0] - holder_->origin[0]);
        vertex.position[1] = static_cast<float>(transformed[1] - holder_->origin[1]);
        vertex.position[2] = static_cast<float>(transformed[2] - holder_->origin[2]);
        // 3MF carries no normals; a flat geometric normal is derived from the
        // transformed triangle. For lattice-generated vertices the generator
        // supplies an analytic normal transformed by the same world matrix's
        // linear part.
        if (normal != nullptr) {
            for (std::uint32_t axis = 0; axis < 3; ++axis) {
                vertex.normal[axis] = static_cast<float>(
                    static_cast<double>(normal[0]) * world[axis]
                    + static_cast<double>(normal[1]) * world[4 + axis]
                    + static_cast<double>(normal[2]) * world[8 + axis]);
            }
            const double length = std::sqrt(
                static_cast<double>(vertex.normal[0]) * vertex.normal[0]
                + static_cast<double>(vertex.normal[1]) * vertex.normal[1]
                + static_cast<double>(vertex.normal[2]) * vertex.normal[2]);
            if (length > 1.0e-20) {
                for (float& value : vertex.normal) {
                    value = static_cast<float>(value / length);
                }
            }
        } else {
            vertex.normal[0] = vertex.normal[1] = vertex.normal[2] = 0.0f;
        }
        std::copy(color, color + 4, vertex.color);
    };

    const auto emit = [&](const double world[16], const double positions[3][3],
                          const float colors[3][4]) -> bool {
        TriangleSample sample{};
        for (std::uint32_t corner = 0; corner < 3; ++corner) {
            const float point[3] = {static_cast<float>(positions[corner][0]),
                                    static_cast<float>(positions[corner][1]),
                                    static_cast<float>(positions[corner][2])};
            emitVertex(world, point, nullptr, colors[corner], sample.vertices[corner]);
        }
        // Derive a flat geometric normal from the transformed positions (3MF
        // carries no normals).
        const Vec3 a{sample.vertices[0].position[0], sample.vertices[0].position[1],
                     sample.vertices[0].position[2]};
        const Vec3 b{sample.vertices[1].position[0], sample.vertices[1].position[1],
                     sample.vertices[1].position[2]};
        const Vec3 c{sample.vertices[2].position[0], sample.vertices[2].position[1],
                     sample.vertices[2].position[2]};
        const Vec3 normal = Normalized(Cross(b - a, c - a));
        for (auto& vertex : sample.vertices) {
            vertex.normal[0] = static_cast<float>(normal.x);
            vertex.normal[1] = static_cast<float>(normal.y);
            vertex.normal[2] = static_cast<float>(normal.z);
        }
        sample.origin[0] = holder_->origin[0];
        sample.origin[1] = holder_->origin[1];
        sample.origin[2] = holder_->origin[2];
        sample.materialIndex = 1;
        ++emittedTriangles;
        return sink.OnTriangle(sample);
    };

    try {
        for (const auto& occurrence : holder_->occurrences) {
            if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
                return ErrorCode::Cancelled;
            }
            const Lib3MF::PMeshObject& mesh = occurrence.mesh;
            const double* world = occurrence.transform;
            const auto lattice = mesh->BeamLattice();
            const bool hasLattice = lattice
                && (lattice->GetBeamCount() != 0 || lattice->GetBallCount() != 0);

            if (!hasLattice) {
                const std::uint32_t vertexCount = mesh->GetVertexCount();
                const std::uint32_t triangleCount = mesh->GetTriangleCount();
                if (triangleCount == 0) {
                    continue;
                }
                std::vector<Lib3MF::sPosition> positions;
                std::vector<Lib3MF::sTriangle> triangles;
                mesh->GetVertices(positions);
                mesh->GetTriangleIndices(triangles);
                if (positions.size() != vertexCount || triangles.size() != triangleCount) {
                    return ErrorCode::MalformedData;
                }
                for (const auto& position : positions) {
                    for (const float coordinate : position.m_Coordinates) {
                        if (!std::isfinite(coordinate)) {
                            return ErrorCode::MalformedData;
                        }
                    }
                }
                std::uint32_t defaultResource = 0;
                std::uint32_t defaultProperty = 0;
                const bool hasDefault =
                    mesh->GetObjectLevelProperty(defaultResource, defaultProperty);

                bool stop = false;
                for (std::uint32_t index = 0; index < triangleCount; ++index) {
                    if ((index & 0x3FFu) == 0 && input_.deadline != nullptr
                        && !input_.deadline->Checkpoint()) {
                        return ErrorCode::Cancelled;
                    }
                    const auto& triangle = triangles[index];
                    Lib3MF::sTriangleProperties properties{};
                    mesh->GetTriangleProperties(index, properties);
                    std::uint32_t resource = properties.m_ResourceID;
                    if (resource == 0 && hasDefault) {
                        resource = defaultResource;
                    }
                    double positions3[3][3]{};
                    float colors[3][4]{};
                    for (std::uint32_t corner = 0; corner < 3; ++corner) {
                        if (triangle.m_Indices[corner] >= positions.size()) {
                            return ErrorCode::MalformedData;
                        }
                        const auto& source = positions[triangle.m_Indices[corner]];
                        positions3[corner][0] = source.m_Coordinates[0];
                        positions3[corner][1] = source.m_Coordinates[1];
                        positions3[corner][2] = source.m_Coordinates[2];
                        std::uint32_t property = properties.m_ResourceID
                            ? properties.m_PropertyIDs[corner] : defaultProperty;
                        bool hasTexture = false;
                        if (!ResolveProperty(model, resource, property, colors[corner],
                                             hasTexture, embeddedImageCount_,
                                             embeddedImagePixels_, validatedTextures)) {
                            return ErrorCode::MalformedData;
                        }
                        omittedUnsupportedTexture_ = omittedUnsupportedTexture_ || hasTexture;
                    }
                    if (!emit(world, positions3, colors)) {
                        stop = true;
                        break;
                    }
                }
                if (stop) {
                    return ErrorCode::None;
                }
                continue;
            }

            // Bounded beam/ball lattice preview.
            usedLattice_ = true;
            std::uint32_t objectResource = 0;
            std::uint32_t objectProperty = 0;
            const bool objectHasProperty =
                mesh->GetObjectLevelProperty(objectResource, objectProperty);
            float latticeColor[4] = {kDefaultVertexColor, kDefaultVertexColor,
                                     kDefaultVertexColor, 1.0f};
            if (objectHasProperty) {
                bool hasTexture = false;
                if (!ResolveProperty(model, objectResource, objectProperty, latticeColor,
                                     hasTexture, embeddedImageCount_, embeddedImagePixels_,
                                     validatedTextures)) {
                    return ErrorCode::MalformedData;
                }
                omittedUnsupportedTexture_ = omittedUnsupportedTexture_ || hasTexture;
            }

            Lib3MF::eBeamLatticeClipMode clipMode = Lib3MF::eBeamLatticeClipMode::NoClipMode;
            std::uint32_t clippingId = 0;
            lattice->GetClipping(clipMode, clippingId);
            Aabb clippingBox{};
            const bool clipped = clipMode == Lib3MF::eBeamLatticeClipMode::Inside;
            if (clipMode != Lib3MF::eBeamLatticeClipMode::NoClipMode) {
                if (!clipped) {
                    return ErrorCode::UnsupportedRequiredFeature;
                }
                const auto clippingResource = model->GetResourceByID(clippingId);
                const auto clippingMesh =
                    std::dynamic_pointer_cast<Lib3MF::CMeshObject>(clippingResource);
                if (!ValidClippingBox(clippingMesh, clippingBox)) {
                    return ErrorCode::UnsupportedRequiredFeature;
                }
            }

            const std::uint32_t vertexCount = mesh->GetVertexCount();
            std::vector<Lib3MF::sPosition> rawPositions;
            mesh->GetVertices(rawPositions);
            if (rawPositions.size() != vertexCount || vertexCount == 0) {
                return ErrorCode::MalformedData;
            }
            std::vector<Vec3> latticePositions;
            latticePositions.reserve(rawPositions.size());
            for (const auto& position : rawPositions) {
                const Vec3 point{position.m_Coordinates[0], position.m_Coordinates[1],
                                 position.m_Coordinates[2]};
                if (!Finite(point.x) || !Finite(point.y) || !Finite(point.z)) {
                    return ErrorCode::MalformedData;
                }
                latticePositions.push_back(point);
            }

            std::vector<Lib3MF::sBeam> beams;
            lattice->GetBeams(beams);
            if (beams.size() > kLatticeTrianglesMax) {
                return ErrorCode::ResourceLimit;
            }
            std::vector<Lib3MF::sBall> balls;
            lattice->GetBalls(balls);
            Lib3MF::eBeamLatticeBallMode ballMode =
                Lib3MF::eBeamLatticeBallMode::BeamLatticeBallModeNone;
            double defaultBallRadius = 0.0;
            lattice->GetBallOptions(ballMode, defaultBallRadius);

            const double beamCount = static_cast<double>(beams.size());
            const double ballCount = static_cast<double>(balls.size());
            if (beamCount + ballCount > static_cast<double>(kLatticeTrianglesMax)) {
                return ErrorCode::ResourceLimit;
            }

            std::vector<Lib3MF::sBeam> validBeams;
            validBeams.reserve(beams.size());
            for (const auto& beam : beams) {
                if (beam.m_Indices[0] >= latticePositions.size()
                    || beam.m_Indices[1] >= latticePositions.size()) {
                    return ErrorCode::MalformedData;
                }
                double radius0 = beam.m_Radii[0];
                double radius1 = beam.m_Radii[1];
                if (!Finite(radius0) || radius0 <= 0.0) {
                    radius0 = radius1 > 0.0 ? radius1 : 0.0;
                }
                if (!Finite(radius1) || radius1 <= 0.0) {
                    radius1 = radius0;
                }
                if (radius0 <= 0.0) {
                    // lib3mf may return an unspecified default radius as 0;
                    // derive a bounded positive preview radius from the mesh
                    // extent rather than dropping the beam.
                    double extent = 0.0;
                    for (const Vec3& point : latticePositions) {
                        extent = (std::max)(extent, std::abs(point.x));
                        extent = (std::max)(extent, std::abs(point.y));
                        extent = (std::max)(extent, std::abs(point.z));
                    }
                    radius0 = radius1 = (std::max)(extent * 0.01, 1.0e-6);
                }
                Lib3MF::sBeam adjusted = beam;
                adjusted.m_Radii[0] = radius0;
                adjusted.m_Radii[1] = radius1;
                validBeams.push_back(adjusted);
            }

            const std::array<std::uint32_t, 6> radialCandidates{16, 12, 8, 6, 4, 3};
            std::uint32_t radial = 0;
            const std::uint64_t remaining = inspectedTriangles_ + emittedTriangles
                    < ProviderLimits::kTrianglesInspectedMax
                ? ProviderLimits::kTrianglesInspectedMax - (inspectedTriangles_ + emittedTriangles)
                : 0;
            for (const std::uint32_t candidate : radialCandidates) {
                std::uint64_t estimate = 0;
                for (const auto& beam : validBeams) {
                    estimate += BeamTriangles(beam, candidate);
                }
                if (ballMode == Lib3MF::eBeamLatticeBallMode::All) {
                    const std::uint64_t unique = static_cast<std::uint64_t>(vertexCount);
                    estimate += unique * SphereTriangles(candidate);
                } else if (ballMode == Lib3MF::eBeamLatticeBallMode::Mixed) {
                    estimate += static_cast<std::uint64_t>(balls.size())
                        * SphereTriangles(candidate);
                }
                if (estimate <= kLatticeTrianglesMax && estimate <= remaining) {
                    radial = candidate;
                    break;
                }
            }
            if (radial == 0) {
                return ErrorCode::ResourceLimit;
            }

            std::vector<GenTriangle> generated;
            for (const auto& beam : validBeams) {
                AddBeam(generated, latticePositions[beam.m_Indices[0]],
                        latticePositions[beam.m_Indices[1]], beam.m_Radii[0], beam.m_Radii[1],
                        beam.m_CapModes[0], beam.m_CapModes[1], latticeColor, radial);
            }
            if (ballMode == Lib3MF::eBeamLatticeBallMode::All) {
                for (const auto& point : latticePositions) {
                    AddSphere(generated, point,
                              (std::max)(defaultBallRadius, 1.0e-6), Vec3{0, 0, 1}, radial,
                              (std::max)(2u, radial / 2), latticeColor);
                }
            } else if (ballMode == Lib3MF::eBeamLatticeBallMode::Mixed) {
                for (const auto& ball : balls) {
                    if (ball.m_Index >= latticePositions.size()) {
                        return ErrorCode::MalformedData;
                    }
                    const double radius = Finite(ball.m_Radius) && ball.m_Radius > 0.0
                        ? ball.m_Radius : (std::max)(defaultBallRadius, 1.0e-6);
                    AddSphere(generated, latticePositions[ball.m_Index], radius, Vec3{0, 0, 1},
                              radial, (std::max)(2u, radial / 2), latticeColor);
                }
            }
            if (clipped) {
                ClipInside(generated, clippingBox);
            }
            if (generated.size() > kLatticeTrianglesMax) {
                return ErrorCode::ResourceLimit;
            }
            latticeTriangles_ += generated.size();

            bool stop = false;
            for (const auto& triangle : generated) {
                double positions3[3][3]{};
                float colors[3][4]{};
                for (std::uint32_t corner = 0; corner < 3; ++corner) {
                    positions3[corner][0] = triangle.vertex[corner].position.x;
                    positions3[corner][1] = triangle.vertex[corner].position.y;
                    positions3[corner][2] = triangle.vertex[corner].position.z;
                    std::copy(triangle.vertex[corner].color,
                              triangle.vertex[corner].color + 4, colors[corner]);
                }
                // Lattice generators supply per-vertex normals; emit each
                // corner with its own analytic normal.
                TriangleSample sample{};
                for (std::uint32_t corner = 0; corner < 3; ++corner) {
                    const float cornerNormal[3] = {
                        static_cast<float>(triangle.vertex[corner].normal.x),
                        static_cast<float>(triangle.vertex[corner].normal.y),
                        static_cast<float>(triangle.vertex[corner].normal.z)};
                    const float point[3] = {
                        static_cast<float>(positions3[corner][0]),
                        static_cast<float>(positions3[corner][1]),
                        static_cast<float>(positions3[corner][2])};
                    emitVertex(world, point, cornerNormal, colors[corner],
                               sample.vertices[corner]);
                }
                sample.origin[0] = holder_->origin[0];
                sample.origin[1] = holder_->origin[1];
                sample.origin[2] = holder_->origin[2];
                sample.materialIndex = 1;
                ++emittedTriangles;
                if (!sink.OnTriangle(sample)) {
                    stop = true;
                    break;
                }
            }
            if (stop) {
                return ErrorCode::None;
            }
        }
        return ErrorCode::None;
    } catch (const Lib3MFException& error) {
        return MapLib3mfError(error);
    } catch (const std::bad_alloc&) {
        return ErrorCode::OutOfMemory;
    } catch (...) {
        return ErrorCode::InternalImporterFailure;
    }
}

} // namespace preview3d::provider
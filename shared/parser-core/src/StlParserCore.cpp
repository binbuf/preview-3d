#include "parser_core/StlParserCore.h"

#include "platform/CheckedMath.h"

#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>

namespace parser_core {

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

double Dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool IsFinite(const Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

float ReadFloatLE(const std::byte* p)
{
    float value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

std::uint32_t ReadU32LE(const std::byte* p)
{
    std::uint32_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

bool NormalizeStlFacet(const StlFacet& facet, NormalizedStlFacet& out)
{
    if (!IsFinite(facet.normal) || !IsFinite(facet.v0) || !IsFinite(facet.v1) || !IsFinite(facet.v2)) {
        return false; // non-finite facet: dropped
    }

    Vec3 flatNormal = Cross(facet.v1 - facet.v0, facet.v2 - facet.v0);
    double flatLengthSquared = Dot(flatNormal, flatNormal);
    if (flatLengthSquared <= 0.0 || !std::isfinite(flatLengthSquared)) {
        return false; // degenerate (near-zero-area) facet: dropped
    }
    double flatInvLength = 1.0 / std::sqrt(flatLengthSquared);
    Vec3 flatNormalized{ flatNormal.x * flatInvLength, flatNormal.y * flatInvLength,
                         flatNormal.z * flatInvLength };

    double suppliedLengthSquared = Dot(facet.normal, facet.normal);
    if (suppliedLengthSquared > 0.81f && suppliedLengthSquared < 1.21f) {
        double invLength = 1.0 / std::sqrt(suppliedLengthSquared);
        out.normal = { facet.normal.x * invLength, facet.normal.y * invLength,
                       facet.normal.z * invLength };
    } else {
        out.normal = flatNormalized;
    }

    out.position[0] = facet.v0;
    out.position[1] = facet.v1;
    out.position[2] = facet.v2;
    return true;
}

bool DecodeStlBinaryHeader(std::span<const std::byte> prefix, StlBinaryHeader& out)
{
    out = StlBinaryHeader{};
    if (prefix.size() < kStlPrefixBytes) {
        return false;
    }
    out.triangleCount = ReadU32LE(prefix.data() + kStlHeaderBytes);
    if (out.triangleCount > kMaxStlFacets) {
        return false;
    }
    auto facetBytes = platform::CheckedMultiply(std::uint64_t(out.triangleCount), std::uint64_t(kStlFacetBytes));
    auto expectedSize = facetBytes
        ? platform::CheckedAdd(std::uint64_t(kStlPrefixBytes), *facetBytes)
        : std::nullopt;
    if (!expectedSize) {
        return false;
    }
    out.expectedMinSize = *expectedSize;
    return true;
}

bool IsAsciiStl(std::span<const std::byte> sourcePrefix, std::uint64_t sourceSize)
{
    StlBinaryHeader header;
    const bool binaryShape = DecodeStlBinaryHeader(sourcePrefix, header) && sourceSize >= header.expectedMinSize;
    constexpr std::string_view keyword = "solid";
    return !binaryShape && sourcePrefix.size() >= keyword.size()
        && std::string_view(reinterpret_cast<const char*>(sourcePrefix.data()), keyword.size()) == keyword;
}

} // namespace parser_core
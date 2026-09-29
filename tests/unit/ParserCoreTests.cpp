// T07 parser-core regression coverage.
//
// These cases pin the behaviour of the format-agnostic STL/PLY parser
// primitives extracted into shared/parser-core. They compile the shared
// sources directly into Tests.Unit.exe with no worker/broker/viewer header,
// which is the same compile boundary the thumbnail provider consumes, and they
// lock the facet-normal policy and PLY scalar/header semantics that both the
// import worker (StlAdapter/PlyAdapter) and the provider rely on.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "parser_core/AsciiTokenizer.h"
#include "parser_core/PlyParserCore.h"
#include "parser_core/StlParserCore.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::span<const std::byte> AsBytes(std::string_view text)
{
    return { reinterpret_cast<const std::byte*>(text.data()), text.size() };
}

} // namespace

TEST_CASE("ascii tokenizer bounds tokens and parses locale-independent numbers",
          "[parser][tokenizer]")
{
    parser_core::AsciiTokenizer tokenizer(AsBytes("solid 1.5 -2e3\r\nfacet"));
    REQUIRE(tokenizer.NextToken().value() == "solid");
    REQUIRE(tokenizer.NextNumber().value() == Catch::Approx(1.5));
    REQUIRE(tokenizer.NextNumber().value() == Catch::Approx(-2000.0));
    REQUIRE(tokenizer.NextToken().value() == "facet");
    REQUIRE_FALSE(tokenizer.NextToken().has_value());
}

TEST_CASE("ascii tokenizer rejects an oversize token", "[parser][tokenizer]")
{
    const std::string oversize(600, 'x');
    parser_core::AsciiTokenizer tokenizer(AsBytes(oversize));
    REQUIRE_FALSE(tokenizer.NextToken().has_value());
}

TEST_CASE("stl facet validation generates and trusts normals", "[parser][stl]")
{
    parser_core::StlFacet flat{};
    flat.v0 = { 0.0, 0.0, 0.0 };
    flat.v1 = { 1.0, 0.0, 0.0 };
    flat.v2 = { 0.0, 1.0, 0.0 };
    parser_core::NormalizedStlFacet out{};
    REQUIRE(parser_core::NormalizeStlFacet(flat, out));
    REQUIRE(out.normal.z == Catch::Approx(1.0));
    REQUIRE(out.position[1].x == Catch::Approx(1.0));

    parser_core::StlFacet supplied{};
    supplied.normal = { 0.0, 1.0, 0.0 };
    supplied.v0 = flat.v0;
    supplied.v1 = flat.v1;
    supplied.v2 = flat.v2;
    REQUIRE(parser_core::NormalizeStlFacet(supplied, out));
    REQUIRE(out.normal.y == Catch::Approx(1.0));

    parser_core::StlFacet degenerate{};
    degenerate.v0 = degenerate.v1 = degenerate.v2 = { 1.0, 2.0, 3.0 };
    REQUIRE_FALSE(parser_core::NormalizeStlFacet(degenerate, out));

    parser_core::StlFacet nonFinite{};
    nonFinite.v0 = { std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0 };
    nonFinite.v1 = flat.v1;
    nonFinite.v2 = flat.v2;
    REQUIRE_FALSE(parser_core::NormalizeStlFacet(nonFinite, out));
}

TEST_CASE("stl dialect detection prefers a valid binary shape", "[parser][stl]")
{
    std::vector<std::byte> binary(84, std::byte{ 0 });
    std::memcpy(binary.data(), "solid", 5);
    const std::uint32_t count = 0;
    std::memcpy(binary.data() + 80, &count, sizeof(count));
    REQUIRE_FALSE(parser_core::IsAsciiStl(binary, binary.size()));

    REQUIRE(parser_core::IsAsciiStl(AsBytes("solid facet"), 11));
    REQUIRE_FALSE(parser_core::IsAsciiStl(AsBytes("notstl"), 6));
}

TEST_CASE("ply header parser accepts ascii and both binary endians", "[parser][ply]")
{
    const auto ascii = AsBytes("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\n"
                               "property float y\nproperty float z\nend_header\n");
    auto parsed = parser_core::ParseHeader(ascii);
    const auto* header = std::get_if<parser_core::PlyHeader>(&parsed);
    REQUIRE(header != nullptr);
    REQUIRE(header->format == parser_core::PlyFormat::Ascii);
    REQUIRE(header->elements.size() == 1);
    REQUIRE(header->elements[0].count == 3);
    REQUIRE(parser_core::IsAsciiPly(ascii));

    auto bigEndian = parser_core::ParseHeader(
        AsBytes("ply\nformat binary_big_endian 1.0\nelement vertex 1\nproperty float x\nend_header\n"));
    const auto* beHeader = std::get_if<parser_core::PlyHeader>(&bigEndian);
    REQUIRE(beHeader != nullptr);
    REQUIRE(beHeader->format == parser_core::PlyFormat::BinaryBigEndian);

    auto malformed = parser_core::ParseHeader(AsBytes("nope\n"));
    REQUIRE(std::get_if<model_core::ImportErrorCode>(&malformed) != nullptr);
    REQUIRE(*std::get_if<model_core::ImportErrorCode>(&malformed) ==
            model_core::ImportErrorCode::MalformedData);
}

TEST_CASE("ply scalar reads honor endianness and integer ranges", "[parser][ply]")
{
    const std::byte littleEndian[] = { std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0x80 },
                                       std::byte{ 0x3f } };
    REQUIRE(parser_core::ReadScalarAsDouble(parser_core::PlyScalarType::Float32, false, littleEndian) ==
            Catch::Approx(1.0));
    const std::byte bigEndian[] = { std::byte{ 0x3f }, std::byte{ 0x80 }, std::byte{ 0x00 },
                                    std::byte{ 0x00 } };
    REQUIRE(parser_core::ReadScalarAsDouble(parser_core::PlyScalarType::Float32, true, bigEndian) ==
            Catch::Approx(1.0));
    REQUIRE(parser_core::ScalarByteSize(parser_core::PlyScalarType::Float64) == 8);
    REQUIRE(parser_core::NormalizeColor(255.0, parser_core::PlyScalarType::UInt8) == Catch::Approx(1.0f));
    REQUIRE(parser_core::NormalizeColor(0.0, parser_core::PlyScalarType::UInt8) == Catch::Approx(0.0f));
}
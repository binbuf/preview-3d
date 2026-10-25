// SEC-16: the shared glTF base64 data-URI preflight that guards the fastgltf
// 0.9.0 fallback-decoder over-write (`base64::fallback_decode_inplace`) on a
// payload whose encoded length is not a multiple of four. Header-only, so the
// shipped helper is exercised directly here.

#include "model_core/GltfDataUriPreflight.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace {

using model_core::GltfDataUriStatus;
using model_core::ValidateGltfDataUri;

constexpr std::uint64_t kCap = 32ull * 1024 * 1024;

} // namespace

TEST_CASE("ValidateGltfDataUri accepts a well-formed base64 buffer URI", "[gltf][data-uri]")
{
    CHECK(ValidateGltfDataUri("data:application/octet-stream;base64,AAAA", kCap)
          == GltfDataUriStatus::Ok);
    CHECK(ValidateGltfDataUri("data:;base64,AAA=", kCap) == GltfDataUriStatus::Ok);
    CHECK(ValidateGltfDataUri("data:image/png;base64,AA==", kCap) == GltfDataUriStatus::Ok);
}

TEST_CASE("ValidateGltfDataUri ignores non-base64 and non-data URIs", "[gltf][data-uri]")
{
    CHECK(ValidateGltfDataUri("buffer.bin", kCap) == GltfDataUriStatus::NotDataUri);
    CHECK(ValidateGltfDataUri("data/thing.bin", kCap) == GltfDataUriStatus::NotDataUri);
    CHECK(ValidateGltfDataUri("data:application/octet-stream,raw%20bytes", kCap)
          == GltfDataUriStatus::NotDataUri);
}

TEST_CASE("ValidateGltfDataUri rejects the fastgltf 0.9.0 overflow shape",
          "[gltf][data-uri][security]")
{
    // The minimized SEC-16 finding: a 66-character payload (66 % 4 == 2).
    const std::string payload(66, 'A');
    CHECK(ValidateGltfDataUri("data:application/octet-stream;base64," + payload, kCap)
          == GltfDataUriStatus::Malformed);
    // A single trailing character is also not a 4-character group.
    CHECK(ValidateGltfDataUri("data:;base64,AAAAA", kCap) == GltfDataUriStatus::Malformed);
}

TEST_CASE("ValidateGltfDataUri rejects invalid base64 characters and misplaced padding",
          "[gltf][data-uri][security]")
{
    CHECK(ValidateGltfDataUri("data:;base64,AAA$", kCap) == GltfDataUriStatus::Malformed);
    CHECK(ValidateGltfDataUri("data:;base64,AA=A", kCap) == GltfDataUriStatus::Malformed);
    CHECK(ValidateGltfDataUri("data:;base64,=AAA", kCap) == GltfDataUriStatus::Malformed);
}

TEST_CASE("ValidateGltfDataUri rejects a decoded payload over the cap",
          "[gltf][data-uri][security]")
{
    // 4 characters decode to 3 bytes; 96 characters decode to 72 > 64.
    CHECK(ValidateGltfDataUri("data:;base64," + std::string(96, 'A'), /*maxDecodedBytes=*/64)
          == GltfDataUriStatus::TooLarge);
    // An empty payload is legal and decodes to zero bytes.
    CHECK(ValidateGltfDataUri("data:;base64,", kCap) == GltfDataUriStatus::Ok);
}

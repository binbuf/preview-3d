#include "OpenWithCache.h"

#include <catch2/catch_test_macros.hpp>

using open_with::CacheState;
using open_with::CachedHandler;

TEST_CASE("Open With cache round-trips bounded Unicode handler metadata", "[open-with]")
{
    CacheState source;
    source.catalogRevision = 7;
    source.lastDiscoveryFileTime = 123456789;
    source.handlers = {
        CachedHandler{ L".stl", L"Applications\\blender.exe", L"Blender", L"blender" },
        CachedHandler{ L".ply", L"Vendor.雪.Modeler", L"Modeler 雪", L"" },
    };

    std::vector<std::byte> bytes;
    REQUIRE(open_with::EncodeCache(source, bytes));
    CacheState decoded;
    REQUIRE(open_with::DecodeCache(bytes, decoded));
    CHECK(decoded.catalogRevision == source.catalogRevision);
    CHECK(decoded.lastDiscoveryFileTime == source.lastDiscoveryFileTime);
    CHECK(decoded.handlers == source.handlers);
}

TEST_CASE("Open With cache rejects truncation, trailing data, and duplicate identities", "[open-with]")
{
    CacheState source;
    source.handlers = { CachedHandler{ L".glb", L"Applications\\viewer.exe", L"Viewer", L"" } };
    std::vector<std::byte> bytes;
    REQUIRE(open_with::EncodeCache(source, bytes));

    CacheState decoded;
    auto truncated = bytes;
    truncated.pop_back();
    CHECK_FALSE(open_with::DecodeCache(truncated, decoded));

    auto trailing = bytes;
    trailing.push_back(std::byte{ 0 });
    CHECK_FALSE(open_with::DecodeCache(trailing, decoded));

    source.handlers.push_back(source.handlers.front());
    REQUIRE(open_with::EncodeCache(source, bytes));
    CHECK_FALSE(open_with::DecodeCache(bytes, decoded));
    CHECK(decoded.handlers.empty());
}

TEST_CASE("Open With cache rejects invalid UTF-8 and oversized fields", "[open-with]")
{
    CacheState source;
    source.handlers = { CachedHandler{ L".stl", L"handler", L"Display", L"" } };
    std::vector<std::byte> bytes;
    REQUIRE(open_with::EncodeCache(source, bytes));
    // First record starts at byte 24. The extension payload starts after its
    // two-byte length; corrupt its first byte into an invalid UTF-8 lead byte.
    bytes[26] = std::byte{ 0xff };
    CacheState decoded;
    CHECK_FALSE(open_with::DecodeCache(bytes, decoded));

    source.handlers.front().displayName.assign(2000, L'x');
    CHECK_FALSE(open_with::EncodeCache(source, bytes));
    CHECK(bytes.empty());
}

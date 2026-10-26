// The update check must never nag because an untrusted response sorted above
// the running build. These cases pin SemVer precedence and the exact fields
// accepted from the release feed.

#include "VersionCompare.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using preview3d_update::ExtractGitHubReleaseTag;
using preview3d_update::IsVersionNewer;
using preview3d_update::NormalizeVersion;

TEST_CASE("update versions normalize to canonical SemVer", "[update]")
{
    REQUIRE(NormalizeVersion(L"1.2.3") == L"1.2.3");
    REQUIRE(NormalizeVersion(L" v1.2.3 \r\n") == L"1.2.3");
    REQUIRE(NormalizeVersion(L"V1.2.3+build.5") == L"1.2.3");
    REQUIRE(NormalizeVersion(L"1.2.3-rc.1+build.5") == L"1.2.3-rc.1");
    REQUIRE(NormalizeVersion(L"1.2") == L"1.2.0");

    REQUIRE(NormalizeVersion(L"").empty());
    REQUIRE(NormalizeVersion(L"v").empty());
    REQUIRE(NormalizeVersion(L"1.2.3.4").empty());
    REQUIRE(NormalizeVersion(L"1.x.3").empty());
    REQUIRE(NormalizeVersion(L"1.2.3 beta").empty());
    REQUIRE(NormalizeVersion(L"1.2.3-").empty());
}

TEST_CASE("update check orders releases by SemVer precedence", "[update]")
{
    REQUIRE(IsVersionNewer(L"1.2.4", L"1.2.3"));
    REQUIRE(IsVersionNewer(L"1.3.0", L"1.2.9"));
    REQUIRE(IsVersionNewer(L"2.0.0", L"1.99.99"));
    REQUIRE(IsVersionNewer(L"1.2.3", L"1.2.3-rc.1"));
    REQUIRE(IsVersionNewer(L"1.2.3-rc.2", L"1.2.3-rc.1"));
    REQUIRE(IsVersionNewer(L"1.2.3-rc.1", L"1.2.3-alpha.9"));
    REQUIRE(IsVersionNewer(L"1.2.3-alpha.10", L"1.2.3-alpha.9"));
    REQUIRE(IsVersionNewer(L"1.2.3-alpha.1", L"1.2.3-alpha"));

    REQUIRE_FALSE(IsVersionNewer(L"1.2.3", L"1.2.3"));
    REQUIRE_FALSE(IsVersionNewer(L"1.2.3", L"1.2.4"));
    REQUIRE_FALSE(IsVersionNewer(L"1.2.3-rc.1", L"1.2.3"));
    REQUIRE_FALSE(IsVersionNewer(L"not-a-version", L"1.2.3"));
    REQUIRE_FALSE(IsVersionNewer(L"9.9.9", L"not-a-version"));
}

TEST_CASE("GitHub release JSON yields only a bounded tag_name", "[update]")
{
    REQUIRE(ExtractGitHubReleaseTag(L"{ \"tag_name\": \"v1.2.3\", \"name\": \"Preview 3D\" }") == L"v1.2.3");
    REQUIRE(ExtractGitHubReleaseTag(L"{\"tag_name\":\"v1.2.3\"}") == L"v1.2.3");
    REQUIRE(ExtractGitHubReleaseTag(L"{ \"name\": \"no tag\" }").empty());
    REQUIRE(ExtractGitHubReleaseTag(L"{ \"tag_name\": \"v1\\u002e2\" }").empty());
    REQUIRE(ExtractGitHubReleaseTag(L"{ \"tag_name\": 12 }").empty());
}

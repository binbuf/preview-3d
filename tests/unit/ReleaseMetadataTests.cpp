// SEC-19/T19: the release SBOM/license metadata is generated from the installed
// vcpkg closure and guarded by packaging/Test-ReleaseMetadata.ps1. This case
// drives that gate, including its negative self-test (a deliberately unmapped
// installed dependency must fail), so the "manual list that cannot fail" defect
// cannot return without turning this suite red.
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

#ifndef PREVIEW3D_REPO_ROOT
#error "PREVIEW3D_REPO_ROOT must be defined by Tests.Unit.vcxproj."
#endif

namespace {

std::string Quote(const std::filesystem::path& path)
{
    return "\"" + path.string() + "\"";
}

}

TEST_CASE("release metadata gate passes and rejects drift", "[security][release-metadata]")
{
    const std::filesystem::path repository = std::filesystem::path(PREVIEW3D_REPO_ROOT);
    const auto script = repository / "packaging" / "Test-ReleaseMetadata.ps1";
    REQUIRE(std::filesystem::exists(script));

    // -SelfTest runs the real checks plus two negative cases: an installed
    // dependency with no SPDX mapping, and a manifest/installer version drift.
    // Invoke the same Windows PowerShell interpreter the MSBuild packaging
    // targets use, so the gate is proven on the shipping interpreter.
    const std::string command =
        "powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File " + Quote(script) +
        " -RepositoryRoot " + Quote(repository) + " -SelfTest";

    const int result = std::system(command.c_str());
    INFO("command: " << command);
    REQUIRE(result == 0);
}
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

std::wstring Quote(const std::filesystem::path& path)
{
    return L"\"" + path.wstring() + L"\"";
}

} // namespace

TEST_CASE("release metadata gate passes and rejects drift", "[security][release-metadata]")
{
    // PREVIEW3D_REPO_ROOT is defined from $(SolutionDir), which ends with a
    // directory separator. A path whose final character is '\' inside a quoted
    // Windows command line escapes the closing quote, so PowerShell would parse
    // "...\preview-3d\" as an unterminated argument and reject the path with
    // "Illegal characters in path". Strip the trailing separator before the
    // argument is assembled.
    std::wstring repository = PREVIEW3D_REPO_ROOT;
    while (!repository.empty() && (repository.back() == L'\\' || repository.back() == L'/'))
    {
        repository.pop_back();
    }

    const std::filesystem::path repositoryPath(repository);
    const auto script = repositoryPath / "packaging" / "Test-ReleaseMetadata.ps1";
    REQUIRE(std::filesystem::exists(script));

    // -SelfTest runs the real checks plus two negative cases: an installed
    // dependency with no SPDX mapping, and a manifest/installer version drift.
    // Invoke the same Windows PowerShell interpreter the MSBuild packaging
    // targets use, so the gate is proven on the shipping interpreter.
    const std::wstring command =
        L"powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File " + Quote(script) +
        L" -RepositoryRoot " + Quote(repositoryPath) + L" -SelfTest";

    const int result = _wsystem(command.c_str());
    INFO("release metadata gate exit code: " << result);
    REQUIRE(result == 0);
}
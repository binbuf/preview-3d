// Direct coverage of import_broker::SourceFileAccess -- the trusted-
// process-side file-open/canonicalize step and the broker-side inheritable-
// handle-duplication step that together make a real on-disk file reachable
// by the sandboxed worker (see GltfImportTests.cpp's
// RunGltfImportFromRealFile for the actual end-to-end proof). No
// AppContainer/SandboxFixture needed here -- these are plain Win32 checks.

#include "import_broker/SourceFileAccess.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <string>

#ifndef PREVIEW3D_TEST_ASSETS_DIR
#error "PREVIEW3D_TEST_ASSETS_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace {

std::wstring TestAssetPath(const wchar_t* fileName)
{
    return std::wstring(PREVIEW3D_TEST_ASSETS_DIR) + fileName;
}

} // namespace

TEST_CASE("OpenAndCanonicalizeSourceFile succeeds on a real fixture and returns a non-empty absolute path",
          "[import-broker]")
{
    auto result = import_broker::OpenAndCanonicalizeSourceFile(TestAssetPath(L"tri_tight.glb"));
    REQUIRE(result.file);
    CHECK(result.error.empty());
    CHECK_FALSE(result.canonicalPath.empty());
    // GetFinalPathNameByHandleW's default (FILE_NAME_NORMALIZED, VOLUME_NAME_DOS)
    // form is an extended-length path beginning with \\?\ or \\?\UNC\.
    CHECK(result.canonicalPath.rfind(LR"(\\?\)", 0) == 0);
}

TEST_CASE("OpenAndCanonicalizeSourceFile fails cleanly on a nonexistent path", "[import-broker]")
{
    auto result = import_broker::OpenAndCanonicalizeSourceFile(TestAssetPath(L"does_not_exist.glb"));
    CHECK_FALSE(static_cast<bool>(result.file));
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("DuplicateInheritableHandle produces a distinct, usable, genuinely inheritable handle",
          "[import-broker]")
{
    auto opened = import_broker::OpenAndCanonicalizeSourceFile(TestAssetPath(L"tri_tight.glb"));
    REQUIRE(opened.file);

    auto duplicated = import_broker::DuplicateInheritableHandle(opened.file.get());
    REQUIRE(duplicated.has_value());
    CHECK(duplicated->get() != opened.file.get());
    CHECK(GetFileType(duplicated->get()) == FILE_TYPE_DISK);

    // The inheritable bit is actually set -- proven directly rather than
    // only inferred from a successful sandboxed launch elsewhere.
    DWORD flags = 0;
    REQUIRE(GetHandleInformation(duplicated->get(), &flags));
    CHECK((flags & HANDLE_FLAG_INHERIT) != 0);
}

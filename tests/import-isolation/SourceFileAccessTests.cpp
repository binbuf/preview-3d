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

TEST_CASE("OpenAndCanonicalizeSourceFile rejects an alternate data stream on the primary source",
          "[import-broker]")
{
    // CreateFileW would silently open the hidden stream named after the
    // colon; the broker must reject the reference instead, matching the
    // sidecar resolver's policy.
    auto result = import_broker::OpenAndCanonicalizeSourceFile(
        TestAssetPath(L"tri_tight.glb") + L":hidden");
    CHECK_FALSE(static_cast<bool>(result.file));
    CHECK(result.errorCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("OpenAndCanonicalizeSourceFile rejects a drive-relative path", "[import-broker]")
{
    auto result = import_broker::OpenAndCanonicalizeSourceFile(L"C:tri_tight.glb");
    CHECK_FALSE(static_cast<bool>(result.file));
    CHECK(result.errorCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("OpenAndCanonicalizeSourceFile rejects remote and device paths before opening", "[import-broker]")
{
    // Every one of these must be refused by classification, never by a
    // post-open canonical check: reaching CreateFileW would initiate an SMB or
    // device open first. A forward-slash UNC and an extended UNC are the cases
    // the old prefix checks missed.
    const wchar_t* hostile[] = {
        L"//server/share/tri_tight.glb",              // forward-slash UNC
        L"\\\\server\\share\\tri_tight.glb",          // backslash UNC
        L"\\\\?\\UNC\\server\\share\\tri_tight.glb",  // extended UNC
        L"\\\\.\\C:\\tri_tight.glb",                  // device namespace
        L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume0\\tri_tight.glb",
    };
    std::size_t index = 0;
    for (const wchar_t* candidate : hostile) {
        INFO("hostile primary " << index);
        auto result = import_broker::OpenAndCanonicalizeSourceFile(candidate);
        CHECK_FALSE(static_cast<bool>(result.file));
        CHECK(result.errorCode == model_core::ImportErrorCode::UnsafeReference);
        ++index;
    }
}

TEST_CASE("OpenAndCanonicalizeSourceFile rejects a relative primary path", "[import-broker]")
{
    // A relative primary resolves against the process CWD, unlike the sidecar
    // resolver; the broker now requires a drive-qualified absolute path.
    auto bare = import_broker::OpenAndCanonicalizeSourceFile(L"tri_tight.glb");
    CHECK_FALSE(static_cast<bool>(bare.file));
    CHECK(bare.errorCode == model_core::ImportErrorCode::UnsafeReference);

    auto nested = import_broker::OpenAndCanonicalizeSourceFile(L"test-assets\\tri_tight.glb");
    CHECK_FALSE(static_cast<bool>(nested.file));
    CHECK(nested.errorCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("OpenAndCanonicalizeSourceFile accepts a forward-slash absolute local path", "[import-broker]")
{
    // The classifier normalizes separators for classification only; a local
    // absolute path with forward slashes is still a valid Win32 path and opens.
    std::wstring forwardSlash = TestAssetPath(L"tri_tight.glb");
    for (wchar_t& character : forwardSlash) {
        if (character == L'\\') character = L'/';
    }
    auto result = import_broker::OpenAndCanonicalizeSourceFile(forwardSlash);
    REQUIRE(result.file);
    CHECK(result.error.empty());
    CHECK(result.canonicalPath.rfind(LR"(\\?\)", 0) == 0);
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

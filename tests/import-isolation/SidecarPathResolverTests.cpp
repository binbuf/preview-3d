// Stage 2 (sidecar path-containment validator) unit tests -- pure, no
// sandbox/worker needed. Exercises import_broker::ResolveSidecarPath
// directly against a real temp-directory tree.

#include "import_broker/SidecarPathResolver.h"
#include "import_broker/SourceFileAccess.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// A fresh scratch directory per test, containing a primary file
// ("scene.gltf") and whatever sidecar files the test writes into it (or
// outside it, to exercise escape rejection). Cleaned up on destruction.
struct ScratchGltfDirectory {
    std::wstring directory;
    std::wstring primaryCanonicalPath;

    ScratchGltfDirectory()
    {
        wchar_t tempDir[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
        wchar_t uniquePath[MAX_PATH]{};
        REQUIRE(GetTempFileNameW(tempDir, L"p3d", 0, uniquePath) != 0);
        REQUIRE(DeleteFileW(uniquePath));
        directory = uniquePath;
        REQUIRE(CreateDirectoryW(directory.c_str(), nullptr));

        std::wstring primaryPath = directory + L"\\scene.gltf";
        std::ofstream primary(primaryPath, std::ios::binary | std::ios::trunc);
        primary << "{}";
        primary.close();

        auto opened = import_broker::OpenAndCanonicalizeSourceFile(primaryPath);
        REQUIRE(opened.file);
        primaryCanonicalPath = opened.canonicalPath;
    }

    void WriteSibling(const std::wstring& relativeName, size_t sizeBytes = 4) const
    {
        WriteRelative(relativeName, sizeBytes);
    }

    // Writes a file at a path relative to the primary's directory, creating
    // any intermediate subdirectories (the "textures/foo.jpg" layout).
    void WriteRelative(const std::wstring& relativeName, size_t sizeBytes = 4) const
    {
        const std::filesystem::path path = std::filesystem::path(directory) / relativeName;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        std::string content(sizeBytes, 'x');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    ~ScratchGltfDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

} // namespace

TEST_CASE("A valid relative sidecar reference resolves successfully", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    dir.WriteSibling(L"mesh.bin");

    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "mesh.bin", 1024);
    REQUIRE(result.file);
    CHECK(result.fileSizeBytes == 4);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::None);
}

TEST_CASE("A sidecar reference into a subdirectory of the primary directory resolves",
          "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    dir.WriteRelative(L"textures\\diffuse.png");

    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "textures/diffuse.png", 1024);
    REQUIRE(result.file);
    CHECK(result.fileSizeBytes == 4);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::None);
}

TEST_CASE("A sidecar reference into a nested subdirectory of the primary directory resolves",
          "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    dir.WriteRelative(L"assets\\textures\\diffuse.png");

    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath,
                                                    "assets/textures/diffuse.png", 1024);
    REQUIRE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::None);
}

TEST_CASE("A reference that traverses up and back into a subdirectory is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    dir.WriteRelative(L"textures\\diffuse.png");

    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath,
                                                    "textures/../diffuse.png", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("An empty sidecar reference is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("A drive-relative reference (contains ':') is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "C:mesh.bin", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("A data: URI scheme reference is rejected by the ':' rule", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "data:application/octet-stream;base64,AAAA", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("A UNC-style reference is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "\\\\localhost\\share\\mesh.bin", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("An absolute reference is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "/etc/mesh.bin", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("A '..'-traversal reference is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "../outside.bin", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("A reference with a disallowed extension is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    dir.WriteSibling(L"payload.exe");
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "payload.exe", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
    DeleteFileW((dir.directory + L"\\payload.exe").c_str());
}

TEST_CASE("An embedded NUL cannot defeat the extension allowlist", "[sidecar-resolver][security]")
{
    ScratchGltfDirectory dir;
    // The real file whose name and extension an attacker wants opened.
    dir.WriteSibling(L"secret.pdf", 8);

    // Each string is a real, disallowed name, then an embedded NUL, then text
    // that makes the untruncated std::filesystem::path::extension() look
    // allowed ("secret.pdf\0.png" -> ".png"). Before this fix CreateFileW
    // truncated at the NUL and opened secret.pdf, which these cases pin.
    const std::string afterExtension("secret.pdf\0.png", 15);
    const std::string bareAfterExtension("secret.pdf\0", 11);
    for (const std::string& attack : { afterExtension, bareAfterExtension }) {
        CAPTURE(attack.size());
        const auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, attack, 1024);
        CHECK_FALSE(result.file);
        CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
    }

    // The mirror shape: a NUL before a real allowed extension still truncates
    // the open to a name that was never referenced.
    dir.WriteSibling(L"mesh.bin", 4);
    const auto leading = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath,
                                                           std::string("mesh.bin\0.png", 13), 1024);
    CHECK_FALSE(leading.file);
    CHECK(leading.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("C0 control characters and DEL in a reference are rejected", "[sidecar-resolver][security]")
{
    ScratchGltfDirectory dir;
    dir.WriteSibling(L"mesh.bin");

    const std::string withSoh("mesh\x01.bin", 9);
    const std::string withLf("mesh\n.bin", 9);
    const std::string withTab("mesh\t.bin", 9);
    const std::string withDel("mesh.bin\x7f", 9);
    for (const std::string& attack : { withSoh, withLf, withTab, withDel }) {
        CAPTURE(static_cast<int>(attack.back()));
        const auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, attack, 1024);
        CHECK_FALSE(result.file);
        CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
    }
}

TEST_CASE("A reference that is not valid UTF-8 is rejected", "[sidecar-resolver][security]")
{
    ScratchGltfDirectory dir;
    dir.WriteSibling(L"mesh.bin");

    const std::string badLead("mesh\xff.bin", 9);
    const std::string badPair("mesh\xc3\x28.bin", 10); // truncated 2-byte sequence
    const std::string loneContinuation("mesh\x80.bin", 9);
    for (const std::string& attack : { badLead, badPair, loneContinuation }) {
        CAPTURE(attack.size());
        const auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, attack, 1024);
        CHECK_FALSE(result.file);
        CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
    }
}

TEST_CASE("The allowed sidecar extension set follows the requesting format",
          "[sidecar-resolver][security]")
{
    ScratchGltfDirectory dir;
    dir.WriteSibling(L"material.mtl");
    dir.WriteSibling(L"layer.usdc");
    dir.WriteSibling(L"mesh.bin");
    const std::vector<std::wstring> noRoots{};

    // glTF may not pull an MTL file or a USD layer.
    for (const char* reference : { "material.mtl", "layer.usdc" }) {
        CAPTURE(reference);
        const auto gltf = import_broker::ResolveSidecarPath(
            dir.primaryCanonicalPath, reference, 1024, /*allowPackageBasenameLookup=*/false, noRoots,
            import_broker::ImportFormat::Gltf);
        CHECK_FALSE(gltf.file);
        CHECK(gltf.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
    }

    // OBJ wants .mtl and images, not a glTF .bin buffer.
    const auto objMtl = import_broker::ResolveSidecarPath(
        dir.primaryCanonicalPath, "material.mtl", 1024, false, noRoots, import_broker::ImportFormat::Obj);
    CHECK(objMtl.file);
    const auto objBin = import_broker::ResolveSidecarPath(
        dir.primaryCanonicalPath, "mesh.bin", 1024, false, noRoots, import_broker::ImportFormat::Obj);
    CHECK_FALSE(objBin.file);
    CHECK(objBin.rejectionCode == model_core::ImportErrorCode::UnsafeReference);

    // USD wants layers, not a .bin buffer.
    const auto usdLayer = import_broker::ResolveSidecarPath(
        dir.primaryCanonicalPath, "layer.usdc", 1024, false, noRoots, import_broker::ImportFormat::Usd);
    CHECK(usdLayer.file);
    const auto usdBin = import_broker::ResolveSidecarPath(
        dir.primaryCanonicalPath, "mesh.bin", 1024, false, noRoots, import_broker::ImportFormat::Usd);
    CHECK_FALSE(usdBin.file);

    // A format that resolves nothing accepts no sidecar at all.
    const auto stl = import_broker::ResolveSidecarPath(
        dir.primaryCanonicalPath, "mesh.bin", 1024, false, noRoots, import_broker::ImportFormat::Stl);
    CHECK_FALSE(stl.file);
    CHECK(stl.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("A missing sidecar file is rejected as FileUnavailable", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "missing.bin", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::FileUnavailable);
}

TEST_CASE("A sidecar file exceeding the byte cap is rejected as FileUnavailable", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    dir.WriteSibling(L"mesh.bin", 100);
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "mesh.bin", /*maxSidecarFileBytes=*/10);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::FileUnavailable);
}

TEST_CASE(".ktx2 is an allowed sidecar extension", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    dir.WriteSibling(L"texture.ktx2");
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "texture.ktx2", 1024);
    CHECK(result.file);
    DeleteFileW((dir.directory + L"\\texture.ktx2").c_str());
}

namespace {

// The downloaded-model layout the FBX package search exists for:
//
//   <root>/source/scene.gltf
//   <root>/textures/tex.png
//
// The primary file is nested one level below the package root, mirroring
// Sketchfab/Unity-style exports whose texture sidecars sit beside `source/`.
struct ScratchPackageDirectory {
    std::wstring root;
    std::wstring primaryCanonicalPath;

    ScratchPackageDirectory()
    {
        wchar_t tempDir[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
        wchar_t uniquePath[MAX_PATH]{};
        REQUIRE(GetTempFileNameW(tempDir, L"p3d", 0, uniquePath) != 0);
        REQUIRE(DeleteFileW(uniquePath));
        root = uniquePath;
        REQUIRE(CreateDirectoryW(root.c_str(), nullptr));
        REQUIRE(CreateDirectoryW((root + L"\\source").c_str(), nullptr));

        std::wstring primaryPath = root + L"\\source\\scene.gltf";
        std::ofstream primary(primaryPath, std::ios::binary | std::ios::trunc);
        primary << "{}";
        primary.close();

        auto opened = import_broker::OpenAndCanonicalizeSourceFile(primaryPath);
        REQUIRE(opened.file);
        primaryCanonicalPath = opened.canonicalPath;
    }

    void WriteRelative(const std::wstring& relativeName, size_t sizeBytes = 4) const
    {
        const std::filesystem::path path = std::filesystem::path(root) / relativeName;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        std::string content(sizeBytes, 'x');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    ~ScratchPackageDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

} // namespace

TEST_CASE("A bare name missing beside the primary resolves under the package root when enabled",
          "[sidecar-resolver][package-lookup]")
{
    ScratchPackageDirectory dir;
    dir.WriteRelative(L"textures\\tex.png");

    // Without the package opt-in the reference stays contained to the
    // primary's own directory tree, exactly as before.
    const auto contained = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "tex.png", 1024);
    CHECK_FALSE(contained.file);
    CHECK(contained.rejectionCode == model_core::ImportErrorCode::FileUnavailable);

    const auto package = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "tex.png", 1024,
                                                           /*allowPackageBasenameLookup=*/true);
    REQUIRE(package.file);
    CHECK(package.fileSizeBytes == 4);
    CHECK(package.rejectionCode == model_core::ImportErrorCode::None);
    CHECK(package.canonicalPath.find(L"textures") != std::wstring::npos);

    // A safe multi-component reference that misses directly (the Honda-E
    // `Substance/Textures/.../<name>` shape) is also retried by file name.
    const auto nested = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath,
                                                          "Substance/Textures/tex.png", 1024,
                                                          /*allowPackageBasenameLookup=*/true);
    REQUIRE(nested.file);
    CHECK(nested.canonicalPath.find(L"textures") != std::wstring::npos);
}

TEST_CASE("Package lookup still prefers a file beside the primary over a package match",
          "[sidecar-resolver][package-lookup]")
{
    ScratchPackageDirectory dir;
    dir.WriteRelative(L"source\\tex.png", 8);
    dir.WriteRelative(L"textures\\tex.png", 4);

    const auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "tex.png", 1024,
                                                          /*allowPackageBasenameLookup=*/true);
    REQUIRE(result.file);
    CHECK(result.fileSizeBytes == 8);
}

TEST_CASE("Package lookup only searches texture folders and prefers the closest",
          "[sidecar-resolver][package-lookup]")
{
    ScratchPackageDirectory dir;
    dir.WriteRelative(L"source\\texture\\tex.png", 2);  // beside the model's own texture folder
    dir.WriteRelative(L"textures\\tex.png", 4);         // parent package texture folder
    dir.WriteRelative(L"assets\\tex.png", 8);           // not a texture folder: ignored

    const auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "tex.png", 1024,
                                                          /*allowPackageBasenameLookup=*/true);
    REQUIRE(result.file);
    CHECK(result.fileSizeBytes == 2); // <primary>/texture beats <parent>/textures
}

TEST_CASE("Package lookup is image-only and never treats a name as a pattern",
          "[sidecar-resolver][package-lookup]")
{
    ScratchPackageDirectory dir;
    dir.WriteRelative(L"textures\\mesh.bin");
    dir.WriteRelative(L"textures\\tex.png");

    // A missing required .bin is not rescued by the image fallback.
    const auto buffer = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "mesh.bin", 1024,
                                                          /*allowPackageBasenameLookup=*/true);
    CHECK_FALSE(buffer.file);
    CHECK(buffer.rejectionCode == model_core::ImportErrorCode::FileUnavailable);

    // A wildcard is a miss, not a match against whatever it happens to hit.
    const auto wildcard = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "*.png", 1024,
                                                            /*allowPackageBasenameLookup=*/true);
    CHECK_FALSE(wildcard.file);
    CHECK(wildcard.rejectionCode == model_core::ImportErrorCode::FileUnavailable);
}

TEST_CASE("Package lookup normalizes renamed texture files",
          "[sidecar-resolver][package-lookup]")
{
    ScratchPackageDirectory dir;
    // A download that renamed the source files: spaces became underscores,
    // case changed, and the container is .jpeg.
    dir.WriteRelative(L"textures\\Foo Bar_BaseColor.jpeg");

    const auto result = import_broker::ResolveSidecarPath(
        dir.primaryCanonicalPath, "Something/Else/Foo_Bar_BaseColor.jpg", 1024,
        /*allowPackageBasenameLookup=*/true);
    REQUIRE(result.file);
    CHECK(result.canonicalPath.find(L"Foo Bar_BaseColor.jpeg") != std::wstring::npos);
}

TEST_CASE("Package lookup matches abbreviated roles and download annotations",
          "[sidecar-resolver][package-lookup]")
{
    ScratchPackageDirectory dir;
    dir.WriteRelative(L"textures\\Thing_B.png");                  // BaseColor abbreviated
    dir.WriteRelative(L"textures\\Other_(Personalizado).png");    // download annotation
    dir.WriteRelative(L"textures\\Third_N.png");                  // normal, wrong role below

    const auto base = import_broker::ResolveSidecarPath(
        dir.primaryCanonicalPath, "exported/Thing_BaseColor.png", 1024,
        /*allowPackageBasenameLookup=*/true);
    REQUIRE(base.file);
    CHECK(base.canonicalPath.find(L"Thing_B.png") != std::wstring::npos);

    const auto annotated = import_broker::ResolveSidecarPath(
        dir.primaryCanonicalPath, "Other.png", 1024, /*allowPackageBasenameLookup=*/true);
    REQUIRE(annotated.file);
    CHECK(annotated.canonicalPath.find(L"Other_(Personalizado).png") != std::wstring::npos);

    // Same stem but a different role is not a match.
    const auto wrongRole = import_broker::ResolveSidecarPath(
        dir.primaryCanonicalPath, "Third_BaseColor.png", 1024,
        /*allowPackageBasenameLookup=*/true);
    CHECK_FALSE(wrongRole.file);
    CHECK(wrongRole.rejectionCode == model_core::ImportErrorCode::FileUnavailable);
}

TEST_CASE("Package lookup never rescues a syntactically unsafe reference",
          "[sidecar-resolver][package-lookup][security]")
{
    ScratchPackageDirectory dir;
    dir.WriteRelative(L"textures\\outside.png");

    for (const char* attack : { "../outside.png", "C:/outside.png", "//server/share/outside.png",
                                "https://example.invalid/outside.png", "outside.png:stream" }) {
        CAPTURE(attack);
        const auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, attack, 1024,
                                                              /*allowPackageBasenameLookup=*/true);
        CHECK_FALSE(result.file);
        CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
    }
}

TEST_CASE("Package lookup finds a model-root texture folder and same-folder images",
          "[sidecar-resolver][package-lookup]")
{
    // `model/model.gltf` + `model/texture/...` (the second common layout): the
    // reference names a folder that does not exist, and the file is found in
    // the model's own tree by name.
    ScratchGltfDirectory model;
    model.WriteRelative(L"texture\\pattern.png");
    const auto textureFolder = import_broker::ResolveSidecarPath(
        model.primaryCanonicalPath, "textures/pattern.png", 1024,
        /*allowPackageBasenameLookup=*/true);
    REQUIRE(textureFolder.file);
    CHECK(textureFolder.canonicalPath.find(L"texture") != std::wstring::npos);

    // Images sitting beside the model resolve the same way even when the
    // authored reference includes a directory that is not present.
    ScratchGltfDirectory sameFolder;
    sameFolder.WriteSibling(L"pattern.png");
    const auto sameFolderResult = import_broker::ResolveSidecarPath(
        sameFolder.primaryCanonicalPath, "images/pattern.png", 1024,
        /*allowPackageBasenameLookup=*/true);
    REQUIRE(sameFolderResult.file);
    CHECK(sameFolderResult.canonicalPath.find(L"images") == std::wstring::npos);
}

TEST_CASE("The model's own folder wins over a same-named file in a sibling package",
          "[sidecar-resolver][package-lookup]")
{
    ScratchGltfDirectory model;
    model.WriteRelative(L"textures\\pkgtex.png", 8);

    // A second package beside this one in the same parent directory. A search
    // rooted at the parent would see both and give up as ambiguous; the
    // closest-tree phase must resolve this model's own file.
    const std::filesystem::path sibling = std::filesystem::path(model.directory) / L".."
        / (std::filesystem::path(model.directory).filename().wstring() + L"-sibling");
    std::filesystem::create_directories(sibling / L"textures");
    { std::ofstream out(sibling / L"textures" / L"pkgtex.png", std::ios::binary); out << "zzzz"; }

    const auto result = import_broker::ResolveSidecarPath(model.primaryCanonicalPath, "pkgtex.png",
                                                          1024, /*allowPackageBasenameLookup=*/true);
    REQUIRE(result.file);
    CHECK(result.fileSizeBytes == 8);

    std::error_code error;
    std::filesystem::remove_all(sibling, error);
}

namespace {

// A user-chosen asset root: an otherwise-unrelated directory the user points
// the viewer at after a missing-asset warning. Mirrors the package fixture but
// deliberately lives outside the primary file's directory tree.
struct ScratchAssetRoot {
    std::wstring path;

    ScratchAssetRoot()
    {
        wchar_t tempDir[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
        wchar_t uniquePath[MAX_PATH]{};
        REQUIRE(GetTempFileNameW(tempDir, L"p3d", 0, uniquePath) != 0);
        REQUIRE(DeleteFileW(uniquePath));
        path = uniquePath;
        REQUIRE(CreateDirectoryW(path.c_str(), nullptr));
    }

    void WriteAt(const std::wstring& relativeName, size_t sizeBytes = 4) const
    {
        const std::filesystem::path file = std::filesystem::path(path) / relativeName;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        std::string content(sizeBytes, 'y');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    ~ScratchAssetRoot()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

} // namespace

TEST_CASE("A user-chosen asset root resolves a missing sidecar outside the primary tree",
          "[sidecar-resolver][asset-root]")
{
    ScratchPackageDirectory model;   // <root>/source/scene.gltf, with no sidecars
    ScratchAssetRoot assets;         // an unrelated directory the user picked
    assets.WriteAt(L"tex.png");

    // Without the asset root the reference stays contained to the primary's
    // own tree and misses, exactly as before this fallback existed.
    const auto contained = import_broker::ResolveSidecarPath(model.primaryCanonicalPath, "tex.png", 1024);
    CHECK_FALSE(contained.file);
    CHECK(contained.rejectionCode == model_core::ImportErrorCode::FileUnavailable);

    // The authored relative path is ignored for a user root: only the leaf
    // name is matched, so a reference into a directory the user's folder does
    // not contain still resolves.
    const std::vector<std::wstring> roots{ assets.path };
    const auto resolved = import_broker::ResolveSidecarPath(
        model.primaryCanonicalPath, "textures/tex.png", 1024, /*allowPackageBasenameLookup=*/true, roots);
    REQUIRE(resolved.file);
    CHECK(resolved.rejectionCode == model_core::ImportErrorCode::None);
    CHECK(resolved.canonicalPath.find(L"tex.png") != std::wstring::npos);
}

TEST_CASE("A user-chosen asset root also searches its texture subfolders",
          "[sidecar-resolver][asset-root]")
{
    ScratchPackageDirectory model;
    ScratchAssetRoot assets;
    assets.WriteAt(L"textures\\pattern.png");

    const std::vector<std::wstring> roots{ assets.path };
    const auto resolved = import_broker::ResolveSidecarPath(
        model.primaryCanonicalPath, "images/pattern.png", 1024, /*allowPackageBasenameLookup=*/true, roots);
    REQUIRE(resolved.file);
    CHECK(resolved.canonicalPath.find(L"pattern.png") != std::wstring::npos);
}

TEST_CASE("A user-chosen asset root resolves non-image sidecars by exact name",
          "[sidecar-resolver][asset-root]")
{
    ScratchPackageDirectory model;
    ScratchAssetRoot assets;
    assets.WriteAt(L"mesh.bin");

    const std::vector<std::wstring> roots{ assets.path };
    const auto resolved = import_broker::ResolveSidecarPath(
        model.primaryCanonicalPath, "data/mesh.bin", 1024, /*allowPackageBasenameLookup=*/true, roots);
    REQUIRE(resolved.file);
    CHECK(resolved.canonicalPath.find(L"mesh.bin") != std::wstring::npos);
}

TEST_CASE("A user-chosen asset root is still bounded to its own directory tree",
          "[sidecar-resolver][asset-root][security]")
{
    ScratchPackageDirectory model;
    ScratchAssetRoot assets;
    // A same-named file one level above the chosen root must never be reached,
    // because only the root and its texture subfolders are searched.
    const std::filesystem::path parent = std::filesystem::path(assets.path).parent_path();
    const std::filesystem::path outside = parent / (L"outside-" +
        std::filesystem::path(assets.path).filename().wstring() + L".png");
    { std::ofstream out(outside, std::ios::binary); out << "zzzz"; }

    const std::vector<std::wstring> roots{ assets.path };
    const auto resolved = import_broker::ResolveSidecarPath(
        model.primaryCanonicalPath, "tex.png", 1024, /*allowPackageBasenameLookup=*/true, roots);
    CHECK_FALSE(resolved.file);
    CHECK(resolved.rejectionCode == model_core::ImportErrorCode::FileUnavailable);

    std::error_code error;
    std::filesystem::remove(outside, error);
}

// The root prefix used for containment is now derived from the opened directory
// handle, so every ordinary spelling of the same root must still resolve: no
// trailing separator, one present, an extended-length "\\?\" prefix (the form
// GetFinalPathNameByHandleW itself returns), and any case.
TEST_CASE("A user-chosen asset root resolves for equivalent path spellings",
          "[sidecar-resolver][asset-root]")
{
    ScratchPackageDirectory model;
    ScratchAssetRoot assets;
    assets.WriteAt(L"spelled.png");

    std::wstring upper = assets.path;
    for (wchar_t& c : upper) {
        c = static_cast<wchar_t>(std::towupper(c));
    }
    const std::vector<std::wstring> roots{
        assets.path,
        assets.path + L"\\",
        L"\\\\?\\" + assets.path,
        upper,
    };

    for (const std::wstring& root : roots) {
        CAPTURE(root);
        const std::vector<std::wstring> single{ root };
        const auto resolved = import_broker::ResolveSidecarPath(
            model.primaryCanonicalPath, "textures/spelled.png", 1024,
            /*allowPackageBasenameLookup=*/true, single);
        REQUIRE(resolved.file);
        CHECK(resolved.canonicalPath.find(L"spelled.png") != std::wstring::npos);
    }
}

// Fail-closed: a root that cannot be opened/canonicalized is skipped, never
// compared as a raw string prefix.
TEST_CASE("A user-chosen asset root that cannot be canonicalized is skipped",
          "[sidecar-resolver][asset-root][security]")
{
    ScratchPackageDirectory model;
    const std::filesystem::path missing =
        std::filesystem::path(model.root) / L"absent-asset-root";

    const std::vector<std::wstring> roots{ missing.wstring() };
    const auto resolved = import_broker::ResolveSidecarPath(
        model.primaryCanonicalPath, "tex.png", 1024,
        /*allowPackageBasenameLookup=*/true, roots);
    CHECK_FALSE(resolved.file);
    CHECK(resolved.rejectionCode == model_core::ImportErrorCode::FileUnavailable);
}
